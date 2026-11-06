#include "RobotIdentity.h"

namespace RM {

RobotIdentity resolveRobotIdentity(qint64 rawId) {
  RobotIdentity result{rawId, std::nullopt, std::nullopt, std::nullopt,
                       IdentityKind::Unknown,
                       QStringLiteral("未知(%1)").arg(rawId), false};

  int number;
  if (rawId >= 1 && rawId <= 11) {
    number = static_cast<int>(rawId);
    result.team = TeamColor::RED;
  } else if (rawId >= 101 && rawId <= 111) {
    number = static_cast<int>(rawId - 100);
    result.team = TeamColor::BLUE;
  } else {
    return result;
  }
  result.normalizedId = number;
  result.kind = IdentityKind::Robot;
  switch (number) {
  case 1:
    result.role = ::RobotType::HEAVY;
    result.displayName = QStringLiteral("重装");
    break;
  case 2:
    result.kind = IdentityKind::Reserved;
    result.displayName = QStringLiteral("保留位");
    break;
  case 3:
  case 4:
  case 5:
    result.role = number == 3 ? ::RobotType::INFANTRY_3
                              : number == 4 ? ::RobotType::INFANTRY_4
                                            : ::RobotType::INFANTRY_5;
    result.displayName = QStringLiteral("步兵%1").arg(number);
    break;
  case 6:
    result.role = ::RobotType::AERIAL;
    result.displayName = QStringLiteral("空中系统");
    break;
  case 7:
    result.role = ::RobotType::SENTRY;
    result.displayName = QStringLiteral("哨兵");
    break;
  case 8:
    result.role = ::RobotType::DART;
    result.displayName = QStringLiteral("飞镖");
    break;
  case 9:
    result.role = ::RobotType::RADAR;
    result.displayName = QStringLiteral("雷达");
    break;
  case 10:
    result.kind = IdentityKind::FieldObject;
    result.displayName = QStringLiteral("基地");
    break;
  case 11:
    result.kind = IdentityKind::FieldObject;
    // R/B 前哨是场地位置，不能推断归属红蓝方。
    result.team.reset();
    result.displayName = rawId == 11 ? QStringLiteral("R前哨")
                                     : QStringLiteral("B前哨");
    break;
  }
  result.rmulSelectable = number == 1 || number == 3 || number == 7;
  return result;
}

} // namespace RM
