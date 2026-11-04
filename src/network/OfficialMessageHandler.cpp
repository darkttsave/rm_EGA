// SPDX-License-Identifier: MIT
#include "OfficialMessageHandler.h"
#include "../core/GameData.h"
#include "robomaster.pb.h"

namespace RM {
OfficialMessageHandler::Result OfficialMessageHandler::handle(
    const QString &topic, const QByteArray &payload, GameData &gameData,
    const ParsedLogger &logger) {
  if (topic == QLatin1String("GlobalSpecialMechanism")) {
    robomaster::GlobalSpecialMechanism msg;
    if (!msg.ParseFromArray(payload.data(), payload.size()))
      return Result::ParseFailed;
    gameData.updateGlobalSpecialMechanism(msg);
    return Result::Parsed;
  }
  if (topic == QLatin1String("Buff")) {
    robomaster::Buff msg;
    if (!msg.ParseFromArray(payload.data(), payload.size()))
      return Result::ParseFailed;
    if (logger)
      logger(QStringLiteral("Buff robot_id=%1 buff_type=%2 buff_level=%3")
                 .arg(msg.robot_id()).arg(msg.buff_type()).arg(msg.buff_level()),
             {{QStringLiteral("robot_id"), static_cast<int>(msg.robot_id())},
              {QStringLiteral("buff_type"), static_cast<int>(msg.buff_type())},
              {QStringLiteral("buff_level"), static_cast<int>(msg.buff_level())}});
    gameData.updateBuff(msg);
    return Result::Parsed;
  }
  if (topic == QLatin1String("DeployModeStatusSync")) {
    robomaster::DeployModeStatusSync msg;
    if (!msg.ParseFromArray(payload.data(), payload.size()))
      return Result::ParseFailed;
    if (logger)
      logger(QStringLiteral("DeployModeStatusSync status=%1").arg(msg.status()),
             {{QStringLiteral("status"), static_cast<int>(msg.status())}});
    gameData.updateDeployModeStatusSync(msg);
    return Result::Parsed;
  }
  return Result::Unsupported;
}
} // namespace RM
