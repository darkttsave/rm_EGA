// SPDX-License-Identifier: MIT
/**
 * @file GameData.cpp
 * @brief 比赛数据中心实现
 * @details 实现了比赛数据的初始化、更新、查询和统计功能。
 * @author Clear
 * @date 2025-11-29
 * @copyright Copyright (c) 2025 Fudan University EGA Team (复旦大学星云 EGA 机器人战队).
 */

#include "GameData.h"
#include "RobotIdentity.h"
#include "robomaster.pb.h"
#include "GameConstants.h"
#include "../config/ConfigManager.h"
#include <QColor>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <chrono>
#include "PopupStateMachine.h"

static QString getRobotName(int robotId);

namespace {
float sanitizeFiniteRuneAverage(float value) {
  if (std::isfinite(static_cast<double>(value))) {
    return value;
  }
  return 0.0f;
}

QList<int> radarRobotOrderForPerspective(bool isBluePerspective) {
  const QList<int> redOrder{1, 2, 3, 4, 6, 7};
  const QList<int> blueOrder{101, 102, 103, 104, 106, 107};
  return isBluePerspective ? (redOrder + blueOrder) : (blueOrder + redOrder);
}

QString dartTargetName(int targetId) {
  switch (targetId) {
  case 1:
    return QStringLiteral("前哨站");
  case 2:
    return QStringLiteral("基地固定目标");
  case 3:
    return QStringLiteral("基地随机固定目标");
  case 4:
    return QStringLiteral("基地随机移动目标");
  case 5:
    return QStringLiteral("基地末端移动目标");
  default:
    return QStringLiteral("未知目标");
  }
}

QString normalizedSystemMessageColor(const QString &color) {
  static const QString kDefaultColor = QStringLiteral("#E5E5E5");
  if (color.isEmpty()) {
    return kDefaultColor;
  }

  const QColor parsedColor(color);
  return parsedColor.isValid() ? color : kDefaultColor;
}
float robotShootSpeedLimit(RobotType type) {
  switch (type) {
  case RobotType::HERO:
    return static_cast<float>(RM::RobotStats::HERO_BULLET_SPEED_LIMIT);
  case RobotType::INFANTRY_3:
  case RobotType::INFANTRY_4:
  case RobotType::INFANTRY_5:
  case RobotType::AERIAL:
  case RobotType::SENTRY:
    return static_cast<float>(RM::RobotStats::ROBOT_BULLET_SPEED_LIMIT);
  case RobotType::ENGINEER:
  case RobotType::DART:
  case RobotType::RADAR:
  default:
    return 0.0f;
  }
}

quint16 robotTabShootSpeedLimit(RobotType type) {
  return static_cast<quint16>(robotShootSpeedLimit(type));
}

QString robotTabAvatarSource(const RobotData &robot) {
  Q_UNUSED(robot)
  return QStringLiteral("qrc:/images/robot_avatar.png");
}

quint16 robotTabDefaultMaxHp(RobotType type) {
  switch (type) {
  case RobotType::HERO:
    return 200;
  case RobotType::ENGINEER:
    return 250;
  case RobotType::INFANTRY_3:
  case RobotType::INFANTRY_4:
  case RobotType::INFANTRY_5:
    return 200;
  case RobotType::AERIAL:
    return 250;
  case RobotType::SENTRY:
    return 600;
  default:
    return 0;
  }
}

bool robotConnectedForTab(const RobotData &robot) {
  const bool hasStaticConnection = robot.tabStaticConnected;
  const bool hasGlobalHealth =
      robot.hasTabGlobalSnapshot && robot.tabGlobalCurrentHP > 0;
  return hasStaticConnection || hasGlobalHealth;
}

quint16 robotDisplayHpForPanels(const RobotData &robot) {
  if (robot.hasTabGlobalSnapshot && robot.tabGlobalCurrentHP > 0) {
    return robot.tabGlobalCurrentHP;
  }
  return robotConnectedForTab(robot) ? robot.currentHP : 0;
}

quint16 robotDisplayMaxHpForPanels(const RobotData &robot) {
  if (!robotConnectedForTab(robot)) {
    return 0;
  }
  return robot.maxHP > 0 ? robot.maxHP : robotTabDefaultMaxHp(robot.type);
}

quint8 robotDisplayLevelForPanels(const RobotData &robot) {
  return robotConnectedForTab(robot)
             ? static_cast<quint8>(std::max(1, static_cast<int>(robot.level)))
             : 0;
}

//根据bufftype获取buff指针
const RobotData::BuffTimedData *timedBuffByType(const RobotData *robot, quint8 buffType) {
  return robot ? robot->buffs.byType(buffType) : nullptr;
}

RobotData::BuffTimedData *timedBuffByType(RobotData *robot, quint8 buffType) {
  return robot ? robot->buffs.byType(buffType) : nullptr;
}

//打包buff信息
QVariantMap toBuffTimedVariant(quint8 buffType,
                               const RobotData::BuffTimedData *state) {
  QVariantMap map;
  map["type"] = static_cast<int>(buffType);
  map["level"] = state ? static_cast<int>(state->level) : 0;
  map["maxTime"] = state ? static_cast<int>(state->maxTime) : 0;
  map["leftTime"] = state ? static_cast<int>(state->leftTimeAtSync) : 0;
  return map;
}

QVariantMap toBuffTimedVariant(quint8 buffType, int level, int maxTime, int leftTime) {
  QVariantMap map;
  map["type"] = static_cast<int>(buffType);
  map["level"] = level;
  map["maxTime"] = qMax(0, maxTime);
  map["leftTime"] = qMax(0, leftTime);
  return map;
}

//机器人基础id
constexpr quint8 kRadarRobotBaseIds[] = {1, 2, 3, 4, 6, 7};

//根据红蓝方和机器人基础id获取机器人id
quint8 teamRobotId(quint8 baseId, TeamColor team) {
  return (team == TeamColor::RED) ? baseId : static_cast<quint8>(baseId + 100);
}

//当前是否是正式比赛阶段
bool isBattleStage(GameStage stage) {
  return stage != GameStage::NOT_STARTED &&
         stage != GameStage::PREPARATION &&
         stage != GameStage::SELF_CHECK &&
         stage != GameStage::COUNTDOWN &&
         stage != GameStage::SETTLEMENT;
}

struct KillEventIdentity {
  RM::RobotIdentity victim;
  RM::RobotIdentity killer;
};

std::optional<KillEventIdentity> parseKillEvent(const QString &param) {
  const auto parts = param.split(',');
  if (parts.size() != 2) return std::nullopt;
  bool victimOk = false, killerOk = false;
  const qint64 victimId = parts[0].trimmed().toLongLong(&victimOk);
  const qint64 killerId = parts[1].trimmed().toLongLong(&killerOk);
  if (!victimOk || !killerOk) return std::nullopt;
  auto victim = RM::resolveRobotIdentity(victimId);
  auto killer = RM::resolveRobotIdentity(killerId);
  if (victim.kind != RM::IdentityKind::Robot || !victim.team ||
      !((killer.kind == RM::IdentityKind::Robot && killer.team) ||
        (killer.kind == RM::IdentityKind::FieldObject && killer.normalizedId &&
         *killer.normalizedId == 10))) return std::nullopt;
  return KillEventIdentity{victim, killer};
}

QString killEventMessage(const KillEventIdentity &event) {
  const auto teamText = [](TeamColor team) {
    return team == TeamColor::RED ? QStringLiteral("红方") : QStringLiteral("蓝方");
  };
  if (event.killer.kind == RM::IdentityKind::FieldObject ||
      event.killer.rawId == event.victim.rawId)
    return QStringLiteral("%1%2机器人阵亡").arg(teamText(*event.victim.team), event.victim.displayName);
  return QStringLiteral("%1%2机器人击毁了%3%4机器人")
      .arg(teamText(*event.killer.team), event.killer.displayName,
           teamText(*event.victim.team), event.victim.displayName);
}

QString officialEventPopupMessageForEvent(const robomaster::Event &data,
                                          const RobotData *currentRobot,
                                          int currentRuneType) {
  const int eventId = data.event_id();
  const QString param = QString::fromStdString(data.param()).trimmed();
  const TeamColor currentTeam = currentRobot ? currentRobot->team : TeamColor::RED;

  auto teamText = [](TeamColor team) {
    return team == TeamColor::RED ? QStringLiteral("红") : QStringLiteral("蓝");
  };

  switch (eventId) {
  case 1: {
    const auto event = parseKillEvent(param);
    return event ? killEventMessage(*event) : QStringLiteral("机器人击毁事件（身份无效）");
  }
  case 2: {
    bool ok = false;
    const uint targetId = param.toUInt(&ok);
    if (ok) {
      if (targetId == 11) {
        return QStringLiteral("红方前哨站被摧毁");
      }
      if (targetId == 111) {
        return QStringLiteral("蓝方前哨站被摧毁");
      }
    }
    return QStringLiteral("前哨站被摧毁");
  }
  case 3: {
    const QStringList parts = param.split(",");
    if (parts.size() >= 2) {
      bool okArms = false;
      const int activatedArms = parts[0].trimmed().toInt(&okArms);
      bool okAvg = false;
      const float averageRings = parts[1].trimmed().toFloat(&okAvg);
      if (okArms && okAvg) {
        QString msg =
            QStringLiteral("能量机关状态更新：已激活%1个灯臂，平均环数%2")
                .arg(activatedArms)
                .arg(QString::number(averageRings, 'f', 1));
        // 附加各组环数/增量对
        QStringList groupParts;
        for (int i = 2; i + 1 < parts.size(); i += 2) {
          bool okGAvg = false;
          const float gAvg = parts[i].trimmed().toFloat(&okGAvg);
          bool okDelta = false;
          const int gDelta = parts[i + 1].trimmed().toInt(&okDelta);
          if (okGAvg) {
            groupParts.append(QStringLiteral("%1/Δ%2").arg(
                QString::number(gAvg, 'f', 1)).arg(gDelta));
          }
        }
        if (!groupParts.isEmpty()) {
          msg += QStringLiteral("，各组 ") + groupParts.join(QStringLiteral(", "));
        }
        return msg;
      }
    }
    if (!parts.isEmpty()) {
      bool ok = false;
      const int activatedArms = parts[0].trimmed().toInt(&ok);
      if (ok) {
        return QStringLiteral("能量机关状态更新：已激活%1个灯臂").arg(activatedArms);
      }
    }
    return QStringLiteral("能量机关状态更新");
  }
  case 4: {
    bool ok = false;
    const int protocolRuneType = param.toInt(&ok);
    const int resolvedRuneType = ok ? ((protocolRuneType == 2) ? 1 : 0) : currentRuneType;
    return QStringLiteral("%1方激活了%2能量机关")
        .arg(teamText(currentTeam), resolvedRuneType == 0 ? QStringLiteral("小")
                                                         : QStringLiteral("大"));
  }
  case 9: {
    QString normalizedParam = param;
    normalizedParam.replace(QStringLiteral("，"), QStringLiteral(","));
    const QStringList parts = normalizedParam.split(",");
    bool okTeam = false;
    bool okTarget = false;
    const int hitTeam = parts.value(0).trimmed().toInt(&okTeam);
    const int targetId = parts.value(1).trimmed().toInt(&okTarget);
    if (okTeam && okTarget) {
      return QStringLiteral("%1方飞镖命中%2")
          .arg(hitTeam == 1 ? QStringLiteral("红") : QStringLiteral("蓝"),
               dartTargetName(targetId));
    }
    return QStringLiteral("飞镖命中事件");
  }
  case 10:
    return QStringLiteral("对方飞镖闸门开启");
  case 11:
    return QStringLiteral("基地遭受攻击");
  case 12:
    return QStringLiteral("对方前哨站停转");
  case 13:
    return QStringLiteral("对方基地护甲展开");
  case 100:
    return param.isEmpty() ? QStringLiteral("自定义事件通知") : param;
  case 2000:
    return QStringLiteral("比赛结果已更新");
  case 3001:
    return QStringLiteral("全员踢出状态已开启");
  case 3002:
    return QStringLiteral("全员踢出状态已解除");
  default:
    return QString();
  }
}

} // namespace



/**
 * @brief 构造函数
 * @details 初始化比赛数据，包括机器人、基地、增益点等，并启动定时器。
 *
 * @param parent 父对象
 */
GameData::GameData(QObject *parent)
    : QObject(parent), m_currentRobotId(1), m_gameTimer(new QTimer(this)) {
  initializeRobots();

  m_mapProfile = MapProfile::loadFromConfig();
  m_positionSource = ConfigManager::instance().getPositionSource();
  connect(&ConfigManager::instance(), &ConfigManager::configReloaded, this,
          [this]() {
            setMapProfile(MapProfile::loadFromConfig());
            setPositionSource(ConfigManager::instance().getPositionSource());
          });
  connect(this, &GameData::robotPositionUpdated, this,
          [this](quint8, float, float, float, quint8) {
            if (m_positionSource == QStringLiteral("official")) {
              emit miniMapMarkersChanged();
            }
          });
  connect(this, &GameData::robotDataUpdated, this, [this](quint8) {
    if (m_positionSource == QStringLiteral("official")) {
      emit miniMapMarkersChanged();
    }
  });

  // 初始化基地数据
  m_redBase.team = TeamColor::RED;
  m_redBase.currentHP = 5000;
  m_redBase.maxHP = 5000;

  m_blueBase.team = TeamColor::BLUE;
  m_blueBase.currentHP = 5000;
  m_blueBase.maxHP = 5000;

  m_totalKills = 0;

  // 初始化前哨站数据
  m_redOutpostData.team = TeamColor::RED;
  m_redOutpostData.currentHP = 1500;
  m_redOutpostData.maxHP = 1500;
  m_redOutpostData.isDestroyed = false;

  m_blueOutpostData.team = TeamColor::BLUE;
  m_blueOutpostData.currentHP = 1500;
  m_blueOutpostData.maxHP = 1500;
  m_blueOutpostData.isDestroyed = false;

  // 初始化比赛状态
  m_gameState.gameTime = 420; // 7分钟
  m_gameState.currentRound = 1;
  m_gameState.gameProgress = GameStage::NOT_STARTED;
  m_gameState.gameStarted = false;
  m_gameState.gameEnded = false;

  // 初始化增益点
  m_buffPoints.clear();
  for (int i = 0; i < 3; ++i) { // 通常有3个增益点
    BuffPointData buffPoint;
    buffPoint.pointId = i + 1;
    buffPoint.energyValue = 0;
    buffPoint.maxEnergyValue = 1000;
    buffPoint.isActivated = false;
    buffPoint.controllingTeam = TeamColor::RED;
    m_buffPoints.append(buffPoint);
  }

  // 初始化哨兵状态
  m_redSentry.isAlive = true;
  m_redSentry.currentHP = 600;
  m_redSentry.maxHP = 600;
  m_redSentry.isInvincible = false;

  m_blueSentry.isAlive = true;
  m_blueSentry.currentHP = 600;
  m_blueSentry.maxHP = 600;
  m_blueSentry.isInvincible = false;

  // 设置定时器
  connect(m_gameTimer, &QTimer::timeout, this, &GameData::onUpdateTimer);
  m_gameTimer->start(100); // 100ms更新一次

  // 连接信号以触发无参数的 QML 代理信号
  connect(this, &GameData::baseHealthUpdated, this,
          [this](TeamColor) { emit baseHealthUpdatedProxy(); });
  connect(this, &GameData::outpostHealthUpdated, this,
          [this](TeamColor) { emit outpostHealthUpdatedProxy(); });
  connect(this, &GameData::outpostRebuildCountChanged, this,
          [this]() { emit outpostRebuildCountProxy(); });
  connect(this, &GameData::gameStateUpdated, this, [this]() {
    emit baseHealthUpdatedProxy();
  }); // GameState更新也可能影响基地无敌状态

  // 初始化弹窗状态机，用于集中管理弹窗展现逻辑
  m_popupStateMachine = new PopupStateMachine(this);
  connect(m_popupStateMachine, &PopupStateMachine::activePopupsChanged, this,
          &GameData::onPopupStateMachineUpdated);
  connect(m_popupStateMachine, &PopupStateMachine::popupPayloadChanged, this,
          &GameData::onPopupPayloadFromStateMachine);

  // 初始化 UI 刷新定时器，用于驱动脱战倒计时等随时间变化的状态
  m_uiRefreshTimer = new QTimer(this);
  connect(m_uiRefreshTimer, &QTimer::timeout, this, [this]() {
    // 检查当前机器人是否处于非脱战状态，如果是，则每秒通知 UI 重新读取倒计时
    const RobotData *robot = getCurrentRobot();
    emit myRobotUpdated();

  });
  m_uiRefreshTimer->start(1000); // 1秒一次
}

QImage GameData::heroFrame() const {
  QMutexLocker locker(&m_mutex);
  return m_heroFrame;
}

bool GameData::hasHeroFrame() const {
  QMutexLocker locker(&m_mutex);
  return !m_heroFrame.isNull();
}

void GameData::onVideoFrameReceived(const QImage &frame) {
  {
    QMutexLocker locker(&m_mutex);
    m_heroFrame = frame;
    ++m_heroFrameRevision;
    m_heroFrameDataUrl.clear();
  }
  emit heroFrameUpdated();
}

void GameData::clearHeroFrame() {
  {
    QMutexLocker locker(&m_mutex);
    m_heroFrame = QImage();
    ++m_heroFrameRevision;
    m_heroFrameDataUrl.clear();
  }
  qDebug() << "[herovideo] GameData: Cleared hero frame";
  emit heroFrameUpdated();
}

quint64 GameData::heroFrameRevision() const {
  QMutexLocker locker(&m_mutex);
  return m_heroFrameRevision;
}

QString GameData::heroFrameSource() const {
  QMutexLocker locker(&m_mutex);
  if (m_heroFrame.isNull()) {
    return QString();
  }
  return QStringLiteral("image://herovideo/frame?rev=%1").arg(m_heroFrameRevision);
}

QString GameData::heroFrameDataUrl() const {
  QMutexLocker locker(&m_mutex);
  return m_heroFrameDataUrl;
}

/**
 * @brief 初始化机器人数据
 * @details 创建红蓝双方共14个机器人的初始数据，设定ID、类型、血量上限等。
 */
void GameData::initializeRobots() {
  m_robots.clear();
  for (int offset : {0, 100}) {
    for (int number : {1, 3, 4, 5, 6, 7, 8, 9}) {
      const auto identity = RM::resolveRobotIdentity(offset + number);
      RobotData robot;
      robot.robotId = static_cast<quint8>(identity.rawId);
      robot.team = *identity.team;
      robot.type = *identity.role;
      robot.maxHP = 0;
      robot.currentHP = 0;
      robot.status = RobotStatus::OFFLINE;
      robot.maxExperience = 100;
      robot.maxPower = 0;
      robot.power = 0;
      robot.heatLimit = 0;
      robot.maxBufferEnergy = 0;
      robot.shootSpeedLimit = 0.0f;
      robot.tabShootSpeedLimit = 0;
      robot.lastUpdateTime = QDateTime::currentDateTime();
      robot.q2HeatThreshold = 100;
      m_robots.append(robot);
    }
  }
}

std::optional<RobotType> GameData::getRobotTypeById(qint64 robotId) {
  return RM::resolveRobotIdentity(robotId).role;
}

/**
 * @brief 根据ID获取机器人数据
 * @param robotId 机器人ID
 * @return const RobotData* 机器人数据指针，如果未找到则返回 nullptr
 */
const RobotData *GameData::getRobotById(qint64 robotId) const {
  for (const auto &robot : m_robots) {
    if (robot.robotId == robotId) {
      return &robot;
    }
  }
  return nullptr;
}

void GameData::setCurrentRobotId(qint64 robotId) {
  const auto identity = RM::resolveRobotIdentity(robotId);
  const quint8 next = identity.kind == RM::IdentityKind::Robot && identity.team
                          ? static_cast<quint8>(robotId) : 0;
  const auto previous = RM::resolveRobotIdentity(m_currentRobotId);
  if (m_currentRobotId == next) return;
  m_currentRobotId = next;
  ++m_mqttIdentityEpoch;
  emit globalHealthObservationChanged();
  emit currentRobotIdentityChanged();
  emit mqttReceptionMetadataChanged();
  clearRobotDynamicStatus();
  m_robotModuleStatusMap.clear();
  m_robotModuleStatusReceived = false;
  m_moduleStatus.clear();
  m_lastModuleOnlineStates.clear();
  emit moduleListChanged();
  emit myRobotUpdated();
  if (previous.team != RM::resolveRobotIdentity(next).team) {
    emit currentTeamEconomyChanged();
    emit runeStatusDataChanged();
  }
}

GameData::~GameData() {
  // 析构函数实现
}

//封装了 HP/Level 等全量状态更新，以及 击杀、复活、伤害检测 逻辑
void GameData::emitDataChanged() {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (now - m_lastDataChangedMs < kDataChangedMinIntervalMs) {
    return; // 节流：不到 50ms 跳过本次发射
  }
  m_lastDataChangedMs = now;
  emit dataChanged();
}

void GameData::updateStandardStatus(RobotData *robot, const robomaster::RobotStatus &data) {
    if (!robot) return;

    // 保存旧状态用于比较(击杀逻辑)
    bool wasAlive = (robot->status != RobotStatus::DESTROYED) && (robot->currentHP > 0);
    quint16 prevHP = robot->currentHP;
    robot->isClientConnected = true;
    robot->isControllerConnected = true;

    robot->shootSpeedLimit = robotShootSpeedLimit(robot->type);
    robot->tabShootSpeedLimit = robotTabShootSpeedLimit(robot->type);
    robot->level = data.level();
    robot->currentHP = data.hp();
    robot->maxHP = data.max_hp();
    robot->currentRoundMaxHP =
        qMax(robot->currentRoundMaxHP, qMax(robot->currentHP, robot->maxHP));
    if (data.heat() > 0) {
        robot->currentHeat = static_cast<quint16>(data.heat());
    }

    //热量上限
    if (data.heat_limit() > 0)
        robot->heatLimit = data.heat_limit();
    //热量冷却值/s
    if (data.cooling_value() > 0)
        robot->coolingValue = static_cast<quint16>(data.cooling_value());
    //底盘功率上限
    if (data.power_limit() > 0) {
        robot->maxPower = data.power_limit();
        robot->maxChassisEnergy = robot->maxPower; // 同步给UI
    }

    // 更新底盘使能状态 (用于推断超速/超功率惩罚)
    robot->chassisEnabled = data.chassis_enabled();
    // 更新枪口使能状态 (0=锁定, 1=使能)
    robot->shooterLocked = !data.shooter_enabled();

    if (data.buff_mask() > 0)
        robot->buffMask = data.buff_mask();

    robot->lastUpdateTime = QDateTime::currentDateTime();

    // 全量状态更新后，立即检查热量超限状态
    checkHeatLimit(robot);

    // 收到标准状态包说明该机器人已在协议侧出现；从 OFFLINE/DESTROYED 恢复到在线状态。
    robot->status = robot->currentHP > 0 ? RobotStatus::NORMAL
                                         : RobotStatus::DESTROYED;

    // 伤害检测：如果血量减少且非死亡重置
    if (robot->currentHP < prevHP && prevHP > 0) {
        robot->isOutOfCombat = false;
        robot->lastDamageTime = QDateTime::currentDateTime();
    }

    // 复活检测：若之前HP为0且当前HP>0，视为"复活"，重置 ~ 面板伤害统计
    if (prevHP == 0 && robot->currentHP > 0) {
        resetInjuryStatsOnRespawn();
    }
}

