// SPDX-License-Identifier: MIT
#ifndef ROSRADARBRIDGEPROTOCOL_H
#define ROSRADARBRIDGEPROTOCOL_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

struct RosRadarRobotPosition {
  int color = 0;
  int radarId = 0;
  double pixelX = 0.0;
  double pixelY = 0.0;
  double worldX = 0.0;
  double worldY = 0.0;
  bool inBounds = false;
};

struct RosRadarSnapshot {
  qint64 stampSec = 0;
  qint64 stampNanosec = 0;
  QString frameId;
  quint32 imageWidth = 0;
  quint32 imageHeight = 0;
  double metersPerPixel = 0.0;
  QList<RosRadarRobotPosition> robots;
};

class RosRadarBridgeProtocol {
public:
  static constexpr int Version = 1;
  static constexpr const char *Schema = "rm26.ros_radar_positions";

  static bool parse(const QByteArray &payload, RosRadarSnapshot &snapshot,
                    QString *error = nullptr);
};

#endif // ROSRADARBRIDGEPROTOCOL_H
