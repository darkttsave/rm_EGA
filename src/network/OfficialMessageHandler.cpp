// SPDX-License-Identifier: MIT
#include "OfficialMessageHandler.h"
#include "../core/GameData.h"
#include "robomaster.pb.h"
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>

namespace RM {
namespace {
template <typename RepeatedFieldT>
QJsonArray repeatedFieldToJsonArray(const RepeatedFieldT &field) {
  QJsonArray array;
  for (const auto value : field) array.append(static_cast<int>(value));
  return array;
}
} // namespace
OfficialMessageHandler::Result OfficialMessageHandler::handle(
    const QString &topic, const QByteArray &payload, GameData &gameData,
    const ParsedLogger &logger, const GameStatusCacheUpdater &cacheUpdater) {
  if (topic == QLatin1String("PenaltyInfo")) {
    robomaster::PenaltyInfo msg;
    if (!msg.ParseFromArray(payload.data(), payload.size())) return Result::ParseFailed;
    if (logger) logger(QStringLiteral("PenaltyInfo type=%1 effect_sec=%2 total=%3")
        .arg(msg.penalty_type()).arg(msg.penalty_effect_sec()).arg(msg.total_penalty_num()),
        {{QStringLiteral("penalty_type"), static_cast<qint64>(msg.penalty_type())},
         {QStringLiteral("penalty_effect_sec"), static_cast<qint64>(msg.penalty_effect_sec())},
         {QStringLiteral("total_penalty_num"), static_cast<qint64>(msg.total_penalty_num())}});
    gameData.updatePenalty(msg);
    return Result::Parsed;
  }
  if (topic == QLatin1String("RobotInjuryStat")) {
    robomaster::RobotInjuryStat msg;
    if (!msg.ParseFromArray(payload.data(), payload.size())) return Result::ParseFailed;
    if (logger) logger(QStringLiteral("RobotInjuryStat total=%1 killer_id=%2")
        .arg(msg.total_damage()).arg(msg.killer_id()),
        {{QStringLiteral("total_damage"), static_cast<qint64>(msg.total_damage())},
         {QStringLiteral("killer_id"), static_cast<qint64>(msg.killer_id())}});
    gameData.updateRobotInjury(msg);
    return Result::Parsed;
  }
  if (topic == QLatin1String("GameStatus")) {
    robomaster::GameStatus msg;
    if (!msg.ParseFromArray(payload.data(), payload.size()))
      return Result::ParseFailed;
    if (cacheUpdater)
      cacheUpdater(msg);
    if (logger)
      logger(QStringLiteral("GameStatus current_stage=%1 countdown=%2 paused=%3")
                 .arg(msg.current_stage()).arg(msg.stage_countdown_sec()).arg(msg.is_paused()),
             {{QStringLiteral("current_stage"), static_cast<int>(msg.current_stage())},
              {QStringLiteral("stage_countdown_sec"), static_cast<int>(msg.stage_countdown_sec())},
              {QStringLiteral("stage_elapsed_sec"), static_cast<int>(msg.stage_elapsed_sec())},
              {QStringLiteral("is_paused"), msg.is_paused()},
              {QStringLiteral("has_current_stage"), msg.has_current_stage()},
              {QStringLiteral("has_stage_countdown_sec"), msg.has_stage_countdown_sec()},
              {QStringLiteral("has_stage_elapsed_sec"), msg.has_stage_elapsed_sec()},
              {QStringLiteral("has_is_paused"), msg.has_is_paused()}});
    gameData.updateGameStatus(msg);
    return Result::Parsed;
  }
  if (topic == QLatin1String("Event")) {
    robomaster::Event msg;
    if (!msg.ParseFromArray(payload.data(), payload.size()))
      return Result::ParseFailed;
    if (logger)
      logger(QStringLiteral("Event event_id=%1 param=%2")
                 .arg(msg.event_id()).arg(QString::fromStdString(msg.param())),
             {{QStringLiteral("event_id"), static_cast<int>(msg.event_id())},
              {QStringLiteral("param"), QString::fromStdString(msg.param())}});
    robomaster::RoboMasterMessage wrapper;
    *wrapper.mutable_event() = msg;
    gameData.processProtocolData(wrapper);
    return Result::Parsed;
  }
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
  if (topic == QLatin1String("GlobalUnitStatus")) {
    robomaster::GlobalUnitStatus msg;
    if (!msg.ParseFromArray(payload.data(), payload.size()))
      return Result::ParseFailed;
    if (logger)
      logger(
        QStringLiteral("GlobalUnitStatus robot_health=%1 robot_bullets=%2")
            .arg(QString::fromUtf8(
                QJsonDocument(repeatedFieldToJsonArray(msg.robot_health()))
                    .toJson(QJsonDocument::Compact)))
            .arg(QString::fromUtf8(
                QJsonDocument(repeatedFieldToJsonArray(msg.robot_bullets()))
                    .toJson(QJsonDocument::Compact))),
        QJsonObject{{QStringLiteral("robot_health"),
                     repeatedFieldToJsonArray(msg.robot_health())},
                    {QStringLiteral("robot_bullets"),
                     repeatedFieldToJsonArray(msg.robot_bullets())},
                    {QStringLiteral("robot_health_size"),
                     msg.robot_health_size()},
                    {QStringLiteral("robot_bullets_size"),
                     msg.robot_bullets_size()},
                    {QStringLiteral("base_health"),
                     static_cast<int>(msg.base_health())},
                    {QStringLiteral("enemy_base_health"),
                     static_cast<int>(msg.enemy_base_health())},
                    {QStringLiteral("outpost_health"),
                     static_cast<int>(msg.outpost_health())},
                    {QStringLiteral("enemy_outpost_health"),
                     static_cast<int>(msg.enemy_outpost_health())},
                    {QStringLiteral("base_status"),
                     static_cast<int>(msg.base_status())},
                    {QStringLiteral("enemy_base_status"),
                     static_cast<int>(msg.enemy_base_status())},
                    {QStringLiteral("outpost_status"),
                     static_cast<int>(msg.outpost_status())},
                    {QStringLiteral("enemy_outpost_status"),
                     static_cast<int>(msg.enemy_outpost_status())},
                    {QStringLiteral("base_shield"),
                     static_cast<int>(msg.base_shield())},
                    {QStringLiteral("enemy_base_shield"),
                     static_cast<int>(msg.enemy_base_shield())},
                    {QStringLiteral("total_damage_ally"),
                     static_cast<int>(msg.total_damage_ally())},
                    {QStringLiteral("total_damage_enemy"),
                     static_cast<int>(msg.total_damage_enemy())}});
    // 详细明文日志：打印所有字段
    QStringList rHp, rBul;
    for (int i = 0; i < msg.robot_health_size(); ++i) rHp << QString::number(msg.robot_health(i));
    for (int i = 0; i < msg.robot_bullets_size(); ++i) rBul << QString::number(msg.robot_bullets(i));
    qInfo().noquote()
        << QStringLiteral("[GlobalUnitStatus] base_hp=%1 base_status=%2 base_shield=%3 "
                          "outpost_hp=%4 outpost_status=%5 "
                          "enemy_base_hp=%6 enemy_base_status=%7 enemy_base_shield=%8 "
                          "enemy_outpost_hp=%9 enemy_outpost_status=%10 "
                          "robot_hp=[%11] robot_bullets=[%12] "
                          "dmg_ally=%13 dmg_enemy=%14")
               .arg(msg.base_health()).arg(msg.base_status())
               .arg(msg.base_shield()).arg(msg.outpost_health())
               .arg(msg.outpost_status()).arg(msg.enemy_base_health())
               .arg(msg.enemy_base_status()).arg(msg.enemy_base_shield())
               .arg(msg.enemy_outpost_health()).arg(msg.enemy_outpost_status())
               .arg(rHp.join(QStringLiteral(",")))
               .arg(rBul.join(QStringLiteral(",")))
               .arg(msg.total_damage_ally()).arg(msg.total_damage_enemy());
    // 统一交给 GameData 的状态入口处理。
    gameData.updateGlobalUnitStatus(msg);
    return Result::Parsed;
  }
  if (topic == QLatin1String("RobotStaticStatus")) {
    robomaster::RobotStaticStatus msg;
    if (!msg.ParseFromArray(payload.data(), payload.size()))
      return Result::ParseFailed;
    if (logger)
      logger(
        QStringLiteral("RobotStaticStatus robot_id=%1 connection=%2 field=%3 alive=%4")
            .arg(msg.robot_id())
            .arg(msg.connection_state())
            .arg(msg.field_state())
            .arg(msg.alive_state()),
        QJsonObject{{QStringLiteral("robot_id"),
                     static_cast<int>(msg.robot_id())},
                    {QStringLiteral("robot_type"),
                     static_cast<int>(msg.robot_type())},
                    {QStringLiteral("connection_state"),
                     static_cast<int>(msg.connection_state())},
                    {QStringLiteral("field_state"),
                     static_cast<int>(msg.field_state())},
                    {QStringLiteral("alive_state"),
                     static_cast<int>(msg.alive_state())},
                    {QStringLiteral("level"),
                     static_cast<int>(msg.level())}});
    qInfo().noquote()
        << QStringLiteral("[RobotStaticStatus] robot_id=%1 type=%2 "
                          "conn=%3 field=%4 alive=%5 "
                          "perf_shooter=%6 perf_chassis=%7 "
                          "level=%8 max_hp=%9 max_heat=%10 cool_rate=%11 "
                          "max_power=%12 max_buf_energy=%13 max_chassis_energy=%14")
               .arg(msg.robot_id()).arg(msg.robot_type())
               .arg(msg.connection_state()).arg(msg.field_state())
               .arg(msg.alive_state()).arg(msg.performance_system_shooter())
               .arg(msg.performance_system_chassis()).arg(msg.level())
               .arg(msg.max_health()).arg(msg.max_heat())
               .arg(msg.heat_cooldown_rate()).arg(msg.max_power())
               .arg(msg.max_buffer_energy()).arg(msg.max_chassis_energy());
    gameData.updateRobotStaticStatus(msg);
    return Result::Parsed;
  }
  if (topic == QLatin1String("RobotModuleStatus")) {
    robomaster::RobotModuleStatus msg;
    if (!msg.ParseFromArray(payload.data(), payload.size()))
      return Result::ParseFailed;
    if (logger)
      logger(QStringLiteral("RobotModuleStatus current_robot_id=%1 modular_42mm_present=%2")
                 .arg(gameData.getCurrentRobotId()).arg(msg.has_modular_42mm()),
             {{QStringLiteral("robot_id"), gameData.getCurrentRobotId()},
              {QStringLiteral("modular_42mm_present"), msg.has_modular_42mm()},
              {QStringLiteral("modular_42mm"), static_cast<qint64>(msg.modular_42mm())}});
    gameData.updateRobotModuleStatus(msg);
    return Result::Parsed;
  }
  if (topic == QLatin1String("RobotDynamicStatus")) {
    robomaster::RobotDynamicStatus msg;
    if (!msg.ParseFromArray(payload.data(), payload.size()))
      return Result::ParseFailed;
    if (logger)
      logger(
        QStringLiteral("RobotDynamicStatus has_no_robot_id current_health=%1 remaining_ammo=%2")
            .arg(msg.current_health())
            .arg(msg.remaining_ammo()),
        QJsonObject{{QStringLiteral("protocol_has_robot_id"), false},
                    {QStringLiteral("has_current_health"), msg.has_current_health()},
                    {QStringLiteral("has_current_heat"), msg.has_current_heat()},
                    {QStringLiteral("has_last_projectile_fire_rate"), msg.has_last_projectile_fire_rate()},
                    {QStringLiteral("has_remaining_ammo"), msg.has_remaining_ammo()},
                    {QStringLiteral("current_health"),
                     static_cast<int>(msg.current_health())},
                    {QStringLiteral("current_heat"), msg.current_heat()},
                    {QStringLiteral("remaining_ammo"),
                     static_cast<int>(msg.remaining_ammo())},
                    {QStringLiteral("can_remote_heal"),
                     msg.can_remote_heal()},
                    {QStringLiteral("can_remote_ammo"),
                     msg.can_remote_ammo()}});
    qInfo().noquote()
        << QStringLiteral("[RobotDynamicStatus] hp=%1 heat=%2 fire_rate=%3 "
                          "reserved=%4 buf_energy=%5 "
                          "exp=%6 exp_up=%7 fired=%8 ammo=%9 "
                          "out_of_combat=%10 combat_countdown=%11 "
                          "remote_heal=%12 remote_ammo=%13 presence_hp=%14 presence_heat=%15 presence_speed=%16 presence_ammo=%17")
               .arg(msg.current_health()).arg(msg.current_heat())
               .arg(msg.last_projectile_fire_rate())
               .arg(msg.reserved())
               .arg(msg.current_buffer_energy())
               .arg(msg.current_experience()).arg(msg.experience_for_upgrade())
               .arg(msg.total_projectiles_fired()).arg(msg.remaining_ammo())
               .arg(msg.is_out_of_combat())
               .arg(msg.out_of_combat_countdown())
               .arg(msg.can_remote_heal()).arg(msg.can_remote_ammo())
               .arg(msg.has_current_health()).arg(msg.has_current_heat())
               .arg(msg.has_last_projectile_fire_rate()).arg(msg.has_remaining_ammo());
    gameData.updateRobotDynamicStatus(msg);
    return Result::Parsed;
  }
  return Result::Unsupported;
}
} // namespace RM
