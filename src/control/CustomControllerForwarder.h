// SPDX-License-Identifier: MIT
#ifndef CUSTOMCONTROLLERFORWARDER_H
#define CUSTOMCONTROLLERFORWARDER_H

#include "CustomControllerSerial.h"
#include <QObject>

namespace RM { class NetworkManager; }

class CustomControllerForwarder : public QObject {
  Q_OBJECT
public:
  explicit CustomControllerForwarder(RM::NetworkManager *network,
                                     QObject *parent = nullptr);
  bool start(const QString &devicePath, bool enabled);
  void stop();
  bool isActive() const;

signals:
  void errorOccurred(const QString &message);

private:
  RM::NetworkManager *m_network;
  CustomControllerSerial m_serial;
  bool m_enabled = false;
};
#endif
