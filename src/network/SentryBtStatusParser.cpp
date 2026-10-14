// SPDX-License-Identifier: MIT
/**
 * @file SentryBtStatusParser.cpp
 * @brief 哨兵自定义 4 字节状态解析实现。
 */

#include "SentryBtStatusParser.h"

namespace RM {

namespace {

/**
 * @brief 校验 4 字节布局能否覆盖所有字段。
 * @details 仅在编译期兜底；真正的布局由 JSON 描述约束。
 */
constexpr bool layoutFitsPayload() {
  // offset 0: uint8(1) | offset 1-2: uint16(2) | offset 3: uint8(1) = 4
  return kSentryBtStatusPayloadLength == 4;
}

static_assert(layoutFitsPayload(),
              "SentryBtStatus 布局与 kSentryBtStatusPayloadLength 不一致");

}  // namespace

SentryBtStatusParseResult parseSentryBtStatus(const QByteArray &data) {
  SentryBtStatusParseResult result;
  result.rawHex = QString::fromLatin1(data.toHex(' '));

  if (data.size() != kSentryBtStatusPayloadLength) {
    result.error = QStringLiteral("payload_length %1 != %2")
                       .arg(data.size())
                       .arg(kSentryBtStatusPayloadLength);
    return result;
  }

  const auto *bytes = reinterpret_cast<const uint8_t *>(data.constData());

  result.status.currentStance = bytes[0];
  // 显式按小端组装，不依赖主机字节序。
  result.status.btRemainTime =
      static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) |
                            (static_cast<uint16_t>(bytes[2]) << 8));
  result.status.btGameStatus = bytes[3];

  result.ok = true;
  return result;
}

QList<SentryBtStatusField> sentryBtStatusFieldList(const SentryBtStatus &status) {
  // 顺序与 tools/protocol_specs/sentry_bt_status_v1.json 的 fields 数组一致，
  // 字段名同样取自该文件；这里不附加任何业务语义。
  return QList<SentryBtStatusField>{
      SentryBtStatusField{QStringLiteral("current_stance"),
                          static_cast<qint64>(status.currentStance)},
      SentryBtStatusField{QStringLiteral("bt_remain_time"),
                          static_cast<qint64>(status.btRemainTime)},
      SentryBtStatusField{QStringLiteral("bt_game_status"),
                          static_cast<qint64>(status.btGameStatus)},
  };
}

}  // namespace RM
