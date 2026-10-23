// SPDX-License-Identifier: MIT
/**
 * @file test_minimap_path_mapping.cpp
 * @brief 小地图「归一化坐标 → 画布像素」映射的回归测试。
 *
 * 依据《RoboMaster 2026 机甲大师高校系列赛通信协议 V2.0.0》表 1-37（命令码 0x0307）：
 *   - 小地图左下角为坐标原点，X 向右、Y 向上；
 *   - 显示位置按场地尺寸与小地图尺寸**等比缩放**；
 *   - 超出边界的位置在边界处显示。
 *
 * 测试直接调用 MiniMap.qml 暴露的 pathPointToCanvas()，不依赖真实 MQTT 数据。
 */

#include <QQmlComponent>
#include <QQmlEngine>
#include <QScopedPointer>
#include <QVariant>
#include <QtTest>

#include <cmath>

namespace {

// 场地尺寸（dm），与协议一致
constexpr double kFieldWidthDm = 280.0;
constexpr double kFieldHeightDm = 150.0;
constexpr double kEpsilon = 1e-6;

}  // namespace

class TestMinimapPathMapping : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() {
    // 静态库中的 Qt 资源需要显式初始化，qrc:/qml/... 才可解析。
    Q_INIT_RESOURCE(qml);
    Q_INIT_RESOURCE(resources);
  }

  void cleanupTestCase() { Q_CLEANUP_RESOURCE(resources); }

  void init() {
    m_engine.reset(new QQmlEngine());
    m_component.reset(new QQmlComponent(
        m_engine.data(), QUrl(QStringLiteral("qrc:/qml/MiniMap.qml"))));
    QVERIFY2(m_component->isReady(), qPrintable(m_component->errorString()));

    m_root.reset(m_component->create());
    QVERIFY2(!m_root.isNull(), qPrintable(m_component->errorString()));
    m_item = m_root.data();
  }

  void cleanup() {
    m_root.reset();
    m_component.reset();
    m_engine.reset();
    m_item = nullptr;
  }

  /// 设计基准必须与场地等比，否则 1 dm 在 X/Y 上不等价
  void designBaseMatchesFieldAspect() {
    const double width = m_item->property("width").toDouble();
    const double height = m_item->property("height").toDouble();

    QCOMPARE(width, 280.0);
    QCOMPARE(height, 150.0);
    // 交叉相乘验证 width/280 == height/150
    QVERIFY2(std::abs(width / kFieldWidthDm - height / kFieldHeightDm) < kEpsilon,
             qPrintable(QStringLiteral("画布 %1x%2 与场地 280x150 不等比")
                            .arg(width)
                            .arg(height)));
  }

  /// 1 dm 在 X 与 Y 方向映射到相同像素数（协议要求"等比缩放"）
  void oneDecimeterIsEqualOnBothAxes() {
    const QPointF origin = mapPoint(0.0, 0.0);
    const QPointF stepX = mapPoint(1.0 / kFieldWidthDm, 0.0);
    const QPointF stepY = mapPoint(0.0, 1.0 / kFieldHeightDm);

    const double pixelsPerDmX = std::abs(stepX.x() - origin.x());
    const double pixelsPerDmY = std::abs(stepY.y() - origin.y());

    QVERIFY2(std::abs(pixelsPerDmX - pixelsPerDmY) < kEpsilon,
             qPrintable(QStringLiteral("X %1 px/dm，Y %2 px/dm")
                            .arg(pixelsPerDmX)
                            .arg(pixelsPerDmY)));
  }

  /// 左下角为原点，Y 向上
  void yAxisPointsUp() {
    const QPointF bottomLeft = mapPoint(0.0, 0.0);
    const QPointF topRight = mapPoint(1.0, 1.0);

    QCOMPARE(bottomLeft.x(), 0.0);
    QCOMPARE(bottomLeft.y(), 150.0);
    QCOMPARE(topRight.x(), 280.0);
    QCOMPARE(topRight.y(), 0.0);
  }

  /// 超出边界的位置显示在边界处
  void clampsOutOfBoundsPointsToBoundary() {
    // x = 295 dm 超出场地宽度 280 dm
    const QPointF clampedRight = mapPoint(295.0 / kFieldWidthDm, 85.0 / kFieldHeightDm);
    QCOMPARE(clampedRight.x(), 280.0);
    QVERIFY(clampedRight.y() >= 0.0 && clampedRight.y() <= 150.0);

    // 两侧同时越界
    const QPointF clampedBoth = mapPoint(-0.2, 1.4);
    QCOMPARE(clampedBoth.x(), 0.0);
    QCOMPARE(clampedBoth.y(), 0.0);
  }

  /// 实际抓到的那条消息（start=(28,22)，累加后 (168,70)、(295,85)）的像素位置
  void mapsRealMessagePoints() {
    // 280x150 且 1 dm = 1 px，因此 x = dm_x，y = 150 - dm_y
    const QPointF start = mapPoint(28.0 / kFieldWidthDm, 22.0 / kFieldHeightDm);
    QCOMPARE(start.x(), 28.0);
    QCOMPARE(start.y(), 128.0);

    const QPointF second = mapPoint(168.0 / kFieldWidthDm, 70.0 / kFieldHeightDm);
    QCOMPARE(second.x(), 168.0);
    QCOMPARE(second.y(), 80.0);

    // 第三个点越界 → 钳到右边界
    const QPointF last = mapPoint(295.0 / kFieldWidthDm, 85.0 / kFieldHeightDm);
    QCOMPARE(last.x(), 280.0);
    QCOMPARE(last.y(), 65.0);
  }

private:
  QPointF mapPoint(double normalizedX, double normalizedY) {
    QVariant result;
    const bool ok = QMetaObject::invokeMethod(
        m_item, "pathPointToCanvas", Q_RETURN_ARG(QVariant, result),
        Q_ARG(QVariant, QVariant(normalizedX)),
        Q_ARG(QVariant, QVariant(normalizedY)));
    if (!ok) {
      return QPointF(std::nan(""), std::nan(""));
    }
    return result.toPointF();
  }

  QScopedPointer<QQmlEngine> m_engine;
  QScopedPointer<QQmlComponent> m_component;
  QScopedPointer<QObject> m_root;
  QObject *m_item = nullptr;
};

QTEST_MAIN(TestMinimapPathMapping)
#include "test_minimap_path_mapping.moc"
