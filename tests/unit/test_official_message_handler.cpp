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
#include <limits>
#include <atomic>
#include <QThread>

using RM::OfficialMessageHandler;
namespace {
std::atomic<bool> dynamicCallbackObserved{false};
std::atomic<unsigned> observedQueuedTopics{0};
void captureDynamicCallback(QtMsgType, const QMessageLogContext &, const QString &message) {
  if (message.contains(QStringLiteral("RM26_MQTT_RX_TIMING direction=rx topic=RobotDynamicStatus")))
    dynamicCallbackObserved.store(true);
  if (message.contains("RM26_MQTT_RX_TIMING direction=rx topic=RobotDynamicStatus")) observedQueuedTopics.fetch_or(1);
  if (message.contains("RM26_MQTT_RX_TIMING direction=rx topic=GameStatus")) observedQueuedTopics.fetch_or(2);
  if (message.contains("RM26_MQTT_RX_TIMING direction=rx topic=RobotStaticStatus")) observedQueuedTopics.fetch_or(4);
  if (message.contains("RM26_MQTT_RX_TIMING direction=rx topic=Event")) observedQueuedTopics.fetch_or(8);
  if (message.contains("RM26_MQTT_RX_TIMING direction=rx topic=RobotModuleStatus")) observedQueuedTopics.fetch_or(16);
  if (message.contains("RM26_MQTT_RX_TIMING direction=rx topic=GlobalUnitStatus")) observedQueuedTopics.fetch_or(32);
  if (message.contains("RM26_MQTT_RX_TIMING direction=rx topic=PenaltyInfo")) observedQueuedTopics.fetch_or(64);
  if (message.contains("RM26_MQTT_RX_TIMING direction=rx topic=RobotInjuryStat")) observedQueuedTopics.fetch_or(128);
  if (message.contains("RM26_MQTT_RX_TIMING direction=rx topic=RobotRespawnStatus")) observedQueuedTopics.fetch_or(256);
  if (message.contains("RM26_MQTT_RX_TIMING direction=rx topic=GlobalLogisticsStatus")) observedQueuedTopics.fetch_or(512);
}
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
  void moduleStatusPresenceAndIdentity() {
    Q_INIT_RESOURCE(qml);
    GameData data;
    data.setCurrentRobotId(1);
    QQuickWidget panel;
    panel.rootContext()->setContextProperty("gameData", &data);
    panel.setSource(QUrl(QStringLiteral("qrc:/qml/RM27DevPanel.qml")));
    QCOMPARE(panel.status(), QQuickWidget::Ready);
    auto *summary = panel.rootObject()->findChild<QObject *>("rm27ModuleSummary");
    QVERIFY(summary);
    QVERIFY(summary->property("text").toString().contains(QStringLiteral("已收到: 否")));
    std::function<QQuickItem *(QQuickItem *, const QString &)> findVisual =
      [&](QQuickItem *item, const QString &name) -> QQuickItem * {
        if (item->objectName() == name) return item;
        for (auto *child : item->childItems())
          if (auto *found = findVisual(child, name)) return found;
        return nullptr;
      };
    robomaster::RobotModuleStatus msg;
    for (int value = 0; value <= 2; ++value) {
      msg.set_power_manager(value); msg.set_rfid(value); msg.set_light_strip(value);
      msg.set_small_shooter(value); msg.set_big_shooter(value); msg.set_uwb(value);
      msg.set_armor(value); msg.set_video_transmission(value); msg.set_capacitor(value);
      msg.set_main_controller(value);
      QCOMPARE(OfficialMessageHandler::handle("RobotModuleStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
      auto snapshot = data.robotModuleStatus();
      QVERIFY(snapshot.value("received").toBool());
      QCOMPARE(snapshot.value("robotId").toInt(), 1);
      for (const char *key : {"power_manager", "rfid", "light_strip", "small_shooter", "big_shooter", "uwb", "armor", "video_transmission", "capacitor", "main_controller"}) {
        QCOMPARE(snapshot.value(key).toInt(), value);
        auto *row = findVisual(panel.rootObject(), QStringLiteral("rm27Module_") + QString::fromLatin1(key));
        QVERIFY(row);
        const QString expected = value == 0 ? QStringLiteral("0（离线）") : value == 1 ? QStringLiteral("1（在线）") : QStringLiteral("2（安装不规范，视为离线）");
        QVERIFY(row->property("text").toString().contains(expected));
      }
      QVERIFY(!snapshot.contains("modular_42mm"));
    }
    QVERIFY(summary->property("text").toString().contains(QStringLiteral("最近权限原值: 未收到")));
    for (int value : {0, 1, 9}) {
      msg.set_modular_42mm(value);
      QCOMPARE(OfficialMessageHandler::handle("RobotModuleStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
      QCOMPARE(data.robotModuleStatus().value("modular_42mm").toInt(), value);
      const QString expected = value == 0 ? QStringLiteral("0（未拥有）") : value == 1 ? QStringLiteral("1（拥有）") : QStringLiteral("非法值 9");
      QTRY_VERIFY(summary->property("text").toString().contains(expected));
      QVERIFY(summary->property("text").toString().contains(QStringLiteral("本会话已上报: 否")));
    }
    const QVariantMap saved = data.robotModuleStatus();
    QCOMPARE(OfficialMessageHandler::handle("RobotModuleStatus", QByteArray::fromHex("5880"), data), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(data.robotModuleStatus(), saved);
    robomaster::Event event; event.set_event_id(16);
    QCOMPARE(OfficialMessageHandler::handle("Event", bytes(event), data), OfficialMessageHandler::Result::Parsed);
    QVERIFY(data.event16Received()); QCOMPARE(data.robotModuleStatus(), saved);
    msg.clear_modular_42mm();
    QCOMPARE(OfficialMessageHandler::handle("RobotModuleStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QVERIFY(!data.robotModuleStatus().contains("modular_42mm"));
    QVERIFY(data.event16Received());
    data.setCurrentRobotId(101);
    QVERIFY(!data.robotModuleStatus().value("received").toBool());
    QVERIFY(!data.robotModuleStatus().contains("modular_42mm"));
    msg.set_modular_42mm(1);
    QCOMPARE(OfficialMessageHandler::handle("RobotModuleStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(data.robotModuleStatus().value("robotId").toInt(), 101);
    data.setCurrentRobotId(3);
    QCOMPARE(OfficialMessageHandler::handle("RobotModuleStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QVERIFY(summary->property("text").toString().contains(QStringLiteral("本会话已上报: 否")));
    QVERIFY(summary->property("text").toString().contains(QStringLiteral("不适用（原始值 1）")));
    data.setCurrentRobotId(2);
    QCOMPARE(OfficialMessageHandler::handle("RobotModuleStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QVERIFY(!data.robotModuleStatus().value("received").toBool());
  }
#ifdef RM_HAS_MQTT
  void moduleStatusViaLocalMqttOnce() {
    if (QStandardPaths::findExecutable(QStringLiteral("mosquitto")).isEmpty()) QSKIP("mosquitto unavailable");
    QTcpServer probe; if (!probe.listen(QHostAddress::LocalHost, 0)) QSKIP("local listening unavailable");
    const quint16 port = probe.serverPort(); probe.close();
    QProcess broker; broker.start(QStringLiteral("mosquitto"), {QStringLiteral("-p"), QString::number(port)});
    QVERIFY(broker.waitForStarted()); QTest::qWait(150);
    GameData data; data.setCurrentRobotId(1);
    RM::NetworkManager network(&data);
    const QString uri = QStringLiteral("tcp://127.0.0.1:%1").arg(port);
    QSignalSpy connected(&network, &RM::NetworkManager::mqttConnectCompleted);
    network.startMqtt(uri, QStringLiteral("1"));
    QTRY_VERIFY_WITH_TIMEOUT(!connected.isEmpty(), 5000); QVERIFY(network.isMqttConnected());
    MqttManager publisher; QSignalSpy pubConnected(&publisher, &MqttManager::connectCompleted);
    publisher.connectToBrokerAsync(uri, QStringLiteral("p2b2-test-publisher"));
    QTRY_VERIFY_WITH_TIMEOUT(!pubConnected.isEmpty(), 5000); QVERIFY(publisher.isConnected());
    robomaster::RobotModuleStatus msg; msg.set_modular_42mm(1); msg.set_small_shooter(2);
    QSignalSpy updates(&data, &GameData::moduleListChanged);
    QVERIFY(publisher.publish(QStringLiteral("RobotModuleStatus"), bytes(msg), 1));
    QTRY_COMPARE_WITH_TIMEOUT(updates.count(), 1, 5000);
    QTest::qWait(150); QCOMPARE(updates.count(), 1);
    QCOMPARE(data.robotModuleStatus().value("modular_42mm").toInt(), 1);
    robomaster::RobotStaticStatus stat; stat.set_robot_id(1); stat.set_max_health(100);
    QVERIFY(publisher.publish(QStringLiteral("RobotStaticStatus"), bytes(stat), 1));
    QTRY_COMPARE_WITH_TIMEOUT(data.getRobotById(1)->maxHP, 100, 5000);
    stat.set_max_health(450); stat.set_heat_cooldown_rate(66.5f);
    QVERIFY(publisher.publish(QStringLiteral("RobotStaticStatus"), bytes(stat), 1));
    QTRY_COMPARE_WITH_TIMEOUT(data.getRobotById(1)->maxHP, 450, 5000);
    QCOMPARE(data.getRobotById(1)->coolingValue, 66.5f);
    data.setCurrentRobotId(101); updates.clear();
    QVERIFY(publisher.publish(QStringLiteral("RobotModuleStatus"), bytes(msg), 1));
    QTest::qWait(400); QCOMPARE(updates.count(), 0);
    QVERIFY(!data.robotModuleStatus().value("received").toBool());
    network.stopMqtt(); publisher.disconnect(); broker.terminate(); QVERIFY(broker.waitForFinished(5000));
  }
#endif
#ifdef RM_HAS_MQTT
  void dynamicIdentityAndQueuedMqttIsolation() {
    if (QStandardPaths::findExecutable(QStringLiteral("mosquitto")).isEmpty()) QSKIP("mosquitto unavailable");
    QTcpServer probe; if (!probe.listen(QHostAddress::LocalHost, 0)) QSKIP("local listening unavailable");
    const quint16 port = probe.serverPort(); probe.close();
    QProcess broker; broker.start(QStringLiteral("mosquitto"), {QStringLiteral("-p"), QString::number(port)});
    QVERIFY(broker.waitForStarted()); QTest::qWait(150);
    GameData data; data.setCurrentRobotId(1); RM::NetworkManager network(&data);
    auto metadata = [&]() { return data.mqttReceptionMetadata(); };
    auto topicMeta = [&](const char *name) { return metadata().value("topics").toMap().value(QLatin1String(name)).toMap(); };
    QVERIFY(!metadata().value("connected").toBool());
    QCOMPARE(topicMeta("GameStatus").value("sessionState").toString(), QString("never"));
    const QString uri = QStringLiteral("tcp://127.0.0.1:%1").arg(port);
    QSignalSpy connected(&network, &RM::NetworkManager::mqttConnectCompleted);
    network.startMqtt(uri, QStringLiteral("1"));
    QTRY_VERIFY_WITH_TIMEOUT(!connected.isEmpty(), 5000); QVERIFY(network.isMqttConnected());
    QVERIFY(metadata().value("connected").toBool());
    const auto session1 = metadata().value("session").toULongLong(); QVERIFY(session1 > 0);
    MqttManager publisher; QSignalSpy pubConnected(&publisher, &MqttManager::connectCompleted);
    publisher.connectToBrokerAsync(uri, QStringLiteral("a1-test-publisher"));
    QTRY_VERIFY_WITH_TIMEOUT(!pubConnected.isEmpty(), 5000); QVERIFY(publisher.isConnected());
    robomaster::RobotDynamicStatus msg; msg.set_current_heat(12.5f);
    QVERIFY(publisher.publish(QStringLiteral("RobotDynamicStatus"), bytes(msg), 1));
    QTRY_COMPARE_WITH_TIMEOUT(data.robotDynamicStatus().value("accepted").toMap().value("current_heat").toFloat(), 12.5f, 5000);
    QCOMPARE(topicMeta("RobotDynamicStatus").value("sessionState").toString(), QString("current"));
    QCOMPARE(topicMeta("RobotDynamicStatus").value("identity").toInt(), 1);
    QVERIFY(topicMeta("RobotDynamicStatus").value("receivedAt").toDateTime().isValid());
    QVERIFY(topicMeta("RobotDynamicStatus").value("receivedMonotonicMs").toLongLong() > 0);
    robomaster::GameStatus gs; gs.set_current_stage(4); gs.set_red_score(0); gs.set_stage_countdown_sec(200);
    QVERIFY(publisher.publish("GameStatus", bytes(gs), 1));
    QTRY_COMPARE_WITH_TIMEOUT(topicMeta("GameStatus").value("sessionState").toString(), QString("current"), 5000);
    const auto redStamp = topicMeta("GameStatus").value("fieldUpdates").toMap().value("red_score").toMap();
    QVERIFY(redStamp.value("receivedAt").toDateTime().isValid());
    gs.Clear(); gs.set_blue_score(2);
    QVERIFY(publisher.publish("GameStatus", bytes(gs), 1));
    QTRY_COMPARE_WITH_TIMEOUT(data.blueScore(), 2, 5000);
    QCOMPARE(topicMeta("GameStatus").value("fieldUpdates").toMap().value("red_score").toMap(), redStamp);
    robomaster::RobotStaticStatus ss; ss.set_robot_id(1); ss.set_max_health(300);
    QVERIFY(publisher.publish("RobotStaticStatus", bytes(ss), 1));
    QTRY_VERIFY_WITH_TIMEOUT(metadata().value("staticByRobot").toMap().contains("1"), 5000);
    ss.set_robot_id(101); ss.set_max_health(400);
    QVERIFY(publisher.publish("RobotStaticStatus", bytes(ss), 1));
    QTRY_VERIFY_WITH_TIMEOUT(metadata().value("staticByRobot").toMap().contains("101"), 5000);
    QCOMPARE(data.getRobotById(1)->maxHP, 300); QCOMPARE(data.getRobotById(101)->maxHP, 400);
    robomaster::GlobalUnitStatus global; global.add_robot_health(111);
    global.add_robot_bullets(-1); global.add_robot_bullets(42); global.set_total_cp_ally(0); global.set_total_damage_ally(100);
    QVERIFY(publisher.publish("GlobalUnitStatus", bytes(global), 1));
    QTRY_COMPARE_WITH_TIMEOUT(topicMeta("GlobalUnitStatus").value("sessionState").toString(), QString("current"), 5000);
    QCOMPARE(data.globalHealthObservation().value("accepted").toMap().value("1").toUInt(), 111u);
    QCOMPARE(data.globalHealthObservation().value("rawFields").toMap().value("total_cp_ally").toMap().value("value").toUInt(), 0u);
    QCOMPARE(data.globalHealthObservation().value("rawFields").toMap().value("robot_bullets").toMap().value("length").toInt(), 2);
    QCOMPARE(data.globalHealthObservation().value("source").toString(), QString("official_mqtt"));
    QCOMPARE(data.globalHealthObservation().value("acceptedSource").toMap().value("1").toString(), QString("official_mqtt"));
    QCOMPARE(topicMeta("GlobalUnitStatus").value("identity").toInt(), 1);
    QVERIFY(topicMeta("GlobalUnitStatus").value("fieldUpdates").toMap().value("1").toMap().value("receivedAt").toDateTime().isValid());
    robomaster::Event event; event.set_event_id(16);
    QVERIFY(publisher.publish("Event", bytes(event), 1));
    QTRY_COMPARE_WITH_TIMEOUT(topicMeta("Event16").value("sessionState").toString(), QString("current"), 5000);
    QCOMPARE(topicMeta("Event16").value("identity").toInt(), 0);
    const auto eventTime = data.event16LastReceivedAt();
    robomaster::PenaltyInfo penalty; penalty.set_penalty_type(1); penalty.set_total_penalty_num(2);
    robomaster::RobotInjuryStat injury; injury.set_total_damage(10); injury.set_killer_id(101);
    QVERIFY(publisher.publish("PenaltyInfo", bytes(penalty), 1));
    QVERIFY(publisher.publish("RobotInjuryStat", bytes(injury), 1));
    QTRY_COMPARE_WITH_TIMEOUT(topicMeta("PenaltyInfo").value("sessionState").toString(), QString("current"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(topicMeta("RobotInjuryStat").value("sessionState").toString(), QString("current"), 5000);
    QCOMPARE(data.injuryObservation().value("identity").toInt(), 1);
    robomaster::RobotRespawnStatus respawn; respawn.set_is_pending_respawn(true); respawn.set_current_respawn_progress(12);
    QVERIFY(publisher.publish("RobotRespawnStatus", bytes(respawn), 1));
    QTRY_COMPARE_WITH_TIMEOUT(topicMeta("RobotRespawnStatus").value("sessionState").toString(), QString("current"), 5000);
    QCOMPARE(data.respawnObservation().value("identity").toInt(), 1);
    QVERIFY(data.robotRespawnStatus().isEmpty());
    robomaster::GlobalLogisticsStatus logistics; logistics.set_remaining_economy(123); logistics.set_total_economy_obtained(9007199254740993ULL);
    QVERIFY(publisher.publish("GlobalLogisticsStatus", bytes(logistics), 1));
    QTRY_COMPARE_WITH_TIMEOUT(topicMeta("GlobalLogisticsStatus").value("sessionState").toString(), QString("current"), 5000);
    QCOMPARE(data.logisticsObservation().value("team").toString(), QString("red"));
    QCOMPARE(data.redEconomy(), 123);
    data.setCurrentRobotId(101);
    QCOMPARE(topicMeta("RobotDynamicStatus").value("sessionState").toString(), QString("identity_changed"));
    QVERIFY(publisher.publish(QStringLiteral("RobotDynamicStatus"), bytes(msg), 1));
    QTest::qWait(200); QVERIFY(!data.robotDynamicStatus().value("received").toBool());
    data.setCurrentRobotId(1);
    QCOMPARE(topicMeta("RobotDynamicStatus").value("sessionState").toString(), QString("identity_changed"));
    // Returning to the same identity must not revalidate a message queued before the switch.
    dynamicCallbackObserved.store(false); observedQueuedTopics.store(0);
    const auto identityHandler = qInstallMessageHandler(captureDynamicCallback);
    msg.set_current_heat(55.5f);
    const bool identitySent = publisher.publish("RobotDynamicStatus", bytes(msg), 1);
    global.Clear(); global.add_robot_health(999); global.set_total_cp_ally(999); publisher.publish("GlobalUnitStatus", bytes(global), 1);
    penalty.set_penalty_type(3); injury.set_total_damage(999);
    publisher.publish("PenaltyInfo", bytes(penalty), 1); publisher.publish("RobotInjuryStat", bytes(injury), 1);
    respawn.set_current_respawn_progress(999); publisher.publish("RobotRespawnStatus", bytes(respawn), 1);
    logistics.set_remaining_economy(999); publisher.publish("GlobalLogisticsStatus", bytes(logistics), 1);
    for (int i = 0; i < 100 && (observedQueuedTopics.load() & 993) != 993; ++i) QThread::msleep(10);
    const bool identitySeen = (observedQueuedTopics.load() & 993) == 993;
    data.setCurrentRobotId(101); data.setCurrentRobotId(1);
    QCoreApplication::processEvents();
    qInstallMessageHandler(identityHandler);
    QVERIFY(identitySent); QVERIFY(identitySeen);
    QVERIFY(!data.robotDynamicStatus().value("received").toBool());
    QCOMPARE(data.globalHealthObservation().value("accepted").toMap().value("1").toUInt(), 111u);
    QCOMPARE(data.globalHealthObservation().value("rawFields").toMap().value("total_cp_ally").toMap().value("value").toUInt(), 0u);
    QCOMPARE(data.injuryObservation().value("fields").toMap().value("total_damage").toMap().value("value").toUInt(), 10u);
    QCOMPARE(data.penaltyObservation().value("fields").toMap().value("penalty_type").toMap().value("value").toUInt(), 1u);
    QCOMPARE(topicMeta("PenaltyInfo").value("sessionState").toString(), QString("identity_changed"));
    QCOMPARE(data.respawnObservation().value("fields").toMap().value("current_respawn_progress").toMap().value("value").toUInt(), 12u);
    QCOMPARE(topicMeta("RobotRespawnStatus").value("sessionState").toString(), QString("identity_changed"));
    QCOMPARE(data.redEconomy(), 123);
    QCOMPARE(data.logisticsObservation().value("accepted").toMap().value("remaining_economy").toUInt(), 123u);
    QCOMPARE(topicMeta("GlobalLogisticsStatus").value("sessionState").toString(), QString("identity_changed"));
    QCOMPARE(topicMeta("GlobalUnitStatus").value("sessionState").toString(), QString("identity_changed"));
    QCOMPARE(topicMeta("RobotDynamicStatus").value("sessionState").toString(), QString("identity_changed"));
    // Ensure a callback has queued the old message while the Qt thread remains blocked.
    dynamicCallbackObserved.store(false); observedQueuedTopics.store(0);
    const auto oldHandler = qInstallMessageHandler(captureDynamicCallback);
    msg.set_current_heat(88.5f);
    const bool sent = publisher.publish(QStringLiteral("RobotDynamicStatus"), bytes(msg), 1);
    gs.Clear(); gs.set_red_score(999); publisher.publish("GameStatus", bytes(gs), 1);
    ss.set_robot_id(1); ss.set_max_health(999); publisher.publish("RobotStaticStatus", bytes(ss), 1);
    event.set_event_id(16); publisher.publish("Event", bytes(event), 1);
    robomaster::RobotModuleStatus queuedModule; queuedModule.set_modular_42mm(1);
    publisher.publish("RobotModuleStatus", bytes(queuedModule), 1);
    publisher.publish("GlobalUnitStatus", bytes(global), 1);
    publisher.publish("PenaltyInfo", bytes(penalty), 1); publisher.publish("RobotInjuryStat", bytes(injury), 1);
    publisher.publish("RobotRespawnStatus", bytes(respawn), 1);
    publisher.publish("GlobalLogisticsStatus", bytes(logistics), 1);
    for (int i = 0; i < 200 && observedQueuedTopics.load() != 1023; ++i) QThread::msleep(10);
    const bool callbackSeen = dynamicCallbackObserved.load() && observedQueuedTopics.load() == 1023;
    network.stopMqtt();
    data.setCurrentRobotId(101);
    QCoreApplication::processEvents();
    qInstallMessageHandler(oldHandler);
    QVERIFY(sent); QVERIFY(callbackSeen);
    QCOMPARE(data.redScore(), 0); QCOMPARE(data.getRobotById(1)->maxHP, 300);
    QCOMPARE(data.event16LastReceivedAt(), eventTime);
    QCOMPARE(topicMeta("RobotModuleStatus").value("sessionState").toString(), QString("never"));
    QCOMPARE(topicMeta("GameStatus").value("sessionState").toString(), QString("disconnected"));
    QVERIFY(!data.robotDynamicStatus().value("received").toBool());
    connected.clear(); network.startMqtt(uri, QStringLiteral("101"));
    QTRY_VERIFY_WITH_TIMEOUT(!connected.isEmpty(), 5000); QVERIFY(network.isMqttConnected());
    QVERIFY(metadata().value("session").toULongLong() > session1);
    QCOMPARE(topicMeta("GameStatus").value("sessionState").toString(), QString("historical"));
    QCOMPARE(topicMeta("GlobalUnitStatus").value("sessionState").toString(), QString("historical"));
    QCOMPARE(topicMeta("PenaltyInfo").value("sessionState").toString(), QString("historical"));
    QCOMPARE(topicMeta("RobotInjuryStat").value("sessionState").toString(), QString("historical"));
    QCOMPARE(topicMeta("RobotRespawnStatus").value("sessionState").toString(), QString("historical"));
    QCOMPARE(topicMeta("GlobalLogisticsStatus").value("sessionState").toString(), QString("historical"));
    logistics.set_remaining_economy(456); QVERIFY(publisher.publish("GlobalLogisticsStatus", bytes(logistics), 1));
    QTRY_COMPARE_WITH_TIMEOUT(topicMeta("GlobalLogisticsStatus").value("sessionState").toString(), QString("current"), 5000);
    QCOMPARE(data.logisticsObservation().value("identity").toInt(), 101);
    QCOMPARE(data.logisticsObservation().value("team").toString(), QString("blue"));
    QCOMPARE(data.blueEconomy(), 456); QCOMPARE(data.redEconomy(), 123);
    QVERIFY(publisher.publish("RobotRespawnStatus", bytes(respawn), 1));
    QTRY_COMPARE_WITH_TIMEOUT(topicMeta("RobotRespawnStatus").value("sessionState").toString(), QString("current"), 5000);
    QCOMPARE(data.respawnObservation().value("identity").toInt(), 101);
    QVERIFY(data.robotRespawnStatus().isEmpty());
    QVERIFY(publisher.publish("PenaltyInfo", bytes(penalty), 1)); QVERIFY(publisher.publish("RobotInjuryStat", bytes(injury), 1));
    QTRY_COMPARE_WITH_TIMEOUT(topicMeta("PenaltyInfo").value("sessionState").toString(), QString("current"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(topicMeta("RobotInjuryStat").value("sessionState").toString(), QString("current"), 5000);
    QCOMPARE(data.injuryObservation().value("identity").toInt(), 101);
    global.Clear(); global.add_robot_health(212); global.set_total_cp_enemy(321); global.add_robot_bullets(-23);
    QVERIFY(publisher.publish("GlobalUnitStatus", bytes(global), 1));
    QTRY_COMPARE_WITH_TIMEOUT(data.globalHealthObservation().value("receiverId").toInt(), 101, 5000);
    QCOMPARE(data.globalHealthObservation().value("fields").toMap().value("101").toMap().value("value").toUInt(), 212u);
    QCOMPARE(data.globalHealthObservation().value("rawFields").toMap().value("total_cp_enemy").toMap().value("value").toUInt(), 321u);
    QCOMPARE(data.globalHealthObservation().value("rawFields").toMap().value("robot_bullets").toMap().value("values").toList().at(0).toInt(), -23);
    QCOMPARE(topicMeta("GlobalUnitStatus").value("fieldUpdates").toMap().value("total_cp_enemy").toMap().value("session").toULongLong(), metadata().value("session").toULongLong());
    QCOMPARE(topicMeta("GlobalUnitStatus").value("sessionState").toString(), QString("current"));
    QCOMPARE(topicMeta("Event16").value("sessionState").toString(), QString("historical"));
    QCOMPARE(topicMeta("RobotDynamicStatus").value("sessionState").toString(), QString("historical"));
    msg.set_current_heat(22.25f); msg.set_remaining_ammo(7);
    QVERIFY(publisher.publish(QStringLiteral("RobotDynamicStatus"), bytes(msg), 1));
    QTRY_COMPARE_WITH_TIMEOUT(data.robotDynamicStatus().value("accepted").toMap().value("current_heat").toFloat(), 22.25f, 5000);
    QCOMPARE(data.robotDynamicStatus().value("robotId").toInt(), 101);
    QCOMPARE(topicMeta("RobotDynamicStatus").value("sessionState").toString(), QString("current"));
    const auto ammoStamp = topicMeta("RobotDynamicStatus").value("fieldUpdates").toMap().value("remaining_ammo").toMap();
    QVERIFY(ammoStamp.value("receivedAt").toDateTime().isValid());
    network.stopMqtt();
    QCOMPARE(topicMeta("RobotDynamicStatus").value("sessionState").toString(), QString("disconnected"));
    QVERIFY(data.robotDynamicStatus().value("received").toBool());
    connected.clear(); network.startMqtt(uri, QStringLiteral("101"));
    QTRY_VERIFY_WITH_TIMEOUT(!connected.isEmpty(), 5000); QVERIFY(network.isMqttConnected());
    QCOMPARE(topicMeta("RobotDynamicStatus").value("sessionState").toString(), QString("historical"));
    QCOMPARE(data.robotDynamicStatus().value("accepted").toMap().value("current_heat").toFloat(), 22.25f);
    msg.Clear(); msg.set_current_heat(23.5f); QVERIFY(publisher.publish("RobotDynamicStatus", bytes(msg), 1));
    QTRY_COMPARE_WITH_TIMEOUT(data.robotDynamicStatus().value("accepted").toMap().value("current_heat").toFloat(), 23.5f, 5000);
    QCOMPARE(topicMeta("RobotDynamicStatus").value("sessionState").toString(), QString("current"));
    QCOMPARE(topicMeta("RobotDynamicStatus").value("fieldUpdates").toMap().value("remaining_ammo").toMap(), ammoStamp);
    QVERIFY(ammoStamp.value("session").toULongLong() < metadata().value("session").toULongLong());
    broker.terminate(); QVERIFY(broker.waitForFinished(5000));
    QTRY_VERIFY_WITH_TIMEOUT(!network.isMqttConnected(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(data.robotDynamicStatus().value("received").toBool(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(topicMeta("RobotDynamicStatus").value("sessionState").toString(), QString("disconnected"), 5000);
    QCOMPARE(topicMeta("GlobalUnitStatus").value("sessionState").toString(), QString("disconnected"));
    QCOMPARE(data.robotDynamicStatus().value("accepted").toMap().value("current_heat").toFloat(), 23.5f);
    network.stopMqtt(); publisher.disconnect();
  }
#endif
  void officialLogisticsObservation() {
    Q_INIT_RESOURCE(qml);
    GameData data; data.setCurrentRobotId(1);
    QQuickWidget panel; panel.rootContext()->setContextProperty("gameData", &data);
    panel.setSource(QUrl(QStringLiteral("qrc:/qml/RM27DevPanel.qml")));
    QCOMPARE(panel.status(), QQuickWidget::Ready);
    std::function<QQuickItem *(QQuickItem *, const QString &)> findVisual = [&](QQuickItem *item, const QString &name) -> QQuickItem * {
      if (item->objectName() == name) return item;
      for (auto *child : item->childItems()) if (auto *found = findVisual(child, name)) return found;
      return nullptr;
    };
    auto *total = findVisual(panel.rootObject(), "rm27Logistics_total_economy_obtained");
    auto *remaining = findVisual(panel.rootObject(), "rm27Logistics_remaining_economy");
    auto *session = findVisual(panel.rootObject(), "rm27LogisticsSession");
    QVERIFY(total); QVERIFY(remaining); QVERIFY(session);
    robomaster::GameStatus game; game.set_current_stage(4); game.set_stage_countdown_sec(240); game.set_is_paused(true);
    data.updateGameStatus(game);
    robomaster::GlobalLogisticsStatus msg; msg.set_remaining_economy(0); msg.set_total_economy_obtained(9007199254740993ULL); msg.set_tech_level(0); msg.set_encryption_level(4294967295u);
    QCOMPARE(OfficialMessageHandler::handle("GlobalLogisticsStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    auto snap = data.logisticsObservation(); auto fields = snap.value("fields").toMap();
    for (const char *key : {"remaining_economy", "total_economy_obtained", "tech_level", "encryption_level"})
      QCOMPARE(fields.value(QLatin1String(key)).toMap().value("state").toString(), QString("valid"));
    QCOMPARE(fields.value("total_economy_obtained").toMap().value("value").toULongLong(), 9007199254740993ULL);
    QCOMPARE(data.redTotalEconomyObtained(), 9007199254740993ULL);
    QCOMPARE(fields.value("encryption_level").toMap().value("value").toULongLong(), 4294967295ULL);
    QTRY_VERIFY(total->property("primary").toString().contains(QStringLiteral("9007199254740993")));
    QTRY_VERIFY(remaining->property("primary").toString().contains(QStringLiteral("当前经济: 0")));
    QTRY_VERIFY(session->property("primary").toString().contains(QStringLiteral("红方")));
    QCOMPARE(data.getGameTime(), 240); QCOMPARE(static_cast<int>(data.getCurrentStage()), 4); QVERIFY(data.is_paused());
    msg.Clear(); msg.set_tech_level(2); data.updateGlobalLogisticsStatus(msg);
    snap = data.logisticsObservation();
    QCOMPARE(snap.value("fields").toMap().value("total_economy_obtained").toMap().value("state").toString(), QString("unreported"));
    QCOMPARE(snap.value("accepted").toMap().value("total_economy_obtained").toULongLong(), 9007199254740993ULL);
    QTRY_VERIFY(total->property("primary").toString().contains(QStringLiteral("上次: 9007199254740993")));
    QCOMPARE(data.redTotalEconomyObtained(), 9007199254740993ULL);
    msg.Clear(); msg.set_remaining_economy(4294967295u); msg.set_total_economy_obtained(18446744073709551615ULL); data.updateGlobalLogisticsStatus(msg);
    snap = data.logisticsObservation();
    QCOMPARE(snap.value("accepted").toMap().value("remaining_economy").toUInt(), 4294967295u);
    QCOMPARE(snap.value("accepted").toMap().value("total_economy_obtained").toULongLong(), 18446744073709551615ULL);
    QCOMPARE(data.redEconomy(), 0); QCOMPARE(data.redRemainingEconomy(), 4294967295u);
    QTRY_VERIFY(total->property("primary").toString().contains(QStringLiteral("18446744073709551615")));
    QTRY_VERIFY(remaining->property("primary").toString().contains(QStringLiteral("超出旧类型范围")));
    // A compatibility GameInfo update must not rewrite the official observation.
    robomaster::GameInfo oldGame; oldGame.set_red_economy(432); data.updateGameState(oldGame);
    QCOMPARE(data.redEconomy(), 432);
    QCOMPARE(data.logisticsObservation().value("accepted").toMap().value("remaining_economy").toUInt(), 4294967295u);
    QCOMPARE(data.logisticsObservation().value("accepted").toMap().value("total_economy_obtained").toULongLong(), 18446744073709551615ULL);
    data.setCurrentRobotId(101); QVERIFY(!data.logisticsObservation().value("identityMatches").toBool());
    msg.Clear(); msg.set_remaining_economy(321); data.updateGlobalLogisticsStatus(msg);
    QCOMPARE(data.blueEconomy(), 321); QCOMPARE(data.redEconomy(), 432);
    QVERIFY(!data.logisticsObservation().value("accepted").toMap().contains("total_economy_obtained"));
    QTRY_VERIFY(session->property("primary").toString().contains(QStringLiteral("蓝方")));
    QCOMPARE(OfficialMessageHandler::handle("GlobalLogisticsStatus", QByteArray::fromHex("0880"), data), OfficialMessageHandler::Result::ParseFailed);
    data.setMqttSessionState(true, "101");
    data.recordMqttObservation("GlobalLogisticsStatus", 101, 100000, 1000);
    auto meta = data.mqttReceptionMetadata().value("topics").toMap().value("GlobalLogisticsStatus").toMap();
    auto stamp = meta.value("fieldUpdates").toMap().value("remaining_economy").toMap();
    QVERIFY(stamp.value("receivedAt").toDateTime().isValid());
    msg.Clear(); msg.set_tech_level(3); data.updateGlobalLogisticsStatus(msg);
    data.recordMqttObservation("GlobalLogisticsStatus", 101, 200000, 2000);
    meta = data.mqttReceptionMetadata().value("topics").toMap().value("GlobalLogisticsStatus").toMap();
    QCOMPARE(meta.value("fieldUpdates").toMap().value("remaining_economy").toMap(), stamp);
    data.setMqttSessionState(false, "101");
    QCOMPARE(data.mqttReceptionMetadata().value("topics").toMap().value("GlobalLogisticsStatus").toMap().value("sessionState").toString(), QString("disconnected"));
    data.setMqttSessionState(true, "101");
    QCOMPARE(data.mqttReceptionMetadata().value("topics").toMap().value("GlobalLogisticsStatus").toMap().value("sessionState").toString(), QString("historical"));
  }
  void officialRespawnObservation() {
    Q_INIT_RESOURCE(qml);
    GameData data; data.setCurrentRobotId(1);
    QQuickWidget panel; panel.rootContext()->setContextProperty("gameData", &data);
    panel.setSource(QUrl(QStringLiteral("qrc:/qml/RM27DevPanel.qml")));
    QCOMPARE(panel.status(), QQuickWidget::Ready);
    std::function<QQuickItem *(QQuickItem *, const QString &)> findVisual = [&](QQuickItem *item, const QString &name) -> QQuickItem * {
      if (item->objectName() == name) return item;
      for (auto *child : item->childItems()) if (auto *found = findVisual(child, name)) return found;
      return nullptr;
    };
    auto *total = findVisual(panel.rootObject(), "rm27Respawn_total_respawn_progress");
    auto *pending = findVisual(panel.rootObject(), "rm27Respawn_is_pending_respawn");
    QVERIFY(total); QVERIFY(pending);
    robomaster::RobotRespawnStatus msg;
    msg.set_is_pending_respawn(false); msg.set_total_respawn_progress(0); msg.set_current_respawn_progress(4294967295u);
    msg.set_can_free_respawn(false); msg.set_gold_cost_for_respawn(4294967295u); msg.set_can_pay_for_respawn(true);
    QSignalSpy oldStatus(&data, &GameData::robotRespawnStatusUpdated);
    QCOMPARE(OfficialMessageHandler::handle("RobotRespawnStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(oldStatus.count(), 0); QVERIFY(data.robotRespawnStatus().isEmpty());
    const auto fields = data.respawnObservation().value("fields").toMap();
    for (const char *key : {"is_pending_respawn", "total_respawn_progress", "current_respawn_progress", "can_free_respawn", "gold_cost_for_respawn", "can_pay_for_respawn"})
      QCOMPARE(fields.value(QLatin1String(key)).toMap().value("state").toString(), QString("valid"));
    QCOMPARE(fields.value("current_respawn_progress").toMap().value("value").toUInt(), 4294967295u);
    QCOMPARE(fields.value("gold_cost_for_respawn").toMap().value("value").toUInt(), 4294967295u);
    QCOMPARE(fields.value("can_free_respawn").toMap().value("value").toBool(), false);
    QTRY_VERIFY(pending->property("primary").toString().contains(QStringLiteral("false")));
    QTRY_VERIFY(total->property("primary").toString().contains(QStringLiteral("总读条: 0")));
    msg.Clear(); msg.set_current_respawn_progress(101); msg.set_can_free_respawn(true);
    data.updateOfficialRespawnObservation(msg);
    auto snap = data.respawnObservation();
    QCOMPARE(snap.value("fields").toMap().value("total_respawn_progress").toMap().value("state").toString(), QString("unreported"));
    QCOMPARE(snap.value("accepted").toMap().value("total_respawn_progress").toUInt(), 0u);
    QCOMPARE(snap.value("accepted").toMap().value("can_pay_for_respawn").toBool(), true);
    QTRY_VERIFY(total->property("primary").toString().contains(QStringLiteral("上次: 0")));
    msg.Clear(); msg.set_total_respawn_progress(10); msg.set_current_respawn_progress(15); msg.set_can_pay_for_respawn(false);
    data.updateOfficialRespawnObservation(msg);
    snap = data.respawnObservation();
    QCOMPARE(snap.value("accepted").toMap().value("total_respawn_progress").toUInt(), 10u);
    QCOMPARE(snap.value("accepted").toMap().value("current_respawn_progress").toUInt(), 15u);
    data.setCurrentRobotId(101); QVERIFY(!data.respawnObservation().value("identityMatches").toBool());
    msg.Clear(); msg.set_current_respawn_progress(7); data.updateOfficialRespawnObservation(msg);
    QVERIFY(!data.respawnObservation().value("accepted").toMap().contains("total_respawn_progress"));
    QCOMPARE(OfficialMessageHandler::handle("RobotRespawnStatus", QByteArray::fromHex("1080"), data), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(oldStatus.count(), 0);
    // A retained value keeps its own identity and receipt provenance.
    data.setMqttSessionState(true, "101");
    msg.Clear(); msg.set_total_respawn_progress(50); data.updateOfficialRespawnObservation(msg);
    data.recordMqttObservation("RobotRespawnStatus", 101, 100000, 1000);
    auto meta = data.mqttReceptionMetadata().value("topics").toMap().value("RobotRespawnStatus").toMap();
    const auto firstStamp = meta.value("fieldUpdates").toMap().value("total_respawn_progress").toMap();
    QVERIFY(firstStamp.value("receivedAt").toDateTime().isValid());
    msg.Clear(); msg.set_current_respawn_progress(20); data.updateOfficialRespawnObservation(msg);
    data.recordMqttObservation("RobotRespawnStatus", 101, 200000, 2000);
    meta = data.mqttReceptionMetadata().value("topics").toMap().value("RobotRespawnStatus").toMap();
    QCOMPARE(meta.value("fieldUpdates").toMap().value("total_respawn_progress").toMap(), firstStamp);
    QCOMPARE(data.respawnObservation().value("acceptedSource").toMap().value("total_respawn_progress").toString(), QString("official_mqtt"));
    msg.Clear(); msg.set_total_respawn_progress(60); data.updateOfficialRespawnObservation(msg);
    msg.Clear(); msg.set_current_respawn_progress(21); data.updateOfficialRespawnObservation(msg);
    data.recordMqttObservation("RobotRespawnStatus", 101, 300000, 3000);
    QCOMPARE(data.respawnObservation().value("acceptedSource").toMap().value("total_respawn_progress").toString(), QString("protocol_unverified"));
    data.setMqttSessionState(false, "101");
    meta = data.mqttReceptionMetadata().value("topics").toMap().value("RobotRespawnStatus").toMap();
    QCOMPARE(meta.value("sessionState").toString(), QString("disconnected"));
    data.setMqttSessionState(true, "101");
    meta = data.mqttReceptionMetadata().value("topics").toMap().value("RobotRespawnStatus").toMap();
    QCOMPARE(meta.value("sessionState").toString(), QString("historical"));
  }
  void penaltyAndInjuryObservations() {
    Q_INIT_RESOURCE(qml);
    GameData data; data.setCurrentRobotId(1);
    QQuickWidget panel; panel.rootContext()->setContextProperty("gameData", &data);
    panel.setSource(QUrl(QStringLiteral("qrc:/qml/RM27DevPanel.qml")));
    QCOMPARE(panel.status(), QQuickWidget::Ready);
    std::function<QQuickItem *(QQuickItem *, const QString &)> findVisual =
      [&](QQuickItem *item, const QString &name) -> QQuickItem * {
        if (item->objectName() == name) return item;
        for (auto *child : item->childItems()) if (auto *found = findVisual(child, name)) return found;
        return nullptr;
      };
    auto *typeRow = findVisual(panel.rootObject(), "rm27Penalty_penalty_type");
    auto *totalRow = findVisual(panel.rootObject(), "rm27Injury_total_damage");
    auto *killerRow = findVisual(panel.rootObject(), "rm27Injury_killer_id");
    QVERIFY(typeRow); QVERIFY(totalRow); QVERIFY(killerRow);
    robomaster::PenaltyInfo penalty;
    QCOMPARE(OfficialMessageHandler::handle("PenaltyInfo", bytes(penalty), data), OfficialMessageHandler::Result::Parsed);
    auto penaltyField = [&data](const char *name) { return data.penaltyObservation().value("fields").toMap().value(QLatin1String(name)).toMap(); };
    QCOMPARE(penaltyField("penalty_type").value("state").toString(), QString("unreported"));
    for (uint32_t v = 1; v <= 6; ++v) {
      penalty.set_penalty_type(v); penalty.set_penalty_effect_sec(0); penalty.set_total_penalty_num(4294967295u);
      QCOMPARE(OfficialMessageHandler::handle("PenaltyInfo", bytes(penalty), data), OfficialMessageHandler::Result::Parsed);
      QCOMPARE(penaltyField("penalty_type").value("state").toString(), QString("valid"));
      QCOMPARE(penaltyField("penalty_type").value("value").toUInt(), v);
    }
    QCOMPARE(penaltyField("penalty_effect_sec").value("value").toUInt(), 0u);
    QCOMPARE(penaltyField("total_penalty_num").value("value").toUInt(), 4294967295u);
    penalty.set_penalty_type(0); data.updatePenalty(penalty);
    QCOMPARE(penaltyField("penalty_type").value("state").toString(), QString("unknown"));
    QTRY_VERIFY(typeRow->property("primary").toString().contains(QStringLiteral("0（未知类型）")));
    penalty.set_penalty_type(99); penalty.set_penalty_effect_sec(65536); data.updatePenalty(penalty);
    QCOMPARE(penaltyField("penalty_type").value("value").toUInt(), 99u);
    QCOMPARE(penaltyField("penalty_effect_sec").value("value").toUInt(), 65536u);
    robomaster::RobotInjuryStat injury;
    injury.set_total_damage(0); injury.set_collision_damage(1); injury.set_small_projectile_damage(2);
    injury.set_large_projectile_damage(3); injury.set_dart_splash_damage(4); injury.set_module_offline_damage(5);
    injury.set_offline_damage(6); injury.set_penalty_damage(7); injury.set_server_kill_damage(4294967295u); injury.set_killer_id(101);
    QCOMPARE(OfficialMessageHandler::handle("RobotInjuryStat", bytes(injury), data), OfficialMessageHandler::Result::Parsed);
    auto injuryFields = data.injuryObservation().value("fields").toMap();
    const char *keys[] = {"total_damage", "collision_damage", "small_projectile_damage", "large_projectile_damage", "dart_splash_damage", "module_offline_damage", "offline_damage", "penalty_damage", "server_kill_damage", "killer_id"};
    const uint32_t values[] = {0,1,2,3,4,5,6,7,4294967295u,101};
    for (int i=0; i<10; ++i) { auto field = injuryFields.value(QLatin1String(keys[i])).toMap(); QCOMPARE(field.value("state").toString(), QString("valid")); QCOMPARE(field.value("value").toUInt(), values[i]); }
    QCOMPARE(injuryFields.value("killer_id").toMap().value("identityKind").toString(), QString("robot"));
    QTRY_VERIFY(totalRow->property("primary").toString().contains(QStringLiteral("总计: 0")));
    QTRY_VERIFY(killerRow->property("primary").toString().contains(QStringLiteral("101")));
    injury.Clear(); injury.set_killer_id(4294967295u); injury.set_collision_damage(25); data.updateRobotInjury(injury);
    injuryFields = data.injuryObservation().value("fields").toMap();
    QCOMPARE(injuryFields.value("total_damage").toMap().value("state").toString(), QString("unreported"));
    QCOMPARE(injuryFields.value("killer_id").toMap().value("identityKind").toString(), QString("unknown"));
    QVERIFY(data.injuryObservation().value("identityMatches").toBool());
    data.setCurrentRobotId(101);
    QVERIFY(!data.injuryObservation().value("identityMatches").toBool());
    QVERIFY(!data.penaltyObservation().value("identityMatches").toBool());
    QCOMPARE(OfficialMessageHandler::handle("RobotInjuryStat", QByteArray::fromHex("0880"), data), OfficialMessageHandler::Result::ParseFailed);
  }
  void staticPerformanceAndDeveloperPanel() {
    Q_INIT_RESOURCE(qml);
    GameData data; data.setCurrentRobotId(1);
    QQuickWidget panel; panel.rootContext()->setContextProperty("gameData", &data);
    panel.setSource(QUrl(QStringLiteral("qrc:/qml/RM27DevPanel.qml")));
    QCOMPARE(panel.status(), QQuickWidget::Ready);
    auto *view = panel.rootObject()->findChild<QObject *>("rm27StaticPerformance");
    QVERIFY(view); QVERIFY(view->property("text").toString().contains(QStringLiteral("最大血量: 未上报")));
    QQuickWidget hud; hud.rootContext()->setContextProperty("gameData", &data);
    hud.setSource(QUrl(QStringLiteral("qrc:/qml/CentralAimingHUD.qml")));
    QCOMPARE(hud.status(), QQuickWidget::Ready);
    QCOMPARE(hud.rootObject()->property("heatLimit").toDouble(), 0.0);
    robomaster::RobotStaticStatus msg; msg.set_robot_id(1); msg.set_connection_state(1);
    msg.set_max_health(100); msg.set_performance_system_shooter(1); msg.set_performance_system_chassis(2);
    msg.set_max_heat(0); msg.set_heat_cooldown_rate(0); msg.set_max_power(50);
    QSignalSpy updates(&data, &GameData::myRobotUpdated);
    QCOMPARE(OfficialMessageHandler::handle("RobotStaticStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    const auto *robot = data.getRobotById(1); QVERIFY(robot);
    QCOMPARE(robot->maxHP, 100); QCOMPARE(robot->shooterPerformanceSelection, 1); QCOMPARE(robot->chassisPerformanceSelection, 2);
    QVERIFY(updates.count() > 0);
    QTRY_VERIFY(view->property("text").toString().contains(QStringLiteral("最大血量: 100")));
    QVERIFY(view->property("text").toString().contains(QStringLiteral("冷却优先")));
    QVERIFY(view->property("text").toString().contains(QStringLiteral("功率优先")));
    msg.set_max_health(450); msg.set_performance_system_shooter(2); msg.set_performance_system_chassis(1);
    msg.set_max_heat(250); msg.set_heat_cooldown_rate(66.5f); msg.set_max_power(55);
    msg.set_max_buffer_energy(80); msg.set_max_chassis_energy(120);
    QCOMPARE(OfficialMessageHandler::handle("RobotStaticStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(robot->maxHP, 450); QCOMPARE(robot->heatLimit, 250); QCOMPARE(robot->coolingValue, 66.5f);
    QTRY_COMPARE(hud.rootObject()->property("heatLimit").toDouble(), 250.0);
    QCOMPARE(robot->maxPower, 55); QCOMPARE(robot->maxBufferEnergy, 80); QCOMPARE(robot->maxChassisEnergy, 120);
    QTRY_VERIFY(view->property("text").toString().contains(QStringLiteral("最大血量: 450")));
    QVERIFY(view->property("text").toString().contains(QStringLiteral("冷却速率: 66.5")));
    QVERIFY(view->property("text").toString().contains(QStringLiteral("爆发优先")));
    QVERIFY(view->property("text").toString().contains(QStringLiteral("血量优先")));
    robomaster::RobotStaticStatus energy;
    energy.set_robot_id(1); energy.set_max_buffer_energy(60); energy.set_max_chassis_energy(140);
    QCOMPARE(OfficialMessageHandler::handle("RobotStaticStatus", bytes(energy), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(robot->maxBufferEnergy, 60); QCOMPARE(robot->maxChassisEnergy, 140);
    QCOMPARE(robot->maxHP, 450); QCOMPARE(robot->heatLimit, 250);
    QTRY_VERIFY(view->property("text").toString().contains(QStringLiteral("最大缓冲能量: 60")));
    QVERIFY(view->property("text").toString().contains(QStringLiteral("最大底盘能量: 140")));
    energy.set_max_buffer_energy(80); energy.set_max_chassis_energy(120);
    QCOMPARE(OfficialMessageHandler::handle("RobotStaticStatus", bytes(energy), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(robot->maxBufferEnergy, 80); QCOMPARE(robot->maxChassisEnergy, 120);
    QTRY_VERIFY(view->property("text").toString().contains(QStringLiteral("最大缓冲能量: 80")));
    QVERIFY(view->property("text").toString().contains(QStringLiteral("最大底盘能量: 120")));
    robomaster::Event event; event.set_event_id(16);
    QCOMPARE(OfficialMessageHandler::handle("Event", bytes(event), data), OfficialMessageHandler::Result::Parsed);
    robomaster::RobotModuleStatus modules; modules.set_modular_42mm(1);
    QCOMPARE(OfficialMessageHandler::handle("RobotModuleStatus", bytes(modules), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(robot->maxHP, 450); QCOMPARE(robot->shooterPerformanceSelection, 2);
    robomaster::RobotStaticStatus dead; dead.set_robot_id(1); dead.set_alive_state(2);
    QCOMPARE(OfficialMessageHandler::handle("RobotStaticStatus", bytes(dead), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(robot->status, RobotStatus::DESTROYED);
    msg.Clear(); msg.set_robot_id(1); msg.set_field_state(0); msg.set_max_health(100);
    const bool connectedBefore = robot->isClientConnected;
    const auto statusBefore = robot->status;
    QCOMPARE(OfficialMessageHandler::handle("RobotStaticStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(robot->isClientConnected, connectedBefore); QCOMPARE(robot->status, statusBefore);
    QCOMPARE(robot->maxHP, 100); QCOMPARE(robot->heatLimit, 250); QCOMPARE(robot->maxPower, 55);
    QTRY_VERIFY(view->property("text").toString().contains(QStringLiteral("最大血量: 100")));
    QVERIFY(view->property("text").toString().contains(QStringLiteral("最大热量: 未上报")));
    for (uint32_t value : {0u, 3u, 4u, 257u}) {
      msg.set_performance_system_shooter(value); msg.set_performance_system_chassis(value);
      QCOMPARE(OfficialMessageHandler::handle("RobotStaticStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
      QCOMPARE(robot->shooterPerformanceSelection, 2); QCOMPARE(robot->chassisPerformanceSelection, 1);
      QVERIFY(view->property("text").toString().contains(QStringLiteral("非法值 %1").arg(value)));
    }
    msg.set_max_health(65536); msg.set_max_heat(65536); msg.set_heat_cooldown_rate(std::numeric_limits<float>::quiet_NaN());
    msg.set_max_power(65536); msg.set_max_buffer_energy(65536); msg.set_max_chassis_energy(65536);
    QCOMPARE(OfficialMessageHandler::handle("RobotStaticStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(robot->maxHP, 100); QCOMPARE(robot->heatLimit, 250); QCOMPARE(robot->coolingValue, 66.5f);
    QCOMPARE(robot->maxPower, 55); QCOMPARE(robot->maxBufferEnergy, 80); QCOMPARE(robot->maxChassisEnergy, 120);
    QCOMPARE(OfficialMessageHandler::handle("RobotStaticStatus", QByteArray::fromHex("2080"), data), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(robot->maxHP, 100);
    data.setCurrentRobotId(101);
    QTRY_VERIFY(view->property("text").toString().contains(QStringLiteral("ID 101")));
    QVERIFY(view->property("text").toString().contains(QStringLiteral("最大血量: 未上报")));
  }
  void dynamicPresencePrecisionAndPanel() {
    Q_INIT_RESOURCE(qml);
    GameData data; data.setCurrentRobotId(1);
    QQuickWidget panel; panel.rootContext()->setContextProperty("gameData", &data);
    panel.setSource(QUrl(QStringLiteral("qrc:/qml/RM27DevPanel.qml")));
    QCOMPARE(panel.status(), QQuickWidget::Ready);
    std::function<QQuickItem *(QQuickItem *, const QString &)> findVisual =
      [&](QQuickItem *item, const QString &name) -> QQuickItem * {
        if (item->objectName() == name) return item;
        for (auto *child : item->childItems()) if (auto *found = findVisual(child, name)) return found;
        return nullptr;
      };
    auto *summary = panel.rootObject()->findChild<QObject *>("rm27DynamicSummary"); QVERIFY(summary);
    QVERIFY(summary->property("text").toString().contains(QStringLiteral("已收到: 否")));
    auto *heat = findVisual(panel.rootObject(), "rm27Dynamic_current_heat");
    auto *ammo = findVisual(panel.rootObject(), "rm27Dynamic_remaining_ammo");
    auto *reserve = findVisual(panel.rootObject(), "rm27Dynamic_reserved");
    QVERIFY(heat); QVERIFY(ammo); QVERIFY(reserve);
    robomaster::RobotDynamicStatus msg;
    msg.set_current_health(99); msg.set_current_heat(12.75f); msg.set_last_projectile_fire_rate(16.5f);
    msg.set_reserved(1234); msg.set_current_buffer_energy(42); msg.set_current_experience(123);
    msg.set_experience_for_upgrade(77); msg.set_total_projectiles_fired(222); msg.set_remaining_ammo(33);
    msg.set_is_out_of_combat(false); msg.set_out_of_combat_countdown(5);
    msg.set_can_remote_heal(false); msg.set_can_remote_ammo(true);
    auto *robot = data.getRobotDataRef(1); QVERIFY(robot);
    robot->power = 17; robot->currentChassisEnergy = 19; robot->firerate = 7; robot->launchingFrequency = 8;
    QSignalSpy updates(&data, &GameData::robotDynamicStatusChanged);
    QCOMPARE(OfficialMessageHandler::handle("RobotDynamicStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(updates.count(), 1);
    const auto snap = data.robotDynamicStatus(); QVERIFY(snap.value("received").toBool());
    QCOMPARE(snap.value("robotId").toInt(), 1);
    const auto fields = snap.value("fields").toMap(); const auto accepted = snap.value("accepted").toMap();
    for (const char *key : {"current_health", "current_heat", "last_projectile_fire_rate", "current_buffer_energy", "current_experience", "experience_for_upgrade", "total_projectiles_fired", "remaining_ammo", "is_out_of_combat", "out_of_combat_countdown", "can_remote_heal", "can_remote_ammo"})
      QCOMPARE(fields.value(key).toMap().value("state").toString(), QStringLiteral("valid"));
    QCOMPARE(fields.value("reserved").toMap().value("state").toString(), QStringLiteral("not_applicable"));
    QCOMPARE(accepted.value("current_heat").toFloat(), 12.75f);
    QCOMPARE(accepted.value("last_projectile_fire_rate").toFloat(), 16.5f);
    QCOMPARE(accepted.value("total_projectiles_fired").toUInt(), 222u); QCOMPARE(accepted.value("remaining_ammo").toUInt(), 33u);
    QCOMPARE(accepted.value("is_out_of_combat").toBool(), false); QCOMPARE(accepted.value("can_remote_heal").toBool(), false);
    QCOMPARE(robot->currentHeat, 12.75f); QCOMPARE(robot->currentHP, 99);
    QCOMPARE(robot->power, 17); QCOMPARE(robot->currentChassisEnergy, 19);
    QCOMPARE(robot->firerate, 7); QCOMPARE(robot->launchingFrequency, 8);
    QCOMPARE(robot->allowedAmmo17mm, 0); QCOMPARE(robot->allowedAmmo42mm, 0);
    QTRY_VERIFY(heat->property("text").toString().contains(QStringLiteral("12.75&nbsp;&nbsp;&nbsp;&nbsp;<font color='#60D8B7'>valid</font>")));
    QVERIFY(ammo->property("text").toString().contains(QStringLiteral("33&nbsp;&nbsp;&nbsp;&nbsp;<font color='#60D8B7'>valid</font>")));
    QVERIFY(reserve->property("text").toString().contains(QStringLiteral("不适用")));
    msg.Clear(); msg.set_current_health(0); msg.set_current_heat(0); msg.set_remaining_ammo(0); msg.set_can_remote_ammo(false);
    QCOMPARE(OfficialMessageHandler::handle("RobotDynamicStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(robot->currentHP, 0); QCOMPARE(robot->currentHeat, 0.0f);
    QCOMPARE(data.robotDynamicStatus().value("fields").toMap().value("remaining_ammo").toMap().value("value").toUInt(), 0u);
    QVERIFY(!data.robotDynamicStatus().value("fields").toMap().value("can_remote_ammo").toMap().value("value").toBool());
    QCOMPARE(data.robotDynamicStatus().value("fields").toMap().value("last_projectile_fire_rate").toMap().value("state").toString(), QStringLiteral("unreported"));
    QCOMPARE(data.robotDynamicStatus().value("accepted").toMap().value("last_projectile_fire_rate").toFloat(), 16.5f);
    msg.Clear(); msg.set_current_heat(std::numeric_limits<float>::quiet_NaN()); msg.set_last_projectile_fire_rate(std::numeric_limits<float>::infinity());
    QCOMPARE(OfficialMessageHandler::handle("RobotDynamicStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(data.robotDynamicStatus().value("fields").toMap().value("current_heat").toMap().value("state").toString(), QStringLiteral("invalid"));
    QCOMPARE(data.robotDynamicStatus().value("fields").toMap().value("last_projectile_fire_rate").toMap().value("state").toString(), QStringLiteral("invalid"));
    QCOMPARE(robot->currentHeat, 0.0f);
    msg.Clear(); msg.set_current_heat(-1); msg.set_current_health(65536); msg.set_current_buffer_energy(65536);
    QCOMPARE(OfficialMessageHandler::handle("RobotDynamicStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(robot->currentHP, 0); QCOMPARE(robot->bufferEnergy, 42);
    QCOMPARE(data.robotDynamicStatus().value("accepted").toMap().value("current_health").toUInt(), 65536u);
    QCOMPARE(data.robotDynamicStatus().value("fields").toMap().value("current_health").toMap().value("legacyProjection").toString(), QStringLiteral("out_of_range"));
    const auto before = data.robotDynamicStatus();
    QCOMPARE(OfficialMessageHandler::handle("RobotDynamicStatus", QByteArray::fromHex("0880"), data), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(data.robotDynamicStatus(), before);
    data.setCurrentRobotId(101); QVERIFY(!data.robotDynamicStatus().value("received").toBool());
    QVERIFY(data.robotDynamicStatus().value("accepted").toMap().isEmpty());
    data.setCurrentRobotId(2);
    QCOMPARE(OfficialMessageHandler::handle("RobotDynamicStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QVERIFY(!data.robotDynamicStatus().value("received").toBool());
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
  void globalRawObservations() {
    Q_INIT_RESOURCE(qml);
    GameData data; data.setCurrentRobotId(1);
    QQuickWidget panel; panel.rootContext()->setContextProperty("gameData", &data);
    panel.setSource(QUrl(QStringLiteral("qrc:/qml/RM27DevPanel.qml")));
    QCOMPARE(panel.status(), QQuickWidget::Ready);
    std::function<QQuickItem *(QQuickItem *, const QString &)> findVisual = [&](QQuickItem *item, const QString &name) -> QQuickItem * {
      if (item->objectName() == name) return item;
      for (auto *child : item->childItems()) if (auto *found = findVisual(child, name)) return found;
      return nullptr;
    };
    auto *bullets = findVisual(panel.rootObject(), "rm27GlobalRawBullets");
    auto *cp = findVisual(panel.rootObject(), "rm27GlobalRaw_total_cp_ally");
    auto *damage = findVisual(panel.rootObject(), "rm27GlobalRaw_total_damage_enemy");
    auto *session = findVisual(panel.rootObject(), "rm27GlobalRawSession");
    QVERIFY(bullets); QVERIFY(cp); QVERIFY(damage); QVERIFY(session);
    robomaster::GlobalUnitStatus msg;
    msg.add_robot_health(120); msg.add_robot_bullets(std::numeric_limits<int32_t>::min()); msg.add_robot_bullets(-1);
    msg.add_robot_bullets(0); msg.add_robot_bullets(std::numeric_limits<int32_t>::max()); msg.add_robot_bullets(17);
    msg.set_total_damage_ally(0); msg.set_total_damage_enemy(4294967295u); msg.set_total_cp_ally(0); msg.set_total_cp_enemy(4294967295u);
    QCOMPARE(OfficialMessageHandler::handle("GlobalUnitStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    auto observation = data.globalHealthObservation(); auto fields = observation.value("rawFields").toMap();
    auto array = fields.value("robot_bullets").toMap();
    QCOMPARE(array.value("length").toInt(), 5); QCOMPARE(array.value("values").toList(), QVariantList({INT32_MIN, -1, 0, INT32_MAX, 17}));
    QCOMPARE(fields.value("total_cp_ally").toMap().value("state").toString(), QString("valid"));
    QCOMPARE(fields.value("total_cp_ally").toMap().value("value").toUInt(), 0u);
    QCOMPARE(fields.value("total_cp_enemy").toMap().value("value").toUInt(), 4294967295u);
    QCOMPARE(fields.value("total_damage_ally").toMap().value("value").toUInt(), 0u);
    QCOMPARE(fields.value("total_damage_enemy").toMap().value("value").toUInt(), 4294967295u);
    QCOMPARE(data.getRobotById(1)->currentHP, 120);
    QTRY_VERIFY(bullets->property("primary").toString().contains(QStringLiteral("[-2147483648, -1, 0, 2147483647, 17]")));
    QTRY_VERIFY(cp->property("primary").toString().contains(QStringLiteral("原始）: 0")));
    QTRY_VERIFY(damage->property("primary").toString().contains(QStringLiteral("4294967295")));
    QTRY_VERIFY(session->property("primary").toString().contains(QStringLiteral("红方")));
    data.setMqttSessionState(true, "1"); data.recordMqttObservation("GlobalUnitStatus", 1, 100000, 1000);
    const auto stamp = data.mqttReceptionMetadata().value("topics").toMap().value("GlobalUnitStatus").toMap().value("fieldUpdates").toMap().value("total_cp_ally").toMap();
    QVERIFY(stamp.value("receivedAt").toDateTime().isValid());
    msg.Clear(); msg.set_total_damage_enemy(8); data.updateGlobalUnitStatus(msg);
    data.recordMqttObservation("GlobalUnitStatus", 1, 200000, 2000);
    observation = data.globalHealthObservation(); fields = observation.value("rawFields").toMap();
    QCOMPARE(fields.value("total_cp_ally").toMap().value("state").toString(), QString("unreported"));
    QCOMPARE(observation.value("rawAccepted").toMap().value("total_cp_ally").toUInt(), 0u);
    QCOMPARE(fields.value("robot_bullets").toMap().value("length").toInt(), 0);
    QVERIFY(fields.value("robot_bullets").toMap().value("emptyPresenceUnknown").toBool());
    QTRY_VERIFY(cp->property("primary").toString().contains(QStringLiteral("上次: 0")));
    QTRY_VERIFY(bullets->property("detail").toString().contains(QStringLiteral("不可区分")));
    data.setCurrentRobotId(101); QVERIFY(!data.globalHealthObservation().value("identityMatches").toBool());
    msg.Clear(); msg.add_robot_bullets(-7); msg.set_total_cp_ally(55); data.updateGlobalUnitStatus(msg);
    QCOMPARE(data.globalHealthObservation().value("receiverId").toInt(), 101);
    QCOMPARE(data.globalHealthObservation().value("rawFields").toMap().value("total_cp_ally").toMap().value("value").toUInt(), 55u);
    QVERIFY(!data.globalHealthObservation().value("rawAccepted").toMap().contains("total_cp_enemy"));
    QTRY_VERIFY(session->property("primary").toString().contains(QStringLiteral("蓝方")));
    QCOMPARE(OfficialMessageHandler::handle("GlobalUnitStatus", QByteArray::fromHex("7880"), data), OfficialMessageHandler::Result::ParseFailed);
  }
  void globalSourcesAndDeveloperPanel() {
    Q_INIT_RESOURCE(qml);
    GameData data; data.setCurrentRobotId(1);
    QQuickWidget panel; panel.rootContext()->setContextProperty("gameData", &data);
    panel.setSource(QUrl(QStringLiteral("qrc:/qml/RM27DevPanel.qml")));
    QCOMPARE(panel.status(), QQuickWidget::Ready);
    auto *health = panel.rootObject()->findChild<QObject *>("rm27HealthSources");
    auto *heat = panel.rootObject()->findChild<QObject *>("rm27HeatSources");
    auto *projection = panel.rootObject()->findChild<QObject *>("rm27GameProjection");
    auto *connection = panel.rootObject()->findChild<QObject *>("rm27ConnectionSources");
    QVERIFY(health); QVERIFY(heat); QVERIFY(projection); QVERIFY(connection);
    QVERIFY(connection->property("text").toString().contains(QStringLiteral("实体控制器连接: 未知")));
    QVERIFY(connection->property("text").toString().contains(QStringLiteral("视频接收: 未知")));
    data.setVideoReceptionState(1);
    QVERIFY(connection->property("text").toString().contains(QStringLiteral("近期收到帧")));
    robomaster::RobotStaticStatus stat; stat.set_robot_id(1); stat.set_connection_state(1); stat.set_alive_state(1);
    data.updateRobotStaticStatus(stat);
    QTRY_VERIFY(connection->property("text").toString().contains(QStringLiteral("裁判机器人连接: 已连接")));
    robomaster::RobotDynamicStatus dyn; dyn.set_current_health(101); dyn.set_current_heat(12.75);
    data.updateRobotDynamicStatus(dyn);
    robomaster::GlobalUnitStatus global; global.add_robot_health(120); global.add_robot_health(30);
    data.updateGlobalUnitStatus(global);
    QCOMPARE(data.globalHealthObservation().value("fields").toMap().value("1").toMap().value("value").toUInt(), 120u);
    QCOMPARE(data.globalHealthObservation().value("acceptedSource").toMap().value("1").toString(), QString("protocol_unverified"));
    QCOMPARE(data.robotDynamicStatus().value("accepted").toMap().value("current_health").toUInt(), 101u);
    QTRY_VERIFY(health->property("text").toString().contains(QStringLiteral("Global 血量: 120")));
    QVERIFY(health->property("text").toString().contains(QStringLiteral("Dynamic 血量: 101")));
    QCOMPARE(data.getRobotById(1)->currentHP, 120);
    dyn.set_current_health(90); data.updateRobotDynamicStatus(dyn);
    QCOMPARE(data.getRobotById(1)->currentHP, 90);
    QCOMPARE(data.globalHealthObservation().value("accepted").toMap().value("1").toUInt(), 120u);
    global.Clear(); global.add_robot_health(0); data.updateGlobalUnitStatus(global);
    QCOMPARE(data.getRobotById(1)->currentHP, 0);
    QCOMPARE(data.globalHealthObservation().value("fields").toMap().value("3").toMap().value("state").toString(), QString("unreported"));
    QCOMPARE(data.globalHealthObservation().value("accepted").toMap().value("3").toUInt(), 30u);
    dyn.set_current_health(100); data.updateRobotDynamicStatus(dyn);
    auto previousStatus = data.getRobotById(1)->status;
    global.Clear(); global.add_robot_health(65536); data.updateGlobalUnitStatus(global);
    QCOMPARE(data.getRobotById(1)->currentHP, 100);
    QCOMPARE(data.getRobotById(1)->status, previousStatus);
    QCOMPARE(data.globalHealthObservation().value("fields").toMap().value("1").toMap().value("legacyProjection").toString(), QString("out_of_range"));
    QCOMPARE(data.globalHealthObservation().value("accepted").toMap().value("1").toUInt(), 65536u);
    data.setCurrentRobotId(101); global.Clear(); global.add_robot_health(210); global.add_robot_health(211); data.updateGlobalUnitStatus(global);
    QCOMPARE(data.globalHealthObservation().value("receiverId").toInt(), 101);
    QCOMPARE(data.globalHealthObservation().value("fields").toMap().value("101").toMap().value("value").toUInt(), 210u);
    QCOMPARE(data.getRobotById(103)->currentHP, 211);
    data.setCurrentRobotId(1); QVERIFY(!data.globalHealthObservation().value("identityMatches").toBool());
    QTRY_VERIFY(health->property("text").toString().contains(QStringLiteral("旧身份观测")));
    data.setRobotHeat(1, 40); data.recordLocalHeatObservation(1, 40, "debug_test");
    dyn.Clear(); dyn.set_current_heat(12.75); data.updateRobotDynamicStatus(dyn);
    QCOMPARE(data.getRobotById(1)->currentHeat, 40.0f); // Existing 1.5 s protection remains.
    QCOMPARE(data.robotDynamicStatus().value("accepted").toMap().value("current_heat").toFloat(), 12.75f);
    QTRY_VERIFY(heat->property("text").toString().contains(QStringLiteral("debug_test")));
    robomaster::GameStatus game; game.set_current_stage(4); game.set_stage_countdown_sec(200); data.updateGameStatus(game);
    data.updateGameStateFromS1(4, 190, 0, 0, 1, false);
    QCOMPARE(data.gameStatusObservation().value("accepted").toMap().value("stage_countdown_sec").toInt(), 200);
    QTRY_VERIFY(projection->property("text").toString().contains(QStringLiteral("剩余 190（不同）")));
  }
  void globalLastValueKeepsOwnSource() {
    GameData data; data.setMqttSessionState(true, "1");
    robomaster::GlobalUnitStatus global; global.add_robot_health(100);
    data.updateGlobalUnitStatus(global); data.recordMqttObservation("GlobalUnitStatus", 1, 100000, 1000);
    QCOMPARE(data.globalHealthObservation().value("acceptedSource").toMap().value("1").toString(), QString("official_mqtt"));
    global.Clear(); global.add_robot_health(110); data.updateGlobalUnitStatus(global);
    QCOMPARE(data.globalHealthObservation().value("acceptedSource").toMap().value("1").toString(), QString("protocol_unverified"));
    global.Clear(); data.updateGlobalUnitStatus(global); data.recordMqttObservation("GlobalUnitStatus", 1, 200000, 2000);
    QCOMPARE(data.globalHealthObservation().value("accepted").toMap().value("1").toUInt(), 110u);
    QCOMPARE(data.globalHealthObservation().value("acceptedSource").toMap().value("1").toString(), QString("protocol_unverified"));
  }
  void mqttObservationMetadata() {
    Q_INIT_RESOURCE(qml);
    GameData data;
    QQuickWidget panel; panel.rootContext()->setContextProperty("gameData", &data);
    panel.setSource(QUrl(QStringLiteral("qrc:/qml/RM27DevPanel.qml")));
    QCOMPARE(panel.status(), QQuickWidget::Ready);
    auto *sessionView = panel.rootObject()->findChild<QObject *>("rm27MqttSession"); QVERIFY(sessionView);
    auto metadata = [&]() { return data.mqttReceptionMetadata(); };
    auto topic = [&](const char *key) { return metadata().value("topics").toMap().value(QLatin1String(key)).toMap(); };
    QCOMPARE(topic("GameStatus").value("sessionState").toString(), QString("never"));
    QVERIFY(sessionView->property("text").toString().contains(QStringLiteral("未连接")));
    data.setMqttSessionState(true, "1");
    data.recordMqttObservation("RobotDynamicStatus", 0, 100000, 1234);
    data.recordMqttObservation("RobotModuleStatus", 0, 100000, 1234);
    QCOMPARE(topic("RobotDynamicStatus").value("sessionState").toString(), QString("never"));
    QCOMPARE(topic("RobotModuleStatus").value("sessionState").toString(), QString("never"));
    const auto session = metadata().value("session").toULongLong(); QVERIFY(session > 0);
    QVERIFY(sessionView->property("text").toString().contains(QStringLiteral("已连接")));
    robomaster::GameStatus status; status.set_current_stage(4); status.set_red_score(0);
    data.updateGameStatus(status); data.recordMqttObservation("GameStatus", 0, 100000, 1234);
    auto first = topic("GameStatus").value("fieldUpdates").toMap().value("red_score").toMap();
    QCOMPARE(first.value("receivedAt").toDateTime().toMSecsSinceEpoch(), qint64(100000));
    QCOMPARE(first.value("receivedMonotonicMs").toLongLong(), qint64(1234));
    status.Clear(); status.set_blue_score(2);
    data.updateGameStatus(status); data.recordMqttObservation("GameStatus", 0, 200000, 2234);
    QCOMPARE(topic("GameStatus").value("fieldUpdates").toMap().value("red_score").toMap(), first);
    QCOMPARE(topic("GameStatus").value("receivedAt").toDateTime().toMSecsSinceEpoch(), qint64(200000));
    const qint64 initialAge = topic("GameStatus").value("ageMs").toLongLong();
    QTest::qWait(10); QVERIFY(topic("GameStatus").value("ageMs").toLongLong() >= initialAge);
    status.Clear(); status.set_current_stage(99); data.updateGameStatus(status);
    data.recordMqttObservation("GameStatus", 0, 300000, 3234);
    QCOMPARE(topic("GameStatus").value("fieldUpdates").toMap().value("current_stage").toMap().value("receivedMonotonicMs").toLongLong(), qint64(1234));
    data.setMqttSessionState(false, "1");
    QCOMPARE(topic("GameStatus").value("sessionState").toString(), QString("disconnected"));
    data.setMqttSessionState(true, "1");
    QVERIFY(metadata().value("session").toULongLong() > session);
    QCOMPARE(topic("GameStatus").value("sessionState").toString(), QString("historical"));
    status.Clear(); status.set_red_score(3); data.updateGameStatus(status);
    data.recordMqttObservation("GameStatus", 0, 400000, 4234);
    QCOMPARE(topic("GameStatus").value("sessionState").toString(), QString("current"));
    data.recordMqttObservation("Event16", 0, 500000, 5234);
    QCOMPARE(topic("Event16").value("identity").toInt(), 0);
  }
  void gameStatusPresenceAndPanel() {
    Q_INIT_RESOURCE(qml);
    GameData data;
    QQuickWidget panel;
    panel.rootContext()->setContextProperty("gameData", &data);
    panel.setSource(QUrl(QStringLiteral("qrc:/qml/RM27DevPanel.qml")));
    QCOMPARE(panel.status(), QQuickWidget::Ready);
    auto field = [&](const char *key) { return data.gameStatusObservation().value("fields").toMap().value(QLatin1String(key)).toMap(); };
    auto accepted = [&]() { return data.gameStatusObservation().value("accepted").toMap(); };
    std::function<QQuickItem *(QQuickItem *, const QString &)> findVisual =
        [&](QQuickItem *item, const QString &name) -> QQuickItem * {
          if (item->objectName() == name) return item;
          for (auto *child : item->childItems()) if (auto *found = findVisual(child, name)) return found;
          return nullptr;
        };
    auto display = [&](const char *key) { return findVisual(panel.rootObject(), QString("rm27GameStatus_") + key); };
    data.simulateGameTimeElapse(3);
    QVERIFY(!data.gameStatusObservation().value("received").toBool());
    QVERIFY(display("stage_countdown_sec"));
    QVERIFY(display("stage_countdown_sec")->property("text").toString().contains(QStringLiteral("未上报")));
    QVERIFY(!display("stage_countdown_sec")->property("text").toString().contains("420"));
    robomaster::GameStatus msg;
    msg.set_current_round(1); msg.set_total_rounds(3); msg.set_red_score(0); msg.set_blue_score(2);
    msg.set_current_stage(4); msg.set_stage_countdown_sec(299); msg.set_stage_elapsed_sec(1); msg.set_is_paused(false);
    msg.set_game_result(255); msg.set_end_reason(255);
    QCOMPARE(OfficialMessageHandler::handle("GameStatus", bytes(msg), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(field("red_score").value("state").toString(), QString("valid"));
    QCOMPARE(field("red_score").value("value").toUInt(), 0u);
    QCOMPARE(field("is_paused").value("value").toBool(), false);
    QCOMPARE(field("game_result").value("state").toString(), QString("not_applicable"));
    QVERIFY(!accepted().contains("game_result"));
    QCOMPARE(data.getGameTime(), 299);
    robomaster::GameStatus partial; partial.set_blue_score(5); partial.set_is_paused(true);
    QCOMPARE(OfficialMessageHandler::handle("GameStatus", bytes(partial), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(field("red_score").value("state").toString(), QString("unreported"));
    QCOMPARE(accepted().value("red_score").toUInt(), 0u);
    QCOMPARE(data.getGameTime(), 299); QCOMPARE(data.blueScore(), 5); QVERIFY(data.is_paused());
    for (unsigned stage = 0; stage <= 5; ++stage) {
      robomaster::GameStatus m; m.set_current_stage(stage);
      QCOMPARE(OfficialMessageHandler::handle("GameStatus", bytes(m), data), OfficialMessageHandler::Result::Parsed);
      QCOMPARE(field("current_stage").value("state").toString(), QString("valid"));
      QCOMPARE(static_cast<int>(data.getCurrentStage()), static_cast<int>(stage));
      QVERIFY(display("current_stage")->property("text").toString().contains(QString::number(stage)));
    }
    for (unsigned reason = 1; reason <= 7; ++reason) {
      robomaster::GameStatus m; m.set_current_stage(5); m.set_game_result(reason % 3); m.set_end_reason(reason);
      QCOMPARE(OfficialMessageHandler::handle("GameStatus", bytes(m), data), OfficialMessageHandler::Result::Parsed);
      QCOMPARE(field("game_result").value("state").toString(), QString("valid"));
      QVERIFY(data.gameStatusObservation().value("resultContextValid").toBool());
      QCOMPARE(field("end_reason").value("state").toString(), QString("valid"));
      QCOMPARE(accepted().value("end_reason").toUInt(), reason);
      QVERIFY(display("end_reason")->property("text").toString().contains(QString::number(reason)));
    }
    robomaster::GameStatus invalid; invalid.set_current_stage(77); invalid.set_game_result(1); invalid.set_end_reason(1);
    invalid.set_current_round(0); invalid.set_red_score(70000);
    invalid.set_stage_countdown_sec(std::numeric_limits<qint32>::min());
    invalid.set_stage_elapsed_sec(std::numeric_limits<qint32>::max());
    QCOMPARE(OfficialMessageHandler::handle("GameStatus", bytes(invalid), data), OfficialMessageHandler::Result::Parsed);
    QCOMPARE(field("current_stage").value("state").toString(), QString("invalid"));
    QCOMPARE(field("game_result").value("state").toString(), QString("not_applicable"));
    QCOMPARE(field("current_round").value("state").toString(), QString("invalid"));
    QCOMPARE(field("red_score").value("legacyProjection").toString(), QString("out_of_range"));
    QCOMPARE(accepted().value("stage_countdown_sec").toInt(), std::numeric_limits<qint32>::min());
    QCOMPARE(accepted().value("stage_elapsed_sec").toInt(), std::numeric_limits<qint32>::max());
    QCOMPARE(data.getGameTime(), 299); QCOMPARE(data.redScore(), 0);
    QVERIFY(!accepted().contains("game_result"));
    robomaster::GameStatus badResult; badResult.set_current_stage(5); badResult.set_game_result(255); badResult.set_end_reason(0);
    OfficialMessageHandler::handle("GameStatus", bytes(badResult), data);
    QCOMPARE(field("game_result").value("state").toString(), QString("invalid"));
    QCOMPARE(field("end_reason").value("state").toString(), QString("invalid"));
    robomaster::GameStatus unknownStage; unknownStage.set_game_result(1); unknownStage.set_end_reason(7);
    OfficialMessageHandler::handle("GameStatus", bytes(unknownStage), data);
    QCOMPARE(field("game_result").value("state").toString(), QString("not_applicable"));
    QVERIFY(!data.gameStatusObservation().value("resultContextValid").toBool());
    QVERIFY(!accepted().contains("game_result"));
    const auto before = data.gameStatusObservation();
    QCOMPARE(OfficialMessageHandler::handle("GameStatus", QByteArray::fromHex("0880"), data), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(data.gameStatusObservation(), before);
  }
  void gameStatusCacheAndOrdering() {
    GameData data;
    bool hasCache = false, paused = false;
    quint32 stage = 0;
    quint16 remaining = 0;
    int logs = 0;
    auto cache = [&](const robomaster::GameStatus &status) {
      hasCache = true; stage = status.current_stage(); remaining = status.stage_countdown_sec(); paused = status.is_paused();
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
    QCOMPARE(signal.count(), 0); QCOMPARE(popup.count(), 0);
    QCOMPARE(data.lastEventObservation().value("eventId").toInt(), 8);
    QCOMPARE(data.lastEventObservation().value("param").toString(), QStringLiteral("RED"));
    QCOMPARE(received.count(), 1);
    network.processMqttMessage("Event", QByteArray::fromHex("0880"));
    QCOMPARE(signal.count(), 0); QCOMPARE(popup.count(), 0);
    QCOMPARE(received.count(), 2);
  }
  void networkGameStatusPartialCache() {
    GameData data;
    RM::NetworkManager network(&data);
    robomaster::GameStatus initial; initial.set_current_stage(4); initial.set_stage_countdown_sec(250); initial.set_is_paused(true);
    network.processMqttMessage("GameStatus", bytes(initial));
    robomaster::GameStatus partial; partial.set_red_score(7);
    network.processMqttMessage("GameStatus", bytes(partial));
    QCOMPARE(network.m_lastMqttStage, robomaster::STAGE_BATTLE);
    QCOMPARE(network.m_lastMqttTimeRemaining, quint16(250)); QVERIFY(network.m_lastMqttPaused);
    partial.Clear(); partial.set_current_stage(99); partial.set_stage_countdown_sec(-1);
    network.processMqttMessage("GameStatus", bytes(partial));
    QCOMPARE(network.m_lastMqttStage, robomaster::STAGE_BATTLE);
    QCOMPARE(network.m_lastMqttTimeRemaining, quint16(250));
    partial.Clear(); partial.set_current_stage(0); partial.set_stage_countdown_sec(0); partial.set_is_paused(false);
    network.processMqttMessage("GameStatus", bytes(partial));
    QCOMPARE(network.m_lastMqttStage, robomaster::STAGE_NOT_STARTED);
    QCOMPARE(network.m_lastMqttTimeRemaining, quint16(0)); QVERIFY(!network.m_lastMqttPaused);
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
    QCOMPARE(data.getGameTime(), 100); // Logistics must not manufacture a GameStatus.
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
  void conflictingEventRetainsRawData() {
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
    QCOMPARE(logs, 1); QCOMPARE(air.count(), 0); QCOMPARE(popup.count(), 0);
    QCOMPARE(OfficialMessageHandler::handle("Event", QByteArray::fromHex("0880"), data, logger), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(logs, 1); QCOMPARE(air.count(), 0); QCOMPARE(popup.count(), 0);
  }
  void conflictingEventsHaveNoLegacyEffects() {
    Q_INIT_RESOURCE(qml);
    Q_INIT_RESOURCE(resources);
    GameData data;
    QSignalSpy raw(&data, &GameData::lastEventObservationChanged);
    QSignalSpy message(&data, &GameData::systemMessageReceived);
    QSignalSpy popup(&data, &GameData::officialEventPopupRequested);
    QSignalSpy started(&data, &GameData::airSupportStarted);
    QSignalSpy countered(&data, &GameData::airSupportCountered);
    QQuickWidget panel;
    panel.rootContext()->setContextProperty("gameData", &data);
    panel.setSource(QUrl(QStringLiteral("qrc:/qml/RM27DevPanel.qml")));
    QCOMPARE(panel.status(), QQuickWidget::Ready);
    QObject *display = panel.rootObject()->findChild<QObject *>("rm27LastEventObservation");
    QVERIFY(display);
    for (int id : {5, 6, 7, 8, 14, 15}) {
      robomaster::Event event; event.set_event_id(id); event.set_param(" 2 ");
      int logged = 0;
      auto logger = [&](const QString &, const QJsonObject &fields) {
        ++logged; QCOMPARE(fields.value("event_id").toInt(), id);
        QCOMPARE(fields.value("param").toString(), QStringLiteral(" 2 "));
      };
      QCOMPARE(OfficialMessageHandler::handle("Event", bytes(event), data, logger), OfficialMessageHandler::Result::Parsed);
      QCOMPARE(logged, 1);
      QCOMPARE(data.lastEventObservation().value("eventId").toInt(), id);
      QCOMPARE(data.lastEventObservation().value("param").toString(), QStringLiteral(" 2 "));
      QVERIFY(display->property("text").toString().contains(QString::number(id)));
      data.eventInfo(event); // Public direct caller must not restore old effects.
    }
    QCOMPARE(raw.count(), 6);
    QCOMPARE(message.count(), 0); QCOMPARE(popup.count(), 0);
    QCOMPARE(started.count(), 0); QCOMPARE(countered.count(), 0);
    const auto last = data.lastEventObservation();
    QCOMPARE(OfficialMessageHandler::handle("Event", QByteArray::fromHex("0880"), data), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(data.lastEventObservation(), last);
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
    network.startMqtt(uri, QStringLiteral("1"));
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
    network.stopMqtt(); connected.clear(); network.startMqtt(uri, QStringLiteral("101"));
    QTRY_VERIFY_WITH_TIMEOUT(!connected.isEmpty(), 5000); QVERIFY(network.isMqttConnected());
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
    QSignalSpy rawEvents(&data, &GameData::lastEventObservationChanged);
    QSignalSpy legacyAir(&data, &GameData::airSupportCountered);
    QSignalSpy legacyMessages(&data, &GameData::systemMessageReceived);
    for (int id : {5, 6, 7, 8, 14, 15}) {
      robomaster::Event rawEvent; rawEvent.set_event_id(id); rawEvent.set_param("2");
      const int expected = rawEvents.count() + 1;
      QVERIFY(publisher.publish(QStringLiteral("Event"), bytes(rawEvent), 1));
      QTRY_COMPARE_WITH_TIMEOUT(rawEvents.count(), expected, 5000);
      QCOMPARE(data.lastEventObservation().value("eventId").toInt(), id);
      QCOMPARE(data.lastEventObservation().value("param").toString(), QStringLiteral("2"));
    }
    QCOMPARE(popups.count(), 0); QCOMPARE(legacyAir.count(), 0); QCOMPARE(legacyMessages.count(), 0);
    QSignalSpy gameStatusChanged(&data, &GameData::gameStatusObservationChanged);
    robomaster::GameStatus game; game.set_current_stage(4); game.set_stage_countdown_sec(240);
    game.set_red_score(7); game.set_blue_score(3); game.set_is_paused(false); game.set_game_result(255);
    QVERIFY(publisher.publish(QStringLiteral("GameStatus"), bytes(game), 1));
    QTRY_COMPARE_WITH_TIMEOUT(gameStatusChanged.count(), 1, 5000);
    QCOMPARE(data.getGameTime(), 240); QCOMPARE(data.redScore(), 7);
    QCOMPARE(data.gameStatusObservation().value("fields").toMap().value("game_result").toMap().value("state").toString(), QString("not_applicable"));
    game.Clear(); game.set_current_stage(5); game.set_game_result(1); game.set_end_reason(2);
    QVERIFY(publisher.publish(QStringLiteral("GameStatus"), bytes(game), 1));
    QTRY_COMPARE_WITH_TIMEOUT(gameStatusChanged.count(), 2, 5000);
    QCOMPARE(data.gameStatusObservation().value("accepted").toMap().value("end_reason").toUInt(), 2u);
    QCOMPARE(data.getGameTime(), 240); QCOMPARE(data.blueScore(), 3);
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
