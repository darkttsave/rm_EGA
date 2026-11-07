// SPDX-License-Identifier: MIT
#include "control/CustomControllerForwarder.h"
#include "control/CustomControllerSerial.h"
#include "network/MqttManager.h"
#include "network/NetworkManager.h"
#include "robomaster.pb.h"

#include <QProcess>
#include <QMutex>
#include <QMutexLocker>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkProxy>
#include <QElapsedTimer>
#include <QtTest>
#include <pty.h>
#include <unistd.h>

namespace {
// waitForStarted() only confirms process creation, not that mosquitto has bound
// its listener. Probe the actual port before starting or resuming MQTT work.
bool waitForBroker(QProcess &broker, quint16 port) {
  QElapsedTimer deadline;
  deadline.start();
  while (deadline.elapsed() < 5000 && broker.state() == QProcess::Running) {
    QTcpSocket socket;
    socket.setProxy(QNetworkProxy::NoProxy);
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (socket.waitForConnected(100)) return true;
    QTest::qWait(20);
  }
  return false;
}

QMutex capturedMutex;
QStringList capturedWarnings;
QStringList capturedInfo;
void captureMessages(QtMsgType type, const QMessageLogContext &, const QString &text) {
  QMutexLocker lock(&capturedMutex);
  if (type == QtWarningMsg) capturedWarnings.append(text);
  if (type == QtInfoMsg) capturedInfo.append(text);
}
class CaptureWarnings {
public:
  CaptureWarnings() { capturedWarnings.clear(); capturedInfo.clear(); previous = qInstallMessageHandler(captureMessages); }
  ~CaptureWarnings() { qInstallMessageHandler(previous); }
  QStringList infoContaining(const QString &needle) const {
    QMutexLocker lock(&capturedMutex);
    return capturedInfo.filter(needle);
  }
  QStringList containing(const QString &needle) const {
    QMutexLocker lock(&capturedMutex);
    return capturedWarnings.filter(needle);
  }
private:
  QtMessageHandler previous = nullptr;
};
}

class TestCustomControllerMqtt : public QObject {
  Q_OBJECT
private slots:
  void openFailureEmitsOnce() {
    CustomControllerSerial serial;
    QSignalSpy errors(&serial, &CustomControllerSerial::errorOccurred);
    QVERIFY(!serial.open(QStringLiteral("/dev/rm-nonexistent-task8-e")));
    QCOMPARE(errors.size(), 1);
  }

  void failureWarningsAreThrottledAndRecover() {
    if (QStandardPaths::findExecutable(QStringLiteral("mosquitto")).isEmpty())
      QSKIP("mosquitto executable unavailable");
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    const quint16 port = probe.serverPort();
    probe.close();
    QProcess broker;
    auto startBroker = [&] {
      broker.start(QStringLiteral("mosquitto"), {QStringLiteral("-p"), QString::number(port)});
      return broker.waitForStarted();
    };
    QVERIFY(startBroker());
    QVERIFY2(waitForBroker(broker, port), "Broker did not start listening within 5 seconds");
    const QString uri = QStringLiteral("tcp://127.0.0.1:%1").arg(port);
    RM::NetworkManager network(nullptr);
    CaptureWarnings logs;
    QVERIFY(!network.sendCustomControl(QByteArray()));
    QVERIFY(!logs.containing("CustomControl input/serialization failure").isEmpty());
    QVERIFY(logs.containing("CustomControl input/serialization failure").first().contains("invalid_input=1"));
    QCOMPARE(logs.containing("CustomControl network failure").size(), 0);
    network.startMqtt(uri, QStringLiteral("task8g-publisher"));
    QTRY_VERIFY_WITH_TIMEOUT(network.isMqttConnected(), 5000);
    QVERIFY(network.sendCustomControl(QByteArray(30, 'a')));
    for (int i = 0; i < 120; ++i) network.sendCustomControl(QByteArray(30, 'a'));
    QCOMPARE(logs.containing("CustomControl network failure").size(), 0);
    QVERIFY(logs.infoContaining("RM26_COMMAND_TIMING direction=tx topic=CustomControl").isEmpty());
    broker.kill();
    QVERIFY(broker.waitForFinished(5000));
    QTRY_VERIFY_WITH_TIMEOUT(!network.isMqttConnected(), 5000);
    for (int i = 0; i < 120; ++i) QVERIFY(!network.sendCustomControl(QByteArray(30, 'x')));
    auto warnings = logs.containing("CustomControl network failure");
    QCOMPARE(warnings.size(), 1);
    QVERIFY(warnings.first().contains("not_connected=1"));
    QTest::qWait(5100);
    QVERIFY(!network.sendCustomControl(QByteArray(30, 'x')));
    warnings = logs.containing("CustomControl network failure");
    QCOMPARE(warnings.size(), 2);
    QVERIFY(warnings.last().contains("not_connected=121"));
    MqttManager other;
    QVERIFY(!other.publish(QStringLiteral("CommonCommand"), QByteArray("x"), 0, 0));
    QVERIFY(!logs.containing("MqttManager: Cannot publish, not connected").isEmpty());
    QVERIFY(startBroker());
    QVERIFY2(waitForBroker(broker, port), "Restarted broker did not listen within 5 seconds");
    // Exercise the actual automatic recovery, allowing multiple 5-second
    // reconnect intervals. Avoid stop/start while an attempt is in flight:
    // that separate production race (H-1) is outside this test's scope.
    QTRY_VERIFY_WITH_TIMEOUT(network.isMqttConnected(), 15000);
    QVERIFY(network.sendCustomControl(QByteArray(30, 'z')));
    network.stopMqtt();
    QVERIFY(!network.sendCustomControl(QByteArray(30, 'z')));
    QCOMPARE(logs.containing("CustomControl network failure").size(), 3);
    QVERIFY(logs.containing("CustomControl network failure").last().contains("not_connected=1"));
    broker.terminate();
    QVERIFY(broker.waitForFinished(5000));
  }

