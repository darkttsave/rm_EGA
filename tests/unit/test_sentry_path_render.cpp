// SPDX-License-Identifier: MIT
/**
 * @file test_sentry_path_render.cpp
 * @brief 哨兵路径坐标转换的回归测试（对应"路径被画成竖线并被上边缘截断"）。
 *
 * 覆盖 GameData::updateSentryPath() 输出的归一化起点与增量，并按
 * src/qml/MiniMap.qml 中 sentryPathCanvas 的同一套公式换算出像素位置：
 *
 *     x_px = addX * width
 *     y_px = (1 - addY) * height
 *
 * 绘制本身（Canvas 实际画线）仍需要人工用同一条真实消息对照选手端确认；
 * 本测试锁住的是进入 QML 之前的数据，以及换算后的像素是否仍在画布内。
 */

#include "core/GameData.h"
#include "robomaster.pb.h"

#include <QList>
#include <QSignalSpy>
#include <QVariantList>
#include <QtTest>

#include <algorithm>
#include <cmath>

namespace {

// 与协议一致的场地尺寸（分米）
constexpr double kFieldWidthDm = 280.0;
constexpr double kFieldHeightDm = 150.0;

// 用于像素换算的画布尺寸；公式是线性的，比例不变即可代表实际渲染。
constexpr double kCanvasWidth = 400.0;
constexpr double kCanvasHeight = 300.0;

struct Point {
  double x;
  double y;
};

robomaster::RobotPathPlanInfo makePath(int startX, int startY,
                                       const QList<int> &offsetsX,
                                       const QList<int> &offsetsY,
                                       int intention = 3) {
  robomaster::RobotPathPlanInfo message;
  message.set_intention(intention);
  message.set_start_pos_x(startX);
  message.set_start_pos_y(startY);
  for (int value : offsetsX) {
    message.add_offset_x(value);
  }
  for (int value : offsetsY) {
    message.add_offset_y(value);
  }
  message.set_sender_id(7);
  return message;
}

/// 按 MiniMap.qml 的方式从起点逐项累加增量（归一化坐标）。
QList<Point> accumulateNormalized(const GameData &gameData) {
  QList<Point> points;
  const QVariantList deltasX = gameData.sentryPathDeltasX();
  const QVariantList deltasY = gameData.sentryPathDeltasY();

  double x = gameData.sentryStartX();
  double y = gameData.sentryStartY();
  points.append({x, y});

  const int count = std::min(deltasX.size(), deltasY.size());
  for (int i = 0; i < count; ++i) {
    x += deltasX.at(i).toDouble();
    y += deltasY.at(i).toDouble();
    points.append({x, y});
  }
  return points;
}

/// 只在绘制时转换为像素，并保留 MiniMap 现有的 Y 轴方向（上为大）。
QList<Point> toPixels(const QList<Point> &normalized) {
  QList<Point> pixels;
  pixels.reserve(normalized.size());
  for (const Point &point : normalized) {
    pixels.append({point.x * kCanvasWidth,
                   (1.0 - point.y) * kCanvasHeight});
  }
  return pixels;
}

bool approximately(double a, double b, double epsilon = 1e-6) {
  return std::abs(a - b) <= epsilon;
}

}  // namespace

class TestSentryPathRender : public QObject {
  Q_OBJECT

private slots:
  /// 起点按地图尺寸换算，且不改变既有数值（111/280、66/150）
  void normalizesStartByFieldSize() {
    GameData gameData;
    gameData.updateSentryPath(makePath(111, 66, {0}, {0}));

    QVERIFY(approximately(gameData.sentryStartX(), 111.0 / kFieldWidthDm));
    QVERIFY(approximately(gameData.sentryStartY(), 66.0 / kFieldHeightDm));
  }

  /// 回归核心：增量为 +1 时必须是一小步，不能变成"归一化坐标 1.0"
  void unitIncrementIsNotFullMapWidth() {
    GameData gameData;
    gameData.updateSentryPath(makePath(111, 66, {1}, {1}));

    const QVariantList deltasX = gameData.sentryPathDeltasX();
    const QVariantList deltasY = gameData.sentryPathDeltasY();
    QCOMPARE(deltasX.size(), 1);
    QCOMPARE(deltasY.size(), 1);

    const double dx = deltasX.at(0).toDouble();
    const double dy = deltasY.at(0).toDouble();
    QVERIFY(approximately(dx, 1.0 / kFieldWidthDm));
    QVERIFY(approximately(dy, 1.0 / kFieldHeightDm));
    // 旧实现会给出 1.0，一步跨越整张地图
    QVERIFY(dx < 0.01);
    QVERIFY(dy < 0.01);
  }

  /// +1 增量在画布上只前进约 1.43 像素，而不是整整一个画布宽度
  void unitIncrementPixelsAreTiny() {
    GameData gameData;
    gameData.updateSentryPath(makePath(111, 66, {1}, {0}));

    const QList<Point> pixels = toPixels(accumulateNormalized(gameData));
    QCOMPARE(pixels.size(), 2);

    const double stepX = pixels.at(1).x - pixels.at(0).x;
    QVERIFY(approximately(stepX, kCanvasWidth / kFieldWidthDm, 1e-3));
    QVERIFY2(stepX < 2.0, "一个 +1 增量不应跨越整张地图");
  }

