// SPDX-License-Identifier: MIT
#include "core/GameData.h"
#include "network/OfficialMessageHandler.h"
#include "network/NetworkManager.h"
#include "robomaster.pb.h"
#include <QSignalSpy>
#include <QJsonArray>
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