//封装了射速上限计算和超速判定逻辑
void GameData::checkSpeedLimit(RobotData *robot, bool isPenaltyTriggered) {
    if (!robot || robot->type == RobotType::HEAVY) return;

    // --- 射速上限计算 ---
    // 计算当前模式下的射速上限
    float limit = 30.0f; // 默认上限(如工程机器人)

    if (robot->type == RobotType::INFANTRY_3 ||
        robot->type == RobotType::INFANTRY_4 ||
        robot->type == RobotType::INFANTRY_5 ||
        robot->type == RobotType::AERIAL ||
        robot->type == RobotType::SENTRY) {
        limit = 25.0f;
    } else if (robot->type == RobotType::HERO) {
        // 英雄机器人特殊逻辑：
        // 默认 12m/s
        // 远程优先 (heroRigMode==0) + 部署模式 (chassisEnabled==false) -> 16.5m/s
        bool isRangePriority = (robot->heroRigMode == 0);
        bool isDeploymentMode = !robot->chassisEnabled; // 底盘断电视为进入部署模式

        if (isRangePriority && isDeploymentMode) {
            limit = 16.5f;
        } else {
            limit = 12.0f;
        }
    }
    robot->shootSpeedLimit = limit;

    if(robot->speedLockState!=SpeedLockState::PermanentLocked){
        // 仅当收到 PenaltyInfo 且射速确实超限时，才判定锁定状态
        // 如果没有 PenaltyInfo，即使超速也不触发锁定逻辑 (等待服务器判罚)
        if (isPenaltyTriggered && robot->muzzleVelocity > limit && robot->currentHP > 0 && robot->status != RobotStatus::DESTROYED) {

            // 根据射击初速度超限惩罚机制计算锁定时间
            float v1 = robot->muzzleVelocity;
            float v0 = limit;
            int lockTime = 0;
            SpeedLockState newState = SpeedLockState::Normal;

            if (robot->type == RobotType::HERO) {
                // 英雄机器人 (42mm 弹丸)
                bool isDeployment = (v0 > 13.0f);

                if (isDeployment) {
                    // 部署模式 (Table Col 3)
                    if (v1 <= 18.0f) {
                        lockTime = 15;
                        newState = SpeedLockState::Locked15s;
                    } else {
                        lockTime = 999;
                        newState = SpeedLockState::PermanentLocked;
                    }
                } else {
                    // 非部署模式 (Table Col 2)
                    if (v1 <= 1.1f * v0) {
                        lockTime = 15;
                        newState = SpeedLockState::Locked15s;
                    } else if (v1 <= 1.2f * v0) {
                        lockTime = 20;
                        newState = SpeedLockState::Locked20s;
                    } else {
                        lockTime = 999;
                        newState = SpeedLockState::PermanentLocked;
                    }
                }
            } else {
                // 17mm 弹丸 (步兵、空中、哨兵) (Table Col 1)
                float diff = v1 - v0;
                if (diff < 5.0f) {
                    lockTime = 15;
                    newState = SpeedLockState::Locked15s;
                } else if (diff < 10.0f) {
                    lockTime = 20;
                    newState = SpeedLockState::Locked20s;
                } else {
                    lockTime = 999;
                    newState = SpeedLockState::PermanentLocked;
                }
            }

            robot->speedLockSeconds = lockTime;
            robot->speedLockState = newState;
        } else {
            robot->speedLockState=SpeedLockState::Normal;
            robot->speedLockSeconds = 0;
        }
    }

}

void GameData::checkHeatLimit(RobotData *robot, bool isPenaltyConfirmed) {
    if (!robot || robot->type == RobotType::HEAVY) return;

    // 基础参数校验
    if (robot->heatLimit == 0) return; // 尚未获取到上限，不进行判断

    // 计算 Q2 (二级超限阈值/永久封锁阈值)
    // 步兵/空中/哨兵 (17mm): Q2 = Limit + 100
    // 英雄 (42mm): Q2 = Limit + 200
    int q2Threshold = robot->heatLimit + 100;
    if (robot->type == RobotType::HERO) {
        q2Threshold = robot->heatLimit + 200;
    }

    if(!robot->shooterPermanentLocked){
        // 判定热量超限状态
        if (robot->currentHeat > robot->heatLimit) {
            robot->isHeatOverLimit = true; // 标记为过热

            // 判断是否达到严重超限(Q2) - 仅作为严重程度标记，不用于推断锁定
            // 引入 PenaltyInfo 判断：只有当收到判罚确认且热量超过 Q2 时，才标记为永久锁定
            if (isPenaltyConfirmed && robot->currentHeat > q2Threshold) {
                robot->shooterPermanentLocked = true;
            }

            // 计算冷却到 Limit 以下所需的时间
            int diff = robot->currentHeat - robot->heatLimit;
            double coolingRate = qMax(1.0, static_cast<double>(robot->coolingValue));

            double secs = std::ceil(static_cast<double>(diff) / coolingRate);
            robot->heatLockSeconds = static_cast<quint16>(secs);

        }else if(robot->isHeatOverLimit  && robot->currentHeat==0){
            robot->isHeatOverLimit = false;
        }else {
            // 热量正常
            robot->isHeatOverLimit = false; // 重置过热标记
            robot->heatLockSeconds = 0;
            // 如果热量恢复正常，重置严重超限标记（具体是否解锁由协议 shooter_enabled 决定）
            robot->shooterPermanentLocked = false;
        }
    }

}


/**
 * @brief 更新比赛时间
 * @details 增加比赛时间计数并发出信号。
 */
void GameData::updateGameTime() {
  // 兼容旧模拟器和测试的自增入口；正式比赛时间仍以服务端状态为准。
  m_gameState.gameTime++;
  emit gameTimeUpdated(m_gameState.gameTime);
  emitDataChanged();
}

QString GameData::outOfCombatStatus()const{
  const RobotData *robot = getCurrentRobot();
  if (!robot) return "";
  if (robot->isOutOfCombat) return "已脱战";
  else{
    int remaining = robot->outOfCombatCountdown;
    if (remaining <= 0) return "已脱战";
    return QString("%1s 后脱战").arg(remaining);
  }
}


QString GameData::chassisStatus() const {
  const RobotData *robot = getCurrentRobot();
  if (!robot) return "";

  if (robot->chassisOverPowerCutSeconds > 0) {
    return QString("超限断电 %1s").arg(robot->chassisOverPowerCutSeconds);
  }

  const auto &chassisPower = robot->buffs.chassisPower;
  if (chassisPower.level > 0 && chassisPower.leftTimeAtSync > 0) {
    return QString("%1s").arg(chassisPower.leftTimeAtSync);
  }

  return "";
}

const RobotData *GameData::getCurrentRobot() const {
  return getRobotById(m_currentRobotId);
}

const QList<RobotData> GameData::getRobotsByTeam(TeamColor team) const {
  QList<RobotData> result;
  for (const auto &robot : m_robots) {
    if (robot.team == team) {
      result.append(robot);
    }
  }
  return result;
}

QString GameData::getFormattedGameTime() const {
  int minutes = m_gameState.gameTime / 60;
  int seconds = m_gameState.gameTime % 60;
  return QString("%1:%2")
      .arg(minutes, 2, 10, QChar('0'))
      .arg(seconds, 2, 10, QChar('0'));
}

QString GameData::getFormattedStageElapsedTime() const {
  int minutes = m_gameState.stageElapsed / 60;
  int seconds = m_gameState.stageElapsed % 60;
  return QString("%1:%2")
      .arg(minutes, 2, 10, QChar('0'))
      .arg(seconds, 2, 10, QChar('0'));
}

QString GameData::gamePhaseString() const {
  switch (m_gameState.gameProgress) {
  case GameStage::NOT_STARTED:
    return "未开始";
  case GameStage::PREPARATION:
    return "准备阶段";
  case GameStage::SELF_CHECK:
    return "自检阶段";
  case GameStage::COUNTDOWN:
    return "倒计时";
  case GameStage::BATTLE:
    return "战斗阶段";
  case GameStage::SETTLEMENT:
    return "结算阶段";
  default:
    return "未知阶段";
  }
}

QVariantList GameData::activePopups() const {
  if (!m_popupStateMachine) return {};
  return m_popupStateMachine->activePopups();
}

QVariantMap GameData::robotRespawnStatus() const {
  return m_lastRespawnStatus;
}

QVariantMap GameData::normalizeRobotRespawnStatus(const QVariantMap &status) const {
  QVariantMap normalized = status;
  auto copyIfMissing = [&](const QString &key) {
    if (!normalized.contains(key) && m_lastRespawnStatus.contains(key)) {
      normalized.insert(key, m_lastRespawnStatus.value(key));
    }
  };

  copyIfMissing(QStringLiteral("is_pending_respawn"));
  copyIfMissing(QStringLiteral("total_respawn_progress"));
  copyIfMissing(QStringLiteral("current_respawn_progress"));
  copyIfMissing(QStringLiteral("robot_index"));
  copyIfMissing(QStringLiteral("robot_id"));

  const bool isPending = normalized.value("is_pending_respawn", false).toBool();
  const bool canFreeRespawn =
      normalized.value("can_free_respawn", false).toBool();
  quint32 total = normalized.value("total_respawn_progress", 0).toUInt();
  quint32 current = normalized.value("current_respawn_progress", 0).toUInt();
  const quint32 lastTotal = m_lastRespawnStatus.value("total_respawn_progress", 0).toUInt();
  const quint32 lastCurrent = m_lastRespawnStatus.value("current_respawn_progress", 0).toUInt();

  if (isPending && m_lastRespawnStatus.value("is_pending_respawn", false).toBool()) {
    if (total == 0 && lastTotal > 0) total = lastTotal;
    if (lastTotal > 0 && total == lastTotal) {
      if (current == 0 && lastCurrent > 0) current = lastCurrent;
      if (current < lastCurrent) current = lastCurrent;
    }
  }

  // 服务端允许免费复活时，以该标志为最终结论。部分完成帧会把 proto3 进度清零，
  // 此处沿用上一帧总进度并投影为完成态，避免界面和快捷键停在最后一步。
  if (isPending && canFreeRespawn && lastTotal > 0) {
    total = lastTotal;
    current = total;
  }

  if (total > 0 && current > total) current = total;
  normalized.insert("total_respawn_progress", total);
  normalized.insert("current_respawn_progress", current);

  return normalized;
}

void GameData::onPopupStateMachineUpdated() {
  // 槽函数：当状态机激活集合变化时通知 QML/外部
  emit activePopupsChanged();
  // 诊断日志：输出数量和类型列表，便于判断状态机是否产出弹窗
  QVariantList list = activePopups();
  QStringList types;
  for (const QVariant &v : list) {
    if (v.canConvert<QVariantMap>()) {
      QVariantMap m = v.toMap();
      types << m.value("type").toString();
    } else {
      types << v.toString();
    }
  }
  qDebug() << "GameData: activePopups changed, count=" << list.size() << "types=" << types;
}

void GameData::onPopupPayloadFromStateMachine(const QString &type, const QVariantMap &payload) {
  // 处理来自状态机的 payload 增量更新，针对不同类型做转发
  // 注意：RobotRespawn 已从状态机解耦，不再经此路径

  // 其他类型：直接转发给 QML/外部，QML 可选择按 type 处理
  emit popupPayloadUpdated(type, payload);
}

//将 C++ 的 RobotData 结构体转换为 QML 可读的 QVariantMap （即 JSON 对象）
QVariantMap GameData::getRobotInfo(int robotId) const {
  const RobotData *robot = getRobotById(robotId);
  QVariantMap map;
  if (robot) {
    const bool panelConnected = robotConnectedForTab(*robot);
    const int panelHp = static_cast<int>(robotDisplayHpForPanels(*robot));
    const int panelMaxHp = static_cast<int>(robotDisplayMaxHpForPanels(*robot));
    const int panelLevel = static_cast<int>(robotDisplayLevelForPanels(*robot));
    // 基础血量
    map["staticPerformance"] = robot->staticPerformanceObservation;
    map["robotId"] = robot->robotId;
    const auto identity = RM::resolveRobotIdentity(robot->robotId);
    map["identityName"] = identity.displayName;
    map["identityNumber"] = identity.normalizedId.value_or(0);
    map["identityTeam"] = identity.team ? (*identity.team == TeamColor::RED ? QStringLiteral("红方") : QStringLiteral("蓝方")) : QStringLiteral("未知阵营");
    map["hp"] = robot->currentHP;
    map["maxHp"] = robot->maxHP;
    map["isDead"] = (robot->status == RobotStatus::DESTROYED);
    map["isOffline"] = (robot->status == RobotStatus::OFFLINE);
    map["isClientConnected"] = robot->isClientConnected;
    map["isControllerConnected"] = robot->isControllerConnected;
    map["status"] = static_cast<int>(robot->status);
    map["type"] = static_cast<int>(robot->type);
    map["isOutOfCombat"] = robot->isOutOfCombat; // 暴露脱战状态
    map["canRemoteHeal"] = robot->canRemoteHeal;
    map["canRemoteAmmo"] = robot->canRemoteAmmo;
    map["canRemoteExchange"] = robot->canRemoteHeal || robot->canRemoteAmmo;

    // 热量数据 (用于热量环)
    map["currentHeat"] = robot->currentHeat;
    map["heatLimit"] = robot->heatLimit;
    map["shooterLocked"] = robot->shooterLocked;
    map["shooterPermanentLocked"] = robot->shooterPermanentLocked;
    map["heatLockSeconds"] = robot->heatLockSeconds;
    map["isHeatOverLimit"] = robot->isHeatOverLimit; // 暴露给 QML

    // 弹丸数据 (允许发弹量)
    map["allowedAmmo17mm"] = robot->allowedAmmo17mm;
    map["allowedAmmo42mm"] = robot->allowedAmmo42mm;
    map["fortressBonusAmmo"] = robot->fortressBonusAmmo; // 堡垒储备弹药
    map["speedLockState"] = static_cast<int>(robot->speedLockState); // 射速超限锁定状态
    map["speedLockSeconds"] = robot->speedLockSeconds; // 超速锁定时间
    map["lastUpdateTime"] = robot->lastUpdateTime;

    // 射击数据 (射速/初速度)
    map["muzzleVelocity"] = robot->muzzleVelocity;         // 初速度 m/s
    map["shootSpeedLimit"] = robot->shootSpeedLimit;       // 初速度上限 m/s
    map["tabShootSpeedLimit"] = static_cast<int>(robot->tabShootSpeedLimit);
    map["launchingFrequency"] = robot->launchingFrequency; // 射频 Hz

    // 等级和能量
    map["level"] = robot->level;
    map["power"] = robot->power;
    map["maxPower"] = robot->maxPower;
    map["maxChassisEnergy"] = robot->maxChassisEnergy;
    map["bufferEnergy"] = robot->bufferEnergy;
    map["maxBufferEnergy"] = robot->maxBufferEnergy;
    map["posX"] = robot->posX;
    map["posY"] = robot->posY;
    map["x"] = robot->posX;
    map["y"] = robot->posY;
    map["angle"] = robot->angle;
    map["isHighLight"] = robot->isHighLight > 0;
    map["hasVulnerability"] = robot->buffs.defenseOrVulnerability.level < 0;
    map["heatCooling"] = robot->coolingValue;
    map["panelHp"] = panelHp;
    map["panelMaxHp"] = panelMaxHp;
    map["panelIsConnected"] = panelConnected;
    map["panelIsOffline"] = !panelConnected;
    map["panelIsDead"] = panelConnected && panelHp <= 0;
    map["panelLevel"] = panelLevel;
  } else {
    const auto identity = RM::resolveRobotIdentity(robotId);
    map["hp"] = 0;
    map["robotId"] = robotId;
    map["identityName"] = identity.displayName;
    map["identityNumber"] = identity.normalizedId.value_or(0);
    map["identityTeam"] = identity.team ? (*identity.team == TeamColor::RED ? QStringLiteral("红方") : QStringLiteral("蓝方")) : QStringLiteral("未知阵营");
    map["maxHp"] = 100;
    map["isDead"] = true;
    map["isOffline"] = true;
    map["isClientConnected"] = false;
    map["isControllerConnected"] = false;
    map["status"] = static_cast<int>(RobotStatus::OFFLINE);
    map["isOutOfCombat"] = false;
    map["canRemoteHeal"] = false;
    map["canRemoteAmmo"] = false;
    map["canRemoteExchange"] = false;
    map["currentHeat"] = 0;
    map["heatLimit"] = 240;
    map["allowedAmmo17mm"] = 0;
    map["allowedAmmo42mm"] = 0;
    map["fortressBonusAmmo"] = 0;
    map["muzzleVelocity"] = 0.0;
    map["shootSpeedLimit"] = 25.0;
    map["tabShootSpeedLimit"] = 25;
    map["launchingFrequency"] = 0;
    map["level"] = 1;
    map["power"] = 0;
    map["bufferEnergy"] = 0;
    map["maxBufferEnergy"] = 60;
    map["posX"] = 0.0;
    map["posY"] = 0.0;
    map["x"] = 0.0;
    map["y"] = 0.0;
    map["angle"] = 0.0;
    map["isHighLight"] = false;
    map["hasVulnerability"] = false;
    map["heatCooling"] = 0;
    map["shooterLocked"] = false;
    map["shooterPermanentLocked"] = false;
    map["heatLockSeconds"] = 0;
    map["panelHp"] = 0;
    map["panelMaxHp"] = 0;
    map["panelIsConnected"] = false;
    map["panelIsOffline"] = true;
    map["panelIsDead"] = false;
    map["panelLevel"] = 0;
  }
  return map;
}

//获取增益
QVariantMap GameData::buffTimedData() const {
  const RobotData *robot = getCurrentRobot();

  QVariantMap result;
  //处理防御和负防御增益
  const auto *defenseOrVulnerability = timedBuffByType(robot, 2);
  const int defenseLevel =
      (defenseOrVulnerability && defenseOrVulnerability->level > 0)
          ? static_cast<int>(defenseOrVulnerability->level)
          : 0;
  const int vulnerabilityLevel =
      (defenseOrVulnerability && defenseOrVulnerability->level < 0)
          ? qAbs(static_cast<int>(defenseOrVulnerability->level))
          : 0;
  const int defenseOrVulnerabilityMaxTime =
      defenseOrVulnerability ? static_cast<int>(defenseOrVulnerability->maxTime) : 0;
  const int defenseOrVulnerabilityLeftTime =
      defenseOrVulnerability ? static_cast<int>(defenseOrVulnerability->leftTimeAtSync) : 0;

  result["attack"] = toBuffTimedVariant(1, timedBuffByType(robot, 1));
  result["defenseOrVulnerability"] = toBuffTimedVariant(2, timedBuffByType(robot, 2));
  result["defense"] =
      toBuffTimedVariant(2, defenseLevel, defenseOrVulnerabilityMaxTime,
                         defenseOrVulnerabilityLeftTime);
  result["vulnerability"] =
      toBuffTimedVariant(2, vulnerabilityLevel, defenseOrVulnerabilityMaxTime,
                         defenseOrVulnerabilityLeftTime);
  result["cooling"] = toBuffTimedVariant(3, timedBuffByType(robot, 3));
  result["chassisPower"] = toBuffTimedVariant(4, timedBuffByType(robot, 4));
  result["recovery"] = toBuffTimedVariant(5, timedBuffByType(robot, 5));
  result["remoteAmmo"] = toBuffTimedVariant(6, timedBuffByType(robot, 6));
  result["terrainPrewarn"] = toBuffTimedVariant(7, timedBuffByType(robot, 7));

  return result;
}

QVariantMap GameData::runeStatusData() const {
  const RobotData *robot = getCurrentRobot();
  const float displayAverageRings =
      std::isfinite(static_cast<double>(m_runeData.averageRings))
          ? m_runeData.averageRings
          : sanitizeFiniteRuneAverage(m_runeData.averageRings);

  QVariantMap result;
  result["status"] = m_runeData.status;
  result["activatedArms"] = m_runeData.activatedArms;
  result["averageRings"] = displayAverageRings;
  result["type"] = m_runeData.type;
  result["isActivatable"] = m_runeData.isActivatable;
  result["isRedTeam"] = robot ? QVariant(robot->team == TeamColor::RED)
                              : QVariant();
  result["totalRings"] =
      qRound(static_cast<double>(displayAverageRings) *
             static_cast<double>(m_runeData.activatedArms));//总环数


  return result;
}

QVariantMap GameData::dartMessageData() const {
  return m_dartMessageData;
}

/**
 * @brief 生成小地图机器人标记列表
 * @details 将所有在线机器人的位置、血量、队色等数据打包为 QVariantList 供 QML 使用。
 *          结构与 MiniMap.qml 的 markersModel 一致。
 */
QVariantList GameData::getMiniMapMarkers() const {
  QVariantList list;

  if (m_positionSource == QStringLiteral("ros_bridge")) {
    if (!isRosRadarFresh() || !m_mapProfile.hasValidSize()) {
      return list;
    }

    for (const RosRadarRobotPosition &robot : m_rosRadarSnapshot.robots) {
      const bool isRed = robot.color == 2;
      const int displayRobotId = isRed ? robot.radarId : 100 + robot.radarId;
      QVariantMap marker;
      marker["key"] = isRed ? robot.radarId : robot.radarId + 7;
      marker["robotId"] = displayRobotId;
      marker["radarColor"] = robot.color;
      marker["radarId"] = robot.radarId;
      marker["pixelX"] = robot.pixelX;
      marker["pixelY"] = robot.pixelY;
      marker["worldX"] = robot.worldX;
      marker["worldY"] = robot.worldY;
      marker["inBounds"] = robot.inBounds;
      marker["isRed"] = isRed;
      marker["posX"] = m_mapProfile.worldToNormalizedX(robot.worldX);
      marker["posY"] = m_mapProfile.worldToNormalizedY(robot.worldY);
      marker["angle"] = 0.0;
      marker["isHighLight"] = false;
      marker["isDead"] = false;
      list.append(marker);
    }
    return list;
  }

  //归一化(当前协议传来已经是标准化，如需要这里可改为实际场地的大小)
  constexpr qreal kMapWidthM = 28.0;
  constexpr qreal kMapHeightM = 15.0;

  auto normalizeCoord = [](qreal value, qreal maxMeters) -> qreal {
    qreal normalized = value;
    // 兼容两种输入：已经是归一化(0~1) 或 以米为单位(0~地图尺寸)
    if (normalized > 1.0 || normalized < 0.0) {
      normalized = (maxMeters > 0.0) ? (normalized / maxMeters) : 0.0;
    }
    if (normalized < 0.0)
      normalized = 0.0;
    if (normalized > 1.0)
      normalized = 1.0;
    return normalized;
  };

  // 此处只统一坐标比例，地图原点和阵营朝向由展示层转换。
  for (const auto &robot : m_robots) {
    // 忽略未更新或从未出现过的机器人
    if (robot.posX == 0 && robot.posY == 0 && robot.angle == 0) continue;

    QVariantMap m;
    // 1->1, 101->8 的逻辑保持一致
    m["key"] = (robot.robotId > 7) ? (robot.robotId - 100 + 7) : robot.robotId;
    m["robotId"] = static_cast<int>(robot.robotId);
    m["isRed"] = (robot.team == TeamColor::RED);
    m["posX"] = normalizeCoord(robot.posX, kMapWidthM);
    m["posY"] = normalizeCoord(robot.posY, kMapHeightM);
    m["angle"] = robot.angle;
    m["isHighLight"] = robot.isHighLight;
    m["isDead"] = robot.currentHP <= 0;

    list.append(m);
  }
  return list;
}

bool GameData::isRosRadarFresh() const {
  if (m_rosRadarLastUpdateMs <= 0 || m_mapProfile.radarStaleTimeoutMs <= 0) {
    return false;
  }
  return QDateTime::currentMSecsSinceEpoch() - m_rosRadarLastUpdateMs <=
         m_mapProfile.radarStaleTimeoutMs;
}

void GameData::setMapProfile(const MapProfile &profile) {
  m_mapProfile = profile;
  rebuildSentryPathCoordinates();
  emit mapProfileChanged();
  emit sentryPathUpdated();
  emit miniMapMarkersChanged();
}

