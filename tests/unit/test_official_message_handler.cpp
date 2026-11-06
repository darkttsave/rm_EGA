// SPDX-License-Identifier: MIT
#include "core/GameData.h"
#include "network/OfficialMessageHandler.h"
#include "network/NetworkManager.h"
#include "robomaster.pb.h"
#include <QSignalSpy>
#include <QJsonArray>
#include <QQuickWidget>
#include <QQmlContext>
#include <QQuickItem>
#include <QTcpServer>
#include <QProcess>
#include <QStandardPaths>
#include <QtTest>

using RM::OfficialMessageHandler;
namespace {
QStringList detailedLogs;
void captureDetails(QtMsgType type, const QMessageLogContext &, const QString &message) {
  if (type == QtInfoMsg && (message.startsWith("[GlobalUnitStatus]") ||
                            message.startsWith("[RobotStaticStatus]") ||
                            message.startsWith("[RobotDynamicStatus]")))
    detailedLogs.append(message);
}
class DetailLogCapture {
public:
  DetailLogCapture() { detailedLogs.clear(); previous = qInstallMessageHandler(captureDetails); }
  ~DetailLogCapture() { qInstallMessageHandler(previous); }
private:
  QtMessageHandler previous = nullptr;
};
template <typename T> QByteArray bytes(const T &message) {
  const std::string data = message.SerializeAsString();
  return QByteArray(data.data(), static_cast<int>(data.size()));
}
}

