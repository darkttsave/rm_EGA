// SPDX-License-Identifier: MIT
#include "core/GameData.h"
#include "network/OfficialMessageHandler.h"
#include "robomaster.pb.h"
#include <QSignalSpy>
#include <QtTest>

using RM::OfficialMessageHandler;
namespace {
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
  void malformedAndUnsupported() {
    GameData data;
    QSignalSpy changed(&data, &GameData::dataChanged);
    int logs = 0;
    auto logger = [&](const QString &, const QJsonObject &) { ++logs; };
    // A valid first field followed by a truncated varint tests partial parses.
    const QByteArray invalid = QByteArray::fromHex("08011080");
    for (const QString &topic : {QString("GlobalSpecialMechanism"), QString("Buff"), QString("DeployModeStatusSync")})
      QCOMPARE(OfficialMessageHandler::handle(topic, invalid, data, logger), OfficialMessageHandler::Result::ParseFailed);
    QCOMPARE(OfficialMessageHandler::handle("GameStatus", invalid, data, logger), OfficialMessageHandler::Result::Unsupported);
    QCOMPARE(data.allyFortressOccupationSec(), 0); QCOMPARE(data.deployModeStatus(), 0);
    QCOMPARE(changed.count(), 0); QCOMPARE(logs, 0);
  }
};
QTEST_MAIN(TestOfficialMessageHandler)
#include "test_official_message_handler.moc"
