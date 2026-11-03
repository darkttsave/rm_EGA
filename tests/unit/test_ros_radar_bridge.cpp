// SPDX-License-Identifier: MIT
#include "core/GameData.h"
#include "core/MapProfile.h"
#include "network/NetworkManager.h"
#include "robomaster.pb.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

using namespace RM;

namespace {

QJsonObject robot(int color, int id, double worldX, double worldY,
                  bool inBounds = true) {
  return {{QStringLiteral("color"), color},
          {QStringLiteral("id"), id},
          {QStringLiteral("x"), 100.25},
          {QStringLiteral("y"), 200.5},
          {QStringLiteral("world_x"), worldX},
          {QStringLiteral("world_y"), worldY},
          {QStringLiteral("in_bounds"), inBounds}};
}

QByteArray payload(const QJsonArray &robots) {
  const QJsonObject root{
      {QStringLiteral("schema"), QStringLiteral("rm26.ros_radar_positions")},
      {QStringLiteral("version"), 1},
      {QStringLiteral("timestamp"),
       QJsonObject{{QStringLiteral("sec"), 123},
                   {QStringLiteral("nanosec"), 456}}},
      {QStringLiteral("frame_id"), QStringLiteral("rm_frame")},
      {QStringLiteral("image_width"), 1920},
      {QStringLiteral("image_height"), 1080},
      {QStringLiteral("meters_per_pixel"), 0.05},
      {QStringLiteral("robots"), robots}};
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

MapProfile trainingProfile(int timeoutMs = 2000) {
  MapProfile profile;
  profile.name = QStringLiteral("training_field");
  profile.widthM = 12.15;
  profile.heightM = 9.15;
  profile.originXM = -4.99842;
  profile.originYM = -4.44607;
  profile.originCalibrated = true;
  profile.sizeFromConfig = true;
  profile.trainingMode = true;
  profile.radarStaleTimeoutMs = timeoutMs;
  return profile;
}

QVariantMap markerByRadarId(const QVariantList &markers, int radarId) {
  for (const QVariant &value : markers) {
    const QVariantMap marker = value.toMap();
    if (marker.value(QStringLiteral("radarId")).toInt() == radarId) {
      return marker;
    }
  }
  return {};
}

} // namespace

class TestRosRadarBridge : public QObject {
  Q_OBJECT

private slots:
  void mqttPayloadMapsWorldCoordinatesAndIdentity() {
    GameData gameData;
    gameData.setMapProfile(trainingProfile());
    gameData.setPositionSource(QStringLiteral("ros_bridge"));
    NetworkManager network(&gameData);

    QSignalSpy markerSpy(&gameData, &GameData::miniMapMarkersChanged);
    const QJsonArray robots{
        robot(2, 1, -4.99842 + 6.075, -4.44607 + 4.575),
        robot(0, 3, -4.99842 + 3.0375, -4.44607 + 2.2875)};
    QVERIFY(network.processRosRadarBridgePayload(payload(robots)));
    QVERIFY(markerSpy.count() >= 1);

    const QVariantList markers = gameData.getMiniMapMarkers();
    QCOMPARE(markers.size(), 2);
    const QVariantMap red = markerByRadarId(markers, 1);
    QCOMPARE(red.value(QStringLiteral("robotId")).toInt(), 1);
    QCOMPARE(red.value(QStringLiteral("isRed")).toBool(), true);
    QVERIFY(qAbs(red.value(QStringLiteral("posX")).toDouble() - 0.5) < 1e-9);
    QVERIFY(qAbs(red.value(QStringLiteral("posY")).toDouble() - 0.5) < 1e-9);

    const QVariantMap blue = markerByRadarId(markers, 3);
    QCOMPARE(blue.value(QStringLiteral("robotId")).toInt(), 103);
    QCOMPARE(blue.value(QStringLiteral("key")).toInt(), 10);
    QCOMPARE(blue.value(QStringLiteral("radarColor")).toInt(), 0);
    QVERIFY(qAbs(blue.value(QStringLiteral("posX")).toDouble() - 0.25) < 1e-9);
    QVERIFY(qAbs(blue.value(QStringLiteral("posY")).toDouble() - 0.25) < 1e-9);
  }

