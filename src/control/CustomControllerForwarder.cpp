// SPDX-License-Identifier: MIT
#include "CustomControllerForwarder.h"
#include "network/NetworkManager.h"
#include <QDebug>

CustomControllerForwarder::CustomControllerForwarder(RM::NetworkManager *network,
                                                       QObject *parent)
    : QObject(parent), m_network(network) {
#ifdef RM_HAS_MQTT
  if (m_network) {
    connect(m_network, &RM::NetworkManager::mqttConnectionStateChanged, this,
            [this](bool) { m_serial.clearInput(); });
  }
#endif
  connect(&m_serial, &CustomControllerSerial::errorOccurred, this,
          &CustomControllerForwarder::errorOccurred);
  connect(&m_serial, &CustomControllerSerial::disconnected, this,
          [this] {
            m_enabled = false;
            qWarning() << "Custom controller disconnected";
          });
  connect(&m_serial, &CustomControllerSerial::payloadReceived, this,
          [this](const QByteArray &payload) {
            if (m_enabled && m_serial.isOpen() && m_network &&
                payload.size() == CustomControllerParser::PayloadSize) {
              m_network->sendCustomControl(payload);
            }
          });
}

bool CustomControllerForwarder::start(const QString &devicePath, bool enabled) {
  stop();
  if (!enabled) return false;
  if (devicePath.trimmed().isEmpty()) {
    emit errorOccurred(QStringLiteral("Custom controller device path is required"));
    return false;
  }
  if (!m_serial.open(devicePath)) return false;
  m_enabled = true;
  qInfo() << "Custom controller forwarding started:" << devicePath;
  return true;
}

void CustomControllerForwarder::stop() {
  if (isActive()) qInfo() << "Custom controller forwarding stopped";
  m_enabled = false;
  m_serial.close();
}

bool CustomControllerForwarder::isActive() const {
  return m_enabled && m_serial.isOpen();
}
