// SPDX-License-Identifier: MIT
/**
 * @file test_sentry_bt_status_state.cpp
 * @brief 哨兵 0x0310 临时诊断状态（GameData::sentryStatusPreview）的状态层测试。
 */

#include "core/GameData.h"
#include "network/SentryBtStatusParser.h"

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>
#include <QtTest>

#include <cstdint>

class TestSentryBtStatusState : public QObject {
  Q_OBJECT

private slots:
  /// 未收到合法消息时 received 必须为 false
  void previewStartsAsNotReceived() {
    GameData gameData;
    const QVariantMap preview = gameData.getSentryStatusPreview();

    QVERIFY(!preview.value(QStringLiteral("received"), false).toBool());
    QVERIFY(preview.value(QStringLiteral("fields"), QVariantList()).toList().isEmpty());
  }

  /// 1. 合法消息能更新 GameData
  void validMessageUpdatesPreview() {
    GameData gameData;

    RM::SentryBtStatus status;
    status.currentStance = 0x01;
    status.btRemainTime = 0x1234;
    status.btGameStatus = 0x09;
    gameData.updateSentryBtStatusPreview(status, QStringLiteral("01 34 12 09"), 1234567);

    const QVariantMap preview = gameData.getSentryStatusPreview();
    QVERIFY(preview.value(QStringLiteral("received")).toBool());
    QCOMPARE(preview.value(QStringLiteral("current_stance")).toInt(), 0x01);
    QCOMPARE(preview.value(QStringLiteral("bt_remain_time")).toInt(), 0x1234);
    QCOMPARE(preview.value(QStringLiteral("bt_game_status")).toInt(), 0x09);
    QCOMPARE(preview.value(QStringLiteral("rawHex")).toString(),
             QStringLiteral("01 34 12 09"));
    QCOMPARE(preview.value(QStringLiteral("receivedAtMs")).toLongLong(),
             static_cast<qint64>(1234567));
  }

  /// 2. notify signal 正确发出；3. received 从 false 变为 true
  void notifySignalIsEmittedAndReceivedFlipsToTrue() {
    GameData gameData;
    QSignalSpy spy(&gameData, &GameData::sentryStatusPreviewChanged);

    QVERIFY(!gameData.getSentryStatusPreview()
                 .value(QStringLiteral("received"), false)
                 .toBool());

    RM::SentryBtStatus status;
    status.currentStance = 2;
    gameData.updateSentryBtStatusPreview(status, QStringLiteral("02 00 00 00"), 0);

    QCOMPARE(spy.count(), 1);
    QVERIFY(gameData.getSentryStatusPreview().value(QStringLiteral("received")).toBool());
  }

  /// 4. 原始字段完整传递（含边界值），且字段名与顺序与 JSON 一致
  void rawFieldsArePassedThroughCompletely() {
    GameData gameData;

    RM::SentryBtStatus status;
    status.currentStance = 0xFF;
    status.btRemainTime = 0xFFFF;
    status.btGameStatus = 0xFF;
    gameData.updateSentryBtStatusPreview(status, QStringLiteral("ff ff ff ff"), 42);

    const QVariantMap preview = gameData.getSentryStatusPreview();
    const QVariantList fields = preview.value(QStringLiteral("fields")).toList();

    const QList<RM::SentryBtStatusField> expected =
        RM::sentryBtStatusFieldList(status);
    QCOMPARE(fields.size(), expected.size());

    for (int i = 0; i < expected.size(); ++i) {
      const QVariantMap item = fields.at(i).toMap();
      QCOMPARE(item.value(QStringLiteral("name")).toString(), expected.at(i).name);
      QCOMPARE(item.value(QStringLiteral("value")).toLongLong(), expected.at(i).value);
      // 顶层同样平铺一份，便于直接读取
      QCOMPARE(preview.value(expected.at(i).name).toLongLong(), expected.at(i).value);
    }
  }

  /// 连续更新时每次都发出 notify，且保存的是最新值
  void successiveUpdatesKeepLatestValues() {
    GameData gameData;
    QSignalSpy spy(&gameData, &GameData::sentryStatusPreviewChanged);

    RM::SentryBtStatus first;
    first.currentStance = 1;
    first.btRemainTime = 100;
    gameData.updateSentryBtStatusPreview(first, QStringLiteral("01 64 00 00"), 10);

    RM::SentryBtStatus second;
    second.currentStance = 3;
    second.btRemainTime = 200;
    gameData.updateSentryBtStatusPreview(second, QStringLiteral("03 c8 00 00"), 20);

    QCOMPARE(spy.count(), 2);
    const QVariantMap preview = gameData.getSentryStatusPreview();
    QCOMPARE(preview.value(QStringLiteral("current_stance")).toInt(), 3);
    QCOMPARE(preview.value(QStringLiteral("bt_remain_time")).toInt(), 200);
    QCOMPARE(preview.value(QStringLiteral("receivedAtMs")).toLongLong(),
             static_cast<qint64>(20));
  }

  /// 临时状态不得与正式比赛状态字段混在一起
  void previewDoesNotMixWithOfficialState() {
    GameData gameData;

    RM::SentryBtStatus status;
    gameData.updateSentryBtStatusPreview(status, QStringLiteral("00 00 00 00"), 0);

    const QVariantMap preview = gameData.getSentryStatusPreview();
    const QStringList officialRobotKeys{
        QStringLiteral("fricEnabled"), QStringLiteral("rammerEnabled"),
        QStringLiteral("chassisMode"), QStringLiteral("spinMode"),
        QStringLiteral("followMode"), QStringLiteral("chassisProtect"),
        QStringLiteral("superCapEnergy"), QStringLiteral("gimbalChassisAngle"),
        QStringLiteral("targetDistance"), QStringLiteral("ballisticCompensation")};
    for (const QString &key : officialRobotKeys) {
      QVERIFY2(!preview.contains(key), qPrintable(key));
    }

    // 反向：正式自定义数据里也不应出现哨兵字段
    const QVariantMap official = gameData.getMyRobotCustomData();
    QVERIFY(!official.contains(QStringLiteral("current_stance")));
    QVERIFY(!official.contains(QStringLiteral("bt_remain_time")));
    QVERIFY(!official.contains(QStringLiteral("bt_game_status")));
  }

  /// 属性可通过元对象系统按 QML 方式读取
  void propertyIsReadableThroughMetaObject() {
    GameData gameData;

    RM::SentryBtStatus status;
    status.currentStance = 7;
    gameData.updateSentryBtStatusPreview(status, QStringLiteral("07 00 00 00"), 5);

    // QML 通过同名 Q_PROPERTY 绑定读取
    const QVariant value = gameData.property("sentryStatusPreview");
    QVERIFY(value.isValid());
    QCOMPARE(value.toMap().value(QStringLiteral("current_stance")).toInt(), 7);

    // 属性声明必须带 NOTIFY，QML 绑定才能自动刷新
    const QMetaObject *meta = gameData.metaObject();
    const int index = meta->indexOfProperty("sentryStatusPreview");
    QVERIFY(index >= 0);
    QVERIFY(meta->property(index).hasNotifySignal());
    QCOMPARE(QString::fromLatin1(meta->property(index).notifySignal().name()),
             QStringLiteral("sentryStatusPreviewChanged"));
  }
};

QTEST_MAIN(TestSentryBtStatusState)
#include "test_sentry_bt_status_state.moc"
