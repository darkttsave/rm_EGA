// SPDX-License-Identifier: MIT
/**
 * @file test_sentry_bt_status_parser.cpp
 * @brief 哨兵 0x0310 4 字节状态解析器的单元测试。
 * @details 期望值由 tools/protocol_specs/sentry_bt_status_v1.json 独立推导，
 *          用于交叉校验 C++ 解析器与 JSON 协议描述是否一致。
 */

#include "network/SentryBtStatusParser.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QtTest>

#include <cstdint>

namespace {

QString specFilePath() {
  return QString::fromUtf8(
      RM26_SOURCE_DIR "/tools/protocol_specs/sentry_bt_status_v1.json");
}

QString parserSourcePath() {
  return QString::fromUtf8(RM26_SOURCE_DIR
                           "/src/network/SentryBtStatusParser.cpp");
}

QJsonObject loadSpec() {
  QFile file(specFilePath());
  if (!file.open(QIODevice::ReadOnly)) {
    return QJsonObject();
  }
  return QJsonDocument::fromJson(file.readAll()).object();
}

/**
 * @brief 完全依据 JSON 字段描述解码，作为解析器的独立对照实现。
 */
qint64 decodeBySpec(const QByteArray &data, int offset, const QString &type,
                    const QString &byteOrder) {
  const bool littleEndian = (byteOrder != QStringLiteral("big"));
  const auto byteAt = [&data](int index) -> quint64 {
    return static_cast<quint8>(data.at(index));
  };

  if (type == QStringLiteral("uint8")) {
    return static_cast<qint64>(byteAt(offset));
  }
  if (type == QStringLiteral("int8")) {
    return static_cast<qint64>(static_cast<qint8>(byteAt(offset)));
  }
  if (type == QStringLiteral("uint16") || type == QStringLiteral("int16")) {
    const quint64 first = byteAt(offset);
    const quint64 second = byteAt(offset + 1);
    const quint64 raw = littleEndian ? (first | (second << 8))
                                     : ((first << 8) | second);
    if (type == QStringLiteral("int16")) {
      return static_cast<qint64>(static_cast<qint16>(raw));
    }
    return static_cast<qint64>(raw);
  }
  return -1;
}

}  // namespace

