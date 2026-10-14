// SPDX-License-Identifier: MIT
/**
 * @file test_sentry_bt_status_network_entry.cpp
 * @brief MQTT CustomByteBlock → NetworkManager → 解析器 → GameData 的入口测试。
 * @details 通过 QMetaObject::invokeMethod 直接驱动私有的 processMqttMessage 槽，
 *          使测试覆盖与生产环境完全相同的处理路径，且不需要真实 broker。
 */

// CustomDataTypes.h 使用了 memset 但自身未包含 <cstring>（既有状况，本轮不改业务代码），
// 因此在本测试中先显式引入标准头。
#include <cstring>

#include "core/CustomDataTypes.h"
#include "core/GameData.h"
#include "network/NetworkManager.h"
#include "robomaster.pb.h"

#include <QByteArray>
#include <QSignalSpy>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtTest>

#include <cstdint>

namespace {

const QString kTopic = QStringLiteral("CustomByteBlock");

/// 把业务字节封装成 CustomByteBlock Protobuf（不手工拼 wire format）。
QByteArray wrapCustomByteBlock(const QByteArray &data) {
  robomaster::CustomByteBlock message;
  message.set_data(data.constData(), static_cast<size_t>(data.size()));

  QByteArray payload(static_cast<int>(message.ByteSizeLong()), '\0');
  message.SerializeToArray(payload.data(), payload.size());
  return payload;
}

/// 构造一份 28 字节的既有 0x0301 机器人自定义状态（非哨兵路径）。
QByteArray robotCustomStatusPayload() {
  RobotCustomStatus status;
  status.friction_status.bits.fric_enabled = 1;
  status.friction_status.bits.rammer_enabled = 1;
  status.chassis_status.bits.chassis_mode = 2;
  status.super_cap_energy = 500;
  status.super_cap_max = 1000;
  status.gimbal_chassis_angle = 450;  // 45.0 度
  status.target_distance_dm = 30;
  status.ballistic_compensation = -5;

  return QByteArray(reinterpret_cast<const char *>(&status),
                    static_cast<int>(sizeof(status)));
}

bool feed(RM::NetworkManager &manager, const QByteArray &payload) {
  return QMetaObject::invokeMethod(
      &manager, "processMqttMessage", Qt::DirectConnection,
      Q_ARG(QString, kTopic), Q_ARG(QByteArray, payload));
}

}  // namespace

class TestSentryBtStatusNetworkEntry : public QObject {
  Q_OBJECT

private slots:
  /// 1. 合法 CustomByteBlock Protobuf 能到达解析器并写入 GameData
  void validCustomByteBlockReachesParserAndGameData() {
#ifndef RM_HAS_MQTT
    QSKIP("Paho MQTT support is not enabled in this build");
#else
    GameData gameData;
    RM::NetworkManager manager(&gameData);
    QSignalSpy previewSpy(&gameData, &GameData::sentryStatusPreviewChanged);

    QVERIFY(feed(manager, wrapCustomByteBlock(QByteArray::fromHex("07341209"))));

    QCOMPARE(previewSpy.count(), 1);
    const QVariantMap preview = gameData.getSentryStatusPreview();
    QVERIFY(preview.value(QStringLiteral("received")).toBool());
    QCOMPARE(preview.value(QStringLiteral("current_stance")).toInt(), 0x07);
    QCOMPARE(preview.value(QStringLiteral("bt_remain_time")).toInt(), 0x1234);
    QCOMPARE(preview.value(QStringLiteral("bt_game_status")).toInt(), 0x09);
    QCOMPARE(preview.value(QStringLiteral("rawHex")).toString(),
             QStringLiteral("07 34 12 09"));
#endif
  }

  /// 2. 非 4 字节 CustomByteBlock.data 不产生哨兵状态
  void nonTargetLengthDoesNotProduceSentryStatus() {
#ifndef RM_HAS_MQTT
    QSKIP("Paho MQTT support is not enabled in this build");
#else
    // 共享 Topic 上还承载 H.264 视频等其它数据
    for (int length : {0, 1, 3, 5, 7, 28, 100, 300, 303}) {
      GameData gameData;
      RM::NetworkManager manager(&gameData);
      QSignalSpy previewSpy(&gameData, &GameData::sentryStatusPreviewChanged);

      const QByteArray data(length, '\x11');
      QVERIFY2(feed(manager, wrapCustomByteBlock(data)),
               qPrintable(QStringLiteral("length %1").arg(length)));

      QCOMPARE(previewSpy.count(), 0);
      QVERIFY2(!gameData.getSentryStatusPreview()
                    .value(QStringLiteral("received"))
                    .toBool(),
               qPrintable(QStringLiteral("length %1 不得产生哨兵状态").arg(length)));
    }
#endif
  }