void GameData::rebuildSentryPathCoordinates() {
  const MapProfile profile =
      m_positionSource == QStringLiteral("ros_bridge")
          ? m_mapProfile
          : MapProfile::officialField();
  if (!profile.hasValidSize()) {
    m_allySentryPath.startX = 0.0f;
    m_allySentryPath.startY = 0.0f;
    m_allySentryPath.pathDeltasX.clear();
    m_allySentryPath.pathDeltasY.clear();
    return;
  }

  const double startWorldX = static_cast<double>(m_allySentryPath.rawStartDmX) / 10.0;
  const double startWorldY = static_cast<double>(m_allySentryPath.rawStartDmY) / 10.0;
  m_allySentryPath.startX =
      static_cast<float>(profile.worldToNormalizedX(startWorldX));
  m_allySentryPath.startY =
      static_cast<float>(profile.worldToNormalizedY(startWorldY));

  m_allySentryPath.pathDeltasX.clear();
  m_allySentryPath.pathDeltasX.reserve(m_allySentryPath.rawDeltasDmX.size());
  for (qint32 delta : m_allySentryPath.rawDeltasDmX) {
    m_allySentryPath.pathDeltasX.append(
        static_cast<float>((static_cast<double>(delta) / 10.0) /
                           profile.widthM));
  }
  m_allySentryPath.pathDeltasY.clear();
  m_allySentryPath.pathDeltasY.reserve(m_allySentryPath.rawDeltasDmY.size());
  for (qint32 delta : m_allySentryPath.rawDeltasDmY) {
    m_allySentryPath.pathDeltasY.append(
        static_cast<float>((static_cast<double>(delta) / 10.0) /
                           profile.heightM));
  }
}

void GameData::setPositionSource(const QString &source) {
  const QString normalized = source.trimmed().toLower();
  const QString accepted = normalized == QStringLiteral("ros_bridge")
                               ? QStringLiteral("ros_bridge")
                               : QStringLiteral("official");
  if (m_positionSource == accepted) {
    return;
  }
  m_positionSource = accepted;
  rebuildSentryPathCoordinates();
  qInfo() << "[MiniMap] position_source=" << m_positionSource;
  emit positionSourceChanged();
  emit sentryPathUpdated();
  emit miniMapMarkersChanged();
}

void GameData::updateRosRadarPositions(const RosRadarSnapshot &snapshot) {
  m_rosRadarSnapshot = snapshot;
  m_rosRadarLastUpdateMs = QDateTime::currentMSecsSinceEpoch();
  m_rosRadarFresh = true;
  if (m_positionSource == QStringLiteral("ros_bridge")) {
    emit miniMapMarkersChanged();
  }
}

/**
 * @brief 生成红方机器人列表（供 QML TabStatsPanel 使用）
 * @details 从内部 m_robots 数据中筛选红方机器人，并转换为 QVariantList。
 *          字段与 QML 一致：number, name, icon, currentHP, maxHP,
 *          chassisPower, heatLimit, heatCooling, shootSpeedLimit, level。
 */
QVariantList GameData::getRedRobotsDataQml() const {
  QVariantList list;

  for (const auto &robot : m_robots) {
    if (robot.team != TeamColor::RED) continue;
    const bool isConnected = robotConnectedForTab(robot);
    const bool useGlobalHp =
        robot.hasTabGlobalSnapshot && robot.tabGlobalCurrentHP > 0;
    const int maxHp = isConnected
                          ? static_cast<int>(robot.maxHP > 0
                                                 ? robot.maxHP
                                                 : robotTabDefaultMaxHp(robot.type))
                          : 0;
    const int level =
        isConnected ? std::max(1, static_cast<int>(robot.level)) : 0;
    QVariantMap m;
    m["number"] = static_cast<int>(robot.robotId);                  // 红方号码（1-7）
    m["name"] = getRobotName(robot.robotId);                               // 中文类型名
    m["icon"] = robotTabAvatarSource(robot);                        // 红方头像
    m["currentHP"] = isConnected
                         ? static_cast<int>(useGlobalHp
                                                ? robot.tabGlobalCurrentHP
                                                : robot.currentHP)
                         : 0;             // 当前血量
    m["maxHP"] = maxHp;                                                              // 最大血量
    m["chassisPower"] = isConnected ? static_cast<int>(robot.maxPower) : 0;           // 底盘功率上限
    m["heatLimit"] = isConnected ? static_cast<int>(robot.heatLimit) : 0;             // 热量上限
    m["heatCooling"] = isConnected ? static_cast<int>(robot.coolingValue) : 0;        // 热量冷却
    m["shootSpeedLimit"] = isConnected
                               ? static_cast<int>(robot.tabShootSpeedLimit > 0
                                                      ? robot.tabShootSpeedLimit
                                                      : robotTabShootSpeedLimit(robot.type))
                               : 0; // 射速上限（m/s）
    m["level"] = level;                                                               // 等级
    m["isConnected"] = isConnected;
    list.append(m);
  }

  return list;
}

/**
 * @brief 生成蓝方机器人列表（供 QML TabStatsPanel 使用）
 * @details 与红方同逻辑，但号码为 robotId % 100，图标使用蓝方前缀。
 */
QVariantList GameData::getBlueRobotsDataQml() const {
  QVariantList list;

  for (const auto &robot : m_robots) {
    if (robot.team != TeamColor::BLUE) continue;
    const bool isConnected = robotConnectedForTab(robot);
    const bool useGlobalHp =
        robot.hasTabGlobalSnapshot && robot.tabGlobalCurrentHP > 0;
    const int maxHp = isConnected
                          ? static_cast<int>(robot.maxHP > 0
                                                 ? robot.maxHP
                                                 : robotTabDefaultMaxHp(robot.type))
                          : 0;
    const int level =
        isConnected ? std::max(1, static_cast<int>(robot.level)) : 0;
    QVariantMap m;
    m["number"] = static_cast<int>(robot.robotId);
    m["name"] = getRobotName(robot.robotId);
    m["icon"] = robotTabAvatarSource(robot);                         // 蓝方头像
    m["currentHP"] = isConnected
                         ? static_cast<int>(useGlobalHp
                                                ? robot.tabGlobalCurrentHP
                                                : robot.currentHP)
                         : 0;
    m["maxHP"] = maxHp;
    m["chassisPower"] = isConnected ? static_cast<int>(robot.maxPower) : 0;
    m["heatLimit"] = isConnected ? static_cast<int>(robot.heatLimit) : 0;
    m["heatCooling"] = isConnected ? static_cast<int>(robot.coolingValue) : 0;
    m["shootSpeedLimit"] = isConnected
                               ? static_cast<int>(robot.tabShootSpeedLimit > 0
                                                      ? robot.tabShootSpeedLimit
                                                      : robotTabShootSpeedLimit(robot.type))
                               : 0; // 射速上限（m/s）
    m["level"] = level;
    m["isConnected"] = isConnected;
    list.append(m);
  }

  return list;
}


/**
 * @brief 红/蓝方总伤害
 */
int GameData::redTotalDamage() const { return static_cast<int>(m_redTeamStats.totalDamageDealt); }
int GameData::blueTotalDamage() const { return static_cast<int>(m_blueTeamStats.totalDamageDealt); }

/**
 * @brief 飞镖命中统计（协议接入前返回占位值）
 */
int GameData::redDartHits() const { return m_redDartHits; }   // 红方飞镖命中次数（累计）
int GameData::blueDartHits() const { return m_blueDartHits; }  // 蓝方飞镖命中次数（累计）
int GameData::dartTotal() const { return RM::Dart::dartTotal; }

/**
 * @brief ~ 面板伤害统计访问器
 */
QVariantList GameData::getModuleData() const {
  QVariantList list;
  for (const auto &module : m_moduleStatus) {
    QVariantMap m;
    m["name"] = QString::fromUtf8(module.moduleName);
    m["online"] = static_cast<bool>(module.isOnline);
    list.append(m);
  }
  return list;
}

/**
 * @brief 获取模块状态整数数组 (0=在线, 1=离线)
 * @details 供 LeftBottomPanel 的模块状态指示灯使用。
 *          对应顺序: 机器人, 遥控器, 自定义, 图传, RFID, 模块, UWB, 底盘, 17mm, 42mm??
 */
QVariantList GameData::moduleStates() const {
  QVariantList list;
  // 预填充 10 个离线状态 (1 代表离线/异常，0 代表正常)
  for (int i = 0; i < 10; ++i)
    list.append(1);

  // 如果没有收到 RobotModuleStatus，直接返回全离线。
  if (m_robotModuleStatusMap.isEmpty())
    return list;

  // RobotModuleStatus 协议字段语义是 1=在线、0=离线；LeftBottom 灯位沿用旧语义 0=在线、1=离线。
  auto legacyState = [this](const char *fieldName) -> int {
    return m_robotModuleStatusMap.value(QString::fromLatin1(fieldName), 0).toUInt() == 1 ? 0 : 1;
  };

  // 机器人、遥控器、底盘当前没有对应 RobotModuleStatus 字段，按离线保留默认值。

  list[3]=legacyState("video_transmission");     // 图传
  list[4]=legacyState("rfid");                   // RFID识别
  // 模块：沿用 ~ 面板模块明细，只要有一项离线/缺字段就是异常。
  int moduleStatus = 0;
  for (const auto &module : m_moduleStatus) {
    if (!module.isOnline) {
      moduleStatus = 1;
      break;
    }
  }
  list[5]=moduleStatus;                         // 模块
  list[6]=legacyState("uwb");                   // UWB


  return list;
}

int GameData::damage17mm() const { return static_cast<int>(m_injury17mm); }
int GameData::damage42mm() const { return static_cast<int>(m_injury42mm); }
int GameData::damageCollision() const { return static_cast<int>(m_injuryCollision); }
int GameData::damageOffline() const { return static_cast<int>(m_injuryOffline); }
int GameData::damageWarning() const { return static_cast<int>(m_injuryWarning); }

static inline int pct(quint32 part, quint32 total) {
  if (total == 0) return 0;
  return static_cast<int>((part * 100) / total);
}
int GameData::damage17mmPercent() const { return pct(m_injury17mm, m_injuryTotal); }
int GameData::damage42mmPercent() const { return pct(m_injury42mm, m_injuryTotal); }
int GameData::damageCollisionPercent() const { return pct(m_injuryCollision, m_injuryTotal); }
int GameData::damageOfflinePercent() const { return pct(m_injuryOffline, m_injuryTotal); }
int GameData::damageWarningPercent() const { return pct(m_injuryWarning, m_injuryTotal); }

/**
 * @brief 机器人复活后重置 ~ 面板伤害统计与比例
 * @details 清零 RobotInjuryStat 映射的所有类别，并触发界面刷新。
 */
void GameData::resetInjuryStatsOnRespawn() {
  m_injuryTotal = 0;
  m_injury17mm = 0;
  m_injury42mm = 0;
  m_injuryCollision = 0;
  m_injuryOffline = 0;
  m_injuryWarning = 0;
  emit injuryStatsUpdated();
  emitDataChanged();
}

/**
 * @brief 更新机器人数据
 * @details 根据接收到的 Protobuf 消息更新指定机器人的状态。
 * @param data Protobuf 格式的机器人状态消息
 */
void GameData::updateRobotData(const robomaster::RobotStatus &data) {
    updateRobotDataImpl(data, false);
}

void GameData::updateLegacyRobotData(const robomaster::RobotStatus &data) {
    updateRobotDataImpl(data, true);
}

void GameData::updateRobotDataImpl(const robomaster::RobotStatus &data, bool legacyTag) {
    // 0、200、201 是旧 UDP 转换器的内部数据标签，不是机器人身份。
    const quint32 id = data.id();
    const bool isSelfUpdate = legacyTag && (id == 0 || id == 200 || id == 201);
    const auto identity = RM::resolveRobotIdentity(isSelfUpdate ? m_currentRobotId : id);
    if (identity.kind != RM::IdentityKind::Robot || !identity.role || !identity.team)
        return;
    const quint8 targetId = static_cast<quint8>(identity.rawId);

    RobotData *robot = findRobotById(targetId);
    if (!robot) return;

    // 2. 根据包类型/ID更新数据

    //非全量数据
    // [0x0202]枪口当前热量、底盘缓冲能量(ID=0)
    if (id == 0) {
        // 更新当前热量
        updateRobotHeat(targetId, data.heat());
        // 如果有底盘功率和缓冲能量数据
        if (data.buffer_energy() > 0 || data.chassis_power() > 0) {
            robot->isClientConnected = true;
            robot->power = static_cast<quint16>(data.chassis_power());
            robot->bufferEnergy = data.buffer_energy();
            robot->currentChassisEnergy = robot->power;
        }
    }
    // [0x0208] 弹量数据(ID=200)
    else if (id == 200) {
        robot->isClientConnected = true;
        robot->allowedAmmo17mm = static_cast<quint16>(data.allowed_ammo_17mm());
        robot->allowedAmmo42mm = static_cast<quint16>(data.allowed_ammo_42mm());
        robot->fortressBonusAmmo = static_cast<quint16>(data.fortress_ammo());
    }
    // [0x0207] 射击数据(ID=201)
    else if (id == 201) {
        robot->isClientConnected = true;
        robot->firerate = data.muzzle_velocity();
      // SHOOT_DATA 包携带的是初速度（initial_speed，m/s），
      // 应写入 muzzleVelocity 而不是 firerate（发/秒）。
      robot->muzzleVelocity = data.muzzle_velocity();
        robot->shootSpeedLimit = robotShootSpeedLimit(robot->type);
        robot->tabShootSpeedLimit = robotTabShootSpeedLimit(robot->type);
        // 射击包只刷新初速度和上限，不在此处触发超限判定。
    }
    // [0x0201] RobotStatus (常规包)
    else {
        // 调用全量更新逻辑
        updateStandardStatus(robot, data);

        // 常规包也可能包含射速(虽然通常为0)，如果有则检查
        // 但根据协议定义，0x0201不含射速，这里仅作防御性编程
        if (data.muzzle_velocity() > 0) {
             robot->muzzleVelocity = data.muzzle_velocity();
             checkSpeedLimit(robot, false); // 更新射速上限，但不触发判罚逻辑
        }
        robot->shootSpeedLimit = robotShootSpeedLimit(robot->type);
        robot->tabShootSpeedLimit = robotTabShootSpeedLimit(robot->type);

    }

    emit robotDataUpdated(targetId);
    if (targetId == m_currentRobotId) {
        emit myRobotUpdated();
    }
    emitDataChanged();
}

/**
 * @brief 更新基地数据
 * @param data 基地血量数据
 */
/*
void GameData::updateBaseData(const BaseHealthData &data) {
  TeamColor team = static_cast<TeamColor>(data.team);
  if (team == TeamColor::RED) {
    m_redBase.currentHP = data.currentHP;
    m_redBase.maxHP = data.maxHP;
    m_redBase.virtualShield = data.virtualShield;
    m_redBase.isInvincible = data.isInvincible;
    m_redBase.status = data.status; // 更新基地状态
    m_redBase.lastUpdateTime = QDateTime::currentDateTime();
  } else {
    m_blueBase.currentHP = data.currentHP;
    m_blueBase.maxHP = data.maxHP;
    m_blueBase.virtualShield = data.virtualShield;
    m_blueBase.isInvincible = data.isInvincible;
    m_blueBase.status = data.status; // 更新基地状态
    m_blueBase.lastUpdateTime = QDateTime::currentDateTime();
  }
  emit baseHealthUpdated(team);
  emitDataChanged();
}
*/

void GameData::updateOutpostHealth(TeamColor team, quint16 currentHP) {
  OutpostData &outpost =
      (team == TeamColor::RED) ? m_redOutpostData : m_blueOutpostData;

  quint16 prevHP = outpost.currentHP;
  outpost.currentHP = currentHP;
  outpost.isDestroyed = (currentHP == 0);
  outpost.lastUpdateTime = QDateTime::currentDateTime();

  emit outpostHealthUpdated(team);
  emitDataChanged();
}

// 基础状态更新

void GameData::updateBaseHP(TeamColor team, quint16 hp) {
    BaseData &base = (team == TeamColor::RED) ? m_redBase : m_blueBase;
    quint16 prevHP = base.currentHP;
    base.currentHP = hp;
    base.lastUpdateTime = QDateTime::currentDateTime();
    trackAllyBaseHealthDropAlert(team, hp, base.lastUpdateTime.toMSecsSinceEpoch());

    // 当血量从 >0 变为 0 时，触发击毁相关信号
    if (prevHP > 0 && hp == 0) {
        // 目前暂无专门的 baseDestroyed 信号，维持 baseHealthUpdated
    }

    emit baseHealthUpdated(team);
}

void GameData::trackAllyBaseHealthDropAlert(TeamColor team, quint16 hp, qint64 nowMs) {
  const RobotData *current = getCurrentRobot();
  if (!current || team != current->team) {
    return;
  }

  QList<BaseHealthSample> &samples =
      (team == TeamColor::RED) ? m_redBaseHealthSamples : m_blueBaseHealthSamples;

  samples.append(BaseHealthSample{nowMs, hp});
  while (samples.size() > 1 && samples.first().timestampMs < nowMs - 2000) {
    samples.removeFirst();
  }

  const BaseHealthSample *referenceSample = nullptr;
  for (const BaseHealthSample &sample : samples) {
    if (sample.timestampMs <= nowMs - 1000) {
      referenceSample = &sample;
    } else {
      break;
    }
  }
  if (!referenceSample) {
    return;
  }

  const int hpDrop = static_cast<int>(referenceSample->hp) - static_cast<int>(hp);
  if (hpDrop < 15) {
    return;
  }

  if (m_allyBaseLastDropAlertMs > 0 && nowMs - m_allyBaseLastDropAlertMs < 5000) {
    return;
  }

  m_allyBaseLastDropAlertMs = nowMs;
  emit allyBaseHealthDropAlertTriggered();
}

void GameData::updateBaseStatus(TeamColor team, quint8 status) {
    BaseData &base = (team == TeamColor::RED) ? m_redBase : m_blueBase;
    bool &statusObserved = (team == TeamColor::RED)
                               ? m_redBaseStatusObserved
                               : m_blueBaseStatusObserved;
    const bool wasObserved = statusObserved;
    quint8 prevStatus = base.status;
    base.status = status;
    statusObserved = true;

    // 0：无敌；1：护盾关闭；2：装甲展开。
    base.isInvincible = (status == 0);
    base.lastUpdateTime = QDateTime::currentDateTime();

    if (prevStatus != status) {
        const RobotData *current = getCurrentRobot();
        if (current && wasObserved && status == 2 && team == current->team &&
            m_gameState.gameProgress == GameStage::BATTLE) {
            emit allyBaseArmorOpenedTriggered();
        }

        // 当状态从 0 变为非 0 时，或者发生改变时，通知 UI
        // BattleMessageWidget 通过 showBaseStatusChange 展示状态变化。
        // 仅在非初始状态或有意义的变化时发送
        if (status > 0 || prevStatus > 0) {
            emit baseStatusChanged(team == TeamColor::RED, status);
        }
        emit baseHealthUpdated(team);
    }
}

void GameData::updateOutpostHP(TeamColor team, quint16 hp) {
    OutpostData &outpost = (team == TeamColor::RED) ? m_redOutpostData : m_blueOutpostData;
    outpost.currentHP = hp;
    outpost.lastUpdateTime = QDateTime::currentDateTime();
    trackAllyOutpostHealthDropAlert(team, hp, outpost.lastUpdateTime.toMSecsSinceEpoch());

    // 仅更新血量，状态流转和摧毁判定完全交给 updateOutpostStatus (MQTT)
    // 注意：这里不再设置 isDestroyed，因为 isDestroyed 现在应当与 status 强绑定

    emit outpostHealthUpdated(team);
}

void GameData::trackAllyOutpostHealthDropAlert(TeamColor team, quint16 hp, qint64 nowMs) {
  const RobotData *current = getCurrentRobot();
  if (!current || team != current->team) {
    return;
  }

  QList<OutpostHealthSample> &samples =
      (team == TeamColor::RED) ? m_redOutpostHealthSamples : m_blueOutpostHealthSamples;

  samples.append(OutpostHealthSample{nowMs, hp});
  while (samples.size() > 1 &&
         samples.first().timestampMs <
             nowMs - RM::Outpost::HEALTH_DROP_ALERT_SAMPLE_RETENTION_MS) {
    samples.removeFirst();
  }

  const OutpostHealthSample *referenceSample = nullptr;
  for (const OutpostHealthSample &sample : samples) {
    if (sample.timestampMs <=
        nowMs - RM::Outpost::HEALTH_DROP_ALERT_WINDOW_MS) {
      referenceSample = &sample;
    } else {
      break;
    }
  }
  if (!referenceSample) {
    return;
  }

  const int hpDrop = static_cast<int>(referenceSample->hp) - static_cast<int>(hp);
  if (hpDrop < RM::Outpost::HEALTH_DROP_ALERT_THRESHOLD) {
    return;
  }

  // B 距上次有效提示 A 不足 15 秒时，直接丢弃 B，
  // 且不用 B 覆盖 A；下一个候选仍与 A 比较。
  if (m_allyOutpostLastAcceptedAlertMs > 0 &&
      nowMs - m_allyOutpostLastAcceptedAlertMs <
          RM::Outpost::HEALTH_DROP_ALERT_COOLDOWN_MS) {
    return;
  }

  m_allyOutpostLastAcceptedAlertMs = nowMs;
  emit allyOutpostHealthDropAlertTriggered();
}

void GameData::updateOutpostStatus(TeamColor team, quint8 status) {
    OutpostData &outpost = (team == TeamColor::RED) ? m_redOutpostData : m_blueOutpostData;
    quint8 prevStatus = outpost.status;
    outpost.status = status;
    outpost.lastUpdateTime = QDateTime::currentDateTime();

    // 根据状态更新 isDestroyed 标记
    // 3:被击毁不可重建, 4:被击毁可重建, 5:被击毁重建中
    bool currentDestroyed = (status == 3 || status == 4 || status == 5);
    outpost.isDestroyed = currentDestroyed;

    // 重建次数追踪: 状态 5(重建中)→存活(0/1/2) = 完成一次重建, 次数-1
    if (prevStatus == 5 && status >= 0 && status <= 2) {
      if (outpost.rebuildCount > 0) {
        outpost.rebuildCount--;
      }
    }

    if (prevStatus != status) {
        emit outpostStatusChanged(team == TeamColor::RED, status);
        emit outpostHealthUpdated(team);
        emit outpostRebuildCountChanged();
    }
}

// --------------------------------------------------
//旧UDP绑定相关代码，统一更改为以-internal为后缀

void GameData::updateGlobalUnitStatusInternal(const robomaster::GlobalUnitStatusInternal &data) {
  // 1. 更新红方基地
  updateBaseHP(TeamColor::RED, data.red_base_health());
  updateBaseStatus(TeamColor::RED, data.red_base_status());
  // 红方虚拟护盾
  m_redBase.virtualShield = static_cast<quint16>(data.red_base_shield());

  // 2. 更新蓝方基地
  updateBaseHP(TeamColor::BLUE, data.blue_base_health());
  updateBaseStatus(TeamColor::BLUE, data.blue_base_status());
  // 蓝方虚拟护盾
  m_blueBase.virtualShield = static_cast<quint16>(data.blue_base_shield());

  // 3. 更新红方前哨站
  updateOutpostHP(TeamColor::RED, data.red_outpost_health());
  updateOutpostStatus(TeamColor::RED, data.red_outpost_status());

  // 4. 更新蓝方前哨站
  updateOutpostHP(TeamColor::BLUE, data.blue_outpost_health());
  updateOutpostStatus(TeamColor::BLUE, data.blue_outpost_status());

  // 5. 更新双方机器人
  auto updateRobots = [this](const auto &healthList, TeamColor team) {
    // 六项列表依次对应英雄、工程、步兵 3、步兵 4、空中机器人和哨兵。
    // 旧版五项列表没有空中机器人槽位，需要单独兼容。
    int robotIds[] = {1, 2, 3, 4, 6, 7};
    int baseId = (team == TeamColor::BLUE) ? 100 : 0;

    for (int i = 0; i < healthList.size(); ++i) {
      if (i >= 6) break;

      // 五项列表使用 {1,2,3,4,7}，六项列表使用 {1,2,3,4,6,7}。
      int typeId = 0;
      if (healthList.size() == 5) {
           int typeIds[] = {1, 2, 3, 4, 7};
           if (i < 5) typeId = typeIds[i];
      } else {
           if (i < 6) typeId = robotIds[i];
      }

      if (typeId == 0) continue;

      int finalId = baseId + typeId;
      RobotData *robot = findRobotById(finalId);
      if (robot) {
        quint16 prevHP = robot->currentHP;
        robot->currentHP = healthList[i];
        robot->currentRoundMaxHP =
            qMax(robot->currentRoundMaxHP, robot->currentHP);
        robot->lastUpdateTime = QDateTime::currentDateTime();

        // GlobalUnitStatus 是血量快照，不代表该机器人已经进入赛事引擎列表。
        // 顶部机器人列表的展示资格必须来自 RobotStaticStatus.field_state。
        if (robot->isClientConnected || robot->isControllerConnected) {
          robot->status = robot->currentHP > 0 ? RobotStatus::NORMAL
                                               : RobotStatus::DESTROYED;
        }

        if (robot->currentHP < prevHP && prevHP > 0) {
          robot->isOutOfCombat = false;
          robot->lastDamageTime = QDateTime::currentDateTime();
        }
        emit robotDataUpdated(finalId);
      }
    }
  };

  updateRobots(data.red_robot_health(), TeamColor::RED);
  updateRobots(data.blue_robot_health(), TeamColor::BLUE);

  // 6. 更新双方累计伤害
  m_redTeamStats.totalDamageDealt = data.total_damage_red();
  m_blueTeamStats.totalDamageDealt = data.total_damage_blue();
  emit injuryStatsUpdated();

  emitDataChanged();
}

