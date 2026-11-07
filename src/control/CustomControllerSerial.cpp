// SPDX-License-Identifier: MIT

#include "CustomControllerSerial.h"

#include <QIODevice>

CustomControllerSerial::CustomControllerSerial(QObject *parent)
    : QObject(parent) {
  connect(&m_serialPort, &QSerialPort::readyRead, this,
          &CustomControllerSerial::readAvailableData);
  connect(&m_serialPort, &QSerialPort::errorOccurred, this,
          &CustomControllerSerial::handleSerialError);
}

CustomControllerSerial::~CustomControllerSerial() { close(); }

bool CustomControllerSerial::open(const QString &devicePath) {
  close();

  if (devicePath.isEmpty()) {
    emit errorOccurred(QStringLiteral("Custom controller device path is empty"));
    return false;
  }

  m_parser.reset();
  m_serialPort.setPortName(devicePath);
  m_serialPort.setBaudRate(QSerialPort::Baud115200);
  m_serialPort.setDataBits(QSerialPort::Data8);
  m_serialPort.setParity(QSerialPort::NoParity);
  m_serialPort.setStopBits(QSerialPort::OneStop);
  m_serialPort.setFlowControl(QSerialPort::NoFlowControl);

  m_opening = true;
  const bool opened = m_serialPort.open(QIODevice::ReadOnly);
  m_opening = false;
  if (!opened) {
    emit errorOccurred(
        QStringLiteral("Failed to open custom controller %1: %2")
            .arg(devicePath, m_serialPort.errorString()));
    m_parser.reset();
    return false;
  }

  // Discard bytes accumulated by the device while this port was closed.
  m_serialPort.clear(QSerialPort::Input);

  return true;
}

void CustomControllerSerial::close() {
  if (m_serialPort.isOpen()) {
    m_serialPort.close();
  }
  m_parser.reset();
}

void CustomControllerSerial::clearInput() {
  m_parser.reset();
  if (m_serialPort.isOpen()) m_serialPort.clear(QSerialPort::Input);
}

bool CustomControllerSerial::isOpen() const { return m_serialPort.isOpen(); }

QString CustomControllerSerial::devicePath() const {
  return m_serialPort.portName();
}

void CustomControllerSerial::readAvailableData() {
  const QList<QByteArray> payloads = m_parser.feed(m_serialPort.readAll());
  for (const QByteArray &payload : payloads) {
    emit payloadReceived(payload);
  }
}

void CustomControllerSerial::handleSerialError(
    QSerialPort::SerialPortError error) {
  if (error == QSerialPort::NoError || m_opening) {
    return;
  }

  emit errorOccurred(
      QStringLiteral("Custom controller serial error: %1")
          .arg(m_serialPort.errorString()));

  if (error == QSerialPort::ResourceError || error == QSerialPort::ReadError ||
      error == QSerialPort::DeviceNotFoundError) {
    const bool wasOpen = m_serialPort.isOpen();
    close();
    if (wasOpen) {
      emit disconnected();
    }
  }
}
