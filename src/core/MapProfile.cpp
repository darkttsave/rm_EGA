// SPDX-License-Identifier: MIT
#include "MapProfile.h"

#include "../config/ConfigManager.h"

#include <QDebug>
#include <QJsonValue>

#include <cmath>

namespace {

bool readOptionalReal(const QJsonValue &value, double &out) {
  if (!value.isDouble()) {
    return false;
  }
  const double parsed = value.toDouble();
  if (!std::isfinite(parsed)) {
    return false;
  }
  out = parsed;
  return true;
}

bool readOptionalPositive(const QJsonValue &value, double &out) {
  double parsed = 0.0;
  if (!readOptionalReal(value, parsed) || parsed <= 0.0) {
    return false;
  }
  out = parsed;
  return true;
}

} // namespace

double MapProfile::aspectRatio() const {
  return hasValidSize() ? heightM / widthM : 15.0 / 28.0;
}

double MapProfile::worldToNormalizedX(double worldXm) const {
  return hasValidSize() ? (worldXm - originXM) / widthM : 0.0;
}

double MapProfile::worldToNormalizedY(double worldYm) const {
  return hasValidSize() ? (worldYm - originYM) / heightM : 0.0;
}

QVariantMap MapProfile::toVariantMap() const {
  return {{QStringLiteral("name"), name},
          {QStringLiteral("widthM"), widthM},
          {QStringLiteral("heightM"), heightM},
          {QStringLiteral("originXM"), originXM},
          {QStringLiteral("originYM"), originYM},
          {QStringLiteral("originCalibrated"), originCalibrated},
          {QStringLiteral("background"), background},
          {QStringLiteral("trainingMode"), trainingMode},
          {QStringLiteral("showGrid"), showGrid},
          {QStringLiteral("gridStepM"), gridStepM},
          {QStringLiteral("showDebugOverlay"), showDebugOverlay},
          {QStringLiteral("radarStaleTimeoutMs"), radarStaleTimeoutMs},
          {QStringLiteral("sizeFromConfig"), sizeFromConfig},
          {QStringLiteral("aspectRatio"), aspectRatio()}};
}

MapProfile MapProfile::officialField() { return MapProfile{}; }

MapProfile MapProfile::fromJson(const QJsonObject &mapSection) {
  MapProfile profile = officialField();
  if (mapSection.isEmpty()) {
    return profile;
  }

  profile.name = mapSection.value(QStringLiteral("name")).toString(profile.name);
  const QString mode =
      mapSection.value(QStringLiteral("mode")).toString().trimmed().toLower();
  if (mode == QStringLiteral("training")) {
    profile.trainingMode = true;
  } else if (!mode.isEmpty() && mode != QStringLiteral("competition")) {
    qWarning() << "[MapProfile] 未知 mode:" << mode << "，按 competition 处理";
  }

  double widthM = profile.widthM;
  double heightM = profile.heightM;
  const bool hasWidth =
      readOptionalPositive(mapSection.value(QStringLiteral("width_m")), widthM);
  const bool hasHeight = readOptionalPositive(
      mapSection.value(QStringLiteral("height_m")), heightM);
  if (hasWidth && hasHeight) {
    profile.widthM = widthM;
    profile.heightM = heightM;
    profile.sizeFromConfig = true;
  } else if (hasWidth || hasHeight) {
    qWarning() << "[MapProfile] width_m/height_m 必须同时给出";
  }

  double originXM = 0.0;
  double originYM = 0.0;
  const bool hasOriginX =
      readOptionalReal(mapSection.value(QStringLiteral("origin_x_m")), originXM);
  const bool hasOriginY =
      readOptionalReal(mapSection.value(QStringLiteral("origin_y_m")), originYM);
  if (hasOriginX && hasOriginY) {
    profile.originXM = originXM;
    profile.originYM = originYM;
    profile.originCalibrated = true;
  } else if (hasOriginX || hasOriginY) {
    qWarning() << "[MapProfile] origin_x_m/origin_y_m 必须同时给出";
  }

  if (mapSection.value(QStringLiteral("background")).isString()) {
    profile.background =
        mapSection.value(QStringLiteral("background")).toString();
  }
  profile.showGrid = mapSection.contains(QStringLiteral("show_grid"))
                         ? mapSection.value(QStringLiteral("show_grid")).toBool()
                         : profile.trainingMode;
  double gridStepM = profile.gridStepM;
  if (readOptionalPositive(mapSection.value(QStringLiteral("grid_step_m")),
                           gridStepM)) {
    profile.gridStepM = gridStepM;
  }
  const int timeout =
      mapSection.value(QStringLiteral("radar_stale_timeout_ms")).toInt(2000);
  if (timeout > 0) {
    profile.radarStaleTimeoutMs = timeout;
  }
  profile.showDebugOverlay =
      mapSection.value(QStringLiteral("show_debug_overlay")).toBool(false);
  return profile;
}

MapProfile MapProfile::loadFromConfig() {
  return fromJson(ConfigManager::instance().getMapConfig());
}