void GameData::updateRobotStaticStatus(const robomaster::RobotStaticStatus &data) {
  const auto identity = RM::resolveRobotIdentity(data.robot_id());
  if (identity.kind != RM::IdentityKind::Robot || !identity.role || !identity.team) {
    qWarning() << "[RobotStaticStatus] Invalid robot id=" << data.robot_id()
               << "raw robot_type=" << data.robot_type();
    return;
  }
  const int robotId = static_cast<int>(identity.rawId);
  QVariantMap referee{{"received", true}, {"robotId", robotId}, {"source", "protocol_unverified"}};
  if (data.has_connection_state()) referee["connection_state"] = data.connection_state();
  if (data.has_field_state()) referee["field_state"] = data.field_state();
  if (data.has_alive_state()) referee["alive_state"] = data.alive_state();
  m_refereeRobotObservations[robotId] = referee;
  qDebug() << "[RobotStaticStatus] robotId=" << robotId
           << "raw robot_type=" << data.robot_type();
  if (*identity.role == RobotType::DART && data.has_connection_state()) {
    const bool newDartOnline = data.connection_state() == 1;
    const bool changed = !m_hasDartRobotConnectionState || m_dartRobotOnline != newDartOnline;
    m_hasDartRobotConnectionState = true;
    m_dartRobotOnline = newDartOnline;
    if (changed) emit siloStatusChanged();
  }
  RobotData *robot = findRobotById(robotId);
  if (!robot) return;

  //当等级发生变化时，发出系统消息
  if (data.has_level() && data.level() <= 255 && robot->level != data.level() && data.level()!=0) {
    addSystemMessage(
                  QStringLiteral("当前机器人已升到%1级")
                      .arg(data.level()),
                  "#60D8B7");
  }

  if (robot) {
    // Store the latest reported fields separately from previously accepted values.
    // Missing or invalid values never overwrite the effective parameters.
    QVariantMap observation;
    observation["received"] = true;
#define OBSERVE(field) if (data.has_##field()) observation[QStringLiteral(#field)] = data.field()
    OBSERVE(performance_system_shooter); OBSERVE(performance_system_chassis);
    OBSERVE(max_health); OBSERVE(max_heat); OBSERVE(heat_cooldown_rate);
    OBSERVE(max_power); OBSERVE(max_buffer_energy); OBSERVE(max_chassis_energy);
#undef OBSERVE
    robot->staticPerformanceObservation = observation;
    if (data.has_level() && data.level() <= 255 && data.level() > 0)
      robot->level = static_cast<quint8>(data.level());
    if (data.has_max_health() && data.max_health() > 0 && data.max_health() <= 65535) {
      robot->maxHP = static_cast<quint16>(data.max_health());
      robot->currentRoundMaxHP = qMax(robot->currentRoundMaxHP, robot->maxHP);
    }
    if (data.has_max_heat() && data.max_heat() <= 65535)
      robot->heatLimit = static_cast<quint16>(data.max_heat());
    if (data.has_heat_cooldown_rate() && std::isfinite(data.heat_cooldown_rate()) &&
        data.heat_cooldown_rate() >= 0 && data.heat_cooldown_rate() <= 65535)
      robot->coolingValue = data.heat_cooldown_rate();
    if (data.has_max_power() && data.max_power() <= 65535)
      robot->maxPower = static_cast<quint16>(data.max_power());
    if (data.has_max_buffer_energy() && data.max_buffer_energy() <= 65535)
      robot->maxBufferEnergy = static_cast<quint16>(data.max_buffer_energy());
    if (data.has_max_chassis_energy() && data.max_chassis_energy() <= 65535)
      robot->maxChassisEnergy = static_cast<quint16>(data.max_chassis_energy());
    if (data.has_performance_system_shooter() && data.performance_system_shooter() >= 1 && data.performance_system_shooter() <= 2)
      robot->shooterPerformanceSelection = static_cast<quint8>(data.performance_system_shooter());
    if (data.has_performance_system_chassis() && data.performance_system_chassis() >= 1 && data.performance_system_chassis() <= 2)
      robot->chassisPerformanceSelection = static_cast<quint8>(data.performance_system_chassis());
    const bool previousEngineConnected =
        robot->isClientConnected || robot->isControllerConnected;
    const RobotStatus previousStatus = robot->status;
    const bool engineConnected = data.has_connection_state()
                                     ? data.connection_state() != 0
                                     : previousEngineConnected;
    if (data.has_connection_state()) {
      robot->tabStaticConnected = (data.connection_state() == 1);
      robot->isClientConnected = engineConnected;
      robot->isControllerConnected = engineConnected;
    }
    // Performance mode is configuration, not a chassis/shooter enable signal.

    // RobotStaticStatus 只要下发就说明机器人在场内已注册。
    // connection_state=0 只表示硬件连接暂时断开，不应直接标记为 OFFLINE。
    // 只有 field_state==1（不在场地清单中）才是真正的离线。
    // currentHP==0 不作为 DESTROYED 条件：非当前机器人的 HP 来自 RobotDynamicStatus
    // 而它只更新 m_currentRobotId，队友机器人只靠 alive_state 判断存亡。
    if (data.has_field_state() && data.field_state() == 1) {
      robot->status = RobotStatus::OFFLINE;
    } else if (data.has_alive_state() && data.alive_state() == 2) {
      robot->status = RobotStatus::DESTROYED;
    } else if (data.has_alive_state() && data.alive_state() == 1) {
      robot->status = RobotStatus::NORMAL;
    }

    if (previousEngineConnected != engineConnected ||
        previousStatus != robot->status) {
      qInfo() << "[RobotStaticStatus] bot=" << robotId
              << "conn=" << data.connection_state()
              << "field=" << data.field_state()
              << "alive=" << data.alive_state()
              << "status=" << static_cast<int>(robot->status);
    }

    robot->lastUpdateTime = QDateTime::currentDateTime();
    checkHeatLimit(robot);

    emit robotDataUpdated(robotId);
    if (robotId == m_currentRobotId) {
      emit myRobotUpdated();
    }
    emitDataChanged();

}
}

QVariantMap GameData::globalHealthObservation() const {
  return {{"received", m_globalHealthReceived}, {"receiverId", m_globalHealthReceiver},
          {"identityMatches", m_globalHealthIdentityEpoch == m_mqttIdentityEpoch && m_globalHealthReceiver == m_currentRobotId},
          {"source", m_globalHealthMqttVerified ? "official_mqtt" : "protocol_unverified"},
          {"fields", m_globalHealthFields}, {"accepted", m_globalHealthAccepted}, {"acceptedSource", m_globalHealthAcceptedSource}};
}
QVariantMap GameData::refereeRobotObservation(int identity) const {
  return m_refereeRobotObservations.value(identity);
}
QVariantMap GameData::legacyGameProjection() const {
  return {{"current_stage", static_cast<int>(m_gameState.gameProgress)}, {"stage_countdown_sec", m_gameState.gameTime},
          {"red_score", m_gameState.redScore}, {"blue_score", m_gameState.blueScore},
          {"current_round", m_gameState.currentRound}, {"is_paused", m_gameState.isPaused}};
}
void GameData::setVideoReceptionState(int state) {
  if (state < -1 || state > 1 || m_videoReceptionState == state) return;
  m_videoReceptionState = state; emit videoReceptionStateChanged();
}
void GameData::recordLocalHeatObservation(int identity, float value, const QString &source) {
  if (source != QLatin1String("debug_test") && source != QLatin1String("simulator") && source != QLatin1String("local_input")) return;
  m_localHeatObservation = {{"robotId", identity}, {"value", value}, {"source", source}, {"recordedAt", QDateTime::currentDateTime()}};
  emit localHeatObservationChanged();
}

void GameData::setMqttSessionState(bool connected, const QString &clientId) {
  if (connected && !m_mqttSessionConnected) ++m_mqttSession;
  m_mqttSessionConnected = connected;
  if (connected) m_mqttSessionIdentity = clientId;
  emit mqttReceptionMetadataChanged();
}

QVariantMap GameData::mqttReceptionMetadata() const {
  auto annotate = [&](QVariantMap record, bool identitySensitive) {
    if (record.isEmpty()) return QVariantMap{{"received", false}, {"sessionState", "never"}};
    QString state = !m_mqttSessionConnected ? "disconnected" :
        record.value("session").toULongLong() != m_mqttSession ? "historical" :
        identitySensitive && (record.value("identity").toInt() != m_currentRobotId ||
            record.value("identityEpoch").toULongLong() != m_mqttIdentityEpoch) ? "identity_changed" : "current";
    record["sessionState"] = state;
    const qint64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    record["ageMs"] = std::max<qint64>(0, now - record.value("receivedMonotonicMs").toLongLong());
    return record;
  };
  QVariantMap topics;
  for (const char *name : {"GameStatus", "RobotDynamicStatus", "RobotModuleStatus", "GlobalUnitStatus", "Event16"}) {
    const QString key = QString::fromLatin1(name);
    topics[key] = annotate(m_mqttTopicMetadata.value(key).toMap(),
        key == QLatin1String("RobotDynamicStatus") || key == QLatin1String("RobotModuleStatus") || key == QLatin1String("GlobalUnitStatus"));
  }
  QVariantMap staticRobots;
  for (auto it = m_mqttStaticMetadata.cbegin(); it != m_mqttStaticMetadata.cend(); ++it)
    staticRobots[it.key()] = annotate(it.value().toMap(), false);
  return {{"connected", m_mqttSessionConnected}, {"session", QVariant::fromValue(m_mqttSession)},
          {"clientId", m_mqttSessionIdentity}, {"configuredIdentity", m_currentRobotId},
          {"topics", topics}, {"staticByRobot", staticRobots}};
}

void GameData::recordMqttObservation(const QString &topic, int identity, qint64 receivedMs, qint64 monotonicMs) {
  if (!m_mqttSessionConnected || receivedMs <= 0) return;
  const bool isStatic = topic == QLatin1String("RobotStaticStatus");
  const bool isIdentityScoped = topic == QLatin1String("RobotDynamicStatus") || topic == QLatin1String("RobotModuleStatus") || topic == QLatin1String("GlobalUnitStatus");
  if (isIdentityScoped) {
    const auto resolved = RM::resolveRobotIdentity(identity);
    if (resolved.kind != RM::IdentityKind::Robot || !resolved.team) return;
  }
  if (isStatic && !getRobotById(identity)) return;
  const QString key = isStatic ? QString::number(identity) : topic;
  QVariantMap &container = isStatic ? m_mqttStaticMetadata : m_mqttTopicMetadata;
  QVariantMap record = container.value(key).toMap();
  QVariantMap fieldUpdates = record.value("fieldUpdates").toMap();
  // No-ID fields belong to their observed identity, never a newly selected robot.
  if (isIdentityScoped &&
      record.contains("identity") && (record.value("identity").toInt() != identity ||
      record.value("identityEpoch").toULongLong() != m_mqttIdentityEpoch)) fieldUpdates.clear();
  record["received"] = true;
  record["receivedAt"] = QDateTime::fromMSecsSinceEpoch(receivedMs);
  record["receivedMonotonicMs"] = monotonicMs;
  record["session"] = QVariant::fromValue(m_mqttSession);
  record["identity"] = identity;
  record["identityEpoch"] = QVariant::fromValue(m_mqttIdentityEpoch);
  record["source"] = QStringLiteral("official_mqtt");
  if (isStatic && m_refereeRobotObservations.contains(identity)) {
    m_refereeRobotObservations[identity]["source"] = QStringLiteral("official_mqtt");
    m_refereeRobotObservations[identity]["session"] = QVariant::fromValue(m_mqttSession);
  }
  if (topic == QLatin1String("GlobalUnitStatus")) {
    m_globalHealthMqttVerified = true;
    for (auto it = m_globalHealthFields.cbegin(); it != m_globalHealthFields.cend(); ++it)
      if (it.value().toMap().value("state") == QLatin1String("valid"))
        m_globalHealthAcceptedSource[it.key()] = QStringLiteral("official_mqtt");
    emit globalHealthObservationChanged();
  }
  QVariantMap fields;
  if (topic == QLatin1String("GlobalUnitStatus")) fields = m_globalHealthFields;
  else if (topic == QLatin1String("GameStatus")) fields = m_gameStatusFields;
  else if (topic == QLatin1String("RobotDynamicStatus")) fields = m_dynamicObservation;
  else if (isStatic) {
    const RobotData *robot = getRobotById(identity);
    if (robot) {
      const auto observed = robot->staticPerformanceObservation;
      for (auto it = observed.cbegin(); it != observed.cend(); ++it) {
        if (it.key() == QLatin1String("received")) continue;
        const double value = it.value().toDouble();
        const bool mode = it.key().startsWith(QLatin1String("performance_system_"));
        const bool valid = std::isfinite(value) && value >= 0 && value <= 65535 &&
            (!mode || (value >= 1 && value <= 2)) && (it.key() != QLatin1String("max_health") || value > 0);
        fields[it.key()] = QVariantMap{{"state", valid ? "valid" : "invalid"}, {"value", it.value()}};
      }
    }
  } else if (topic == QLatin1String("RobotModuleStatus")) {
    for (auto it = m_robotModuleStatusMap.cbegin(); it != m_robotModuleStatusMap.cend(); ++it)
      fields[it.key()] = QVariantMap{{"state", it.value().toUInt() <= (it.key() == QLatin1String("modular_42mm") ? 1u : 2u) ? "valid" : "invalid"}, {"value", it.value()}};
  }
  for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
    if (it.value().toMap().value("state").toString() == QLatin1String("valid"))
      fieldUpdates[it.key()] = QVariantMap{{"receivedAt", QDateTime::fromMSecsSinceEpoch(receivedMs)},
          {"receivedMonotonicMs", monotonicMs}, {"session", QVariant::fromValue(m_mqttSession)}};
  }
  record["fieldUpdates"] = fieldUpdates;
  container[key] = record;
  emit mqttReceptionMetadataChanged();
}

QVariantMap GameData::robotDynamicStatus() const {
  return {{QStringLiteral("received"), m_dynamicReceived},
          {QStringLiteral("robotId"), m_dynamicReceived ? m_currentRobotId : 0},
          {QStringLiteral("fields"), m_dynamicObservation},
          {QStringLiteral("accepted"), m_dynamicAccepted}};
}

void GameData::clearRobotDynamicStatus() {
  if (!m_dynamicReceived && m_dynamicObservation.isEmpty() && m_dynamicAccepted.isEmpty()) return;
  m_dynamicReceived = false;
  m_dynamicObservation.clear();
  m_dynamicAccepted.clear();
  emit robotDynamicStatusChanged();
}

void GameData::updateRobotDynamicStatus(const robomaster::RobotDynamicStatus &data) {
  const auto identity = RM::resolveRobotIdentity(m_currentRobotId);
  if (identity.kind != RM::IdentityKind::Robot || !identity.team) return;
  RobotData *robot = findRobotById(m_currentRobotId);
  if (!robot) return;

  QVariantMap fields;
  auto observe = [&](const char *name, bool present, const QVariant &value, bool valid = true) {
    QVariantMap item;
    item["state"] = !present ? QStringLiteral("unreported") : valid ? QStringLiteral("valid") : QStringLiteral("invalid");
    if (present) item["value"] = value;
    fields[QString::fromLatin1(name)] = item;
    if (present && valid) m_dynamicAccepted[QString::fromLatin1(name)] = value;
  };
#define OBS_UINT(field) observe(#field, data.has_##field(), QVariant::fromValue(data.field()))
#define OBS_FLOAT(field) observe(#field, data.has_##field(), data.field(), std::isfinite(data.field()) && data.field() >= 0)
  OBS_UINT(current_health); OBS_FLOAT(current_heat); OBS_FLOAT(last_projectile_fire_rate);
  QVariantMap reservedField{{"state", QStringLiteral("not_applicable")}};
  if (data.has_reserved()) reservedField["value"] = data.reserved();
  fields["reserved"] = reservedField;
  OBS_UINT(current_buffer_energy); OBS_UINT(current_experience); OBS_UINT(experience_for_upgrade);
  OBS_UINT(total_projectiles_fired); OBS_UINT(remaining_ammo); OBS_UINT(is_out_of_combat);
  OBS_UINT(out_of_combat_countdown); OBS_UINT(can_remote_heal); OBS_UINT(can_remote_ammo);
#undef OBS_UINT
#undef OBS_FLOAT
  auto markLegacyRange = [&](const char *name, bool representable) {
    auto item = fields.value(QString::fromLatin1(name)).toMap();
    if (item.value("state") == QStringLiteral("valid") && !representable) {
      item["legacyProjection"] = QStringLiteral("out_of_range");
      fields[QString::fromLatin1(name)] = item;
    }
  };
  markLegacyRange("current_health", data.current_health() <= 65535);
  markLegacyRange("current_heat", data.current_heat() <= 65535);
  markLegacyRange("current_buffer_energy", data.current_buffer_energy() <= 65535);
  markLegacyRange("current_experience", data.current_experience() <= 65535);
  markLegacyRange("experience_for_upgrade", data.experience_for_upgrade() <= 65535);
  markLegacyRange("out_of_combat_countdown", data.out_of_combat_countdown() <= 65535);
  m_dynamicObservation = fields;
  m_dynamicReceived = true;

  // Legacy projections are updated only when a value fits their existing type.
  if (data.has_current_health() && data.current_health() <= 65535) {
    robot->currentHP = static_cast<quint16>(data.current_health());
    robot->currentRoundMaxHP = qMax(robot->currentRoundMaxHP, robot->currentHP);
    if (robot->currentHP == 0) robot->status = RobotStatus::DESTROYED;
    else if (robot->status == RobotStatus::DESTROYED || robot->status == RobotStatus::OFFLINE)
      robot->status = RobotStatus::NORMAL;
    if (robot->currentHP == 0) emit robotPositionUpdated(robot->robotId, 0, 0, 0, false);
  }
  if (data.has_current_heat() && std::isfinite(data.current_heat()) && data.current_heat() >= 0 && data.current_heat() <= 65535) {
    const QDateTime now = QDateTime::currentDateTime();
    const bool localOverride = m_lastLocalHeatSet.isValid() && m_lastLocalHeatSet.msecsTo(now) < 1500;
    if (!localOverride) {
      robot->currentHeat = data.current_heat();
      if (robot->currentHeat == 0) robot->isHeatOverLimit = false;
      checkHeatLimit(robot);
    }
  }
  if (data.has_current_buffer_energy() && data.current_buffer_energy() <= 65535)
    robot->bufferEnergy = static_cast<quint16>(data.current_buffer_energy());
  if (data.has_current_experience() && data.current_experience() <= 65535)
    robot->experience = static_cast<quint16>(data.current_experience());
  if (m_dynamicAccepted.contains("current_experience") && m_dynamicAccepted.contains("experience_for_upgrade")) {
    const quint64 total = m_dynamicAccepted.value("current_experience").toULongLong() + m_dynamicAccepted.value("experience_for_upgrade").toULongLong();
    if (total <= 65535) robot->maxExperience = static_cast<quint16>(total);
  }
  if (data.has_is_out_of_combat()) {
    robot->isOutOfCombat = data.is_out_of_combat();
    if (!robot->isOutOfCombat) robot->lastDamageTime = QDateTime::currentDateTime();
  }
  if (data.has_out_of_combat_countdown() && data.out_of_combat_countdown() <= 65535)
    robot->outOfCombatCountdown = static_cast<quint16>(data.out_of_combat_countdown());
  if (data.has_can_remote_heal()) robot->canRemoteHeal = data.can_remote_heal();
  if (data.has_can_remote_ammo()) robot->canRemoteAmmo = data.can_remote_ammo();
  // #3 is projectile speed, #4 is reserved, and #9 has no confirmed ammunition-type mapping.
  robot->lastUpdateTime = QDateTime::currentDateTime();
  emit robotDynamicStatusChanged();
  emit myRobotUpdated();
  emit robotDataUpdated(robot->robotId);
  emitDataChanged();
}

bool GameData::isRobotConnected(int robotId) const {
  const RobotData *r = getRobotById(robotId);
  if (!r) return false;
  return r->isClientConnected || r->isControllerConnected;
}

bool GameData::isAerialConnected() const {
  // 找到与当前选中机器人同队的 AERIAL 类型机器人并判断其连接状态
  const RobotData *current = getCurrentRobot();
  if (!current) return false;
  const TeamColor team = current->team;

  for (const RobotData &r : m_robots) {
    if (r.team == team && r.type == RobotType::AERIAL) {
      return r.isClientConnected || r.isControllerConnected;
    }
  }
  return false;
}

bool GameData::isSiloConnected() const {
  // 优先使用 RobotModuleStatus（如果上报了与飞镖相关的模块字段）
  // 在 m_robotModuleStatusMap 中，约定值为 0=在线, 1=离线
  if (m_robotModuleStatusMap.contains("dart")) {
    return (m_robotModuleStatusMap.value("dart").toInt() == 0);
  }

  // 次优先：使用 RobotStaticStatus 中飞镖机器人的连接态。
  if (m_hasDartRobotConnectionState) {
    return m_dartRobotOnline;
  }

  // 如果存在类型为 DART 的机器人，则基于 RobotStaticStatus.connection_state 判断
  for (const RobotData &r : m_robots) {
    if (r.type == RobotType::DART) {
      if (r.isClientConnected || r.isControllerConnected)
        return true;
    }
  }

  // 回退到 DartSelectTargetStatusSync 的最近心跳窗口判断（保持向后兼容）
  if (m_lastSiloUpdateMs <= 0) {
    return false;
  }

  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  return (now - m_lastSiloUpdateMs) < 5000;
}

