// SPDX-License-Identifier: MIT

#include "CustomControllerParser.h"

#include "network/CRC.h"

#include <algorithm>

namespace {
constexpr unsigned char kSof = 0xA5;
constexpr quint16 kCommandId = 0x0302;
constexpr qsizetype kHeaderSize = 5;
constexpr qsizetype kCommandSize = 2;

// The repository's shared CRC8 table does not match DJI's published table.
// Keep the protocol-correct implementation local instead of changing a shared
// module outside this task's scope. The reflected form of polynomial 0x31 is
// 0x8C, with the RoboMaster initial value 0xFF.
bool verifyRoboMasterHeaderCrc8(const QByteArray &frame) {
  quint8 crc = 0xFF;
  for (qsizetype index = 0; index < kHeaderSize - 1; ++index) {
    crc ^= static_cast<quint8>(frame.at(index));
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1) ? static_cast<quint8>((crc >> 1) ^ 0x8C)
                      : static_cast<quint8>(crc >> 1);
    }
  }
  return crc == static_cast<quint8>(frame.at(kHeaderSize - 1));
}
} // namespace

QList<QByteArray> CustomControllerParser::feed(const QByteArray &bytes) {
  QList<QByteArray> payloads;

  const auto parseBufferedData = [this, &payloads]() {
    while (!m_buffer.isEmpty()) {
      const qsizetype sofIndex = m_buffer.indexOf(static_cast<char>(kSof));
      if (sofIndex < 0) {
        m_buffer.clear();
        break;
      }
      if (sofIndex > 0) {
        m_buffer.remove(0, sofIndex);
      }

      if (m_buffer.size() < kHeaderSize) {
        break;
      }

      if (!verifyRoboMasterHeaderCrc8(m_buffer)) {
        m_buffer.remove(0, 1);
        continue;
      }

      const quint16 dataLength =
          static_cast<quint8>(m_buffer.at(1)) |
          (static_cast<quint16>(static_cast<quint8>(m_buffer.at(2))) << 8);
      if (dataLength != PayloadSize) {
        m_buffer.remove(0, 1);
        continue;
      }

      if (m_buffer.size() < kHeaderSize + kCommandSize) {
        break;
      }
      const quint16 commandId =
          static_cast<quint8>(m_buffer.at(5)) |
          (static_cast<quint16>(static_cast<quint8>(m_buffer.at(6))) << 8);
      if (commandId != kCommandId) {
        m_buffer.remove(0, 1);
        continue;
      }

      if (m_buffer.size() < FrameSize) {
        break;
      }
      auto *frame = reinterpret_cast<unsigned char *>(m_buffer.data());
      if (!Verify_CRC16_Check_Sum(frame, static_cast<uint32_t>(FrameSize))) {
        m_buffer.remove(0, 1);
        continue;
      }

      payloads.append(m_buffer.mid(kHeaderSize + kCommandSize, PayloadSize));
      m_buffer.remove(0, FrameSize);
    }
  };

  // Consume large caller-owned chunks incrementally so the internal receive
  // buffer never exceeds its limit and valid frames are not dropped.
  qsizetype offset = 0;
  while (offset < bytes.size()) {
    const qsizetype capacity = MaxBufferSize - m_buffer.size();
    const qsizetype chunkSize =
        std::min(capacity, bytes.size() - offset);
    m_buffer.append(bytes.constData() + offset, chunkSize);
    offset += chunkSize;
    parseBufferedData();
  }

  return payloads;
}

void CustomControllerParser::reset() { m_buffer.clear(); }

qsizetype CustomControllerParser::bufferedByteCount() const {
  return m_buffer.size();
}
