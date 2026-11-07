// SPDX-License-Identifier: MIT

#include "control/CustomControllerParser.h"

#include <QtTest>

namespace {

quint8 independentCrc8(const QByteArray &bytes) {
  quint8 crc = 0xFF;
  for (const char value : bytes) {
    crc ^= static_cast<quint8>(value);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1) ? static_cast<quint8>((crc >> 1) ^ 0x8C)
                      : static_cast<quint8>(crc >> 1);
    }
  }
  return crc;
}

quint16 independentCrc16(const QByteArray &bytes) {
  quint16 crc = 0xFFFF;
  for (const char value : bytes) {
    crc ^= static_cast<quint8>(value);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1) ? static_cast<quint16>((crc >> 1) ^ 0x8408)
                      : static_cast<quint16>(crc >> 1);
    }
  }
  return crc;
}

QByteArray makeFrame(const QByteArray &payload, quint16 commandId = 0x0302,
                     quint16 declaredLength = 30, quint8 sequence = 0) {
  QByteArray frame;
  frame.reserve(9 + payload.size());
  frame.append(char(0xA5));
  frame.append(static_cast<char>(declaredLength & 0xFF));
  frame.append(static_cast<char>((declaredLength >> 8) & 0xFF));
  frame.append(static_cast<char>(sequence));
  frame.append(static_cast<char>(independentCrc8(frame)));
  frame.append(static_cast<char>(commandId & 0xFF));
  frame.append(static_cast<char>((commandId >> 8) & 0xFF));
  frame.append(payload);
  const quint16 crc = independentCrc16(frame);
  frame.append(static_cast<char>(crc & 0xFF));
  frame.append(static_cast<char>((crc >> 8) & 0xFF));
  return frame;
}

QByteArray sequentialPayload() {
  QByteArray payload;
  for (int value = 0; value < CustomControllerParser::PayloadSize; ++value) {
    payload.append(static_cast<char>(value));
  }
  return payload;
}

} // namespace

class TestCustomControllerParser : public QObject {
  Q_OBJECT

private slots:
  void validKnownFrame() {
    // Fixed vector calculated independently with the reflected RoboMaster
    // polynomials (CRC8 0x8C, CRC16 0x8408), not the production CRC helpers.
    const QByteArray frame = QByteArray::fromHex(
        "A51E0042870203000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D6DAE");

    CustomControllerParser parser;
    const QList<QByteArray> result = parser.feed(frame);
    QCOMPARE(result.size(), 1);
    QCOMPARE(result.first(), sequentialPayload());
  }

  void splitFrame() {
    const QByteArray payload = sequentialPayload();
    const QByteArray frame = makeFrame(payload);
    CustomControllerParser parser;

    QVERIFY(parser.feed(frame.left(1)).isEmpty());
    QVERIFY(parser.feed(frame.mid(1, 4)).isEmpty());
    QVERIFY(parser.feed(frame.mid(5, 17)).isEmpty());
    const QList<QByteArray> result = parser.feed(frame.mid(22));
    QCOMPARE(result, QList<QByteArray>{payload});
  }

  void consecutiveFrames() {
    const QByteArray first(30, char(0x11));
    const QByteArray second(30, char(0xE2));
    CustomControllerParser parser;
    QCOMPARE(parser.feed(makeFrame(first, 0x0302, 30, 1) +
                         makeFrame(second, 0x0302, 30, 2)),
             (QList<QByteArray>{first, second}));
  }

  void crc8Error() {
    QByteArray frame = makeFrame(sequentialPayload());
    frame[4] ^= char(0x01);
    CustomControllerParser parser;
    QVERIFY(parser.feed(frame).isEmpty());
  }

  void crc16Error() {
    QByteArray frame = makeFrame(sequentialPayload());
    frame[frame.size() - 1] ^= char(0x01);
    CustomControllerParser parser;
    QVERIFY(parser.feed(frame).isEmpty());
  }

  void commandIdError() {
    CustomControllerParser parser;
    QVERIFY(parser.feed(makeFrame(sequentialPayload(), 0x0301)).isEmpty());
  }

  void dataLengthError() {
    CustomControllerParser parser;
    QVERIFY(parser.feed(makeFrame(sequentialPayload(), 0x0302, 29)).isEmpty());
  }

  void resynchronizesAfterGarbage() {
    const QByteArray payload = sequentialPayload();
    CustomControllerParser parser;
    QCOMPARE(parser.feed(QByteArray::fromHex("0012A50102030455") +
                         makeFrame(payload)),
             QList<QByteArray>{payload});
  }

  void sofInsidePayload() {
    QByteArray payload = sequentialPayload();
    payload[3] = char(0xA5);
    payload[17] = char(0xA5);
    CustomControllerParser parser;
    QCOMPARE(parser.feed(makeFrame(payload)), QList<QByteArray>{payload});
  }

  void payloadBytesAreUnchanged() {
    const QByteArray payload = QByteArray::fromHex(
        "0000FF7F8001A5FE102030405060708090A0B0C0D0E0F011223344556677");
    QCOMPARE(payload.size(), CustomControllerParser::PayloadSize);
    CustomControllerParser parser;
    const QList<QByteArray> result = parser.feed(makeFrame(payload));
    QCOMPARE(result.first().toHex(), payload.toHex());
  }

  void oversizedGarbageIsBounded() {
    const QByteArray first(30, char(0x31));
    const QByteArray second(30, char(0x72));
    const QByteArray input =
        QByteArray(CustomControllerParser::MaxBufferSize * 2, char(0x55)) +
        makeFrame(first) +
        QByteArray(CustomControllerParser::MaxBufferSize * 2, char(0x44)) +
        makeFrame(second);
    CustomControllerParser parser;
    QCOMPARE(parser.feed(input), (QList<QByteArray>{first, second}));
    QVERIFY(parser.bufferedByteCount() <=
            CustomControllerParser::MaxBufferSize);
  }

  void resetClearsPartialFrame() {
    const QByteArray payload = sequentialPayload();
    const QByteArray frame = makeFrame(payload);
    CustomControllerParser parser;
    QVERIFY(parser.feed(frame.left(20)).isEmpty());
    QVERIFY(parser.bufferedByteCount() > 0);

    parser.reset();
    QCOMPARE(parser.bufferedByteCount(), qsizetype(0));
    QVERIFY(parser.feed(frame.mid(20)).isEmpty());
    QCOMPARE(parser.feed(frame), QList<QByteArray>{payload});
  }
};

QTEST_APPLESS_MAIN(TestCustomControllerParser)
#include "test_custom_controller_parser.moc"