class TestOfficialMessageHandler : public QObject {
  Q_OBJECT
private slots:
  void identityAndGlobalHealthThroughOfficialTopics() {
    GameData data;
    data.setMyRobotId(101);
    robomaster::RobotStaticStatus status;
    status.set_robot_id(101);
    status.set_robot_type(8);
    status.set_connection_state(1);
    QCOMPARE(OfficialMessageHandler::handle("RobotStaticStatus", bytes(status), data, {}), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(data.getRobotById(101)->type, RobotType::HEAVY);
    robomaster::GlobalUnitStatus units;
    for (int i = 0; i < 8; ++i) units.add_robot_health(101 + i);
    QCOMPARE(OfficialMessageHandler::handle("GlobalUnitStatus", bytes(units), data, {}), OfficialMessageHandler::Result::Parsed);
    const QList<int> ids{101, 103, 104, 107, 1, 3, 4, 7};
    for (int i = 0; i < ids.size(); ++i)
      QCOMPARE(data.getRobotById(ids[i])->currentHP, 101 + i);
    QSignalSpy kills(&data, &GameData::killEventOccurred);
    robomaster::Event event;
    event.set_event_id(1);
    event.set_param("1,101");
    QCOMPARE(OfficialMessageHandler::handle("Event", bytes(event), data, {}), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(kills.count(), 1);
    event.set_event_id(16);
    event.clear_param();
    QCOMPARE(OfficialMessageHandler::handle("Event", bytes(event), data, {}), OfficialMessageHandler::Result::Parsed);
    QVERIFY(data.event16Received());
  }
  void identityDisplayUsesGameData() {
    Q_INIT_RESOURCE(qml);
    Q_INIT_RESOURCE(resources);
    GameData data;
    QQuickWidget top;
    top.rootContext()->setContextProperty("gameData", &data);
    top.setSource(QUrl(QStringLiteral("qrc:/qml/TopInfoBar.qml")));
    QCOMPARE(top.status(), QQuickWidget::Ready);
    QQuickWidget panel;
    panel.rootContext()->setContextProperty("gameData", &data);
    panel.setSource(QUrl(QStringLiteral("qrc:/qml/RM27DevPanel.qml")));
    QCOMPARE(panel.status(), QQuickWidget::Ready);
    QVERIFY(!top.rootObject()->findChild<QObject *>("topRobot2"));
    QVERIFY(!top.rootObject()->findChild<QObject *>("topRobot102"));
    std::function<QQuickItem *(QQuickItem *, const QString &)> findVisual =
      [&](QQuickItem *item, const QString &name) -> QQuickItem * {
        if (item->objectName() == name) return item;
        for (auto *child : item->childItems())
          if (auto *found = findVisual(child, name)) return found;
        return nullptr;
      };
    QVERIFY(!findVisual(panel.rootObject(), "rm27Robot2"));
    for (int id : {1, 3, 4, 7, 101, 103, 104, 107}) {
      auto *card = top.rootObject()->findChild<QObject *>(QString("topRobot%1").arg(id));
      auto *row = findVisual(panel.rootObject(), QString("rm27Robot%1").arg(id));
      QVERIFY(card); QVERIFY(row);
      const QVariantMap info = data.getRobotInfo(id);
      QCOMPARE(card->property("normalizedRobotId").toInt(), info.value("identityNumber").toInt());
      QCOMPARE(card->property("isBlue").toBool(), info.value("identityTeam").toString() == QStringLiteral("蓝方"));
      QCOMPARE(card->property("avatarSource").toString(), QStringLiteral("qrc:/images/robot_avatar.png"));
      QVERIFY(row->property("text").toString().contains(info.value("identityName").toString()));
      QVERIFY(row->property("text").toString().contains(info.value("identityTeam").toString()));
    }
    QCOMPARE(data.getRobotInfo(2).value("identityName").toString(), QStringLiteral("保留位"));
    QCOMPARE(data.getRobotInfo(102).value("identityName").toString(), QStringLiteral("保留位"));
    QCOMPARE(data.getRobotInfo(201).value("identityTeam").toString(), QStringLiteral("未知阵营"));
    QCOMPARE(data.getRobotInfo(201).value("robotId").toInt(), 201);
    robomaster::RobotStaticStatus status;
    status.set_robot_id(104); status.set_connection_state(1); status.set_max_health(500);
    QCOMPARE(OfficialMessageHandler::handle("RobotStaticStatus", bytes(status), data), OfficialMessageHandler::Result::Parsed);
    robomaster::GlobalUnitStatus health;
    for (int i = 0; i < 8; ++i) health.add_robot_health(101 + i);
    QCOMPARE(OfficialMessageHandler::handle("GlobalUnitStatus", bytes(health), data), OfficialMessageHandler::Result::Parsed);
    auto *card = top.rootObject()->findChild<QObject *>("topRobot104");
    auto *row = findVisual(panel.rootObject(), "rm27Robot104");
    QTRY_COMPARE(card->property("currentHP").toInt(), int(data.getRobotById(104)->currentHP));
    QTRY_VERIFY(row->property("text").toString().contains(QString::number(data.getRobotById(104)->currentHP)));
  }
  void supportedMessagesAndLogging() {
    GameData data;
    QSignalSpy fortress(&data, &GameData::allyFortressOccupationAlertTriggered);
    QSignalSpy buff(&data, &GameData::buffPointUpdated);
    QSignalSpy deploy(&data, &GameData::deployModeStatusChanged);
    int logs = 0;
    QString detail;
    QJsonObject fields;
    auto logger = [&](const QString &d, const QJsonObject &f) {
      ++logs; detail = d; fields = f;
      // Parsed logging must happen before the update and its signals.
      if (d.startsWith("Buff")) QCOMPARE(buff.count(), 0);
      if (d.startsWith("Deploy")) QCOMPARE(deploy.count(), 0);
    };
    robomaster::GlobalSpecialMechanism mechanism;
    mechanism.add_mechanism_id(1); mechanism.add_mechanism_time_sec(1);
    QCOMPARE(OfficialMessageHandler::handle("GlobalSpecialMechanism", bytes(mechanism), data, logger), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(data.allyFortressOccupationSec(), 1);
    QCOMPARE(fortress.count(), 1); QCOMPARE(logs, 0);
    robomaster::Buff message;
    message.set_robot_id(1); message.set_buff_type(1); message.set_buff_level(2);
    message.set_buff_max_time(30); message.set_buff_left_time(25);
    QCOMPARE(OfficialMessageHandler::handle("Buff", bytes(message), data, logger), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(buff.count(), 1); QCOMPARE(logs, 1);
    QCOMPARE(detail, QString("Buff robot_id=1 buff_type=1 buff_level=2"));
    QCOMPARE(fields.value("buff_level").toInt(), 2);
    QCOMPARE(data.getRobotById(1)->buffs.attack.level, 2);
    robomaster::DeployModeStatusSync mode; mode.set_status(2);
    QCOMPARE(OfficialMessageHandler::handle("DeployModeStatusSync", bytes(mode), data, logger), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(data.deployModeStatus(), 2); QCOMPARE(deploy.count(), 1); QCOMPARE(logs, 2);
    QCOMPARE(detail, QString("DeployModeStatusSync status=2"));
    QCOMPARE(fields.value("status").toInt(), 2);
  }
  void unitAndRobotStatus() {
    DetailLogCapture capture;
    GameData data;
    QSignalSpy changed(&data, &GameData::dataChanged);
    data.setCurrentRobotId(3);
    QString detail;
    QJsonObject fields;
    int logs = 0;
    auto logger = [&](const QString &d, const QJsonObject &f) {
      ++logs; detail = d; fields = f;
      if (d.startsWith("GlobalUnitStatus")) QCOMPARE(data.redBaseHealth(), 5000);
      if (d.startsWith("RobotDynamicStatus")) QVERIFY(data.getRobotById(3)->currentHP != 345);
    };
    robomaster::GlobalUnitStatus units;
    units.set_base_health(3210); units.set_enemy_base_health(4100);
    units.add_robot_health(123); units.add_robot_health(456);
    units.add_robot_bullets(-1); units.add_robot_bullets(25);
    QCOMPARE(OfficialMessageHandler::handle("GlobalUnitStatus", bytes(units), data, logger), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(data.redBaseHealth(), 3210); QCOMPARE(data.blueBaseHealth(), 4100);
    QCOMPARE(detail, QString("GlobalUnitStatus robot_health=[123,456] robot_bullets=[-1,25]"));
    QCOMPARE(fields.value("robot_health_size").toInt(), 2);
    QCOMPARE(fields.value("robot_bullets").toArray().at(0).toInt(), -1);
    robomaster::RobotStaticStatus stat;
    stat.set_robot_id(3); stat.set_max_health(500); stat.set_level(2); stat.set_connection_state(2);
    QCOMPARE(OfficialMessageHandler::handle("RobotStaticStatus", bytes(stat), data, logger), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(data.getRobotById(3)->maxHP, 500); QCOMPARE(fields.value("robot_id").toInt(), 3);
    robomaster::RobotDynamicStatus dynamic;
    dynamic.set_current_health(345); dynamic.set_remaining_ammo(67);
    QCOMPARE(OfficialMessageHandler::handle("RobotDynamicStatus", bytes(dynamic), data, logger), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(data.getRobotById(3)->currentHP, 345);
    QCOMPARE(fields.value("protocol_has_robot_id").toBool(true), false);
    QCOMPARE(fields.value("remaining_ammo").toInt(), 67); QCOMPARE(logs, 3);
    QVERIFY(changed.count() > 0);
    QCOMPARE(detailedLogs.filter("robot_hp=[123,456] robot_bullets=[-1,25]").size(), 1);
    QCOMPARE(detailedLogs.filter("[RobotStaticStatus] robot_id=3").size(), 1);
    QCOMPARE(detailedLogs.filter("[RobotDynamicStatus] hp=345").size(), 1);
    const QByteArray invalid = QByteArray::fromHex("0880");
    for (const QString &topic : {QString("GlobalUnitStatus"), QString("RobotStaticStatus"), QString("RobotDynamicStatus")})
      QCOMPARE(OfficialMessageHandler::handle(topic, invalid, data, logger), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(data.redBaseHealth(), 3210); QCOMPARE(data.getRobotById(3)->currentHP, 345); QCOMPARE(logs, 3);
  }
  void gameStatusCacheAndOrdering() {
    GameData data;
    bool hasCache = false, paused = false;
    quint32 stage = 0;
    quint16 remaining = 0;
    int logs = 0;
    auto cache = [&](quint32 s, quint16 time, bool p) {
      hasCache = true; stage = s; remaining = time; paused = p;
      QCOMPARE(data.getGameTime(), 420);
    };
    auto logger = [&](const QString &detail, const QJsonObject &fields) {
      ++logs;
      QVERIFY(hasCache); QCOMPARE(stage, quint32(4)); QCOMPARE(remaining, quint16(250)); QVERIFY(paused);
      QCOMPARE(data.getGameTime(), 420);
      QCOMPARE(detail, QString("GameStatus current_stage=4 countdown=250 paused=1"));
      QCOMPARE(fields.value("stage_elapsed_sec").toInt(), 12);
    };
    robomaster::GameStatus msg;
    msg.set_current_stage(robomaster::STAGE_BATTLE); msg.set_stage_countdown_sec(250);
    msg.set_stage_elapsed_sec(12); msg.set_is_paused(true); msg.set_red_score(37);
    QCOMPARE(OfficialMessageHandler::handle("GameStatus", bytes(msg), data, logger, cache), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(logs, 1); QCOMPARE(data.getGameTime(), 250); QCOMPARE(data.redScore(), 37);
    QCOMPARE(OfficialMessageHandler::handle("GameStatus", QByteArray::fromHex("0880"), data, logger, cache), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(logs, 1); QCOMPARE(remaining, quint16(250)); QCOMPARE(data.getGameTime(), 250);
  }
#ifdef RM_HAS_MQTT
  void networkEventDispatchOnce() {
    GameData data;
    RM::NetworkManager network(&data);
    QSignalSpy signal(&data, &GameData::airSupportCountered);
    QSignalSpy popup(&data, &GameData::officialEventPopupRequested);
    QSignalSpy received(&network, &RM::NetworkManager::dataReceived);
    robomaster::Event evt; evt.set_event_id(8); evt.set_param("RED");
    network.processMqttMessage("Event", bytes(evt));
    QCOMPARE(signal.count(), 1); QCOMPARE(popup.count(), 1);
    QCOMPARE(received.count(), 1);
    network.processMqttMessage("Event", QByteArray::fromHex("0880"));
    QCOMPARE(signal.count(), 1); QCOMPARE(popup.count(), 1);
    QCOMPARE(received.count(), 2);
  }
  void networkCacheLogisticsAndUdpPriority() {
    GameData data;
    data.setCurrentRobotId(1);
    RM::NetworkManager network(&data);
    robomaster::GlobalLogisticsStatus logistics;
    logistics.set_remaining_economy(123);
    // The legacy simplified GAME_STATUS frame, stage 1 / time 100 / paused.
    const QByteArray udp = QByteArray::fromHex("010011001164000000000001040000000000000000");
    robomaster::RoboMasterMessage decoded;
    QVERIFY(Protocol::parsePacket(udp, decoded));
    QVERIFY(decoded.has_game_info());
    network.processMqttMessage("GameStatus", QByteArray::fromHex("0880"));
    QVERIFY(!network.m_hasMqttGameStatus);
    network.processData(udp);
    QCOMPARE(data.getGameTime(), 100);
    QCOMPARE(static_cast<int>(data.getCurrentStage()), 1);
    QVERIFY(data.is_paused());
    network.processMqttMessage("GlobalLogisticsStatus", bytes(logistics));
    QCOMPARE(data.getGameTime(), 420); // Existing cache default before first GameStatus.
    QCOMPARE(data.redEconomy(), 123);
    robomaster::GameStatus status;
    status.set_current_stage(4); status.set_stage_countdown_sec(250);
    status.set_is_paused(true); status.set_red_score(37); status.set_blue_score(9);
    network.processMqttMessage("GameStatus", bytes(status));
    QVERIFY(network.m_hasMqttGameStatus);
    QCOMPARE(network.m_lastMqttTimeRemaining, quint16(250));
    QCOMPARE(data.getGameTime(), 250); QCOMPARE(data.redScore(), 37); QCOMPARE(data.blueScore(), 9);
    network.processMqttMessage("GameStatus", QByteArray::fromHex("0880"));
    QCOMPARE(network.m_lastMqttTimeRemaining, quint16(250));
    QCOMPARE(static_cast<int>(network.m_lastMqttStage), 4); QVERIFY(network.m_lastMqttPaused);
    logistics.set_remaining_economy(234);
    network.processMqttMessage("GlobalLogisticsStatus", bytes(logistics));
    QCOMPARE(data.getGameTime(), 250); QCOMPARE(static_cast<int>(data.getCurrentStage()), 4);
    QVERIFY(data.is_paused()); QCOMPARE(data.redEconomy(), 234);
    network.processData(udp);
    QCOMPARE(data.getGameTime(), 250); QCOMPARE(static_cast<int>(data.getCurrentStage()), 4);
    QVERIFY(data.is_paused());
  }
#endif
  void legacyEventDispatchOnce() {
    GameData data;
    QSignalSpy air(&data, &GameData::airSupportCountered);
    QSignalSpy popup(&data, &GameData::officialEventPopupRequested);
    int logs = 0;
    auto logger = [&](const QString &detail, const QJsonObject &fields) {
      ++logs;
      QCOMPARE(detail, QString("Event event_id=8 param=RED"));
      QCOMPARE(fields.value("event_id").toInt(), 8);
      QCOMPARE(air.count(), 0);
    };
    robomaster::Event evt; evt.set_event_id(8); evt.set_param("RED");
    QCOMPARE(OfficialMessageHandler::handle("Event", bytes(evt), data, logger), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(logs, 1); QCOMPARE(air.count(), 1); QCOMPARE(popup.count(), 1);
    QCOMPARE(OfficialMessageHandler::handle("Event", QByteArray::fromHex("0880"), data, logger), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(logs, 1); QCOMPARE(air.count(), 1); QCOMPARE(popup.count(), 1);
  }
  void event16Observation() {
    GameData data;
    QSignalSpy observed(&data, &GameData::event16ReceivedChanged);
    QSignalSpy popup(&data, &GameData::officialEventPopupRequested);
    QVERIFY(!data.event16Received());
    QVERIFY(!data.event16LastReceivedAt().isValid());
    robomaster::Event other; other.set_event_id(17);
    QCOMPARE(OfficialMessageHandler::handle("Event", bytes(other), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(OfficialMessageHandler::handle("Event", QByteArray::fromHex("081080"), data), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(observed.count(), 0); QVERIFY(!data.event16Received());
    const QDateTime before = QDateTime::currentDateTime();
    robomaster::Event event; event.set_event_id(16);
    QCOMPARE(bytes(event).toHex(), QByteArray("0810"));
    QCOMPARE(OfficialMessageHandler::handle("Event", bytes(event), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(observed.count(), 1); QVERIFY(data.event16Received());
    QVERIFY(data.event16LastReceivedAt() >= before);
    QVERIFY(data.event16LastReceivedAt() <= QDateTime::currentDateTime());
    QCOMPARE(popup.count(), 0);
    QTest::qWait(2);
    const QDateTime first = data.event16LastReceivedAt();
    QCOMPARE(OfficialMessageHandler::handle("Event", bytes(event), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(observed.count(), 2); QVERIFY(data.event16LastReceivedAt() >= first);
    const QDateTime last = data.event16LastReceivedAt();
    QCOMPARE(OfficialMessageHandler::handle("Event", QByteArray::fromHex("081080"), data), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(observed.count(), 2); QCOMPARE(data.event16LastReceivedAt(), last);
  }
#ifdef RM_HAS_MQTT
  void event16ViaLocalMqttAndQml() {
    if (QStandardPaths::findExecutable(QStringLiteral("mosquitto")).isEmpty())
      QSKIP("mosquitto unavailable");
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0))
      QSKIP("local listening unavailable");
    const quint16 port = probe.serverPort();
    probe.close();
    QProcess broker;
    broker.start(QStringLiteral("mosquitto"), {QStringLiteral("-p"), QString::number(port)});
    QVERIFY(broker.waitForStarted());
    QTest::qWait(150);
    QVERIFY(broker.state() == QProcess::Running);
    Q_INIT_RESOURCE(qml);
    GameData data;
    RM::NetworkManager network(&data);
    QQuickWidget panel;
    panel.rootContext()->setContextProperty("gameData", &data);
    panel.setSource(QUrl(QStringLiteral("qrc:/qml/RM27DevPanel.qml")));
    QCOMPARE(panel.status(), QQuickWidget::Ready);
    QObject *display = panel.rootObject()->findChild<QObject *>(QStringLiteral("rm27Event16Status"));
    QVERIFY(display);
    QVERIFY(display->property("text").toString().contains(QStringLiteral("已收到: 否")));
    QSignalSpy observed(&data, &GameData::event16ReceivedChanged);
    QSignalSpy received(&network, &RM::NetworkManager::dataReceived);
    const QString uri = QStringLiteral("tcp://127.0.0.1:%1").arg(port);
    QSignalSpy connected(&network, &RM::NetworkManager::mqttConnectCompleted);
    network.startMqtt(uri, QStringLiteral("rm-taskb1-subscriber"));
    QTRY_VERIFY_WITH_TIMEOUT(!connected.isEmpty(), 5000);
    QVERIFY(network.isMqttConnected());
    MqttManager publisher;
    QSignalSpy publisherConnected(&publisher, &MqttManager::connectCompleted);
    publisher.connectToBrokerAsync(uri, QStringLiteral("rm-taskb1-publisher"));
    QTRY_VERIFY_WITH_TIMEOUT(!publisherConnected.isEmpty(), 5000);
    QVERIFY(publisher.isConnected());
    QVERIFY(publisher.publish(QStringLiteral("Event"), QByteArray::fromHex("081080"), 1));
    QTRY_COMPARE_WITH_TIMEOUT(received.count(), 1, 5000);
    QCOMPARE(observed.count(), 0); QVERIFY(!data.event16Received());
    QVERIFY(publisher.publish(QStringLiteral("Event"), QByteArray::fromHex("0811"), 1));
    QTRY_COMPARE_WITH_TIMEOUT(received.count(), 2, 5000);
    QCOMPARE(observed.count(), 0); QVERIFY(!data.event16Received());
    QVERIFY(publisher.publish(QStringLiteral("Event"), QByteArray::fromHex("0810"), 1));
    QTRY_COMPARE_WITH_TIMEOUT(observed.count(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(display->property("text").toString().contains(QStringLiteral("已收到: 是")), 5000);
    QVERIFY(data.event16LastReceivedAt().isValid());
    QVERIFY(display->property("text").toString().contains(
        data.event16LastReceivedAt().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))));
    QVERIFY(display->property("text").toString().contains(QStringLiteral("尚未独立确认")));
    QTest::qWait(100);
    QCOMPARE(observed.count(), 1); QCOMPARE(received.count(), 3);
    data.setMyRobotId(101);
    robomaster::RobotStaticStatus invalidIdentity;
    invalidIdentity.set_robot_id(257);
    invalidIdentity.set_robot_type(8);
    invalidIdentity.set_connection_state(1);
    QSignalSpy silo(&data, &GameData::siloStatusChanged);
    QVERIFY(publisher.publish(QStringLiteral("RobotStaticStatus"), bytes(invalidIdentity), 1));
    QTRY_COMPARE_WITH_TIMEOUT(received.count(), 4, 5000);
    QCOMPARE(silo.count(), 0);
    QCOMPARE(data.getRobotById(1)->isClientConnected, false);
    robomaster::GlobalUnitStatus health;
    for (int i = 0; i < 8; ++i) health.add_robot_health(101 + i);
    QVERIFY(publisher.publish(QStringLiteral("GlobalUnitStatus"), bytes(health), 1));
    QTRY_COMPARE_WITH_TIMEOUT(data.getRobotById(7)->currentHP, 108, 5000);
    const QList<int> ids{101, 103, 104, 107, 1, 3, 4, 7};
    for (int i = 0; i < ids.size(); ++i)
      QCOMPARE(data.getRobotById(ids[i])->currentHP, 101 + i);
    QSignalSpy kills(&data, &GameData::killEventOccurred);
    QSignalSpy popups(&data, &GameData::officialEventPopupRequested);
    robomaster::Event kill;
    kill.set_event_id(1);
    kill.set_param("1,101");
    QVERIFY(publisher.publish(QStringLiteral("Event"), bytes(kill), 1));
    QTRY_COMPARE_WITH_TIMEOUT(kills.count(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(popups.count(), 1, 5000);
    QVERIFY(popups.first().at(1).toString().contains(QStringLiteral("蓝方重装机器人击毁了红方重装机器人")));
    network.stopMqtt();
    publisher.disconnect();
    broker.terminate();
    QVERIFY(broker.waitForFinished(5000));
  }
#endif
  void malformedAndUnsupported() {
    GameData data;
    QSignalSpy changed(&data, &GameData::dataChanged);
    int logs = 0;
    auto logger = [&](const QString &, const QJsonObject &) { ++logs; };
    // A valid first field followed by a truncated varint tests partial parses.
    const QByteArray invalid = QByteArray::fromHex("08011080");
    for (const QString &topic : {QString("GlobalSpecialMechanism"), QString("Buff"), QString("DeployModeStatusSync")})
      QCOMPARE(OfficialMessageHandler::handle(topic, invalid, data, logger), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(OfficialMessageHandler::handle("UnrelatedTopic", invalid, data, logger), OfficialMessageHandler::Result::Unsupported);
    QCOMPARE(data.allyFortressOccupationSec(), 0); QCOMPARE(data.deployModeStatus(), 0);
    QCOMPARE(changed.count(), 0); QCOMPARE(logs, 0);
  }
};
QTEST_MAIN(TestOfficialMessageHandler)
#include "test_official_message_handler.moc"
