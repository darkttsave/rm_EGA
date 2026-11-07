// SPDX-License-Identifier: MIT

#ifndef CUSTOMCONTROLLERPARSER_H
#define CUSTOMCONTROLLERPARSER_H

#include <QByteArray>
#include <QList>
#include <QtGlobal>

/**
 * @brief Incremental parser for the legacy RoboMaster custom-controller frame.
 *
 * The parser has no serial-port dependency. Call feed() with arbitrary chunks;
 * every returned QByteArray is the unmodified 30-byte payload of one valid
 * 0x0302 frame.
 */
class CustomControllerParser {
public:
  static constexpr qsizetype PayloadSize = 30;
  static constexpr qsizetype FrameSize = 39;
  static constexpr qsizetype MaxBufferSize = 4096;

  QList<QByteArray> feed(const QByteArray &bytes);
  void reset();

  qsizetype bufferedByteCount() const;

private:
  QByteArray m_buffer;
};

#endif // CUSTOMCONTROLLERPARSER_H