  void localBrokerAndSerial() {
    if (QStandardPaths::findExecutable(QStringLiteral("mosquitto")).isEmpty())
      QSKIP("mosquitto executable unavailable");

    QTcpServer portProbe;
    QVERIFY(portProbe.listen(QHostAddress::LocalHost, 0));
    const quint16 port = portProbe.serverPort();
    portProbe.close();
    QProcess broker;
    broker.start(QStringLiteral("mosquitto"),
                 {QStringLiteral("-p"), QString::number(port)});
    QVERIFY(broker.waitForStarted());
    QVERIFY2(waitForBroker(broker, port), "Broker did not start listening within 5 seconds");

    const QString uri = QStringLiteral("tcp://127.0.0.1:%1").arg(port);
    MqttManager subscriber;
    QSignalSpy subscriberConnected(&subscriber, &MqttManager::connectCompleted);
    QSignalSpy messages(&subscriber, &MqttManager::messageReceived);
    subscriber.connectToBrokerAsync(uri, QStringLiteral("task8e-subscriber"));
    QTRY_VERIFY_WITH_TIMEOUT(!subscriberConnected.isEmpty(), 5000);
    QVERIFY(subscriber.isConnected());
    QVERIFY(subscriber.subscribe(QStringLiteral("CustomControl"), 0));

    RM::NetworkManager network(nullptr);
    QVERIFY(!network.sendCustomControl(QByteArray()));
    QVERIFY(!network.sendCustomControl(QByteArray(61, 'x')));
    QVERIFY(!network.sendCustomControl(QByteArray(30, 'x')));
    QSignalSpy connected(&network, &RM::NetworkManager::mqttConnectCompleted);
    network.startMqtt(uri, QStringLiteral("task8e-publisher"));
    QTRY_VERIFY_WITH_TIMEOUT(!connected.isEmpty(), 5000);
    QVERIFY(network.isMqttConnected());
    QVERIFY(!network.sendCustomControl(QByteArray()));
    QVERIFY(!network.sendCustomControl(QByteArray(61, 'x')));

    int master = -1, slave = -1;
    char slaveName[256] = {};
    QCOMPARE(openpty(&master, &slave, slaveName, nullptr, nullptr), 0);
    ::close(slave);
    CustomControllerForwarder forwarder(&network);
    QVERIFY(!forwarder.start(QString::fromLocal8Bit(slaveName), false));
    QVERIFY(!forwarder.isActive());
    QVERIFY(forwarder.start(QString::fromLocal8Bit(slaveName), true));
    const QByteArray frame = QByteArray::fromHex(
        "A51E0042870203000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D6DAE");
    const QByteArray expected = frame.mid(7, 30);
    QCOMPARE(::write(master, frame.constData(), frame.size()), ssize_t(frame.size()));
    QTRY_COMPARE_WITH_TIMEOUT(messages.size(), 1, 5000);
    robomaster::CustomControl decoded;
    QVERIFY(decoded.ParseFromArray(messages.at(0).at(1).toByteArray().constData(),
                                   messages.at(0).at(1).toByteArray().size()));
    QCOMPARE(QByteArray(decoded.data().data(), decoded.data().size()), expected);

    // Several complete frames in one serial read: the limiter drops excess,
    // without holding a controller packet for later replay.
    const QByteArray burst = frame + frame + frame;
    QTest::qWait(20);
    QCOMPARE(::write(master, burst.constData(), burst.size()), ssize_t(burst.size()));
    QTRY_COMPARE_WITH_TIMEOUT(messages.size(), 2, 5000);
    const QByteArray secondMessage = messages.at(1).at(1).toByteArray();
    QVERIFY(decoded.ParseFromArray(secondMessage.constData(), secondMessage.size()));
    QCOMPARE(QByteArray(decoded.data().data(), decoded.data().size()), expected);
    QTest::qWait(60);
    QCOMPARE(messages.size(), 2);
    QCOMPARE(network.customControlControllerStats().published, quint64(2));
    QVERIFY(network.customControlControllerStats().dropped >= 3); // includes offline frame

    // A rate-limited outpost update is delivered on the next slot even if
    // arm traffic continues, with its 10 bytes preserved.
    QTest::qWait(20);
    QVERIFY(network.sendCustomControl(QByteArray(30, 'a')));
    const QByteArray outpost = QByteArray::fromHex("0102030405060708090A");
    network.sendData(outpost, QHostAddress::LocalHost, 0);
    for (int i = 0; i < 30; ++i)
      network.sendCustomControl(QByteArray(30, 'b'));
    QTRY_COMPARE_WITH_TIMEOUT(network.customControlOutpostStats().published, quint64(1), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(messages.size() >= 4, 5000);
    const QByteArray encoded = messages.last().at(1).toByteArray();
    QVERIFY(decoded.ParseFromArray(encoded.constData(), encoded.size()));
    QCOMPARE(QByteArray(decoded.data().data(), decoded.data().size()), outpost);

    forwarder.stop();
    const int before = messages.size();
    ::write(master, frame.constData(), frame.size());
    QTest::qWait(80);
    QCOMPARE(messages.size(), before);
    QVERIFY(forwarder.start(QString::fromLocal8Bit(slaveName), true));
    QTest::qWait(50);
    QCOMPARE(messages.size(), before);

    // A partially received frame must not survive an MQTT reconnect either.
    QCOMPARE(::write(master, frame.constData(), 20), ssize_t(20));
    QTest::qWait(30);
    network.stopMqtt();
    ::write(master, frame.constData(), frame.size());
    QTest::qWait(50);
    network.startMqtt(uri, QStringLiteral("task8e-publisher"));
    QTRY_VERIFY_WITH_TIMEOUT(network.isMqttConnected(), 5000);
    QTest::qWait(80);
    QCOMPARE(messages.size(), before);
    QCOMPARE(::write(master, frame.constData() + 20, frame.size() - 20),
             ssize_t(frame.size() - 20));
    QTest::qWait(50);
    QCOMPARE(messages.size(), before);
    QCOMPARE(::write(master, frame.constData(), frame.size()), ssize_t(frame.size()));
    QTRY_COMPARE_WITH_TIMEOUT(messages.size(), before + 1, 5000);

    ::close(master);
    QTRY_VERIFY_WITH_TIMEOUT(!forwarder.isActive(), 5000);
    forwarder.stop();
    network.stopMqtt();
    subscriber.disconnect();
    broker.terminate();
    QVERIFY(broker.waitForFinished(5000));
  }
};

QTEST_GUILESS_MAIN(TestCustomControllerMqtt)
#include "test_custom_controller_mqtt.moc"