  /// 负增量必须保留，不能被截断成 0
  void preservesNegativeIncrements() {
    GameData gameData;
    gameData.updateSentryPath(makePath(111, 66, {-2, 1}, {-1, 1}));

    const QVariantList deltasX = gameData.sentryPathDeltasX();
    const QVariantList deltasY = gameData.sentryPathDeltasY();

    QVERIFY(approximately(deltasX.at(0).toDouble(), -2.0 / kFieldWidthDm));
    QVERIFY(approximately(deltasY.at(0).toDouble(), -1.0 / kFieldHeightDm));
    QVERIFY2(deltasX.at(0).toDouble() < 0.0, "负增量不得被截成 0");
    QVERIFY2(deltasY.at(0).toDouble() < 0.0, "负增量不得被截成 0");

    // 累加后必须真的往回走
    const QList<Point> points = accumulateNormalized(gameData);
    QVERIFY(points.at(1).x < points.at(0).x);
    QVERIFY(points.at(1).y < points.at(0).y);
  }

  /// 49 组含 ±1 的增量：累计点与像素都应留在画布内，且路径不是竖线
  void accumulatesMixedIncrementsWithinCanvas() {
    // 构造一条横向为主、Y 小幅上下摆动的路径，复现本次缺陷的数据形状
    QList<int> offsetsX;
    QList<int> offsetsY;
    int expectedSumX = 0;
    int expectedSumY = 0;
    for (int i = 0; i < 49; ++i) {
      const int dx = (i % 3 == 0) ? -1 : ((i % 2 == 0) ? 2 : 1);
      const int dy = (i % 2 == 0) ? 1 : -1;
      offsetsX.append(dx);
      offsetsY.append(dy);
      expectedSumX += dx;
      expectedSumY += dy;
    }

    GameData gameData;
    QSignalSpy spy(&gameData, &GameData::sentryPathUpdated);
    gameData.updateSentryPath(makePath(111, 66, offsetsX, offsetsY));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(gameData.sentryPathDeltasX().size(), 49);
    QCOMPARE(gameData.sentryPathDeltasY().size(), 49);

    // 增量之和应等于 int 之和除以场地尺寸（逐项归一化后再累加）
    const QList<Point> normalized = accumulateNormalized(gameData);
    QCOMPARE(normalized.size(), 50);
    QVERIFY(approximately(normalized.last().x,
                          (111.0 + expectedSumX) / kFieldWidthDm, 1e-5));
    QVERIFY(approximately(normalized.last().y,
                          (66.0 + expectedSumY) / kFieldHeightDm, 1e-5));

    // 归一化坐标应留在 0~1 内
    for (const Point &point : normalized) {
      QVERIFY2(point.x >= 0.0 && point.x <= 1.0,
               qPrintable(QStringLiteral("x=%1 越界").arg(point.x)));
      QVERIFY2(point.y >= 0.0 && point.y <= 1.0,
               qPrintable(QStringLiteral("y=%1 越界").arg(point.y)));
    }

    // 像素坐标应留在画布内（旧实现会跑到负 Y，表现为被上边缘截断）
    const QList<Point> pixels = toPixels(normalized);
    double minX = pixels.first().x;
    double maxX = pixels.first().x;
    for (const Point &point : pixels) {
      QVERIFY2(point.x >= 0.0 && point.x <= kCanvasWidth,
               qPrintable(QStringLiteral("x_px=%1 越界").arg(point.x)));
      QVERIFY2(point.y >= 0.0 && point.y <= kCanvasHeight,
               qPrintable(QStringLiteral("y_px=%1 越界").arg(point.y)));
      minX = std::min(minX, point.x);
      maxX = std::max(maxX, point.x);
    }

    // 路径必须有横向跨度，不能退化成竖线
    QVERIFY2(maxX - minX > 10.0,
             qPrintable(QStringLiteral("横向跨度只有 %1 px").arg(maxX - minX)));
  }

  /// Y 只有 ±1 摆动时，整条路径的纵向变化应保持在一个小范围内
  void smallYWobbleStaysFlat() {
    QList<int> offsetsX;
    QList<int> offsetsY;
    for (int i = 0; i < 49; ++i) {
      offsetsX.append(2);
      offsetsY.append((i % 2 == 0) ? 1 : -1);
    }

    GameData gameData;
    gameData.updateSentryPath(makePath(111, 66, offsetsX, offsetsY));

    const QList<Point> pixels = toPixels(accumulateNormalized(gameData));
    double minY = pixels.first().y;
    double maxY = pixels.first().y;
    for (const Point &point : pixels) {
      minY = std::min(minY, point.y);
      maxY = std::max(maxY, point.y);
    }

    // 66 dm / 150 dm 处约 168 px；±1 dm 摆动只有约 2 px
    QVERIFY2(maxY - minY < 5.0,
             qPrintable(QStringLiteral("纵向摆动 %1 px 过大").arg(maxY - minY)));
    QVERIFY(minY > 0.0);
  }
};

QTEST_MAIN(TestSentryPathRender)
#include "test_sentry_path_render.moc"
