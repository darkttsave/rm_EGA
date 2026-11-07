// SPDX-License-Identifier: MIT

#ifndef CUSTOMCONTROLLERSERIAL_H
#define CUSTOMCONTROLLERSERIAL_H

#include "CustomControllerParser.h"

#include <QObject>
#include <QSerialPort>
#include <QString>

/**
 * @brief QSerialPort adapter for the legacy custom controller.
 */
class CustomControllerSerial : public QObject {
  Q_OBJECT

public:
  explicit CustomControllerSerial(QObject *parent = nullptr);
  ~CustomControllerSerial() override;

  bool open(const QString &devicePath);
  void close();
  void clearInput();

  bool isOpen() const;
  QString devicePath() const;

signals:
  void payloadReceived(const QByteArray &payload);
  void errorOccurred(const QString &message);
  void disconnected();

private slots:
  void readAvailableData();
  void handleSerialError(QSerialPort::SerialPortError error);

private:
  QSerialPort m_serialPort;
  CustomControllerParser m_parser;
  bool m_opening = false;
};

#endif // CUSTOMCONTROLLERSERIAL_H