// 将裁判系统事件转换为己方视角的状态更新和播报。
void GameData::eventInfo(const robomaster::Event &data) {
  // 获取当前机器人所属阵营
  const RobotData *currentRobot = getCurrentRobot();
  const TeamColor currentTeam = currentRobot ? currentRobot->team : TeamColor::RED;

  const int eventId = data.event_id();
  const int runeType = currentRuneType();
  const QString param = QString::fromStdString(data.param()).trimmed();
  switch (eventId) {
  case 1: {
      const auto event = parseKillEvent(param);
      if (!event) break;
      recordKill(static_cast<quint8>(event->killer.rawId),
                 static_cast<quint8>(event->victim.rawId));
      const QString color = currentRobot
          ? (*event->victim.team == currentRobot->team ? QStringLiteral("#d52424")
                                                       : QStringLiteral("#60D8B7"))
          : QStringLiteral("#808080");
      addSystemMessage(killEventMessage(*event), color);
      break;
  }
  case 2: {
      bool ok = false;
      const uint targetId = param.toUInt(&ok);
      if (ok) {
          if (targetId == 11) {
              m_redOutpostData.currentHP = 0;
              m_redOutpostData.isDestroyed = true;
              m_redOutpostData.lastUpdateTime = QDateTime::currentDateTime();
              emit outpostDestroyed(true);
              emit outpostHealthUpdated(TeamColor::RED);
              emitDataChanged();
          } else if (targetId == 111) {
              m_blueOutpostData.currentHP = 0;
              m_blueOutpostData.isDestroyed = true;
              m_blueOutpostData.lastUpdateTime = QDateTime::currentDateTime();
              emit outpostDestroyed(false);
              emit outpostHealthUpdated(TeamColor::BLUE);
              emitDataChanged();
          }
      }

      //当前前哨战阵营
      TeamColor outpostTeam = (targetId == 11) ? TeamColor::RED : (targetId == 111) ? TeamColor::BLUE : currentTeam;
      addSystemMessage(QStringLiteral("%1方前哨站被摧毁")
          .arg((targetId == 11) ? QStringLiteral("红") : (targetId == 111) ? QStringLiteral("蓝") : QStringLiteral("未知")),
          outpostTeam == currentTeam ?  "#d52424" :"#60D8B7" );
      break;
  }
  case 3: {
      const QString p = param;
      QStringList parts = p.split(",");
      if (!parts.isEmpty()) {
          bool ok = false;
          int activatedArms = parts[0].trimmed().toInt(&ok);
          if (ok && m_runeData.activatedArms != activatedArms) {
          const int previousStatus = m_runeData.status;
              m_runeData.activatedArms = activatedArms;
          if (activatedArms > 0 && previousStatus < 2 && m_gameState.gameTime > 0) {
            m_runeData.activationStartRemainingTime = m_gameState.gameTime;
          }
          if (activatedArms <= 0) {
            m_runeData.activationStartRemainingTime = -1;
          }
          // 尝试解析随后的每组数据（平均环数与增量）
          m_runeData.groupAverageRings.clear();
          m_runeData.groupDeltas.clear();
          for (int i = 1; i < parts.size(); ) {
            bool okAvg = false;
            float avg = parts[i].trimmed().toFloat(&okAvg);
            int delta = 0;
            if (i + 1 < parts.size()) {
              bool okDelta = false;
              delta = parts[i + 1].trimmed().toInt(&okDelta);
              // 增量格式无效时沿用默认值 0。
            }
            if (okAvg) {
              m_runeData.groupAverageRings.append(avg);
              m_runeData.groupDeltas.append(delta);
            }
            i += 2;
          }
          emit runeStatusChanged(m_runeData);
              notifyRuneStatusChanged();
          }
      }
      break;
  }
  case 4: {
      bool ok = false;
      const int protocolRuneType = QString::fromStdString(data.param()).toInt(&ok);
      const int resolvedRuneType = ok ? ((protocolRuneType == 2) ? 1 : 0) : runeType;
      if (m_runeData.status < 2 && m_gameState.gameTime > 0) {
        m_runeData.activationStartRemainingTime = m_gameState.gameTime;
      }
      if (m_runeData.status <= 1) {
        m_runeData.activationStartRemainingTime = -1;
      }
      bool runeDataChanged = false;
      if (m_runeData.type != resolvedRuneType) {
        m_runeData.type = resolvedRuneType;
        runeDataChanged = true;
      }
      if (m_runeData.status != 3) {
        m_runeData.status = 3;
        runeDataChanged = true;
      }
      if (runeDataChanged) {
        notifyRuneStatusChanged();
      }

      //更新哪一方激活能量机关
      const RobotData *currentRobot = getCurrentRobot();
      const bool myIsRed =
          currentRobot ? (currentRobot->team == TeamColor::RED)
                       : (m_currentRobotId < 100);
      const bool allyRuneActive = myIsRed ? m_isRedRuneActive : m_isBlueRuneActive;

      if(myIsRed && !allyRuneActive){
        m_isRedRuneActive = true;
        emit redRuneStatusUpdated();
      }
      else if (!myIsRed && !allyRuneActive){
        m_isBlueRuneActive = true;
        emit blueRuneStatusUpdated();
      }

      qInfo() << "[Event] Rune activation received:"
              << "eventId=" << eventId
              << "param=" << param
              << "team=" << (myIsRed ? "RED" : "BLUE")
              << "runeType=" << (resolvedRuneType == 0 ? "small" : "large")
              << "parsed=" << ok;

      //发送系统消息（当前只能收到己方的）
      addSystemMessage(QStringLiteral("%1方激活了%2能量机关")
          .arg(myIsRed ? QStringLiteral("红") : QStringLiteral("蓝"),
               (resolvedRuneType == 0) ? QStringLiteral("小") : QStringLiteral("大")), "#60D8B7");
      emit runeActived(m_runeData.type,m_runeData.status);
      break;
  }
  case 9: {
      const QString normalizedParam =
          QString(param).replace(QStringLiteral("，"), QStringLiteral(","));
      const QStringList parts = normalizedParam.split(",");
      bool okTeam = false;
      bool okTarget = false;
      const int hitTeam = parts.value(0).trimmed().toInt(&okTeam);
      const int targetId = parts.value(1).trimmed().toInt(&okTarget);
      const bool isRedTeam = okTeam && hitTeam == 1;
      const bool isBlueTeam = okTeam && hitTeam == 2;

      if (isRedTeam) {
          ++m_redDartHits;
      } else if (isBlueTeam) {
          ++m_blueDartHits;
      }

      if ((isRedTeam || isBlueTeam) && okTarget) {
          qInfo() << "[Event] Dart hit received:"
                  << "eventId=" << eventId
                  << "param=" << param
                  << "normalizedParam=" << normalizedParam
                  << "team=" << (isRedTeam ? "RED" : "BLUE")
                  << "targetId=" << targetId
                  << "targetName=" << dartTargetName(targetId);
          const int teamDartHitCount =
              isRedTeam ? m_redDartHits : m_blueDartHits;
          setDartMessageData(isRedTeam, targetId, teamDartHitCount);
          emit dartMessageTriggered();
          addSystemMessage(
              QStringLiteral("%1方飞镖命中%2")
                  .arg(isRedTeam ? QStringLiteral("红") : QStringLiteral("蓝"),
                       dartTargetName(targetId)));
      } else {
          qWarning() << "[Event] Dart hit parse failed:"
                     << "eventId=" << eventId
                     << "param=" << param
                     << "normalizedParam=" << normalizedParam
                     << "parts=" << parts
                     << "okTeam=" << okTeam
                     << "hitTeam=" << hitTeam
                     << "okTarget=" << okTarget
                     << "targetId=" << targetId;
      }

      if (isRedTeam || isBlueTeam) {
          emitDataChanged();
      }
      break;
  }
  case 10: {
      // 仅在正式战斗阶段处理飞镖闸门事件，避免倒计时/准备阶段
      // 赛事引擎发送的初始化事件被误播为闸门开启音效。
      if (!isBattleStage(m_gameState.gameProgress)) {
          break;
      }
      const QString p = QString::fromStdString(data.param()).trimmed();
      const RobotData *currentRobot = getCurrentRobot();
      const bool myIsRed = currentRobot ? (currentRobot->team == TeamColor::RED) : true;
      bool openedTeamKnown = false;
      bool openedTeamIsRed = false;

      if (p.isEmpty()) {
          openedTeamKnown = true;
          openedTeamIsRed = !myIsRed;
      } else if (p.compare("RED", Qt::CaseInsensitive) == 0) {
          openedTeamKnown = true;
          openedTeamIsRed = true;
      } else if (p.compare("BLUE", Qt::CaseInsensitive) == 0) {
          openedTeamKnown = true;
          openedTeamIsRed = false;
      } else if (p == "1") {
          openedTeamKnown = true;
          openedTeamIsRed = myIsRed;
      } else if (p == "2") {
          openedTeamKnown = true;
          openedTeamIsRed = !myIsRed;
      }

      // 协议语义上 event 10 是"对方飞镖闸门开启"；
      // 但 web 模拟器为了便于测试，会显式传入 RED/BLUE，此时直接按传入队伍显示。
      if (openedTeamKnown) {
          qDebug() << "[DartDebug] dartGateOpened -> openedTeamIsRed:" << openedTeamIsRed;
          emit dartGateOpened(openedTeamIsRed, true);
      }
      addSystemMessage(QStringLiteral("对方飞镖闸门开启"), "#d52424");
      break;
  }
  case 11:
      {
          const RobotData *currentRobot = getCurrentRobot();
          const bool myIsRed =
              currentRobot ? (currentRobot->team == TeamColor::RED)
                           : (m_currentRobotId < 100);

          // 5000ms debounce: 语音时长约 2-3s，留足余量避免打断其它语音
          const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
          if (m_lastBaseUnderAttackEventMs <= 0 || nowMs - m_lastBaseUnderAttackEventMs >= 5000) {
            m_lastBaseUnderAttackEventMs = nowMs;
            emit baseUnderAttackEvent(myIsRed);
          }

          emit baseAttacked(myIsRed ? TeamColor::RED : TeamColor::BLUE);
          if (myIsRed) {
            emit redBaseAttackedChanged();
          } else {
            emit blueBaseAttackedChanged();
          }
          addSystemMessage(QStringLiteral("基地正在遭受攻击！")
             , "#d52424");
      }
      break;
  case 12:{
      if (const RobotData *currentRobot = getCurrentRobot()) {
          emit enemyOutpostStoppedEvent(currentRobot->team != TeamColor::RED);
      }
      addSystemMessage(QStringLiteral("对方前哨站停转")
          , "#60D8B7");//绿色
      break;
    }
  case 13:{
      if (const RobotData *currentRobot = getCurrentRobot()) {
          emit enemyBaseShieldOpenedEvent(currentRobot->team != TeamColor::RED);
      }
      addSystemMessage(QStringLiteral("对方基地护甲展开")
          , "#60D8B7");//绿色
      break;
    }
  case 14:
      break;
  case 15:
      break;
  default:
      break;
  }
}

int GameData::currentRuneType() const {
  return m_runeData.type;
}

void GameData::notifyRuneStatusChanged() {
  emit runeStatusDataChanged();
  emit runeStatusChanged(m_runeData);
}


void GameData::setDartMessageData(bool isRedTeam, int targetId,
                                  int teamDartHitCount) {
  QVariantMap result;
  result["isRedTeam"] = isRedTeam;
  result["targetId"] = targetId;
  result["targetName"] = dartTargetName(targetId);
  result["occlusionDurationSec"] =
      RM::Dart::dartOcclusionDurationSeconds(targetId, teamDartHitCount);
  m_dartMessageData = result;
  emit dartMessageDataChanged();
}

void GameData::simulateDartHit(int team, int targetId) {
  // 走完整 processProtocolData 路径，与 MQTT 赛事引擎 Event topic 完全一致
  robomaster::RoboMasterMessage wrapper;
  auto *event = wrapper.mutable_event();
  event->set_event_id(9);
  event->set_param(std::to_string(team) + "," + std::to_string(targetId));
  qInfo() << "[Simulator] Injecting dart hit via processProtocolData: team="
          << team << "targetId=" << targetId;
  processProtocolData(wrapper);
}

void GameData::simulateGameTimeElapse(int seconds) {
  if (seconds <= 0) return;
  // Local simulation updates the legacy projection, never the official observation.
  const int newTime = std::max(0, static_cast<int>(m_gameState.gameTime) - seconds);
  m_gameState.stageElapsed = static_cast<quint16>(
      std::min(65535, static_cast<int>(m_gameState.stageElapsed) + seconds));
  updateGameStateFromS1(static_cast<int>(m_gameState.gameProgress), newTime,
                        m_gameState.redScore, m_gameState.blueScore,
                        m_gameState.currentRound, m_gameState.isPaused);
}



int GameData::activatedRuneArms() const {
  return m_runeData.activatedArms;
}

QVariantList GameData::runeGroupAverageRings() const {
  QMutexLocker locker(&m_mutex);
  QVariantList list;
  for (float v : m_runeData.groupAverageRings) list << v;
  return list;
}

QVariantList GameData::runeGroupDeltas() const {
  QMutexLocker locker(&m_mutex);
  QVariantList list;
  for (int v : m_runeData.groupDeltas) list << v;
  return list;
}

void GameData::updateGlobalUnitStatus(const robomaster::GlobalUnitStatus &data) {
  const auto self = RM::resolveRobotIdentity(m_currentRobotId);
  if (self.kind != RM::IdentityKind::Robot || !self.team) {
    qWarning() << "[GlobalUnitStatus] missing valid current robot identity";
    return;
  }
  const bool isBluePerspective = *self.team == TeamColor::BLUE;
  const TeamColor allyTeam = *self.team;
  const TeamColor enemyTeam = isBluePerspective ? TeamColor::RED : TeamColor::BLUE;
  const QList<int> allyRoster = isBluePerspective ? QList<int>{101, 103, 104, 107}
                                                   : QList<int>{1, 3, 4, 7};
  const QList<int> enemyRoster = isBluePerspective ? QList<int>{1, 3, 4, 7}
                                                    : QList<int>{101, 103, 104, 107};
  if (m_globalHealthIdentityEpoch != m_mqttIdentityEpoch) {
    m_globalHealthAccepted.clear();
    m_globalHealthAcceptedSource.clear();
  }
  m_globalHealthIdentityEpoch = m_mqttIdentityEpoch;
  m_globalHealthReceiver = m_currentRobotId;
  m_globalHealthReceived = true;
  m_globalHealthMqttVerified = false;
  m_globalHealthFields.clear();
  const QList<int> roster = allyRoster + enemyRoster;
  for (int i = 0; i < roster.size(); ++i) {
    QVariantMap observation{{"state", i < data.robot_health_size() ? "valid" : "unreported"}};
    if (i < data.robot_health_size()) {
      const quint32 value = data.robot_health(i);
      observation["value"] = QVariant::fromValue(value);
      if (value > 65535) observation["legacyProjection"] = QStringLiteral("out_of_range");
      m_globalHealthAccepted[QString::number(roster.at(i))] = QVariant::fromValue(value);
      m_globalHealthAcceptedSource[QString::number(roster.at(i))] = QStringLiteral("protocol_unverified");
    }
    m_globalHealthFields[QString::number(roster.at(i))] = observation;
  }
  emit globalHealthObservationChanged();
  const bool hasEnemySnapshot = data.robot_health_size() > 4 ||
      data.enemy_base_health() > 0 || data.enemy_outpost_health() > 0 ||
      data.enemy_base_status() > 0 || data.enemy_outpost_status() > 0 ||
      data.total_damage_enemy() > 0;

  updateBaseHP(allyTeam, static_cast<quint16>(data.base_health()));
  updateBaseStatus(allyTeam, static_cast<quint8>(data.base_status()));
  // 协议字段 base_shield -> BaseData.virtualShield (proto3 默认 0)
  if (isBluePerspective) {
    m_blueBase.virtualShield = static_cast<quint16>(data.base_shield());
  } else {
    m_redBase.virtualShield = static_cast<quint16>(data.base_shield());
  }
  updateOutpostHP(allyTeam, static_cast<quint16>(data.outpost_health()));
  updateOutpostStatus(allyTeam, static_cast<quint8>(data.outpost_status()));

  if (hasEnemySnapshot) {
    updateBaseHP(enemyTeam, static_cast<quint16>(data.enemy_base_health()));
    updateBaseStatus(enemyTeam, static_cast<quint8>(data.enemy_base_status()));
    // 敌方虚拟护盾映射
    if (isBluePerspective) {
      m_redBase.virtualShield = static_cast<quint16>(data.enemy_base_shield());
    } else {
      m_blueBase.virtualShield = static_cast<quint16>(data.enemy_base_shield());
    }
    updateOutpostHP(enemyTeam,
                    static_cast<quint16>(data.enemy_outpost_health()));
    updateOutpostStatus(enemyTeam,
                        static_cast<quint8>(data.enemy_outpost_status()));
  }

  auto applyRobotSnapshot = [this, &data](int topicIndex, int robotId) -> int {
    if (topicIndex >= data.robot_health_size()) {
      return 0;
    }

    RobotData *robot = findRobotById(robotId);
    if (!robot) {
      return 0;
    }

    if (data.robot_health(topicIndex) > 65535) return -1;
    const quint16 globalHp =
        static_cast<quint16>(data.robot_health(topicIndex));
    const quint16 prevHP = robot->currentHP;
    robot->tabGlobalCurrentHP = globalHp;
    robot->currentRoundMaxHP = qMax(robot->currentRoundMaxHP, globalHp);
    robot->currentHP = globalHp;
    robot->lastUpdateTime = QDateTime::currentDateTime();
    robot->hasTabGlobalSnapshot = true;

    // robot_bullets 的索引规则尚未确认，不从血量顺序推断。

    if (globalHp == 0 && robot->status != RobotStatus::OFFLINE) {
      robot->status = RobotStatus::DESTROYED;
    } else if (globalHp > 0 && robot->status == RobotStatus::DESTROYED) {
      robot->status = RobotStatus::NORMAL;
    }

    if (globalHp > 0 && robot->currentHP < prevHP && prevHP > 0) {
      robot->isOutOfCombat = false;
      robot->lastDamageTime = QDateTime::currentDateTime();
    }

    emit robotDataUpdated(static_cast<quint8>(robotId));
    return globalHp;
  };

  bool allyRepresentable = true, enemyRepresentable = true;
  int allyTotalHp = 0;
  const int allyRobotCount =
      std::min(data.robot_health_size(), static_cast<int>(allyRoster.size()));
  for (int i = 0; i < allyRobotCount; ++i) {
    const int hp = applyRobotSnapshot(i, allyRoster.at(i));
    if (hp < 0) allyRepresentable = false; else allyTotalHp += hp;
  }

  int enemyTotalHp = 0;
  for (int i = 0; hasEnemySnapshot && i < enemyRoster.size(); ++i) {
    const int hp = applyRobotSnapshot(i + allyRoster.size(), enemyRoster.at(i));
    if (hp < 0) enemyRepresentable = false; else enemyTotalHp += hp;
  }

  if (isBluePerspective) {
    if (data.robot_health_size() >= 4 && allyRepresentable) m_blueRobotTotalHP = allyTotalHp;
    if (hasEnemySnapshot) {
      if (data.robot_health_size() >= 8 && enemyRepresentable) m_redRobotTotalHP = enemyTotalHp;
    }
    m_blueTeamStats.totalDamageDealt = data.total_damage_ally();
    if (hasEnemySnapshot || data.total_damage_enemy() > 0) {
      m_redTeamStats.totalDamageDealt = data.total_damage_enemy();
    }
  } else {
    if (data.robot_health_size() >= 4 && allyRepresentable) m_redRobotTotalHP = allyTotalHp;
    if (hasEnemySnapshot) {
      if (data.robot_health_size() >= 8 && enemyRepresentable) m_blueRobotTotalHP = enemyTotalHp;
    }
    m_redTeamStats.totalDamageDealt = data.total_damage_ally();
    if (hasEnemySnapshot || data.total_damage_enemy() > 0) {
      m_blueTeamStats.totalDamageDealt = data.total_damage_enemy();
    }
  }

  emit myRobotUpdated();
  emitDataChanged();
  emit injuryStatsUpdated();
}

void GameData::processRobotRespawnStatusMap(const QVariantMap &status) {
  // 归一化网络侧的增量状态，并同时驱动统一弹窗状态机与 QML。
  qDebug() << "GameData: processRobotRespawnStatusMap ->" << status;
    // 采样日志：记录处理时间戳与关键字段，便于与 NetworkManager 链路比对
    qInfo() << "[RespawnDebug] GameData: processRobotRespawnStatusMap ts=" << QDateTime::currentMSecsSinceEpoch()
      << "is_pending=" << status.value("is_pending_respawn")
      << "cur=" << status.value("current_respawn_progress")
      << "total=" << status.value("total_respawn_progress");

  const QVariantMap normalized = normalizeRobotRespawnStatus(status);
  const bool isPending = normalized.value("is_pending_respawn", false).toBool();
  const uint32_t cur = normalized.value("current_respawn_progress", 0).toUInt();
  const uint32_t total = normalized.value("total_respawn_progress", 0).toUInt();

  // 启发式识别"金币立即复活"：
  // 上一帧待复活，本帧退出待复活，且当前进度未完成（cur < total）。
  if (m_lastPendingRespawn && !isPending && total > 0 && cur < total) {
    m_paidRespawnPowerBoostEndTime = QDateTime::currentDateTime().addSecs(4);
    emit myRobotUpdated();
  }

  m_lastRespawnStatus = normalized;
  m_lastPendingRespawn = isPending;
  m_lastRespawnProgress = cur;
  m_lastRespawnTotal = total;

  emit robotRespawnStatusUpdated(normalized);
}

const OutpostData &GameData::getOutpostByTeam(TeamColor team) const {
  if (team == TeamColor::RED) {
    return m_redOutpostData;
  } else {
    return m_blueOutpostData;
  }
}

