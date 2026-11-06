#pragma once

#include "network/Protocol.h"

#include <QString>
#include <QtGlobal>
#include <optional>

namespace RM {

enum class IdentityKind { Robot, Reserved, FieldObject, Unknown };

// 仅为一次解析结果，不持有或更新比赛状态。类型值不是官方 robot_type 枚举。
struct RobotIdentity {
  qint64 rawId;
  std::optional<int> normalizedId;
  std::optional<TeamColor> team;
  std::optional<::RobotType> role;
  IdentityKind kind;
  QString displayName;
  bool rmulSelectable;
};

// 接收完整 uint32 协议 ID 及负数哨兵，先验证再归一化，不发生窄化。
RobotIdentity resolveRobotIdentity(qint64 rawId);

} // namespace RM
