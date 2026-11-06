#include "network/Protocol.h"
#include "robomaster.pb.h"
#include <QtTest>

/**
 * @brief Protocol 单元测试类
 *
 * 测试 Protocol 类的序列化和反序列化功能。
 */
class TestProtocol : public QObject {
  Q_OBJECT

private slots:
  void test2027RobotHealthPacket() {
    game_robot_HP_t health{};
    health.ally_1_robot_HP = 101;
    health.reserved = 999;
    health.ally_3_robot_HP = 103;
    health.ally_4_robot_HP = 0;
    health.damage_difference = 888;
    health.ally_7_robot_HP = 107;
    SimpleFrameHeader header{};
    header.cmdId = static_cast<quint16>(CmdID::GAME_ROBOT_HP);
    header.dataLength = sizeof(health);
    QByteArray packet(reinterpret_cast<const char *>(&header), sizeof(header));
    packet.append(reinterpret_cast<const char *>(&health), sizeof(health));
    robomaster::RoboMasterMessage msg;
    QVERIFY(Protocol::parsePacket(packet, msg));
    const auto &hp = msg.global_unit_status().robot_health();
    QCOMPARE(hp.size(), 4);
    QCOMPARE(hp.Get(0), 101);
    QCOMPARE(hp.Get(1), 103);
    QCOMPARE(hp.Get(2), 0);
    QCOMPARE(hp.Get(3), 107);
  }
  /**
   * @brief 测试序列化和反序列化
   */
  void testSerializeAndParse() {
    // 1. 创建一个 RoboMasterMessage 对象
    robomaster::RoboMasterMessage originalMsg;
    robomaster::RobotStatus *status = originalMsg.mutable_robot_status();
    status->set_id(1);
    status->set_hp(100);
    status->set_max_hp(200);

    // 2. 序列化
    QByteArray data = Protocol::serializePacket(originalMsg);
    QVERIFY(!data.isEmpty());

    // 3. 反序列化
    robomaster::RoboMasterMessage parsedMsg;
    bool success = Protocol::parsePacket(data, parsedMsg);

    // 4. 验证
    QVERIFY(success);
    QVERIFY(parsedMsg.has_robot_status());
    QCOMPARE(parsedMsg.robot_status().id(), 1);
    QCOMPARE(parsedMsg.robot_status().hp(), 100);
    QCOMPARE(parsedMsg.robot_status().max_hp(), 200);
  }

  /**
   * @brief 测试无效数据解析
   */
  void testParseInvalidData() {
    QByteArray invalidData = "Invalid Data";
    robomaster::RoboMasterMessage parsedMsg;
    bool success = Protocol::parsePacket(invalidData, parsedMsg);
    QVERIFY(!success);
  }

  /**
   * @brief 测试空数据解析
   */
  void testParseEmptyData() {
    QByteArray emptyData;
    robomaster::RoboMasterMessage parsedMsg;
    bool success = Protocol::parsePacket(emptyData, parsedMsg);
    QVERIFY(!success); // 应该失败，因为 Protobuf 需要有效数据
  }
};

QTEST_MAIN(TestProtocol)
#include "test_protocol.moc"