//比赛状态协议的统一落点（重置逻辑在此函数中）
void GameData::applyOfficialGameStateSnapshot(GameStage stage,
                                             quint16 remainingTime,
                                             quint16 redScore,
                                             quint16 blueScore,
                                             quint8 currentRound,
                                             bool isPaused) {
  const GameStage previousStage = m_gameState.gameProgress;
  const quint8 previousRound = m_gameState.currentRound;
  const bool enteredBattle =
      previousStage != GameStage::BATTLE && stage == GameStage::BATTLE;
  const bool advancedRound =
      currentRound > 0 && m_gameState.currentRound < currentRound;
  const bool stageChanged = (m_gameState.gameProgress != stage);
  const bool timeChanged = (m_gameState.gameTime != remainingTime);
  const bool scoreChanged =
      (m_gameState.redScore != redScore || m_gameState.blueScore != blueScore);
  const bool pauseChanged = (m_gameState.isPaused != isPaused);
  const bool roundChanged =
      (currentRound > 0 && m_gameState.currentRound != currentRound);
  bool currentRobotRoundStatsReset = false;//标记当前机器人本回合是否被重置
  const bool shouldResetDartHits =
      enteredBattle || advancedRound || stage == GameStage::SETTLEMENT;

  //进入新回合
  if (currentRound > 0 && m_gameState.currentRound < currentRound) {
    m_gameState.lastRound = m_gameState.currentRound;
    // 结算阶段不重置战斗分数基线，避免结算时基线丢失导致无法判定胜负
    if (stage != GameStage::SETTLEMENT) {
      m_hasBattleScoreBaseline = false;
      m_battleScoreBaselineRound = 0;
    }

    //黄牌清零
    if (RobotData *robot = findRobotById(m_currentRobotId)) {
      robot->yellowCardCount = 0;
      currentRobotRoundStatsReset = true;
    }
    // 系统消息按回合重置，避免上一回合消息残留到下一回合
    if (!m_systemMessages.isEmpty()) {
      m_systemMessages.clear();
      emit systemMessagesChanged();
    }
  }

  if (shouldResetDartHits) {
    m_redDartHits = 0;
    m_blueDartHits = 0;
    qDebug() << "[DartDebug] applyOfficialGameStateSnapshot: reset dart hits"
             << "enteredBattle=" << enteredBattle
             << "advancedRound=" << advancedRound
             << "inSettlement=" << (stage == GameStage::SETTLEMENT)
             << "currentRound=" << currentRound
             << "previousStage=" << static_cast<int>(previousStage)
             << "newStage=" << static_cast<int>(stage);
  }

  if (roundChanged && stage != GameStage::SETTLEMENT) {
    for (auto &robot : m_robots) {
      robot.currentRoundMaxHP = 0;
    }
  }

  updateBattleScoreBaseline(previousStage, stage, redScore, blueScore,
                            currentRound);

  m_gameState.gameProgress = stage;
  m_gameState.gameTime = remainingTime;
  m_gameState.redScore = redScore;
  m_gameState.blueScore = blueScore;
  if (currentRound > 0) {
    m_gameState.currentRound = currentRound;
  }
  m_gameState.isPaused = isPaused;
  m_gameState.lastUpdateTime = QDateTime::currentDateTime();

  if (stage == GameStage::BATTLE) {
    if (enteredBattle || advancedRound) {
      const bool observedFullStart =
          previousStage == GameStage::COUNTDOWN || remainingTime >= 419;
      qInfo().noquote()
          << QStringLiteral(
                 "[RuneVoice] GameData initializing battle previousStage=%1 "
                 "stage=%2 remaining=%3 previousRound=%4 currentRound=%5 "
                 "enteredBattle=%6 advancedRound=%7 observedFullStart=%8")
                 .arg(static_cast<int>(previousStage))
                 .arg(static_cast<int>(stage))
                 .arg(remainingTime)
                 .arg(previousRound)
                 .arg(currentRound)
                 .arg(enteredBattle)
                 .arg(advancedRound)
                 .arg(observedFullStart);
      m_runeVoicePromptTracker.startBattle(remainingTime, observedFullStart);
    }
    emitRuneVoicePromptIfDue();
  } else {
    if (previousStage == GameStage::BATTLE) {
      qInfo().noquote()
          << QStringLiteral(
                 "[RuneVoice] GameData leaving battle previousStage=%1 "
                 "stage=%2 remaining=%3 previousRound=%4 currentRound=%5")
                 .arg(static_cast<int>(previousStage))
                 .arg(static_cast<int>(stage))
                 .arg(remainingTime)
                 .arg(static_cast<int>(previousRound))
                 .arg(static_cast<int>(currentRound));
    }
    m_runeVoicePromptTracker.reset(
        stage == GameStage::SETTLEMENT
            ? QStringLiteral("settlement")
            : QStringLiteral("non_battle_stage"));
  }

  // 根据阶段设置比赛开始/结束标记，保证本地计时路径可用
  switch (m_gameState.gameProgress) {
  case GameStage::NOT_STARTED:
    m_gameState.gameStarted = false;
    m_gameState.gameEnded = false;
    m_hasBattleScoreBaseline = false;
    m_battleScoreBaselineRound = 0;
    break;
  case GameStage::SETTLEMENT:
    m_gameState.gameStarted = false;
    m_gameState.gameEnded = true;
    break;
  default:
    m_gameState.gameStarted = true;
    m_gameState.gameEnded = false;
    break;
  }

  // 根据阶段与暂停状态驱动弹窗意图（仅在状态变化时触发，避免帧级重复提交）
  if (m_popupStateMachine && (stageChanged || pauseChanged)) {
    // 使用批量事务：对阶段类弹窗（Prep/Countdown/BattlePause）做互斥选择并一次性提交，避免 UI 看到中间态
    m_popupStateMachine->beginBatchUpdate();
    // 决策顺序：Paused(BattlePause) > COUNTDOWN > PREPARATION/SELF_CHECK (PrepPhase)
    if (m_gameState.isPaused) {
      // 统一由 BattlePausePopup 渲染暂停文案（技术暂停/战斗阶段暂停）
      m_popupStateMachine->submitIntent(Popup::PopupType::BattlePause,
                    Popup::PopupPriority::High,
                    Popup::PopupIntent::Show);
      m_popupStateMachine->submitIntent(Popup::PopupType::PrepPhase,
                                        Popup::PopupPriority::Normal,
                                        Popup::PopupIntent::Dismiss);
      m_popupStateMachine->submitIntent(Popup::PopupType::Countdown,
                                        Popup::PopupPriority::Normal,
                                        Popup::PopupIntent::Dismiss);
    } else if (m_gameState.gameProgress == GameStage::COUNTDOWN) {
      // 显示 Countdown，仅显示此类
      m_popupStateMachine->submitIntent(Popup::PopupType::Countdown,
                                        Popup::PopupPriority::Normal,
                                        Popup::PopupIntent::Show);
      // 其余阶段类隐藏
      m_popupStateMachine->submitIntent(Popup::PopupType::PrepPhase,
                                        Popup::PopupPriority::Normal,
                                        Popup::PopupIntent::Dismiss);
      m_popupStateMachine->submitIntent(Popup::PopupType::BattlePause,
                    Popup::PopupPriority::High,
                    Popup::PopupIntent::Dismiss);
    } else if (m_gameState.gameProgress == GameStage::PREPARATION ||
               m_gameState.gameProgress == GameStage::SELF_CHECK) {
      m_popupStateMachine->submitIntent(Popup::PopupType::PrepPhase,
                                        Popup::PopupPriority::Normal,
                                        Popup::PopupIntent::Show);
      m_popupStateMachine->submitIntent(Popup::PopupType::Countdown,
                                        Popup::PopupPriority::Normal,
                                        Popup::PopupIntent::Dismiss);
      m_popupStateMachine->submitIntent(Popup::PopupType::BattlePause,
                    Popup::PopupPriority::High,
                    Popup::PopupIntent::Dismiss);
    } else {
      // 非阶段弹窗时确保都关闭
      m_popupStateMachine->submitIntent(Popup::PopupType::PrepPhase,
                                        Popup::PopupPriority::Normal,
                                        Popup::PopupIntent::Dismiss);
      m_popupStateMachine->submitIntent(Popup::PopupType::Countdown,
                                        Popup::PopupPriority::Normal,
                                        Popup::PopupIntent::Dismiss);
      m_popupStateMachine->submitIntent(Popup::PopupType::BattlePause,
                    Popup::PopupPriority::High,
                    Popup::PopupIntent::Dismiss);
    }
    m_popupStateMachine->endBatchUpdate();
  }

  if (stageChanged) {
    emit gameStageChanged(stage);
  }
  if (timeChanged) {
    emit gameTimeUpdated(m_gameState.gameTime);
  }
  if (stageChanged || scoreChanged || pauseChanged || roundChanged) {
    emit gameStateUpdated();
  }
  if (currentRobotRoundStatsReset) {
    emit robotDataUpdated(m_currentRobotId);
    emit myRobotUpdated();
  }
  emitDataChanged();
}

void GameData::updateBattleScoreBaseline(GameStage previousStage,
                                         GameStage nextStage,
                                         quint16 redScore,
                                         quint16 blueScore,
                                         quint8 currentRound) {
  if (nextStage != GameStage::BATTLE) {
    return;
  }

  const quint8 normalizedRound = currentRound > 0 ? currentRound : m_gameState.currentRound;
  const bool enteringBattle = previousStage != GameStage::BATTLE;
  const bool roundChanged =
      m_hasBattleScoreBaseline && m_battleScoreBaselineRound != normalizedRound;
  if (m_hasBattleScoreBaseline && !enteringBattle && !roundChanged) {
    return;
  }

  m_hasBattleScoreBaseline = true;
  m_battleScoreBaselineRound = normalizedRound;
  m_battleScoreBaselineRed = redScore;
  m_battleScoreBaselineBlue = blueScore;
}

int GameData::roundScoreDelta(bool redSide) const {
  if (!m_hasBattleScoreBaseline) {
    return 0;
  }

  const int currentScore =
      redSide ? static_cast<int>(m_gameState.redScore)
              : static_cast<int>(m_gameState.blueScore);
  const int baselineScore =
      redSide ? static_cast<int>(m_battleScoreBaselineRed)
              : static_cast<int>(m_battleScoreBaselineBlue);
  return currentScore - baselineScore;
}

int GameData::redRoundScoreDelta() const {
  return roundScoreDelta(true);
}

int GameData::blueRoundScoreDelta() const {
  return roundScoreDelta(false);
}

/**
 * @brief 更新比赛状态
 * @param data Protobuf 格式的比赛信息消息
 */
void GameData::updateGameState(const robomaster::GameInfo &data) {
  const bool economyChanged =
      (m_redEconomy != static_cast<int>(data.red_economy()) ||
       m_blueEconomy != static_cast<int>(data.blue_economy()));

  m_redEconomy = data.red_economy();
  m_blueEconomy = data.blue_economy();
  if (economyChanged) {
    emit economyUpdated();
    emit currentTeamEconomyChanged();
  }

  applyOfficialGameStateSnapshot(static_cast<GameStage>(data.stage()),
                                 static_cast<quint16>(data.time_remaining()),
                                 static_cast<quint16>(data.red_score()),
                                 static_cast<quint16>(data.blue_score()),
                                 static_cast<quint8>(data.current_round()),
                                 data.is_paused());
}
// The latest official observation is separate from legacy/simulator projections.
void GameData::updateGameStatus(const robomaster::GameStatus &data) {
  QVariantMap fields;
  const bool hasSettlement = data.has_current_stage() && data.current_stage() == 5;
  const auto previousRound = m_gameStatusAccepted.value("current_round");
  if ((data.has_current_stage() && data.current_stage() != 5) ||
      (data.has_current_round() && data.current_round() > 0 && previousRound.isValid() &&
       previousRound.toUInt() != data.current_round())) {
    m_gameStatusAccepted.remove("game_result");
    m_gameStatusAccepted.remove("end_reason");
  }
  auto observe = [&](const char *name, bool present, const QVariant &value, bool valid, bool applicable = true) {
    QVariantMap item;
    item["state"] = !present ? QStringLiteral("unreported") : !applicable ? QStringLiteral("not_applicable") :
                    valid ? QStringLiteral("valid") : QStringLiteral("invalid");
    if (present) item["value"] = value;
    fields[QString::fromLatin1(name)] = item;
    if (present && valid && applicable) m_gameStatusAccepted[QString::fromLatin1(name)] = value;
  };
#define OBS(field, valid) observe(#field, data.has_##field(), QVariant::fromValue(data.field()), (valid))
  OBS(current_round, data.current_round() > 0);
  OBS(total_rounds, true);
  OBS(red_score, true); OBS(blue_score, true);
  OBS(current_stage, data.current_stage() <= 5);
  OBS(stage_countdown_sec, true); OBS(stage_elapsed_sec, true); OBS(is_paused, true);
#undef OBS
  observe("game_result", data.has_game_result(), QVariant::fromValue(data.game_result()), data.game_result() <= 2, hasSettlement);
  observe("end_reason", data.has_end_reason(), QVariant::fromValue(data.end_reason()), data.end_reason() >= 1 && data.end_reason() <= 7, hasSettlement);
  auto markRange = [&](const char *name, bool fits) {
    QVariantMap item = fields.value(QLatin1String(name)).toMap();
    if (item.value("state") == QLatin1String("valid") && !fits) {
      item["legacyProjection"] = QStringLiteral("out_of_range");
      fields[QLatin1String(name)] = item;
    }
  };
  markRange("current_round", data.current_round() <= 255);
  markRange("red_score", data.red_score() <= 65535);
  markRange("blue_score", data.blue_score() <= 65535);
  markRange("stage_countdown_sec", data.stage_countdown_sec() >= 0 && data.stage_countdown_sec() <= 65535);
  markRange("stage_elapsed_sec", data.stage_elapsed_sec() >= 0 && data.stage_elapsed_sec() <= 65535);
  m_gameStatusReceived = true;
  m_gameStatusResultContextValid = hasSettlement;
  m_gameStatusFields = fields;
  emit gameStatusObservationChanged();

  const auto stage = data.has_current_stage() && data.current_stage() <= 5
      ? static_cast<GameStage>(data.current_stage()) : m_gameState.gameProgress;
  const auto time = data.has_stage_countdown_sec() && data.stage_countdown_sec() >= 0 && data.stage_countdown_sec() <= 65535
      ? static_cast<quint16>(data.stage_countdown_sec()) : m_gameState.gameTime;
  const auto red = data.has_red_score() && data.red_score() <= 65535 ? static_cast<quint16>(data.red_score()) : m_gameState.redScore;
  const auto blue = data.has_blue_score() && data.blue_score() <= 65535 ? static_cast<quint16>(data.blue_score()) : m_gameState.blueScore;
  const auto round = data.has_current_round() && data.current_round() > 0 && data.current_round() <= 255
      ? static_cast<quint8>(data.current_round()) : m_gameState.currentRound;
  const auto paused = data.has_is_paused() ? data.is_paused() : m_gameState.isPaused;
  if (data.has_stage_elapsed_sec() && data.stage_elapsed_sec() >= 0 && data.stage_elapsed_sec() <= 65535)
    m_gameState.stageElapsed = static_cast<quint16>(data.stage_elapsed_sec());
  if ((data.has_current_stage() && data.current_stage() <= 5) ||
      (data.has_stage_countdown_sec() && data.stage_countdown_sec() >= 0 && data.stage_countdown_sec() <= 65535) ||
      (data.has_red_score() && data.red_score() <= 65535) || (data.has_blue_score() && data.blue_score() <= 65535) ||
      (data.has_current_round() && data.current_round() > 0 && data.current_round() <= 255) || data.has_is_paused())
    applyOfficialGameStateSnapshot(stage, time, red, blue, round, paused);
}

void GameData::updateGlobalLogisticsStatus(
    const robomaster::GlobalLogisticsStatus &data) {
  const bool isBluePerspective = getMyRobotId() >= 100;
  TeamLogisticsStatusData &logistics =
      isBluePerspective ? m_blueLogisticsStatus : m_redLogisticsStatus;

  const quint32 previousRemainingEconomy = logistics.remainingEconomy;
  const quint64 previousTotalEconomyObtained = logistics.totalEconomyObtained;
  const quint32 previousTechLevel = logistics.techLevel;
  const quint32 previousEncryptionLevel = logistics.encryptionLevel;

  logistics.remainingEconomy = data.remaining_economy();
  logistics.totalEconomyObtained = data.total_economy_obtained();
  logistics.techLevel = data.tech_level();
  logistics.encryptionLevel = data.encryption_level();

  const int remainingEconomy = static_cast<int>(logistics.remainingEconomy);
  const bool economyChanged = isBluePerspective
                                  ? (m_blueEconomy != remainingEconomy)
                                  : (m_redEconomy != remainingEconomy);
  if (isBluePerspective) {
    m_blueEconomy = remainingEconomy;
  } else {
    m_redEconomy = remainingEconomy;
  }
  if (economyChanged) {
    emit economyUpdated();
    emit currentTeamEconomyChanged();
  }

  if (previousRemainingEconomy != logistics.remainingEconomy ||
      previousTotalEconomyObtained != logistics.totalEconomyObtained ||
      previousTechLevel != logistics.techLevel ||
      previousEncryptionLevel != logistics.encryptionLevel) {
    emitDataChanged();
  }
}

void GameData::updateGlobalSpecialMechanism(
    const robomaster::GlobalSpecialMechanism &data) {
  const int previousAllyFortressOccupationSec = m_allyFortressOccupationSec;
  const int previousEnemyFortressOccupationSec = m_enemyFortressOccupationSec;
  int nextAllyFortressOccupationSec = 0;
  int nextEnemyFortressOccupationSec = 0;
  const int mechanismCount =
      data.mechanism_id_size() < data.mechanism_time_sec_size()
          ? data.mechanism_id_size()
          : data.mechanism_time_sec_size();

  for (int i = 0; i < mechanismCount; ++i) {
    const quint32 mechanismId = data.mechanism_id(i);
    const int mechanismSec =
        data.mechanism_time_sec(i) < 0 ? 0 : data.mechanism_time_sec(i);

    if (mechanismId == 1) {
      nextAllyFortressOccupationSec = mechanismSec;
    } else if (mechanismId == 2) {
      nextEnemyFortressOccupationSec = mechanismSec;
    }
  }

  if (previousAllyFortressOccupationSec == nextAllyFortressOccupationSec &&
      previousEnemyFortressOccupationSec == nextEnemyFortressOccupationSec) {
    return;
  }

  m_allyFortressOccupationSec = nextAllyFortressOccupationSec;
  m_enemyFortressOccupationSec = nextEnemyFortressOccupationSec;

  if (previousAllyFortressOccupationSec != 1 &&
      nextAllyFortressOccupationSec == 1) {
    emit allyFortressOccupationAlertTriggered();
  }

  emitDataChanged();
}

void GameData::updateGameStateFromS1(int stage, int remainingTime, int redScore, int blueScore, int round, bool isPaused)
{
  applyOfficialGameStateSnapshot(static_cast<GameStage>(stage),
                                 static_cast<quint16>(remainingTime),
                                 static_cast<quint16>(redScore),
                                 static_cast<quint16>(blueScore),
                                 static_cast<quint8>(round), isPaused);
}

void GameData::updateAirSupportStatusSync(
  const robomaster::AirSupportStatusSync &data) {
  const int oldStatus = m_airSupportStatus;
  const bool wasBeingTargeted = m_airSupportIsBeingTargeted == 1;
  const int newStatus = static_cast<int>(data.airsupport_status());
  const int newLeftTime = static_cast<int>(data.left_time());
  const int newCostCoins = static_cast<int>(data.cost_coins());
  const int newIsBeingTargeted =
      static_cast<int>(data.is_being_targeted());
  const int newShooterStatus = static_cast<int>(data.shooter_status());

  if (m_airSupportStatus == newStatus &&
      m_airSupportLeftTime == newLeftTime &&
      m_airSupportCostCoins == newCostCoins &&
      m_airSupportIsBeingTargeted == newIsBeingTargeted &&
      m_airSupportShooterStatus == newShooterStatus) {
    return;
  }

  m_airSupportStatus = newStatus;
  m_airSupportLeftTime = newLeftTime;
  m_airSupportCostCoins = newCostCoins;
  m_airSupportIsBeingTargeted = newIsBeingTargeted;
  m_airSupportShooterStatus = newShooterStatus;

  if (oldStatus != 1 && newStatus == 1) {
    const RobotData *currentRobot = getCurrentRobot();
    const bool myIsRed = currentRobot ? (currentRobot->team == TeamColor::RED) : true;
    emit airSupportStarted(myIsRed);
  }

  const bool isBeingTargeted = m_airSupportIsBeingTargeted == 1;
  if (wasBeingTargeted != isBeingTargeted) {
    emit airSupportTargetingStateChanged(isBeingTargeted);
  }

  emit airSupportStatusUpdated();
  emitDataChanged();
}

void GameData::updateDeployModeStatusSync(
    const robomaster::DeployModeStatusSync &data) {
  const int newStatus = static_cast<int>(data.status());
  if (m_deployModeStatus == newStatus) {
    return;
  }

  m_deployModeStatus = newStatus;
  emit deployModeStatusChanged();
  emitDataChanged();
}

void GameData::updateSiloStatusFromSync(int targetId, int open) {
  const int boundedTargetId = qBound(1, targetId, 5);
  const int boundedGateState = qBound(0, open, 2);
  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  // 无论状态是否变化，都刷新最近同步时间，避免在线状态被误判离线。
  m_lastSiloUpdateMs = nowMs;
  if (m_siloTargetId == boundedTargetId && m_siloGateState == boundedGateState) {
    // 值未变化也视为一次有效下行回执（用于结束前端"切换中"状态）。
    emit siloStatusChanged();
    return;
  }
  m_siloTargetId = boundedTargetId;
  m_siloGateState = boundedGateState;
  qDebug() << "[DartDebug] updateSiloStatusFromSync -> target:" << m_siloTargetId
           << "open=" << m_siloGateState;
  emit siloStatusChanged();
  emitDataChanged();
}

// QML 调用：选择下一个飞镖目标（本地切换/优先同步后端）
int GameData::selectNextSiloTarget() {
  QMutexLocker locker(&m_mutex);
  // 1..5 循环
  int next = m_siloTargetId;
  if (next < 1 || next > 5)
    next = 1;
  else
    next = (next % 5) + 1;

  // 不做本地状态写入，避免出现"本地先切过去、随后被服务器纠正"的抖动。
  // 由服务器下发 DartSelectTargetStatusSync 作为唯一真相源。
  qDebug() << "[DartDebug] selectNextSiloTarget request ->" << next;
  emit siloCommandRequested(next, false, false);
  return next;
}

// QML 调用：请求开启闸门（确认）
bool GameData::requestSiloOpen() {
  QMutexLocker locker(&m_mutex);
  if (!isSiloConnected()) {
    qWarning() << "[DartDebug] requestSiloOpen blocked: silo offline";
    return false;
  }
  int target = qBound(1, m_siloTargetId, 5);
  // open=true, launchConfirm=false 表示请求开启闸门（非触发发射）
  qDebug() << "[DartDebug] requestSiloOpen -> target:" << target;
  emit siloCommandRequested(target, true, false);
  return true;
}

// QML 调用：请求发射飞镖（发射确认）
bool GameData::requestSiloFire() {
  QMutexLocker locker(&m_mutex);
  if (!isSiloConnected()) {
    qWarning() << "[DartDebug] requestSiloFire blocked: silo offline";
    return false;
  }
  int target = qBound(1, m_siloTargetId, 5);
  // open=false, launchConfirm=true 表示发射动作
  qDebug() << "[DartDebug] requestSiloFire -> target:" << target;
  emit siloCommandRequested(target, false, true);
  return true;
}

bool GameData::siloOnline() const {
  return isSiloConnected();
}

void GameData::updateBuff(const robomaster::Buff &data) {
  // 获取对应机器人
  quint8 robotId = 0;
  const quint32 robotIdRaw = static_cast<quint32>(data.robot_id());
  if (robotIdRaw == 0) {
    robotId = m_currentRobotId;
  } else if (robotIdRaw <= 255) {
    robotId = static_cast<quint8>(robotIdRaw);
  } else {
    qDebug() << "Ignore buff with invalid robot id:" << robotIdRaw;
    return;
  }

  RobotData *robot = findRobotById(robotId);
  if (!robot) {
    return;
  }

  // 更新对应的 buff 数据
  const quint32 buffTypeRaw = static_cast<quint32>(data.buff_type());
  if (buffTypeRaw > 255) {
    qDebug() << "Ignore buff with invalid buff type:" << buffTypeRaw << "robot:" << robotId;
    return;
  }
  const quint8 buffType = static_cast<quint8>(buffTypeRaw);
  const int buffLevel = static_cast<int>(data.buff_level());
  const quint32 buffMaxTime = static_cast<quint32>(data.buff_max_time());
  const quint32 buffLeftTime = static_cast<quint32>(data.buff_left_time());

  RobotData::BuffTimedData *slot = timedBuffByType(robot, buffType);
  if (!slot) {
    qDebug() << "Ignore unknown buff type:" << buffType << "robot:" << robotId;
    return;
  }

  //更新对应机器人的增益
  slot->level = buffLevel;
  slot->maxTime = buffMaxTime;
  slot->leftTimeAtSync = buffLeftTime;
  robot->lastUpdateTime = QDateTime::currentDateTime();

  emit robotDataUpdated(robotId);
  if (robotId == m_currentRobotId) {
    emit myRobotUpdated();
  }
  emit buffPointUpdated();
  emitDataChanged();
}


void GameData::processGameResult(quint8 winner) {
  Q_UNUSED(winner);

  // GameStatus 是官方阶段和比分真相源。GameResult 事件可能早于携带最终
  // 分数的结算快照到达，不能抢先把阶段改成 SETTLEMENT。
  m_gameState.gameEnded = true;
  m_gameState.lastUpdateTime = QDateTime::currentDateTime();

  if (m_gameState.gameProgress == GameStage::SETTLEMENT) {
    emit gameResultReceived(determineWinner());
  }

  // 同步触发界面更新信号
  emit gameStateUpdated();
  emitDataChanged();
}

// 根据战斗阶段起点与结算阶段当前比分的增量判定胜利方。
// 返回值：0=平局,1=红方,2=蓝方
quint8 GameData::determineWinner() const {
  if (m_hasBattleScoreBaseline) {
    const int redDelta = redRoundScoreDelta();
    const int blueDelta = blueRoundScoreDelta();
    if (redDelta != blueDelta) {
      return redDelta > blueDelta ? 1 : 2;
    }
    return 0;
  }

  // 基线丢失时的回退：使用绝对分数判定（适用于客户端在战斗中后连接等场景）
  if (m_gameState.redScore > m_gameState.blueScore) return 1;
  if (m_gameState.blueScore > m_gameState.redScore) return 2;
  return 0;
}

void GameData::updateRadarMarkData(const robomaster::RadarMarkData &data) {
    // 缓存旧状态用于比较
    bool wasActivatable = m_runeData.isActivatable;

    // 更新 RuneData
    m_runeData.isActivatable = data.energy_activatable();

    // 只有在状态真正改变时才发射信号
    if (m_runeData.isActivatable != wasActivatable) {
         // 检测上升沿: 不可激活 -> 可激活，此时可能需要推断类型
         if (m_runeData.isActivatable) {
             // 判断是大符还是小符
             // 小符状态: bit 3-4 (0=未激活, 1=已激活, 2=正在激活)
             // 类型沿用最近一次 EventData 快照；两类协议消息异步到达，可能短暂滞后一帧。
             quint8 smallRuneStatus = (m_eventData >> 3) & 0x03;

             // 若小符已激活(1)，则提示大符；否则提示小符
             m_runeData.type = (smallRuneStatus == 1) ? 1 : 0;
         }

         notifyRuneStatusChanged();
    }

    // 更新历史状态
    m_runeData.wasActivatable = m_runeData.isActivatable;

    // 雷达标记进度字段目前未纳入客户端状态投影。
}