  void completeSnapshotsHandleIdJumpAndEmptyFrame() {
    GameData gameData;
    gameData.setMapProfile(trainingProfile());
    gameData.setPositionSource(QStringLiteral("ros_bridge"));
    NetworkManager network(&gameData);

    QVERIFY(network.processRosRadarBridgePayload(
        payload(QJsonArray{robot(2, 1, 0.0, 0.0)})));
    QCOMPARE(gameData.getMiniMapMarkers().size(), 1);
    QCOMPARE(gameData.getMiniMapMarkers().first().toMap().value("radarId").toInt(),
             1);

    QVERIFY(network.processRosRadarBridgePayload(
        payload(QJsonArray{robot(2, 4, 1.0, 1.0)})));
    QCOMPARE(gameData.getMiniMapMarkers().size(), 1);
    QCOMPARE(gameData.getMiniMapMarkers().first().toMap().value("radarId").toInt(),
             4);

    QVERIFY(network.processRosRadarBridgePayload(payload(QJsonArray{})));
    QVERIFY(gameData.getMiniMapMarkers().isEmpty());
  }

  void invalidPayloadDoesNotReplaceLastSnapshot() {
    GameData gameData;
    gameData.setMapProfile(trainingProfile());
    gameData.setPositionSource(QStringLiteral("ros_bridge"));
    NetworkManager network(&gameData);

    QVERIFY(network.processRosRadarBridgePayload(
        payload(QJsonArray{robot(2, 5, 0.0, 0.0)})));
    QVERIFY(!network.processRosRadarBridgePayload(QByteArrayLiteral("{bad json")));
    QCOMPARE(gameData.getMiniMapMarkers().size(), 1);
    QCOMPARE(gameData.getMiniMapMarkers().first().toMap().value("radarId").toInt(),
             5);
  }

  void staleSnapshotDisappears() {
    GameData gameData;
    gameData.setMapProfile(trainingProfile(20));
    gameData.setPositionSource(QStringLiteral("ros_bridge"));
    NetworkManager network(&gameData);

    QVERIFY(network.processRosRadarBridgePayload(
        payload(QJsonArray{robot(0, 2, 0.0, 0.0)})));
    QCOMPARE(gameData.getMiniMapMarkers().size(), 1);
    QSignalSpy markerSpy(&gameData, &GameData::miniMapMarkersChanged);
    QTest::qWait(35);
    gameData.update();
    QVERIFY(gameData.getMiniMapMarkers().isEmpty());
    QVERIFY(markerSpy.count() >= 1);
  }

  void officialSourceIsNotOverwrittenByRosSnapshot() {
    GameData gameData;
    gameData.setMapProfile(trainingProfile());
    gameData.setPositionSource(QStringLiteral("official"));
    NetworkManager network(&gameData);

    robomaster::RobotPosition official;
    official.set_robot_id(1);
    official.set_x(2.0f);
    official.set_y(3.0f);
    official.set_yaw(45.0f);
    gameData.updateRobotPosition(official);
    const double officialX =
        gameData.getMiniMapMarkers().first().toMap().value("posX").toDouble();

    QVERIFY(network.processRosRadarBridgePayload(
        payload(QJsonArray{robot(2, 4, 1.0, 1.0)})));
    QCOMPARE(gameData.getMiniMapMarkers().size(), 1);
    QCOMPARE(gameData.getMiniMapMarkers().first().toMap().value("robotId").toInt(),
             1);
    QCOMPARE(gameData.getMiniMapMarkers().first().toMap().value("posX").toDouble(),
             officialX);

    gameData.setPositionSource(QStringLiteral("ros_bridge"));
    QCOMPARE(gameData.getMiniMapMarkers().size(), 1);
    QCOMPARE(gameData.getMiniMapMarkers().first().toMap().value("radarId").toInt(),
             4);
  }
};

QTEST_MAIN(TestRosRadarBridge)
#include "test_ros_radar_bridge.moc"