  /// 非法消息不覆盖上一条合法状态
  void nonTargetLengthDoesNotOverwriteLastValidState() {
#ifndef RM_HAS_MQTT
    QSKIP("Paho MQTT support is not enabled in this build");
#else
    GameData gameData;
    RM::NetworkManager manager(&gameData);

    QVERIFY(feed(manager, wrapCustomByteBlock(QByteArray::fromHex("01341209"))));
    QCOMPARE(gameData.getSentryStatusPreview()
                 .value(QStringLiteral("current_stance"))
                 .toInt(),
             0x01);

    // 之后到来的非目标长度与非法 Protobuf 都不得改写上一条合法状态
    QVERIFY(feed(manager, wrapCustomByteBlock(QByteArray(7, '\x22'))));
    QVERIFY(feed(manager, QByteArray("\xff\xff\xff", 3)));

    const QVariantMap preview = gameData.getSentryStatusPreview();
    QVERIFY(preview.value(QStringLiteral("received")).toBool());
    QCOMPARE(preview.value(QStringLiteral("current_stance")).toInt(), 0x01);
    QCOMPARE(preview.value(QStringLiteral("bt_remain_time")).toInt(), 0x1234);
    QCOMPARE(preview.value(QStringLiteral("bt_game_status")).toInt(), 0x09);
#endif
  }

  /// 3. 原有非哨兵（28 字节 0x0301）CustomByteBlock 处理路径未被移除
  void existingRobotCustomStatusPathIsPreserved() {
#ifndef RM_HAS_MQTT
    QSKIP("Paho MQTT support is not enabled in this build");
#else
    GameData gameData;
    RM::NetworkManager manager(&gameData);
    QSignalSpy robotDataSpy(&gameData, &GameData::robotCustomDataUpdated);
    QSignalSpy previewSpy(&gameData, &GameData::sentryStatusPreviewChanged);

    quint8 robotId = 0;
    connect(&gameData, &GameData::robotCustomDataUpdated, this,
            [&robotId](quint8 id) { robotId = id; });

    QVERIFY(feed(manager, wrapCustomByteBlock(robotCustomStatusPayload())));

    // 既有路径仍然生效
    QCOMPARE(robotDataSpy.count(), 1);
    QVERIFY(robotId != 0);

    const QVariantMap custom = gameData.getMyRobotCustomData();
    QVERIFY(custom.value(QStringLiteral("fricEnabled")).toBool());
    QVERIFY(custom.value(QStringLiteral("rammerEnabled")).toBool());
    QCOMPARE(custom.value(QStringLiteral("chassisMode")).toInt(), 2);
    QCOMPARE(custom.value(QStringLiteral("superCapEnergy")).toDouble(), 50.0);
    QCOMPARE(custom.value(QStringLiteral("gimbalChassisAngle")).toDouble(), 45.0);
    QCOMPARE(custom.value(QStringLiteral("targetDistance")).toInt(), 30);
    QCOMPARE(custom.value(QStringLiteral("ballisticCompensation")).toInt(), -5);

    // 28 字节不是哨兵协议长度，不得产生哨兵状态
    QCOMPARE(previewSpy.count(), 0);
    QVERIFY(!gameData.getSentryStatusPreview()
                 .value(QStringLiteral("received"))
                 .toBool());
#endif
  }

  /// 非法 Protobuf 不得导致崩溃
  void invalidProtobufDoesNotCrash() {
#ifndef RM_HAS_MQTT
    QSKIP("Paho MQTT support is not enabled in this build");
#else
    GameData gameData;
    RM::NetworkManager manager(&gameData);
    QSignalSpy previewSpy(&gameData, &GameData::sentryStatusPreviewChanged);

    QVERIFY(feed(manager, QByteArray("\xff\xff\xff", 3)));
    QVERIFY(feed(manager, QByteArray("\x0a\x04\x01", 3)));
    QVERIFY(feed(manager, QByteArray()));

    QCOMPARE(previewSpy.count(), 0);
    QVERIFY(!gameData.getSentryStatusPreview()
                 .value(QStringLiteral("received"))
                 .toBool());
#endif
  }

  /// 哨兵 4 字节数据仍然照常进入既有转发路径（本轮只旁路观察，不截断、不重定向）
  void sentryBytesAreStillForwardedToExistingPath() {
#ifndef RM_HAS_MQTT
    QSKIP("Paho MQTT support is not enabled in this build");
#else
    GameData gameData;
    RM::NetworkManager manager(&gameData);
    QSignalSpy videoSpy(&manager, &RM::NetworkManager::customVideoPayloadReceived);

    const QByteArray data = QByteArray::fromHex("01341209");
    QVERIFY(feed(manager, wrapCustomByteBlock(data)));

    QCOMPARE(videoSpy.count(), 1);
    QCOMPARE(videoSpy.at(0).at(0).toByteArray(), data);
#endif
  }

  /// 原点到 GameData 的字段清单顺序与 JSON 描述一致
  void fieldsOrderMatchesSpec() {
#ifndef RM_HAS_MQTT
    QSKIP("Paho MQTT support is not enabled in this build");
#else
    GameData gameData;
    RM::NetworkManager manager(&gameData);

    QVERIFY(feed(manager, wrapCustomByteBlock(QByteArray::fromHex("01020304"))));

    const QVariantList fields = gameData.getSentryStatusPreview()
                                    .value(QStringLiteral("fields"))
                                    .toList();
    QCOMPARE(fields.size(), 3);
    QCOMPARE(fields.at(0).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("current_stance"));
    QCOMPARE(fields.at(1).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("bt_remain_time"));
    QCOMPARE(fields.at(2).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("bt_game_status"));
    QCOMPARE(fields.at(1).toMap().value(QStringLiteral("value")).toInt(), 0x0302);
#endif
  }
};

QTEST_MAIN(TestSentryBtStatusNetworkEntry)
#include "test_sentry_bt_status_network_entry.moc"