void GameData::updateGroundRobotPosition(const robomaster::GroundRobotPosition &data) {
  // 确定己方阵营
  const RobotData *myRobot = getCurrentRobot();
  if (!myRobot) return;
  TeamColor allyTeam = myRobot->team;

  // 己方 ID 基数 (Red: 1-7, Blue: 101-107)
  int baseId = (allyTeam == TeamColor::BLUE) ? 100 : 0;

  // 使用统一更新方法更新各兵种数据
  auto updateIfValid = [&](int id, float x, float y) {
    if (x == 0 && y == 0) return; // 0,0 视为未发送数据

    RobotData *robot = findRobotById(id);
    if (robot) {
        robot->posX = static_cast<float>(x);
        robot->posY = static_cast<float>(y) ;
        // 发出位置更新信号供小地图使用
        emit robotPositionUpdated(id, robot->posX, robot->posY, robot->angle, false);
    }
  };

  updateIfValid(baseId + 1, data.hero_x(), data.hero_y());
  updateIfValid(baseId + 2, data.engineer_x(), data.engineer_y());
  updateIfValid(baseId + 3, data.infantry_3_x(), data.infantry_3_y());
  updateIfValid(baseId + 4, data.infantry_4_x(), data.infantry_4_y());

  emitDataChanged();
}

void GameData::updateEnemyPositions(const robomaster::MapRobotData &data) {
  // 确定敌方阵营
  const RobotData *myRobot = getCurrentRobot();
  if (!myRobot) return;
  TeamColor enemyTeam = (myRobot->team == TeamColor::RED) ? TeamColor::BLUE : TeamColor::RED;

  // 敌方 ID 基数 (Red: 1-7, Blue: 101-107)
  int baseId = (enemyTeam == TeamColor::BLUE) ? 100 : 0;

  // 定义更新辅助 Lambda
  auto updateEnemy = [&](int id, uint32_t x, uint32_t y) {
    if (x == 0 && y == 0) return; // 0,0 视为未发送数据

    RobotData *robot = findRobotById(id);
    if (robot) {
        // 协议单位是 cm，转换为 m (GameData 统一使用 m)
        robot->posX = static_cast<float>(x) / 100.0f;
        robot->posY = static_cast<float>(y) / 100.0f;

        // 发出位置更新信号供小地图使用
        emit robotPositionUpdated(id, robot->posX, robot->posY, robot->angle, false);
    }
  };

  // 映射各兵种数据
  updateEnemy(baseId + 1, data.hero_x(), data.hero_y());
  updateEnemy(baseId + 2, data.engineer_x(), data.engineer_y());
  updateEnemy(baseId + 3, data.infantry_3_x(), data.infantry_3_y());
  updateEnemy(baseId + 4, data.infantry_4_x(), data.infantry_4_y());
  updateEnemy(baseId + 5, data.infantry_5_x(), data.infantry_5_y());
  updateEnemy(baseId + 7, data.sentry_x(), data.sentry_y());

  emitDataChanged();
}

void GameData::updateRuneStatusSync(const robomaster::RuneStatusSync &data) {
    // RuneStatusSync 协议反映的是当前机器人/己方队伍的能量机关状态
    // 我们主要用它来补充 0x0101 中缺失的细节（如已激活的灯臂数量）

    bool changed = false;
    const int previousStatus = m_runeData.status;
    const int incomingRuneStatus = static_cast<int>(data.rune_status());
    const bool hasValidRuneStatus =
        incomingRuneStatus >= 1 && incomingRuneStatus <= 3;

    if (hasValidRuneStatus &&
        incomingRuneStatus != m_lastRuneStatusSyncStatus) {
      const QString oldStatus =
          m_lastRuneStatusSyncStatus == 0
              ? QStringLiteral("unknown")
              : QString::number(m_lastRuneStatusSyncStatus);
      qInfo().noquote()
          << QStringLiteral(
                 "[RuneVoice] RuneStatusSync status changed old=%1 new=%2 "
                 "stage=%3 gameTime=%4")
                 .arg(oldStatus)
                 .arg(incomingRuneStatus)
                 .arg(static_cast<int>(m_gameState.gameProgress))
                 .arg(m_gameState.gameTime);
      m_lastRuneStatusSyncStatus = incomingRuneStatus;
    }

    // 更新状态 (1: 未激活, 2: 正在激活, 3: 已激活)
    // 注意：以 0x0101 (Event Data) 为主，这里仅作辅助更新或在缺失时补充
    if (hasValidRuneStatus && m_runeData.status != incomingRuneStatus) {
        m_runeData.status = incomingRuneStatus;
        changed = true;
    if (m_runeData.status >= 2 && previousStatus < 2 && m_gameState.gameTime > 0) {
      m_runeData.activationStartRemainingTime = m_gameState.gameTime;
    } else if (m_runeData.status <= 1) {
      m_runeData.activationStartRemainingTime = -1;
    }

        //更新入能量机关哪一方激活的状态(默认当前协议是己方激活)
        if(data.rune_status() == 3){
          //根据当前视角或数据包属性分配给红/蓝方
          const RobotData *currentRobot = getCurrentRobot();
          if (currentRobot) {
              if (currentRobot->team == TeamColor::RED) {
                      m_isRedRuneActive = true;
                      emit redRuneStatusUpdated();
              } else {
                      m_isBlueRuneActive = true;
                      emit blueRuneStatusUpdated();
                  }
              }
          }
    }

    m_runeVoicePromptTracker.updateRuneStatus(
        static_cast<int>(data.rune_status()));

    // 更新已激活灯臂数量 (0-5)
    if (m_runeData.activatedArms != (int)data.activated_arms()) {
        m_runeData.activatedArms = data.activated_arms();
        changed = true;
    }

    const float incomingAverageRings = data.average_rings();
    const float sanitizedAverageRings =
        sanitizeFiniteRuneAverage(incomingAverageRings);
    if (!std::isfinite(static_cast<double>(incomingAverageRings))) {
        qWarning() << "GameData: RuneStatusSync average_rings is not finite,"
                   << "resetting to 0"
                   << "value=" << incomingAverageRings;
    }

    if (!qFuzzyCompare(m_runeData.averageRings, sanitizedAverageRings)) {
        m_runeData.averageRings = sanitizedAverageRings;
        changed = true;
    }

    // 如果状态或进度有变化，通知 UI
    if (changed) {
        notifyRuneStatusChanged();
    }

    // RuneStatusSync 是持续同步包，只在真正进入"已激活"时才触发一次性激活提示，
    // 避免小地图/事件提示被周期性同步反复续时，表现为一直显示。
    if (m_runeData.status == 3 && previousStatus != 3) {
        emit runeActived(m_runeData.type, m_runeData.status);
    }
}

void GameData::emitRuneVoicePromptIfDue() {
  const std::optional<RM::RuneVoicePrompt> prompt =
      m_runeVoicePromptTracker.updateTime(m_gameState.gameTime);
  if (!prompt.has_value()) {
    return;
  }

  qInfo().noquote()
      << QStringLiteral(
             "[RuneVoice] GameData prompt signal type=%1 chances=%2 file=%3 "
             "stage=%4 gameTime=%5")
             .arg(prompt->type == RM::RuneVoiceType::Small
                      ? QStringLiteral("small")
                      : QStringLiteral("large"))
             .arg(prompt->remainingChances)
             .arg(prompt->soundFileName)
             .arg(static_cast<int>(m_gameState.gameProgress))
             .arg(m_gameState.gameTime);
  emit runeVoicePromptRequested(static_cast<int>(prompt->type),
                                prompt->remainingChances);
}

void GameData::updateTechCoreMotionStateSync(
    const robomaster::TechCoreMotionStateSync &data) {
  qInfo() << "GameData: TechCoreMotionStateSync"
          << "maxLevel=" << data.maximum_difficulty_level()
          << "basicState=" << data.basic_state()
          << "putinState=" << data.putin_state()
          << "moveState=" << data.move_state()
          << "rotateState=" << data.rotate_state()
          << "enemyStatus=" << data.enemy_core_status()
          << "remainAll=" << data.remain_time_all()
          << "remainStep=" << data.remain_time_step();
  m_techCoreMotionState.maximumDifficultyLevel =
      data.maximum_difficulty_level();
  m_techCoreMotionState.basicState = data.basic_state();
  m_techCoreMotionState.putinState = data.putin_state();
  m_techCoreMotionState.moveState = data.move_state();
  m_techCoreMotionState.rotateState = data.rotate_state();
  m_techCoreMotionState.enemyCoreStatus = data.enemy_core_status();
  m_techCoreMotionState.remainTimeAll = data.remain_time_all();
  m_techCoreMotionState.remainTimeStep = data.remain_time_step();
  emit techCoreMotionStateSyncUpdated();
  emitDataChanged();
}

void GameData::updateSentryStatus(const SentryStatusData &data) {
  // 该兼容入口暂未映射字段；哨兵状态目前由统一机器人状态链路更新。
}

/**
 * @brief 处理协议数据
 * @details 根据消息类型分发到相应的更新函数。
 * @param message 接收到的 Protobuf 消息
 */
void GameData::processProtocolData(
    const robomaster::RoboMasterMessage &message) {
  switch (message.payload_case()) {
  case robomaster::RoboMasterMessage::PAYLOAD_NOT_SET:
    // 忽略空 payload，避免高频日志刷屏导致性能问题
    break;
  case robomaster::RoboMasterMessage::kRobotStatus:
    updateRobotData(message.robot_status());
    break;
  case robomaster::RoboMasterMessage::kGameInfo:
    updateGameState(message.game_info());
    // 0x020D: 雷达标记数据/哨兵与能量机关信息
    if (message.game_info().has_radar_mark_data()) {
        const auto &radar = message.game_info().radar_mark_data();
        updateRadarMarkData(radar);
    }
    break;
  case robomaster::RoboMasterMessage::kMapClick:
    // 处理地图点击
    break;
  case robomaster::RoboMasterMessage::kGuardCtrl:
    // 处理哨兵控制
    break;
  case robomaster::RoboMasterMessage::kDartCmd:
    // 处理飞镖指令
    qDebug() << "[DartDebug] processProtocolData: received kDartCmd";
    // 当前仅记录收到指令，字段尚未映射到状态中心。
    break;
  case robomaster::RoboMasterMessage::kBattleMsg:
    // 处理战场消息 (旧逻辑保留，如果还有其他BattleMessage来源)
    if (message.has_battle_msg()) {
      const auto &msg = message.battle_msg();
      QString content = QString::fromStdString(msg.content());
      emit battleMessageReceived(content, msg.duration(), msg.color_hex());
    }
    break;
  case robomaster::RoboMasterMessage::kRefereeWarning:
    // 处理统一的裁判警告消息
    if (message.has_referee_warning()) {
        updateRefereeWarning(message.referee_warning());
    }
    break;
  case robomaster::RoboMasterMessage::kPenaltyInfo:
    if (message.has_penalty_info()) {
      updatePenalty(message.penalty_info());
    }
    break;
  case robomaster::RoboMasterMessage::kEnemyPositions:
    updateEnemyPositions(message.enemy_positions());
    break;
  case robomaster::RoboMasterMessage::kGroundRobotPosition:
    updateGroundRobotPosition(message.ground_robot_position());
    break;
  case robomaster::RoboMasterMessage::kVideoCtrl:
    updateVideoControl(message.video_ctrl());
    break;
  case robomaster::RoboMasterMessage::kGlobalUnitStatus:
    updateGlobalUnitStatus(message.global_unit_status());
    break;
  case robomaster::RoboMasterMessage::kInternalStatus:
    updateGlobalUnitStatusInternal(message.internal_status());
    break;
  case robomaster::RoboMasterMessage::kEvent: {
    const auto &evt = message.event();
    m_lastEventObservation = {{QStringLiteral("eventId"), evt.event_id()},
                              {QStringLiteral("param"), QString::fromStdString(evt.param())}};
    emit lastEventObservationChanged();
    const int eventId = evt.event_id();
    const QString param = QString::fromStdString(evt.param()).trimmed();

    if (eventId == 0x0101) {
      bool ok = false;
      const quint32 eventData = param.toUInt(&ok);
       if (ok) {
        const quint8 dartTarget = (eventData >> 20) & 0x07;
        const quint8 prevDartTarget = (m_eventData >> 20) & 0x07;

        if (dartTarget != prevDartTarget && dartTarget != 0) {
          const RobotData *currentRobot = getCurrentRobot();
          const bool myIsRed =
              currentRobot ? (currentRobot->team == TeamColor::RED)
                           : (m_currentRobotId < 100);
          const bool enemyIsRed = !myIsRed;

          setDartMessageData(enemyIsRed, dartTarget,
                             enemyIsRed ? m_redDartHits : m_blueDartHits);
          emit dartMessageTriggered();
          emit systemMessageReceived(
              QString("对方飞镖击中己方%1").arg(dartTargetName(dartTarget)));
        }

        const quint8 smallRune = (eventData >> 3) & 0x03;
        const quint8 bigRune = (eventData >> 5) & 0x03;

        int newStatus = 1;
        int newType = m_runeData.type;
        if (bigRune == 2) {
          newStatus = 2;
          newType = 1;
        } else if (bigRune == 1) {
          newStatus = 3;
          newType = 1;
        } else if (smallRune == 2) {
          newStatus = 2;
          newType = 0;
        } else if (smallRune == 1) {
          newStatus = 3;
          newType = 0;
        }

        const int previousStatus = m_runeData.status;
        if (m_runeData.status != newStatus || m_runeData.type != newType) {
          m_runeData.status = newStatus;
          m_runeData.type = newType;
          if (newStatus >= 2 && previousStatus < 2 &&
              m_gameState.gameTime > 0) {
            m_runeData.activationStartRemainingTime = m_gameState.gameTime;
          } else if (newStatus <= 1) {
            m_runeData.activationStartRemainingTime = -1;
          }
          notifyRuneStatusChanged();
        }


        const quint8 outpostBuff = (eventData >> 27) & 0x03;
        const quint8 prevOutpostBuff = (m_eventData >> 27) & 0x03;
        if (outpostBuff != prevOutpostBuff && outpostBuff == 1) {
          emit systemMessageReceived("己方前哨站增益已激活");
        }

        m_eventData = eventData;
      }
    } else if (eventId == 1) {
      eventInfo(evt);
    } else if (eventId == 2) {
      eventInfo(evt);
    } else if (eventId == 3) {

      const QStringList parts = param.split(",");
      if (parts.size() >= 2) {
        const int previousStatus = m_runeData.status;
        m_runeData.type = 1;
        m_runeData.status = std::max(m_runeData.status, 3);
        if (m_runeData.status >= 2 && previousStatus < 2 &&
            m_gameState.gameTime > 0) {
          m_runeData.activationStartRemainingTime = m_gameState.gameTime;
        }
        const int activatedArms = parts[0].toInt();
        const float averageRings = parts[1].toFloat();
        m_runeData.activatedArms = activatedArms;
        m_runeData.averageRings = averageRings;
        notifyRuneStatusChanged();

        addSystemMessage(
            QStringLiteral("大能量机关激活成功，点亮%1个灯臂，平均环数%2")
                .arg(activatedArms)
                .arg(QString::number(averageRings, 'f', 1)),
            QStringLiteral("#FFFFFF"));
      }

    } else if (eventId == 4) {
      eventInfo(evt);
      bool ok = false;
      const int protocolRuneType = param.toInt(&ok);
      const int resolvedRuneType =
          ok ? ((protocolRuneType == 2) ? 1 : 0) : currentRuneType();

      bool runeDataChanged = false;
      if (m_runeData.type != resolvedRuneType) {
        m_runeData.type = resolvedRuneType;
        runeDataChanged = true;
      }
      //强制转激活
      if (m_runeData.status != 3) {
        m_runeData.status = 3;
        runeDataChanged = true;
      }
      if (m_runeData.status >= 2 && m_gameState.gameTime > 0) {
        m_runeData.activationStartRemainingTime = m_gameState.gameTime;
      }
      if (runeDataChanged) {
        notifyRuneStatusChanged();
      }

      const RobotData *currentRobot = getCurrentRobot();
      const bool myIsRed =
          currentRobot ? (currentRobot->team == TeamColor::RED)
                       : (m_currentRobotId < 100);
      if (myIsRed) {
        m_isRedRuneActive = true;
        emit redRuneStatusUpdated();
      } else {
        m_isBlueRuneActive = true;
        emit blueRuneStatusUpdated();
      }
      emit runeActived(m_runeData.type, m_runeData.status);
    } else if (eventId == 9) {
      eventInfo(evt);
    } else if (eventId == 10) {
      eventInfo(evt);
    } else if (eventId == 11) {
      eventInfo(evt);
    } else if (eventId == 12) {
      eventInfo(evt);
    } else if (eventId == 13) {
      eventInfo(evt);
    } else if (eventId == 16) {
      m_event16LastReceivedAt = QDateTime::currentDateTime();
      emit event16ReceivedChanged();
    } else if (eventId == 100) {
      emit systemMessageReceived(param);
    } else if (eventId == 2000) {
      bool ok = false;
      const int winner = param.toInt(&ok);
      if (ok) {
        processGameResult(static_cast<quint8>(winner));
      }
    } else if (eventId == 3001) {
      setKickedAll(true);
    } else if (eventId == 3002) {
      setKickedAll(false);
    }

    const QString officialPopupMessage = officialEventPopupMessageForEvent(
        evt, getCurrentRobot(), currentRuneType());
    if (!officialPopupMessage.isEmpty()) {
      emit officialEventPopupRequested(eventId, officialPopupMessage);
    }
    break;
  }
  case robomaster::RoboMasterMessage::kBaseHealth: {
    const auto &bh = message.base_health();
    // 协议使用 1=红方、2=蓝方；内部枚举使用 0=红方、1=蓝方。
    TeamColor team = (bh.team() == 1) ? TeamColor::RED : TeamColor::BLUE;

    updateBaseHP(team, bh.hp());

    // 旧协议只有无敌布尔值，对应到新状态码的 0（无敌）和 1（可攻击）。
    updateBaseStatus(team, bh.is_invincible() ? 0 : 1);
  } break;
  case robomaster::RoboMasterMessage::kRobotInjury: {
    updateRobotInjury(message.robot_injury());
  } break;
  case robomaster::RoboMasterMessage::kRobotModuleStatus: {
    updateRobotModuleStatus(message.robot_module_status());
  } break;
  case robomaster::RoboMasterMessage::kRobotRespawnStatus: {
    // 将 protobuf 字段转换为 QVariantMap 并转发
    const auto &r = message.robot_respawn_status();
    QVariantMap map;
    map["is_pending_respawn"] = r.is_pending_respawn();
    map["total_respawn_progress"] = static_cast<uint32_t>(r.total_respawn_progress());
    map["current_respawn_progress"] = static_cast<uint32_t>(r.current_respawn_progress());
    map["can_free_respawn"] = r.can_free_respawn();
    map["gold_cost_for_respawn"] = static_cast<uint32_t>(r.gold_cost_for_respawn());
    map["can_pay_for_respawn"] = r.can_pay_for_respawn();
    processRobotRespawnStatusMap(map);
  } break;
  case robomaster::RoboMasterMessage::kTechCoreMotionStateSync: {
    updateTechCoreMotionStateSync(message.tech_core_motion_state_sync());
  } break;
  case robomaster::RoboMasterMessage::kDeployModeStatusSync: {
    updateDeployModeStatusSync(message.deploy_mode_status_sync());
  } break;
  case robomaster::RoboMasterMessage::kBuff: {
    updateBuff(message.buff());
  } break;
  case robomaster::RoboMasterMessage::kRobotPathPlanInfo: {
    updateSentryPath(message.robot_path_plan_info());
  } break;
  default:
    qDebug() << "Unknown message type:" << message.payload_case();
    break;
  }
}

void GameData::updateVideoControl(const robomaster::VideoControl &data) {
  emit videoSourceChanged(QString::fromStdString(data.video_url()),
                          data.is_playing());
}

void GameData::updateRobotPosition(const robomaster::RobotPosition &data) {
  // 1.查找对应的机器人并更新内部 m_robots 管理的数据（默认当前机器人）
  RobotData *robot = findRobotById(data.robot_id());
  if (robot) {
    robot->posX = data.x();
    robot->posY = data.y();
    robot->angle = data.yaw();
    // RobotPosition 不携带高亮态；收到该包时清掉旧的雷达高亮，避免地图残留旧标记。
    robot->isHighLight = 0;
    robot->lastUpdateTime = QDateTime::currentDateTime();

    // 2. 发出完整信号，确保 UI（小地图）获得 ID 和 高亮状态
    emit robotPositionUpdated(robot->robotId, robot->posX, robot->posY, robot->angle,
                              false);
  }
}

// 更新雷达发送位置信息
void GameData::updateRadarInfo(const robomaster::RadarInfoToClient &data) {

  //获取己方阵营和敌方阵营
  const RobotData *currentRobot = getCurrentRobot();
  if (!currentRobot) return;
  const TeamColor myTeam = currentRobot->team;
  const TeamColor enemyTeam =
      (myTeam == TeamColor::RED) ? TeamColor::BLUE : TeamColor::RED;
  const QDateTime now = QDateTime::currentDateTime();

  //构造12个机器人的ID
  QList<quint8> orderedRobotIds;
  orderedRobotIds.reserve(12);
  for (quint8 baseId : kRadarRobotBaseIds) {
    orderedRobotIds.append(teamRobotId(baseId, enemyTeam));
  }
  for (quint8 baseId : kRadarRobotBaseIds) {
    orderedRobotIds.append(teamRobotId(baseId, myTeam));
  }

  //检查数据量是否正常
  const int packetCount = data.robot_info_size();
  const int expectedRobotCount = static_cast<int>(orderedRobotIds.size());
  const int robotCount = qMin(packetCount, expectedRobotCount);
  if (packetCount != expectedRobotCount) {
    qWarning() << "RadarInfoToClient count mismatch, expected"
               << expectedRobotCount << "got" << packetCount;
  }

  //更新每个机器人的坐标
  for (int i = 0; i < robotCount; ++i) {
    RobotData *robot = findRobotById(orderedRobotIds.at(i));
    if (!robot) {
      continue;
    }

    const auto &info = data.robot_info(i);
    robot->posX = static_cast<float>(info.target_pos_x()) / 100.0f;
    robot->posY = static_cast<float>(info.target_pos_y()) / 100.0f;
    robot->isHighLight = static_cast<quint8>(info.is_high_light());
    robot->lastUpdateTime = now;

    emit robotPositionUpdated(robot->robotId, robot->posX, robot->posY,
                              robot->angle, robot->isHighLight);
  }

  emitDataChanged();
}




// === 机器人伤害更新 ===
void GameData::updateRobotInjury(const robomaster::RobotInjuryStat &data) {
  // 将 RobotInjuryStat 映射到 ~ 面板的五类统计
  m_injuryTotal = data.total_damage();
  m_injuryCollision = data.collision_damage();
  m_injury17mm = data.small_projectile_damage();
  m_injury42mm = data.large_projectile_damage();
  // 官方 1.2 中模块离线扣血与异常离线扣血分字段上报，这里统一汇总到离线类目。
  m_injuryOffline = data.module_offline_damage() + data.offline_damage();
  // 警告：penalty + server_kill + dart_splash
  m_injuryWarning = data.penalty_damage() + data.server_kill_damage() + data.dart_splash_damage();

  emit injuryStatsUpdated();
  emitDataChanged();
}

// === 模块在线状态更新（RobotModuleStatus）MQTT协议处理 ===
QVariantMap GameData::robotModuleStatus() const {
  QVariantMap result = m_robotModuleStatusMap;
  result["received"] = m_robotModuleStatusReceived;
  result["robotId"] = m_robotModuleStatusReceived ? m_currentRobotId : 0;
  return result;
}

