// SPDX-License-Identifier: MIT
#include "devhooks/ClientDevHooks.h"
#include "network/NetworkManager.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

namespace {
quint16 unusedPort() {
  QTcpServer server;
  if (!server.listen(QHostAddress::LocalHost, 0)) return 0;
  return server.serverPort();
}

QJsonObject readState(quint16 port) {
  QNetworkAccessManager client;
  client.setProxy(QNetworkProxy::NoProxy);
  auto *reply = client.get(QNetworkRequest(
      QUrl(QStringLiteral("http://127.0.0.1:%1/state").arg(port))));
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  timeout.start(5000);
  loop.exec();
  if (!reply->isFinished() || reply->error() != QNetworkReply::NoError) return {};
  return QJsonDocument::fromJson(reply->readAll()).object()
      .value(QStringLiteral("network_state")).toObject()
      .value(QStringLiteral("custom_control")).toObject();
}
} // namespace

class TestMqttTxDevHooks : public QObject {
  Q_OBJECT
private slots:
  void stateReflectsRealPublisher() {
    const quint16 httpPort = unusedPort();
    QVERIFY(httpPort);
    qputenv("RM_DEVTOOLS_PORT", QByteArray::number(httpPort));
    RM::NetworkManager network(nullptr);
    RM::ClientDevHooks hooks(nullptr, &network, nullptr, nullptr, nullptr, nullptr);
    QVERIFY(hooks.start());
    auto state = readState(httpPort);
    QVERIFY(!state.isEmpty());
    QCOMPARE(state.value("mqtt_connected").toBool(), false);
    QVERIFY(state.value("broker").isNull());
    QCOMPARE(state.value("controller").toObject().value("published").toInt(), 0);
    QVERIFY(!network.sendCustomControl(QByteArray(30, 'x')));
    state = readState(httpPort);
    QCOMPARE(state.value("controller").toObject().value("dropped").toInt(), 1);

#ifndef RM_HAS_MQTT
    QSKIP("MQTT support unavailable");
#else
    if (QStandardPaths::findExecutable(QStringLiteral("mosquitto")).isEmpty())
      QSKIP("mosquitto executable unavailable");
    const quint16 brokerPort = unusedPort();
    QVERIFY(brokerPort);
    QProcess broker;
    broker.start(QStringLiteral("mosquitto"), {QStringLiteral("-p"), QString::number(brokerPort)});
    QVERIFY(broker.waitForStarted());
    QTcpSocket probe;
    probe.setProxy(QNetworkProxy::NoProxy);
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
      probe.abort();
      probe.connectToHost(QHostAddress::LocalHost, brokerPort);
      return probe.waitForConnected(100);
    })(), 5000);
    const QString uri = QStringLiteral("tcp://127.0.0.1:%1").arg(brokerPort);
    network.startMqtt(uri, QStringLiteral("task-v08-test"));
    QTRY_VERIFY_WITH_TIMEOUT(network.isMqttConnected(), 5000);
    QVERIFY(network.sendCustomControl(QByteArray(30, 'c')));
    QVERIFY(!network.sendCustomControl(QByteArray(30, 'd')));
    network.sendData(QByteArray(10, 'o'), QHostAddress::LocalHost, 0);
    QTRY_COMPARE_WITH_TIMEOUT(network.customControlOutpostStats().published, quint64(1), 5000);
    state = readState(httpPort);
    QCOMPARE(state.value("mqtt_connected").toBool(), true);
    QCOMPARE(state.value("broker").toString(), uri);
    QCOMPARE(state.value("controller").toObject().value("published").toInt(), 1);
    QCOMPARE(state.value("controller").toObject().value("dropped").toInt(), 2);
    QCOMPARE(state.value("outpost").toObject().value("published").toInt(), 1);
    QCOMPARE(state.value("outpost").toObject().value("dropped").toInt(), 0);
    QProcess monitor;
    monitor.start(QStringLiteral("python3"),
                  {QStringLiteral(RM_PROJECT_SOURCE_DIR "/tools/mqtt_tx_monitor.py"),
                   QStringLiteral("--port"), QString::number(httpPort),
                   QStringLiteral("--once")});
    QTRY_COMPARE_WITH_TIMEOUT(monitor.state(), QProcess::NotRunning, 5000);
    QCOMPARE(monitor.exitCode(), 0);
    const QString output = QString::fromUtf8(monitor.readAllStandardOutput());
    QVERIFY(output.contains(QStringLiteral("MQTT: 已连接")));
    QVERIFY(output.contains(QStringLiteral("控制器 30B: 本地接受=1 丢弃=2")));
    QVERIFY(output.contains(QStringLiteral("前哨站 10B: 本地接受=1 丢弃=0")));
    QVERIFY(output.contains(uri));
    network.stopMqtt();
    state = readState(httpPort);
    QCOMPARE(state.value("mqtt_connected").toBool(), false);
    QCOMPARE(state.value("broker").toString(), uri);
    broker.terminate();
    QVERIFY(broker.waitForFinished(5000));
#endif
  }
};
QTEST_MAIN(TestMqttTxDevHooks)
#include "test_mqtt_tx_devhooks.moc"
