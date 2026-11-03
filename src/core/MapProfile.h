// SPDX-License-Identifier: MIT
#ifndef MAPPROFILE_H
#define MAPPROFILE_H

#include <QJsonObject>
#include <QString>
#include <QVariantMap>

struct MapProfile {
  QString name = QStringLiteral("rmuc_official");
  double widthM = 28.0;
  double heightM = 15.0;
  double originXM = 0.0;
  double originYM = 0.0;
  bool originCalibrated = false;

  QString background;
  bool trainingMode = false;
  bool showGrid = false;
  double gridStepM = 1.0;
  bool showDebugOverlay = false;
  int radarStaleTimeoutMs = 2000;
  bool sizeFromConfig = false;

  bool hasValidSize() const { return widthM > 0.0 && heightM > 0.0; }
  double aspectRatio() const;
  double worldToNormalizedX(double worldXm) const;
  double worldToNormalizedY(double worldYm) const;
  QVariantMap toVariantMap() const;

  static MapProfile officialField();
  static MapProfile fromJson(const QJsonObject &mapSection);
  static MapProfile loadFromConfig();
};

#endif // MAPPROFILE_H