void GameData::updateRobotModuleStatus(const robomaster::RobotModuleStatus &data) {
  const auto identity = RM::resolveRobotIdentity(m_currentRobotId);
  if (identity.kind != RM::IdentityKind::Robot || !identity.team) {
    qWarning() << "RobotModuleStatus ignored: no valid current robot identity";
    return;
  }
  // Each message is a fresh snapshot. Missing optional fields remain unknown.
  m_robotModuleStatusMap.clear();
#define COPY_MODULE(field) if (data.has_##field()) m_robotModuleStatusMap[QStringLiteral(#field)] = data.field()
  COPY_MODULE(power_manager); COPY_MODULE(rfid); COPY_MODULE(light_strip);
  COPY_MODULE(small_shooter); COPY_MODULE(big_shooter); COPY_MODULE(uwb);
  COPY_MODULE(armor); COPY_MODULE(video_transmission); COPY_MODULE(capacitor);
  COPY_MODULE(main_controller); COPY_MODULE(modular_42mm);
#undef COPY_MODULE
  m_robotModuleStatusReceived = true;

  m_moduleStatus.clear();
  auto addModule = [&](const QString &name, const char *field, bool notify = true) {
    const auto it = m_robotModuleStatusMap.constFind(QString::fromLatin1(field));
    const bool present = it != m_robotModuleStatusMap.constEnd();
    const bool online = present && it.value().toUInt() == 1;
    if (notify && present) {
      const auto previous = m_lastModuleOnlineStates.constFind(name);
      if ((!online && previous == m_lastModuleOnlineStates.constEnd()) ||
          (!online && previous != m_lastModuleOnlineStates.constEnd() && previous.value()))
        addSystemMessage(QStringLiteral("机器人%1模块离线").arg(name), QStringLiteral("#d52424"));
      m_lastModuleOnlineStates.insert(name, online);
    }
    ModuleStatusData module{};
    const QByteArray utf8 = name.toUtf8();
    strncpy(module.moduleName, utf8.constData(), sizeof(module.moduleName) - 1);
    module.isOnline = online;
    m_moduleStatus.append(module);
  };
  for (int i = 0; i < 4; ++i) addModule(QStringLiteral("装甲%1").arg(i), "armor");
  if (data.has_small_shooter()) addModule(QStringLiteral("17mm测速"), "small_shooter");
  if (data.has_big_shooter()) addModule(QStringLiteral("42mm测速"), "big_shooter");
  addModule(QStringLiteral("图传"), "video_transmission");
  addModule(QStringLiteral("RFID"), "rfid");
  addModule(QStringLiteral("UWB"), "uwb");
  addModule(QStringLiteral("WIFI"), "wifi", false);
  addModule(QStringLiteral("灯条0"), "light_strip");
  addModule(QStringLiteral("电容"), "capacitor");
  emit moduleListChanged();
  emitDataChanged();
}

void GameData::recordDamageEvent(const DamageEventData &data) {
  DamageRecord record;
  record.attackerId = data.attackerId;
  record.victimId = data.victimId;
  record.damage = data.damage;
  record.armorId = data.armorId;
  record.hurtType = data.hurtType;
  record.timestamp = QDateTime::currentDateTime();

  m_damageHistory.append(record);

  // 更新伤害统计
  updateDamageStats();

  // 机器人的 HP 和 Status 完全由 updateStandardStatus 权威数据驱动。
  RobotData *victim = findRobotById(data.victimId);
  if (victim) {
      // 仅发送信号通知 UI 刷新（例如显示受击特效），但不修改核心状态
      emit robotDataUpdated(data.victimId);
  }
}

// 辅助函数：获取机器人名称
static QString getRobotName(int robotId) {
    return RM::resolveRobotIdentity(robotId).displayName;
}

void GameData::recordKill(quint8 killerId, quint8 victimId) {
    // 记录击杀事件
    RobotData *killer = findRobotById(killerId);
    RobotData *victim = findRobotById(victimId);

    if (victim) {
        bool isFirstBlood = false;
        int currentKillStreak = 1;

        if (killerId != 0 && killer) {
            // 首杀判定。
            if (m_totalKills == 0) {
                isFirstBlood = true;
            }
            m_totalKills++;

            // 连杀判定 (双杀/三杀等通常要求在一定时间内完成，如 10s 内)
            QDateTime now = QDateTime::currentDateTime();
            if (m_lastKillTimes.contains(killerId)) {
                QDateTime lastKillTime = m_lastKillTimes[killerId];
                if (lastKillTime.secsTo(now) <= 10) { // 10秒内连杀
                    m_killStreaks[killerId]++;
                } else {
                    m_killStreaks[killerId] = 1;
                }
            } else {
                m_killStreaks[killerId] = 1;
            }
            m_lastKillTimes[killerId] = now;
            currentKillStreak = m_killStreaks[killerId];
        }

        // 记录击杀信息
        KillRecord record(killerId, victimId, currentKillStreak, isFirstBlood);
        m_killRecords.append(record);

        emit robotDataUpdated(killerId);
        emit robotDataUpdated(victimId);

        // 发送信号，包含完整击杀记录
        emit killEventOccurred(record);
        emitDataChanged();
    }
}

// MQTT PenaltyInfo 默认为当前机器人判罚信息。
void GameData::updatePenalty(const robomaster::PenaltyInfo &data) {
  //超功率
  if (data.penalty_type() == 4) {
    if (RobotData *robot = findRobotById(m_currentRobotId)) {
      const quint32 effectSec = data.penalty_effect_sec();
      robot->chassisOverPowerCutSeconds =
          static_cast<quint16>(qMin<quint32>(effectSec, 65535u));
      emit robotDataUpdated(m_currentRobotId);
      emit myRobotUpdated();
    }
  }

  //黄牌
  robomaster::RefereeWarningData warning;
  warning.set_level(data.penalty_type());
  warning.set_offending_robot_id(m_currentRobotId);
  warning.set_penalty_effect_sec(data.penalty_effect_sec());
  warning.set_total_penalty_num(data.total_penalty_num());
  warning.set_source("MQTT");
  warning.set_timestamp(QDateTime::currentMSecsSinceEpoch());

  updateRefereeWarning(warning);
}

//哨兵路径规划更新
void GameData::updateSentryPath(const robomaster::RobotPathPlanInfo &data) {
  // 排查用：路径起点按协议应等于哨兵当前位置，可与 RobotPosition 上报值对照。
  qDebug() << "[SentryPath] rx intention=" << data.intention() << "start=("
           << data.start_pos_x() << "," << data.start_pos_y()
           << ") dm, deltas=" << data.offset_x_size() << "/"
           << data.offset_y_size();

  // 保留协议分米原值，按当前 MapProfile 统一换算。这样切换训练场配置时，
  // 哨兵路径与 ROS 世界坐标标记使用同一场地几何，不改变增量累计语义。
  m_allySentryPath.rawStartDmX = static_cast<qint32>(data.start_pos_x());
  m_allySentryPath.rawStartDmY = static_cast<qint32>(data.start_pos_y());
  m_allySentryPath.rawDeltasDmX.clear();
  m_allySentryPath.rawDeltasDmX.reserve(data.offset_x_size());
  for (int i = 0; i < data.offset_x_size(); ++i) {
    m_allySentryPath.rawDeltasDmX.append(static_cast<qint32>(data.offset_x(i)));
  }
  m_allySentryPath.rawDeltasDmY.clear();
  m_allySentryPath.rawDeltasDmY.reserve(data.offset_y_size());
  for (int i = 0; i < data.offset_y_size(); ++i) {
    m_allySentryPath.rawDeltasDmY.append(static_cast<qint32>(data.offset_y(i)));
  }
  rebuildSentryPathCoordinates();
  emit sentryPathUpdated();
}

void GameData::resetDamageStats() {
  m_damageHistory.clear();
  m_redTeamStats = TeamDamageStats();
  m_blueTeamStats = TeamDamageStats();
  emit injuryStatsUpdated();  // 伤害统计属性（redTotalDamage/blueTotalDamage）刷新
  emitDataChanged();
}

void GameData::updateDamageStats() {
  // 重新计算伤害统计
  m_redTeamStats = TeamDamageStats();
  m_blueTeamStats = TeamDamageStats();

  for (const DamageRecord &record : m_damageHistory) {
    RobotData *attacker = findRobotById(record.attackerId);
    if (!attacker)
      continue;

    TeamDamageStats *stats = nullptr;
    if (attacker->team == TeamColor::RED) {
      stats = &m_redTeamStats;
    } else {
      stats = &m_blueTeamStats;
    }

    if (stats) {
      stats->totalDamageDealt += record.damage;
    }
  }
  emit injuryStatsUpdated();
  emitDataChanged();
}

QMap<int, quint32> GameData::getDamageByRobot() const {
  QMap<int, quint32> map;
  for (const DamageRecord &rec : m_damageHistory) {
    int id = static_cast<int>(rec.attackerId);
    if (id <= 0) continue;
    map[id] = map.value(id, 0) + static_cast<quint32>(rec.damage);
  }
  return map;
}

void GameData::setKickedAll(bool value) {
  if (m_kickedAll == value)
    return;
  m_kickedAll = value;
  emit kickAllStateChanged(m_kickedAll);
  // 驱动 Out 弹窗：true -> Show, false -> Dismiss
  if (m_popupStateMachine) {
    if (m_kickedAll) {
      m_popupStateMachine->submitIntent(Popup::PopupType::Out,
                                        Popup::PopupPriority::Critical,
                                        Popup::PopupIntent::Show);
    } else {
      m_popupStateMachine->submitIntent(Popup::PopupType::Out,
                                        Popup::PopupPriority::Critical,
                                        Popup::PopupIntent::Dismiss);
    }
  }
  emitDataChanged();
}

/**
 * @brief 定时更新函数
 * @details 处理复活倒计时、热量冷却、比赛时间递减等逻辑。
 */
void GameData::update() {
  // 更新机器人状态
  bool currentRobotChanged = false;

  if (m_rosRadarFresh && !isRosRadarFresh()) {
    m_rosRadarFresh = false;
    if (m_positionSource == QStringLiteral("ros_bridge")) {
      qInfo() << "[MiniMap] ROS 雷达位置已超时，清除旧目标";
      emit miniMapMarkersChanged();
    }
  }

  // 比赛时间由外部权威来源（例如 NetworkManager -> updateGameState）写入时
  // 禁止本地自行递减以避免与网络写入冲突导致视觉"横跳"。
  // 仅当 m_useExternalGameClock == false 时启用本地倒计时（可用于无外部时间源的离线模式）。
  if (!m_useExternalGameClock) {
        if (isGameActive() && m_gameState.gameTime > 0) {
          m_gameState.gameTime--;
          emitRuneVoicePromptIfDue();
          // 发出倒计时更新，确保 UI 持续刷新剩余时间
          emit gameTimeUpdated(m_gameState.gameTime);
          if (m_gameState.gameTime <= 0) {
            m_gameState.gameEnded = true;
            // 进入 0 时补发比赛状态更新，驱动 UI 切换到结算或结束显示
            emit gameStateUpdated();
          }
        }
    }

  emitDataChanged();
  if (currentRobotChanged) {
      emit myRobotUpdated();
  }
}

void GameData::onUpdateTimer() {
    // 更新脱战状态 (6秒无伤害且无射击视为脱战)
    QDateTime now = QDateTime::currentDateTime();
    for (auto &robot : m_robots) {
        if (robot.currentHP <= 0 || robot.status == RobotStatus::DESTROYED || robot.status == RobotStatus::OFFLINE) {
            continue;
        }
        if (!robot.isOutOfCombat) {
            bool safeFromDamage = !robot.lastDamageTime.isValid() || robot.lastDamageTime.msecsTo(now) > 6000;
            bool safeFromFire = !robot.lastFireTime.isValid() || robot.lastFireTime.msecsTo(now) > 6000;

            if (safeFromDamage && safeFromFire) {
                robot.isOutOfCombat = true;
                emit robotDataUpdated(robot.robotId);
            }
        }
    }
    update();
}

// 私有辅助方法实现
RobotData *GameData::findRobotById(qint64 robotId) {
  for (auto &robot : m_robots) {
    if (robot.robotId == robotId) {
      return &robot;
    }
  }
  return nullptr;
}

BuffPointData *GameData::findBuffPointById(quint8 pointId) {
  for (auto &buffPoint : m_buffPoints) {
    if (buffPoint.pointId == pointId) {
      return &buffPoint;
    }
  }
  return nullptr;
}

ModuleStatusData *GameData::findModuleByName(const QString &moduleName) {
  for (auto &module : m_moduleStatus) {
    if (QString::fromUtf8(module.moduleName) == moduleName) {
      return &module;
    }
  }
  return nullptr;
}

void GameData::updateRobotHealth(quint8 id, quint16 hp) {
  RobotData *robot = findRobotById(id);
  if (robot) {
    robot->currentHP = hp;
    robot->currentRoundMaxHP = qMax(robot->currentRoundMaxHP, hp);
    emit robotDataUpdated(id);
    if (id == m_currentRobotId) {
        emit myRobotUpdated();
    }
  }
}

void GameData::updateRobotHeat(quint8 id, quint16 heat) {
  RobotData *robot = findRobotById(id);
  if (robot) {
    robot->currentHeat = heat;
    // 立即进行热量超限检查
    checkHeatLimit(robot);
    emit robotDataUpdated(id);
    if (id == m_currentRobotId) {
        emit myRobotUpdated();
    }
  }
}

void GameData::updateRobotPower(quint8 id, quint16 power) {
  RobotData *robot = findRobotById(id);
  if (robot) {
    robot->power = power;
    emit robotDataUpdated(id);
    if (id == m_currentRobotId) {
        emit myRobotUpdated();
    }
  }
}

void GameData::updateRobotLevel(quint8 id, quint16 level) {
  RobotData *robot = findRobotById(id);
  if (robot) {
    robot->level = level;
    // 等级改变可能影响最大血量、热量上限等，这里简单更新等级
    emit robotDataUpdated(id);
    if (id == m_currentRobotId) {
        emit myRobotUpdated();
    }
  }
}

void GameData::updateRobotBuffer(quint8 id, quint16 buffer) {
  RobotData *robot = findRobotById(id);
  if (robot) {
    robot->bufferEnergy = buffer;
    emit robotDataUpdated(id);
    if (id == m_currentRobotId) {
        emit myRobotUpdated();
    }
  }
}


void GameData::setFortressBonusAmmo(quint8 id, quint16 ammo) {
  RobotData *robot = findRobotById(id);
  if (robot) {
    robot->fortressBonusAmmo = ammo;
    emit robotDataUpdated(id);
    if (id == m_currentRobotId) { // 使用 m_currentRobotId 而不是 m_myRobotId，因为 getMyRobot 使用的是 getRobotInfo(m_currentRobotId)
         emit myRobotUpdated();
    }
  }
}

// 已取消控制区/额外弹药字段，仅保留堡垒与允许发弹量

void GameData::setAllowedAmmo17mm(quint8 id, quint16 ammo) {
  RobotData *robot = findRobotById(id);
  if (robot) {
    robot->allowedAmmo17mm = ammo;
    emit robotDataUpdated(id);
    if (id == m_currentRobotId) {
         emit myRobotUpdated();
    }
  }
}

void GameData::setAllowedAmmo42mm(quint8 id, quint16 ammo) {
  RobotData *robot = findRobotById(id);
  if (robot) {
    robot->allowedAmmo42mm = ammo;
    emit robotDataUpdated(id);
    if (id == m_currentRobotId) {
         emit myRobotUpdated();
    }
  }
}

void GameData::setSpeedOverLimit(quint8 id, bool overLimit, quint16 lockSeconds) {
  // 仅供模拟器和界面调试注入状态，不接入正式比赛判罚链路。
  RobotData *robot = findRobotById(id);
  if (robot) {
    if (overLimit) {
        // 未提供受支持的锁定时长时，按永久锁定处理。
        robot->speedLockState = SpeedLockState::PermanentLocked;
        if (lockSeconds == 15) robot->speedLockState = SpeedLockState::Locked15s;
        else if (lockSeconds == 20) robot->speedLockState = SpeedLockState::Locked20s;
        else if (lockSeconds > 20) robot->speedLockState = SpeedLockState::PermanentLocked;
    } else {
        robot->speedLockState = SpeedLockState::Normal;
    }
    robot->speedLockSeconds = lockSeconds;
    emit robotDataUpdated(id);
    if (id == m_currentRobotId) {
         emit myRobotUpdated();
    }
  }
}

void GameData::setRobotType(quint8 id, int type) {
  Q_UNUSED(type)
  // 身份由 ID 唯一决定，调试接口不得覆盖身份。
  if (RobotData *robot = findRobotById(id)) {
    const auto identity = RM::resolveRobotIdentity(id);
    if (identity.role) robot->type = *identity.role;
  }
}

void GameData::setRobotHeat(quint8 id, quint16 heat) {
  RobotData *robot = findRobotById(id);
  if (robot) {
    robot->currentHeat = heat;
    recordLocalHeatObservation(id, heat, QStringLiteral("local_input"));
    m_lastLocalHeatSet = QDateTime::currentDateTime();
    emit robotDataUpdated(id);
    if (id == m_currentRobotId) {
      emit myRobotUpdated();
    }
  }
}

void GameData::toggleCurrentRobotOutOfCombat() {
  RobotData *robot = findRobotById(m_currentRobotId);
  if (!robot) {
    return;
  }

  robot->isOutOfCombat = !robot->isOutOfCombat;
  if (!robot->isOutOfCombat) {
    QDateTime now = QDateTime::currentDateTime();
    robot->lastDamageTime = now;
    robot->lastFireTime = now;
  }

  emit robotDataUpdated(robot->robotId);
  emit myRobotUpdated(); // 显式通知"我的机器人"面板更新
  emitDataChanged();
}

void GameData::setTeamLevelCap(TeamColor team, int cap) {
  if (team == TeamColor::RED)
    m_redLevelCap = cap;
  else
    m_blueLevelCap = cap;
  emitDataChanged();
  emit myRobotUpdated();
}

void GameData::setTeamDefenseBonus(TeamColor team, int percent) {
  if (team == TeamColor::RED)
    m_redDefenseBonusPercent = percent;
  else
    m_blueDefenseBonusPercent = percent;
  for (auto &robot : m_robots) {
    if (robot.team != team)
      continue;
    if (percent > 0)
      robot.buffMask |= 0x04;
    else
      robot.buffMask &= ~0x04;
    emit robotDataUpdated(robot.robotId);
  }
  emitDataChanged();
  emit myRobotUpdated();
}

void GameData::incrementAmmo17mmExchangeCount() {
  m_ammo17mmExchangeCount+=100;
  emit exchangeCountsUpdated();
}

void GameData::incrementAmmo42mmExchangeCount() {
  m_ammo42mmExchangeCount+=10;
  emit exchangeCountsUpdated();
}

// === 消息系统实现 ===

QVariantList GameData::getSystemMessages() const {
  return m_systemMessages;
}

QVariantList GameData::getRobotMessages() const {
  QVariantList list;
  for (const QString &msg : m_robotMessages) {
    list.append(msg);
  }
  return list;
}

void GameData::addSystemMessage(const QString &message) {
  addSystemMessage(message, QString());
}

void GameData::addSystemMessage(const QString &message, const QString &color) {
  if (message.isEmpty())
    return;

  // 在开头添加时间戳
  QString timeStamp = QString("%1:%2")
                          .arg(m_gameState.gameTime / 60)
                          .arg(m_gameState.gameTime % 60, 2, 10, QChar('0'));
  QString fullMessage = timeStamp + " " + message;

  QVariantMap messageItem;
  messageItem["text"] = fullMessage;
  messageItem["color"] = normalizedSystemMessageColor(color);
  m_systemMessages.append(messageItem);

  qInfo() << "[SystemMessage]" << "text=" << fullMessage
          << "requestedColor=" << color
          << "storedColor=" << messageItem.value("color").toString();

  // 限制最大消息数量
  while (m_systemMessages.size() > MAX_MESSAGES) {
    m_systemMessages.removeFirst();
  }

  emit systemMessagesChanged();
  emit systemMessageReceived(fullMessage);
}

void GameData::addRobotMessage(const QString &message) {
  if (message.isEmpty())
    return;

  m_robotMessages.append(message);

  // 限制最大消息数量
  while (m_robotMessages.size() > MAX_MESSAGES) {
    m_robotMessages.removeFirst();
  }

  emit robotMessagesChanged();
}

//QML 通过属性绑定访问 gameData.myRobot
QVariantMap GameData::getMyRobot() const { return getRobotInfo(m_currentRobotId); }

int GameData::getMyRobotId() const { return m_currentRobotId; }

void GameData::updateRefereeWarning(const robomaster::RefereeWarningData &data) {
    // 统一收口处理判罚逻辑

    // 1. 数据清洗/去重
    // 可以添加逻辑：如果在短时间内收到相同内容的警告（来自不同源），则忽略
    // 目前简单实现：只要收到就分发

    // 2. 更新内部状态 (黄牌计数等)
    if (data.offending_robot_id() > 0) {
        int robotId = data.offending_robot_id();
        RobotData *robot = findRobotById(robotId);

        if (robot) {
            // PenaltyInfo: 1=黄牌，2=双方黄牌；两者对当前机器人都按一次黄牌累计。
            // 仅在明确指向某机器人的情况下更新
            if (data.level() == 1 ) {
                // 兼容旧逻辑：每次警告视为一次新的违规事件，执行累加
                // 注意：由于是UDP，可能存在重包问题，理想情况应结合 count 字段做去重
                // 这里暂按旧逻辑处理：收到一次加一次
                int increment = 1;

                if (robot->type == RobotType::ENGINEER) {
                     // 工程机器人上限2张
                    if (robot->yellowCardCount < 2) robot->yellowCardCount = data.total_penalty_num(); // 直接使用总数字段，避免重包导致的重复累加问题
                    //如果是当前机器人，则显示系统消息
                    if (robotId == m_currentRobotId)
                    addSystemMessage(QString("黄牌已累计%1张，累计达2张将罚下机器人").arg(robot->yellowCardCount), "#d52424");
                } else {
                     // 其他机器人上限3张
                    if (robot->yellowCardCount < 3) robot->yellowCardCount = data.total_penalty_num(); // 直接使用总数字段，避免重包导致的重复累加问题
                    if (robotId == m_currentRobotId)
                    addSystemMessage(QString("黄牌已累计%1张，累计达3张将罚下机器人").arg(robot->yellowCardCount), "#d52424");
                }

                emit robotDataUpdated(robotId);
                if (robotId == m_currentRobotId) {
                    emit myRobotUpdated();
                }
            }

            // 处理超热量惩罚 (Level 5)
            if (data.level() == 5) {
                // 如果收到超热量警告，调用 checkHeatLimit 并标记 penaltyConfirmed=true
                // 这样如果热量确实超过 Q2，就会触发 permanent lock 标记
                checkHeatLimit(robot, true);

                // 通知 UI 更新状态
                emit robotDataUpdated(robotId);
                if (robotId == m_currentRobotId) {
                    emit myRobotUpdated();
                }
            } else {
                // 检查是否为射速超限惩罚 (Level 未知，假设非 Level 5/1/2 即可能为其他)
                // 或者我们直接检查：如果收到任意判罚 且 当前射速 > 射速上限，则判定为射速超限
                if (robot->muzzleVelocity > robot->shootSpeedLimit) {
                     checkSpeedLimit(robot, true);

                     // 通知 UI 更新状态
                     emit robotDataUpdated(robotId);
                     if (robotId == m_currentRobotId) {
                         emit myRobotUpdated();
                     }
                }
            }
        }
    }

    emit refereeWarningUpdated(data);
}

// ==================== 自定义 UI 数据支持 (CustomByteBlock) ====================

RobotData *GameData::getRobotDataRef(qint64 robotId) {
    return findRobotById(robotId);
}

QVariantMap GameData::getMyRobotCustomData() const {
    QVariantMap data;

    quint8 myId = m_currentRobotId;

    const RobotData* robot = getRobotById(myId);
    if (!robot) {
        return data;
    }

    data["fricEnabled"] = robot->fricEnabled;
    data["rammerEnabled"] = robot->rammerEnabled;
    data["chassisMode"] = robot->chassisMode;
    data["spinMode"] = robot->spinMode;
    data["followMode"] = robot->followMode;
    data["chassisProtect"] = robot->chassisProtect;
    data["chassisWarning"] = robot->chassisWarning;
    data["superCapEnergy"] = robot->superCapEnergyPercent;
    data["gimbalChassisAngle"] = robot->gimbalChassisAngle;
    data["targetDistance"] = robot->targetDistance;
    data["ballisticCompensation"] = robot->ballisticCompensation;

    return data;
}

void GameData::setCustomUIEnabled(bool v) {
  if (m_customUIEnabled != v) {
    m_customUIEnabled = v;
    emit customUIEnabledChanged();
  }
}
