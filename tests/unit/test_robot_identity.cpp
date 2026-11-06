#include "core/RobotIdentity.h"

#include <QtTest>
#include <limits>

class TestRobotIdentity : public QObject {
  Q_OBJECT

private slots:
  void officialIdentities_data() {
    QTest::addColumn<qint64>("id");
    QTest::addColumn<int>("number");
    QTest::addColumn<int>("team"); // -1: 未定义
    QTest::addColumn<int>("kind");
    QTest::addColumn<int>("role"); // -1: 无角色
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("selectable");

    struct Row {
      int number;
      RM::IdentityKind kind;
      int role;
      const char *name;
      bool selectable;
    };
    const Row rows[] = {
        {1, RM::IdentityKind::Robot, int(RobotType::HEAVY), "重装", true},
        {2, RM::IdentityKind::Reserved, -1, "保留位", false},
        {3, RM::IdentityKind::Robot, int(RobotType::INFANTRY_3), "步兵3", true},
        {4, RM::IdentityKind::Robot, int(RobotType::INFANTRY_4), "步兵4", false},
        {5, RM::IdentityKind::Robot, int(RobotType::INFANTRY_5), "步兵5", false},
        {6, RM::IdentityKind::Robot, int(RobotType::AERIAL), "空中系统", false},
        {7, RM::IdentityKind::Robot, int(RobotType::SENTRY), "哨兵", true},
        {8, RM::IdentityKind::Robot, int(RobotType::DART), "飞镖", false},
        {9, RM::IdentityKind::Robot, int(RobotType::RADAR), "雷达", false},
        {10, RM::IdentityKind::FieldObject, -1, "基地", false},
        {11, RM::IdentityKind::FieldObject, -1, "", false},
    };
    for (int offset : {0, 100}) {
      for (const auto &row : rows) {
        const qint64 id = offset + row.number;
        const QByteArray label = QByteArray::number(id);
        const QString name = row.number == 11
                                 ? (offset == 0 ? QStringLiteral("R前哨")
                                                : QStringLiteral("B前哨"))
                                 : QString::fromUtf8(row.name);
        const int team = row.number == 11 ? -1 : offset == 0
                                                         ? int(TeamColor::RED)
                                                         : int(TeamColor::BLUE);
        QTest::newRow(label.constData()) << id << row.number << team
                                        << int(row.kind) << row.role << name
                                        << row.selectable;
      }
    }
  }

  void officialIdentities() {
    QFETCH(qint64, id);
    QFETCH(int, number);
    QFETCH(int, team);
    QFETCH(int, kind);
    QFETCH(int, role);
    QFETCH(QString, name);
    QFETCH(bool, selectable);
    const auto identity = RM::resolveRobotIdentity(id);
    QCOMPARE(identity.rawId, id);
    QVERIFY(identity.normalizedId.has_value());
    QCOMPARE(*identity.normalizedId, number);
    QCOMPARE(identity.team.has_value(), team != -1);
    if (identity.team) QCOMPARE(int(*identity.team), team);
    QCOMPARE(int(identity.kind), kind);
    QCOMPARE(identity.role.has_value(), role != -1);
    if (identity.role) QCOMPARE(int(*identity.role), role);
    QCOMPARE(identity.displayName, name);
    QCOMPARE(identity.rmulSelectable, selectable);
  }

  void unknownIdentities_data() {
    QTest::addColumn<qint64>("id");
    for (qint64 id : {std::numeric_limits<qint64>::min(), qint64(-1),
                      qint64(0), qint64(12), qint64(99), qint64(100),
                      qint64(112), qint64(201), qint64(256), qint64(257),
                      qint64(4294967295LL), std::numeric_limits<qint64>::max()}) {
      QTest::newRow(QByteArray::number(id).constData()) << id;
    }
  }

  void unknownIdentities() {
    QFETCH(qint64, id);
    const auto identity = RM::resolveRobotIdentity(id);
    QCOMPARE(identity.rawId, id);
    QCOMPARE(int(identity.kind), int(RM::IdentityKind::Unknown));
    QVERIFY(!identity.normalizedId);
    QVERIFY(!identity.team);
    QVERIFY(!identity.role);
    QVERIFY(!identity.rmulSelectable);
    QCOMPARE(identity.displayName, QStringLiteral("未知(%1)").arg(id));
  }

  void repeatedCallsHaveNoSharedState() {
    const auto first = RM::resolveRobotIdentity(1);
    const auto invalid = RM::resolveRobotIdentity(201);
    const auto blue = RM::resolveRobotIdentity(101);
    const auto again = RM::resolveRobotIdentity(1);
    QVERIFY(!invalid.role);
    QCOMPARE(int(*blue.team), int(TeamColor::BLUE));
    QCOMPARE(int(*first.team), int(TeamColor::RED));
    QCOMPARE(first.displayName, again.displayName);
    QCOMPARE(first.role, again.role);
    QCOMPARE(first.rmulSelectable, again.rmulSelectable);
    QVERIFY(*first.role != RobotType::HERO);
  }
};

QTEST_APPLESS_MAIN(TestRobotIdentity)
#include "test_robot_identity.moc"
