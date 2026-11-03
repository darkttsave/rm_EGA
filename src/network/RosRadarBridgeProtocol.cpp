// SPDX-License-Identifier: MIT
#include "RosRadarBridgeProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>

#include <cmath>

namespace {

bool fail(QString *error, const QString &message) {
  if (error) {
    *error = message;
  }
  return false;
}

bool readInteger(const QJsonObject &object, const QString &name, qint64 minimum,
                 qint64 maximum, qint64 &result, QString *error) {
  const QJsonValue value = object.value(name);
  if (!value.isDouble()) {
    return fail(error, QStringLiteral("%1 必须是整数").arg(name));
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::floor(number) != number ||
      number < static_cast<double>(minimum) ||
      number > static_cast<double>(maximum)) {
    return fail(error, QStringLiteral("%1 超出有效范围").arg(name));
  }
  result = static_cast<qint64>(number);
  return true;
}

bool readFinite(const QJsonObject &object, const QString &name, double &result,
                QString *error) {
  const QJsonValue value = object.value(name);
  if (!value.isDouble() || !std::isfinite(value.toDouble())) {
    return fail(error, QStringLiteral("%1 必须是有限数").arg(name));
  }
  result = value.toDouble();
  return true;
}

} // namespace

bool RosRadarBridgeProtocol::parse(const QByteArray &payload,
                                   RosRadarSnapshot &snapshot,
                                   QString *error) {
  QJsonParseError jsonError;
  const QJsonDocument document = QJsonDocument::fromJson(payload, &jsonError);
  if (jsonError.error != QJsonParseError::NoError || !document.isObject()) {
    return fail(error, QStringLiteral("JSON 解析失败：%1").arg(jsonError.errorString()));
  }

  const QJsonObject root = document.object();
  if (root.value(QStringLiteral("schema")).toString() !=
      QString::fromLatin1(Schema)) {
    return fail(error, QStringLiteral("schema 不匹配"));
  }
  qint64 version = 0;
  if (!readInteger(root, QStringLiteral("version"), Version, Version, version,
                   error)) {
    return false;
  }

  const QJsonValue timestampValue = root.value(QStringLiteral("timestamp"));
  if (!timestampValue.isObject()) {
    return fail(error, QStringLiteral("timestamp 必须是对象"));
  }

  RosRadarSnapshot parsed;
  const QJsonObject timestamp = timestampValue.toObject();
  if (!readInteger(timestamp, QStringLiteral("sec"), 0,
                   9007199254740991LL, parsed.stampSec, error) ||
      !readInteger(timestamp, QStringLiteral("nanosec"), 0, 999999999,
                   parsed.stampNanosec, error)) {
    return false;
  }

  const QJsonValue frameId = root.value(QStringLiteral("frame_id"));
  if (!frameId.isString()) {
    return fail(error, QStringLiteral("frame_id 必须是字符串"));
  }
  parsed.frameId = frameId.toString();

  qint64 width = 0;
  qint64 height = 0;
  if (!readInteger(root, QStringLiteral("image_width"), 1, 0xffffffffLL,
                   width, error) ||
      !readInteger(root, QStringLiteral("image_height"), 1, 0xffffffffLL,
                   height, error) ||
      !readFinite(root, QStringLiteral("meters_per_pixel"),
                  parsed.metersPerPixel, error)) {
    return false;
  }
  if (parsed.metersPerPixel <= 0.0) {
    return fail(error, QStringLiteral("meters_per_pixel 必须大于 0"));
  }
  parsed.imageWidth = static_cast<quint32>(width);
  parsed.imageHeight = static_cast<quint32>(height);

  const QJsonValue robotsValue = root.value(QStringLiteral("robots"));
  if (!robotsValue.isArray()) {
    return fail(error, QStringLiteral("robots 必须是数组"));
  }

  QSet<QString> identities;
  const QJsonArray robots = robotsValue.toArray();
  parsed.robots.reserve(robots.size());
  for (int index = 0; index < robots.size(); ++index) {
    if (!robots.at(index).isObject()) {
      return fail(error, QStringLiteral("robots[%1] 必须是对象").arg(index));
    }
    const QJsonObject object = robots.at(index).toObject();
    qint64 color = 0;
    qint64 radarId = 0;
    RosRadarRobotPosition robot;
    if (!readInteger(object, QStringLiteral("color"), 0, 2, color, error) ||
        (color != 0 && color != 2)) {
      return fail(error,
                  QStringLiteral("robots[%1].color 必须是 0 或 2").arg(index));
    }
    if (!readInteger(object, QStringLiteral("id"), 0, 255, radarId, error) ||
        !readFinite(object, QStringLiteral("x"), robot.pixelX, error) ||
        !readFinite(object, QStringLiteral("y"), robot.pixelY, error) ||
        !readFinite(object, QStringLiteral("world_x"), robot.worldX, error) ||
        !readFinite(object, QStringLiteral("world_y"), robot.worldY, error)) {
      return false;
    }
    const QJsonValue inBounds = object.value(QStringLiteral("in_bounds"));
    if (!inBounds.isBool()) {
      return fail(error,
                  QStringLiteral("robots[%1].in_bounds 必须是布尔值").arg(index));
    }

    robot.color = static_cast<int>(color);
    robot.radarId = static_cast<int>(radarId);
    robot.inBounds = inBounds.toBool();
    const QString identity =
        QStringLiteral("%1:%2").arg(robot.color).arg(robot.radarId);
    if (identities.contains(identity)) {
      return fail(error,
                  QStringLiteral("robots 中存在重复身份 %1").arg(identity));
    }
    identities.insert(identity);
    parsed.robots.append(robot);
  }

  snapshot = parsed;
  if (error) {
    error->clear();
  }
  return true;
}