class TestSentryBtStatusParser : public QObject {
  Q_OBJECT

private slots:
  /// 1. 合法 4 字节载荷解析成功
  void parsesValidFourBytePayload() {
    const RM::SentryBtStatusParseResult result =
        RM::parseSentryBtStatus(QByteArray::fromHex("01341209"));

    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.status.currentStance, static_cast<uint8_t>(0x01));
    QCOMPARE(result.status.btRemainTime, static_cast<uint16_t>(0x1234));
    QCOMPARE(result.status.btGameStatus, static_cast<uint8_t>(0x09));
    QCOMPARE(result.error, QString());
    QCOMPARE(result.rawHex, QStringLiteral("01 34 12 09"));
  }

  /// 2. 多字节字段按 JSON 规定的字节序（小端）解析
  void parsesMultiByteFieldAsLittleEndian() {
    const RM::SentryBtStatusParseResult low =
        RM::parseSentryBtStatus(QByteArray::fromHex("000100ff"));
    const RM::SentryBtStatusParseResult high =
        RM::parseSentryBtStatus(QByteArray::fromHex("000001ff"));

    QCOMPARE(low.status.btRemainTime, static_cast<uint16_t>(0x0001));
    QCOMPARE(high.status.btRemainTime, static_cast<uint16_t>(0x0100));
    // 若误按大端解释，两个用例会得到相反结果
    QVERIFY(low.status.btRemainTime != high.status.btRemainTime);
  }

  /// 3. 长度为 0、3、5 时解析失败
  void rejectsLengthsOtherThanFour() {
    const QList<int> lengths{0, 3, 5};
    for (int length : lengths) {
      const QByteArray data(length, '\x7f');
      const RM::SentryBtStatusParseResult result = RM::parseSentryBtStatus(data);

      QVERIFY2(!result.ok,
               qPrintable(QStringLiteral("length %1 必须解析失败").arg(length)));
      QVERIFY(!result.error.isEmpty());
      QVERIFY(result.error.contains(QStringLiteral("payload_length")));
    }
  }

  /// 3b. 超长/其它非目标长度同样失败，不会误报为哨兵状态
  void rejectsNonTargetLengths() {
    for (int length : {1, 2, 6, 28, 100, 300, 303}) {
      const QByteArray data(length, '\x00');
      QVERIFY2(!RM::parseSentryBtStatus(data).ok,
               qPrintable(QStringLiteral("length %1 必须解析失败").arg(length)));
    }
  }

  /// 4. 边界原始值不越界、不崩溃
  void handlesBoundaryValues() {
    const RM::SentryBtStatusParseResult maxed =
        RM::parseSentryBtStatus(QByteArray::fromHex("ffffffff"));
    QVERIFY(maxed.ok);
    QCOMPARE(maxed.status.currentStance, static_cast<uint8_t>(0xFF));
    QCOMPARE(maxed.status.btRemainTime, static_cast<uint16_t>(0xFFFF));
    QCOMPARE(maxed.status.btGameStatus, static_cast<uint8_t>(0xFF));

    const RM::SentryBtStatusParseResult zeroed =
        RM::parseSentryBtStatus(QByteArray::fromHex("00000000"));
    QVERIFY(zeroed.ok);  // 全零是合法原始值，与“解析失败”必须区分
    QCOMPARE(zeroed.status.btRemainTime, static_cast<uint16_t>(0));
  }

  /// 解析失败必须明确返回失败，而不是给出一份全零的“合法”状态
  void failedParseIsNotAZeroedStatus() {
    const RM::SentryBtStatusParseResult result =
        RM::parseSentryBtStatus(QByteArray::fromHex("000000"));

    QVERIFY(!result.ok);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(result.rawHex == QStringLiteral("00 00 00"));
  }

  /// 5. 解析结果与 sentry_bt_status_v1.json 一致
  void matchesProtocolSpecFile() {
    const QJsonObject spec = loadSpec();
    QVERIFY2(!spec.isEmpty(),
             qPrintable(QStringLiteral("无法读取 %1").arg(specFilePath())));

    QCOMPARE(spec.value(QStringLiteral("mqtt_topic")).toString(),
             QStringLiteral("CustomByteBlock"));
    QCOMPARE(spec.value(QStringLiteral("protobuf_message_type")).toString(),
             QStringLiteral("robomaster.CustomByteBlock"));
    QCOMPARE(spec.value(QStringLiteral("source_command_id")).toString(),
             QStringLiteral("0x0310"));
    QCOMPARE(spec.value(QStringLiteral("payload_length")).toInt(),
             RM::kSentryBtStatusPayloadLength);

    const QJsonArray fields = spec.value(QStringLiteral("fields")).toArray();
    QVERIFY(!fields.isEmpty());

    // 用一份可区分各字节的载荷，逐字段与 JSON 描述对照
    const QByteArray probe = QByteArray::fromHex("aa3412bb");
    const RM::SentryBtStatusParseResult parsed = RM::parseSentryBtStatus(probe);
    QVERIFY(parsed.ok);

    const QList<RM::SentryBtStatusField> values =
        RM::sentryBtStatusFieldList(parsed.status);
    QCOMPARE(values.size(), fields.size());

    for (int i = 0; i < fields.size(); ++i) {
      const QJsonObject field = fields.at(i).toObject();
      const QString name = field.value(QStringLiteral("name")).toString();
      const int offset = field.value(QStringLiteral("offset")).toInt();
      const QString type = field.value(QStringLiteral("type")).toString();
      const QString order = field.value(QStringLiteral("byte_order")).toString();

      // 字段名与顺序必须与 JSON 完全一致
      QCOMPARE(values.at(i).name, name);
      // 实测值必须等于按 JSON 描述独立解码的结果
      QCOMPARE(values.at(i).value, decodeBySpec(probe, offset, type, order));
    }
  }

  /// 5b. 字段列表顺序稳定，且覆盖 JSON 定义的全部字段
  void fieldListCoversSpecFieldsInOrder() {
    const QJsonArray fields =
        loadSpec().value(QStringLiteral("fields")).toArray();
    const QList<RM::SentryBtStatusField> empty =
        RM::sentryBtStatusFieldList(RM::SentryBtStatus{});

    QCOMPARE(empty.size(), fields.size());
    for (int i = 0; i < fields.size(); ++i) {
      QCOMPARE(empty.at(i).name,
               fields.at(i).toObject().value(QStringLiteral("name")).toString());
      QCOMPARE(empty.at(i).value, static_cast<qint64>(0));
    }
  }

  /// 6. 不引入未经确认的姿态业务语义
  void exposesRawValuesWithoutBusinessSemantics() {
    const RM::SentryBtStatusParseResult result =
        RM::parseSentryBtStatus(QByteArray::fromHex("01000002"));
    QVERIFY(result.ok);

    const QList<RM::SentryBtStatusField> values =
        RM::sentryBtStatusFieldList(result.status);
    QCOMPARE(values.at(0).name, QStringLiteral("current_stance"));
    QCOMPARE(values.at(0).value, static_cast<qint64>(1));

    const QStringList forbidden{QStringLiteral("attack"), QStringLiteral("defend"),
                               QStringLiteral("retreat"), QStringLiteral("idle"),
                               QStringLiteral("进攻"), QStringLiteral("防守"),
                               QStringLiteral("撤退"), QStringLiteral("待命")};

    QFile source(parserSourcePath());
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QString sourceText = QString::fromUtf8(source.readAll());

    for (const QString &word : forbidden) {
      for (const RM::SentryBtStatusField &field : values) {
        QVERIFY(!field.name.contains(word, Qt::CaseInsensitive));
      }
      QVERIFY2(!sourceText.contains(word, Qt::CaseInsensitive),
               qPrintable(QStringLiteral("解析器不得出现业务语义：%1").arg(word)));
    }
  }

  /// 解析器不依赖全局状态：连续解析互不影响
  void parsingIsStatelessAcrossCalls() {
    const RM::SentryBtStatusParseResult first =
        RM::parseSentryBtStatus(QByteArray::fromHex("01000002"));
    const RM::SentryBtStatusParseResult bad =
        RM::parseSentryBtStatus(QByteArray::fromHex("0100"));
    const RM::SentryBtStatusParseResult second =
        RM::parseSentryBtStatus(QByteArray::fromHex("03040506"));

    QVERIFY(first.ok);
    QVERIFY(!bad.ok);
    QVERIFY(second.ok);
    QCOMPARE(second.status.currentStance, static_cast<uint8_t>(0x03));
    QCOMPARE(second.status.btRemainTime, static_cast<uint16_t>(0x0504));
    QCOMPARE(second.status.btGameStatus, static_cast<uint8_t>(0x06));
  }
};

QTEST_MAIN(TestSentryBtStatusParser)
#include "test_sentry_bt_status_parser.moc"
