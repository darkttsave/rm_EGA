// SPDX-License-Identifier: MIT
/**
 * @file SentryBtStatusParser.h
 * @brief 哨兵自定义 4 字节状态（官方命令字 0x0310）的独立解析器。
 * @details 机器可读的协议描述位于 tools/protocol_specs/sentry_bt_status_v1.json，
 *          本解析器是它在客户端侧的唯一 C++ 实现。
 *
 *          本文件只负责字节布局解析：
 *          - 不含 QML；
 *          - 不负责 MQTT；
 *          - 不负责 UI；
 *          - 不解释姿态/局状态业务语义；
 *          - 不操作全局状态。
 */

#ifndef SENTRYBTSTATUSPARSER_H
#define SENTRYBTSTATUSPARSER_H

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstdint>

namespace RM {

/**
 * @brief 协议要求的 CustomByteBlock.data 长度。
 * @details 与 sentry_bt_status_v1.json 的 payload_length 一致，
 *          由 test_sentry_bt_status_parser 与 JSON 交叉校验。
 */
constexpr int kSentryBtStatusPayloadLength = 4;

/**
 * @brief 哨兵自定义状态的原始取值。
 * @details 只保留官方命令字 0x0310 传出的原始数值，不携带业务语义。
 */
struct SentryBtStatus {
  uint8_t currentStance = 0;  ///< offset 0，uint8
  uint16_t btRemainTime = 0;  ///< offset 1-2，uint16 小端
  uint8_t btGameStatus = 0;   ///< offset 3，uint8
};

/**
 * @brief 解析结果。
 * @details ok 为 false 时 status 中的数值没有任何意义，调用方不得使用，
 *          以免把失败当成一份全零的“合法”状态。
 */
struct SentryBtStatusParseResult {
  bool ok = false;          ///< 是否解析成功
  SentryBtStatus status;    ///< 仅在 ok 为 true 时有效
  QString rawHex;           ///< 诊断用：原始字节的十六进制表示
  QString error;            ///< ok 为 false 时的简单失败原因
};

/**
 * @brief 字段名与原始数值的配对，用于把字段清单交给状态层/UI。
 * @details name 与 sentry_bt_status_v1.json 中 fields[].name 完全一致，
 *          使字段名在 C++ 侧只有一个来源。
 */
struct SentryBtStatusField {
  QString name;  ///< JSON 中定义的字段名
  qint64 value = 0;  ///< 原始数值，未做任何语义解释
};

/**
 * @brief 解析 CustomByteBlock.data 中的哨兵 4 字节状态。
 * @param data 已从 CustomByteBlock Protobuf 中取出的业务字节。
 * @return 解析结果；长度不等于 kSentryBtStatusPayloadLength 时 ok 为 false。
 * @note 长度不符时不会返回任何合法状态值，也不会修改任何全局状态。
 */
SentryBtStatusParseResult parseSentryBtStatus(const QByteArray &data);

/**
 * @brief 按 JSON 中 fields 的顺序给出字段名与原始数值。
 * @param status 已解析成功的状态。
 */
QList<SentryBtStatusField> sentryBtStatusFieldList(const SentryBtStatus &status);

}  // namespace RM

#endif  // SENTRYBTSTATUSPARSER_H
