#include "core/GameData.h"
#include "config/ConfigManager.h"
#include "network/MqttManager.h"
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QFile>
#include "ui/MainWindow.h"
#include "widgets/HeroVideoWidget.h"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickWidget>
#include <QSignalSpy>
#include <QWidget>
#include <QtTest>

#include "robomaster.pb.h"

namespace {

int childStackIndex(QWidget *parent, QObject *child) {
  if (!parent || !child) {
    return -1;
  }
  return parent->children().indexOf(child);
}

} // namespace

class TestMainWindowTacticalHotkeyIntegration : public QObject {
  Q_OBJECT
  QTemporaryDir configDir;

private slots:
  void initTestCase() {
    qputenv("RM_DISABLE_AUDIO", "1");
    qunsetenv("RM_CLIENT_ROBOT_ID");
    QVERIFY(configDir.isValid());
    const QString example = QFileInfo(QStringLiteral(__FILE__)).absolutePath() + "/../../config.example.json";
    QVERIFY(QFile::copy(example, configDir.filePath("config.json")));
    QVERIFY(ConfigManager::instance().loadConfig(configDir.filePath("config.json")));
    qunsetenv("RM_DEVTOOLS");
    qunsetenv("RM_S1_ENGINE_HOST");
    Q_INIT_RESOURCE(qml);
    Q_INIT_RESOURCE(resources);
  }

  void cleanupTestCase() {
    Q_CLEANUP_RESOURCE(resources);
    Q_CLEANUP_RESOURCE(qml);
  }

  void testIdentitySelectionAndLegacyCommandBoundaries() {
    RM::MainWindow window;
    auto *data = window.findChild<GameData *>();
    auto *network = window.findChild<RM::NetworkManager *>();
    QVERIFY(data);
    QVERIFY(network);
    QSignalSpy sent(network, &RM::NetworkManager::dataSent);
    for (int id : {1, 3, 7, 101, 103, 107}) {
      QVERIFY(QMetaObject::invokeMethod(&window, "onSettingsLoginRequested",
                                       Qt::DirectConnection, Q_ARG(int, id)));
      QCOMPARE(data->getCurrentRobotId(), id);
      QCOMPARE(ConfigManager::instance().getClientRobotId(), id);
#ifdef RM_HAS_MQTT
      QTRY_VERIFY_WITH_TIMEOUT(!network->isMqttConnecting(), 5000);
#endif
    }
    for (int id : {2, 102, 4, 201, 257}) {
      QVERIFY(QMetaObject::invokeMethod(&window, "onSettingsLoginRequested",
                                       Qt::DirectConnection, Q_ARG(int, id)));
      QCOMPARE(data->getCurrentRobotId(), 107);
      QCOMPARE(ConfigManager::instance().getClientRobotId(), 107);
    }
    network->sendAssemblyCommand(1, 1);
    network->sendHeroDeployMode(1);
    QCOMPARE(sent.count(), 0);
    data->setCurrentRobotId(0);
    QVERIFY(!network->sendKeyboardMouseControl(1, 2, 0, false, false, false, 1));
    network->sendCommonCommand(6, 1);
    QCOMPARE(sent.count(), 0);
  }

  void testInvalidConfiguredIdentityIsNotDefaulted() {
    qputenv("RM_CLIENT_ROBOT_ID", "102");
    {
      RM::MainWindow window;
      auto *data = window.findChild<GameData *>();
      auto *network = window.findChild<RM::NetworkManager *>();
      QVERIFY(data);
      QVERIFY(network);
      QCOMPARE(data->getCurrentRobotId(), 0);
      QSignalSpy sent(network, &RM::NetworkManager::dataSent);
      QVERIFY(!network->sendKeyboardMouseControl(1, 1, 0, false, false, false, 1));
      network->sendCommonCommand(6, 1);
      QCOMPARE(sent.count(), 0);
    }
    qunsetenv("RM_CLIENT_ROBOT_ID");
  }

  void testSelectionAndCommandSafetyWithLocalBroker() {
#ifndef RM_HAS_MQTT
    QSKIP("Paho MQTT unavailable");
#else
    if (QStandardPaths::findExecutable(QStringLiteral("mosquitto")).isEmpty()) QSKIP("mosquitto unavailable");
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) QSKIP("local listening unavailable");
    const quint16 port = probe.serverPort();
    probe.close();
    QProcess broker;
    broker.start(QStringLiteral("mosquitto"), {QStringLiteral("-p"), QString::number(port)});
    QVERIFY(broker.waitForStarted());
    QTest::qWait(150);
    QVERIFY(broker.state() == QProcess::Running);
    qputenv("RM_MQTT_BROKER", "127.0.0.1");
    qputenv("RM_MQTT_PORT", QByteArray::number(port));
    QVERIFY(ConfigManager::instance().setClientRobotId(1));
    {
      RM::MainWindow window;
      auto *data = window.findChild<GameData *>();
      auto *network = window.findChild<RM::NetworkManager *>();
      QVERIFY(data); QVERIFY(network);
      QTRY_VERIFY_WITH_TIMEOUT(network->isMqttConnected(), 5000);
      auto *mqtt = network->findChild<MqttManager *>();
      QVERIFY(mqtt);
      QCOMPARE(mqtt->clientId(), QStringLiteral("1"));
      auto *settings = window.findChild<QQuickWidget *>(QStringLiteral("settingsPanel"));
      QVERIFY(settings && settings->rootObject());
      for (int id : {3, 7, 101, 103, 107, 1, 101}) {
        QVariant selected;
        QVERIFY(QMetaObject::invokeMethod(settings->rootObject(), "selectRobotByShortcut",
                                         Q_RETURN_ARG(QVariant, selected), Q_ARG(QVariant, QVariant(id))));
        QVERIFY(selected.toBool());
        QVariant confirmed;
        QVERIFY(QMetaObject::invokeMethod(settings->rootObject(), "confirmRobotShortcutLogin",
                                         Q_RETURN_ARG(QVariant, confirmed)));
        QVERIFY(confirmed.toBool());
        QTRY_VERIFY_WITH_TIMEOUT(network->isMqttConnected() && mqtt->clientId() == QString::number(id), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(settings->rootObject()->property("mqttIdentity").toString(), QString::number(id), 5000);
        QCOMPARE(data->getCurrentRobotId(), id);
        QCOMPARE(ConfigManager::instance().getClientRobotId(), id);
      }
      QSignalSpy sent(network, &RM::NetworkManager::dataSent);
      network->sendAssemblyCommand(1, 1);
      network->sendHeroDeployMode(1);
      network->sendCommonCommand(2, 10);
      network->sendRobotPerformanceSelection(1, 1, 0);
      QCOMPARE(sent.count(), 0);
      network->sendCommonCommand(6, 1);
      QCOMPARE(sent.count(), 1);
      data->setCurrentRobotId(0);
      network->sendCommonCommand(6, 1);
      QVERIFY(!network->sendKeyboardMouseControl(1, 0, 0, false, false, false, 1));
      QCOMPARE(sent.count(), 1);
      data->setCurrentRobotId(1); // 有效但与 MQTT 身份 101 不同，也不能发送。
      network->sendCommonCommand(6, 1);
      QCOMPARE(sent.count(), 1);
    }
    {
      RM::MainWindow restored;
      auto *data = restored.findChild<GameData *>();
      auto *network = restored.findChild<RM::NetworkManager *>();
      QVERIFY(data); QVERIFY(network);
      QCOMPARE(data->getCurrentRobotId(), 101);
      QTRY_VERIFY_WITH_TIMEOUT(network->isMqttConnected(), 5000);
      auto *mqtt = network->findChild<MqttManager *>();
      QVERIFY(mqtt);
      QCOMPARE(mqtt->clientId(), QStringLiteral("101"));
    }
    qunsetenv("RM_MQTT_BROKER");
    qunsetenv("RM_MQTT_PORT");
    broker.terminate();
    QVERIFY(broker.waitForFinished(5000));
#endif
  }

  void testRM27DevPanelGatingAndLiveData() {
    qunsetenv("RM_DEVTOOLS");
    {
      RM::MainWindow window;
      window.show();
      auto *network = window.findChild<RM::NetworkManager *>();
      QVERIFY(network);
      QVERIFY(QMetaObject::invokeMethod(&window, "onSettingsLoginRequested",
                                       Qt::DirectConnection,
                                       Q_ARG(int, 1)));
      QTest::keyClick(window.centralWidget(), Qt::Key_T, Qt::ControlModifier);
      QKeyEvent ctrlDown(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
      QApplication::sendEvent(window.centralWidget(), &ctrlDown);
      QTRY_VERIFY_WITH_TIMEOUT(network->lastKeyboardMouseControlAttemptMs() > 0, 1000);
      QKeyEvent ctrlUp(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
      QApplication::sendEvent(window.centralWidget(), &ctrlUp);
      QTest::keyClick(window.centralWidget(), Qt::Key_F10,
                      Qt::ControlModifier | Qt::ShiftModifier);
      QVERIFY(!window.findChild<QQuickWidget *>(QStringLiteral("rm27DevPanel")));
    }
    qputenv("RM_DEVTOOLS", "1");
    {
      RM::MainWindow window;
      window.resize(1000, 700);
      window.show();
      auto *data = window.findChild<GameData *>();
      auto *network = window.findChild<RM::NetworkManager *>();
      QVERIFY(data); QVERIFY(network);
      QVERIFY(QMetaObject::invokeMethod(&window, "onSettingsLoginRequested",
                                       Qt::DirectConnection,
                                       Q_ARG(int, 1)));
      QTest::keyClick(window.centralWidget(), Qt::Key_T, Qt::ControlModifier);
      QKeyEvent ctrlDown(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
      QApplication::sendEvent(window.centralWidget(), &ctrlDown);
      QTest::qWait(30);
      QKeyEvent shiftDown(QEvent::KeyPress, Qt::Key_Shift,
                          Qt::ControlModifier | Qt::ShiftModifier);
      QApplication::sendEvent(window.centralWidget(), &shiftDown);
      QTest::qWait(30);
      QCOMPARE(network->lastKeyboardMouseControlAttemptMs(), qint64(0));
      QTest::keyClick(window.centralWidget(), Qt::Key_F10,
                      Qt::ControlModifier | Qt::ShiftModifier);
      auto *panel = window.findChild<QQuickWidget *>(QStringLiteral("rm27DevPanel"));
      QVERIFY(panel);
      QVERIFY(panel->isVisible());
      QCOMPARE(panel->geometry(), window.rect());
      QCOMPARE(panel->status(), QQuickWidget::Ready);
      QWidget competingOverlay(&window);
      competingOverlay.show();
      competingOverlay.raise();
      QTRY_VERIFY(childStackIndex(&window, panel) > childStackIndex(&window, &competingOverlay));
      auto *root = panel->rootObject();
      QVERIFY(root);
      auto *score = root->findChild<QObject *>(QStringLiteral("rm27Scores"));
      auto *state = root->findChild<QObject *>(QStringLiteral("rm27GameState"));
      auto *robot = root->findChild<QObject *>(QStringLiteral("rm27CurrentRobot"));
      QVERIFY(score); QVERIFY(state); QVERIFY(robot);
      data->setCurrentRobotId(3);
      data->updateGameStateFromS1(4, 237, 42, 19, 1, true);
      robomaster::RobotStatus status;
      status.set_id(3); status.set_hp(345); status.set_max_hp(500);
      data->updateRobotData(status);
      QTRY_VERIFY(score->property("text").toString().contains("42"));
      QVERIFY(score->property("text").toString().contains("19"));
      QTRY_VERIFY(state->property("text").toString().contains("237"));
      QTRY_VERIFY(robot->property("text").toString().contains("345/500"));
      const bool mapMode = window.tacticalLargeMapMode();
      QTest::keyClick(panel, Qt::Key_M);
      QCOMPARE(window.tacticalLargeMapMode(), mapMode);
      QCOMPARE(network->lastKeyboardMouseControlAttemptMs(), qint64(0));
      QTest::keyClick(panel, Qt::Key_Escape);
      QVERIFY(!panel->isVisible());
      QTest::keyClick(window.centralWidget(), Qt::Key_F10,
                      Qt::ControlModifier | Qt::ShiftModifier);
      QVERIFY(panel->isVisible());
      QTest::keyClick(panel, Qt::Key_F10,
                      Qt::ControlModifier | Qt::ShiftModifier);
      QVERIFY(!panel->isVisible());
    }
    qunsetenv("RM_DEVTOOLS");
  }

  void testTacticalMapStartsLargeAndMIsScoped() {
    // MainWindow 使用零延时 single-shot 延后启动服务。保持本测试同步执行，
    // 这样只覆盖 UI 路由，不启动网络、模拟器和视频服务。
    RM::MainWindow window;
    QWidget *clientTarget = window.centralWidget();
    QVERIFY(clientTarget);
    auto *tacticalPage =
        window.findChild<QQuickWidget *>(QStringLiteral("tacticalPageWidget"));
    QVERIFY(tacticalPage);
    QVERIFY(tacticalPage->rootObject());
    auto *gameData = window.findChild<GameData *>();
    auto *heroVideo = window.findChild<RM::HeroVideoWidget *>();
    QVERIFY(gameData);
    QVERIFY(heroVideo);
    QVERIFY(tacticalPage->rootObject()
                ->property("cameraRefreshEnabled")
                .toBool());
    QVERIFY(window.tacticalLargeMapMode());
    QVERIFY(window.tacticalLargeMapRendered());

    QSignalSpy modeChangedSpy(&window,
                              &RM::MainWindow::tacticalLargeMapModeChanged);
    QVERIFY(modeChangedSpy.isValid());

    QKeyEvent shortcutOverride(QEvent::ShortcutOverride, Qt::Key_M,
                               Qt::NoModifier);
    QApplication::sendEvent(clientTarget, &shortcutOverride);
    QVERIFY(shortcutOverride.isAccepted());
    QVERIFY(window.tacticalLargeMapMode());
    QCOMPARE(modeChangedSpy.count(), 0);

    QWidget unrelatedTopLevel;
    QKeyEvent unrelatedWindowM(QEvent::KeyPress, Qt::Key_M, Qt::NoModifier);
    QApplication::sendEvent(&unrelatedTopLevel, &unrelatedWindowM);
    QVERIFY(window.tacticalLargeMapMode());
    QCOMPARE(modeChangedSpy.count(), 0);

    // 现有飞镖模拟会依次经过 GameData 的飞镖信号和真实敌方命中共用的
    // 处理路径；触发后应退出启动时的大地图，切到操作/视频页。
    QKeyEvent dartHit(QEvent::KeyPress, Qt::Key_D,
                      Qt::ControlModifier | Qt::ShiftModifier);
    QApplication::sendEvent(&window, &dartHit);
    QVERIFY(dartHit.isAccepted());
    QVERIFY(!window.tacticalLargeMapMode());
    QVERIFY(!tacticalPage->rootObject()
                 ->property("cameraRefreshEnabled")
                 .toBool());
    QVERIFY(!window.tacticalLargeMapRendered());
    QCOMPARE(modeChangedSpy.count(), 1);

    // 操作页沿用原有 M 键行为，但不能改动战术页新增状态。
    QKeyEvent operationScreenM(QEvent::KeyPress, Qt::Key_M, Qt::NoModifier);
    QApplication::sendEvent(clientTarget, &operationScreenM);
    QVERIFY(operationScreenM.isAccepted());
    QVERIFY(!window.tacticalLargeMapMode());
    QCOMPARE(modeChangedSpy.count(), 1);

    QKeyEvent enterTacticalMode(QEvent::KeyPress, Qt::Key_T,
                                Qt::ControlModifier);
    QApplication::sendEvent(clientTarget, &enterTacticalMode);
    QVERIFY(enterTacticalMode.isAccepted());
    QVERIFY(tacticalPage->rootObject()
                ->property("cameraRefreshEnabled")
                .toBool());
    QVERIFY(!window.tacticalLargeMapMode());
    QCOMPARE(modeChangedSpy.count(), 1);

    QKeyEvent tacticalMAgain(QEvent::KeyPress, Qt::Key_M, Qt::NoModifier);
    QApplication::sendEvent(clientTarget, &tacticalMAgain);
    QVERIFY(tacticalMAgain.isAccepted());
    QVERIFY(window.tacticalLargeMapMode());
    QCOMPARE(modeChangedSpy.count(), 2);

    // QML 战术页显示期间，从工业相机机器人切换到 R3 后，不能重新显示
    // 先前非战术页面遗留的原生视频帧。
    heroVideo->setCurrentRobotId(6);
    heroVideo->setFrame(QImage(16, 16, QImage::Format_RGB32));
    heroVideo->hide();
    gameData->setCurrentRobotId(3);
    gameData->robotDataUpdated(3);
    QVERIFY(heroVideo->isHidden());

    QKeyEvent leaveTacticalMode(QEvent::KeyPress, Qt::Key_T,
                                Qt::ControlModifier);
    QApplication::sendEvent(clientTarget, &leaveTacticalMode);
    QVERIFY(leaveTacticalMode.isAccepted());
    QVERIFY(!tacticalPage->rootObject()
                 ->property("cameraRefreshEnabled")
                 .toBool());
    QVERIFY(heroVideo->isHidden());
    QVERIFY(!window.tacticalLargeMapMode());
    QCOMPARE(modeChangedSpy.count(), 3);
  }

  void testDartOcclusionReturnsToMapOnlyTacticalScreen() {
    RM::MainWindow window;
    QWidget *clientTarget = window.centralWidget();
    QVERIFY(clientTarget);

    auto *tacticalPage =
        window.findChild<QQuickWidget *>(QStringLiteral("tacticalPageWidget"));
    auto *gameData = window.findChild<GameData *>();
    QVERIFY(tacticalPage);
    QVERIFY(gameData);

    QKeyEvent dartHit(QEvent::KeyPress, Qt::Key_D,
                      Qt::ControlModifier | Qt::ShiftModifier);
    QApplication::sendEvent(&window, &dartHit);
    QVERIFY(dartHit.isAccepted());
    QVERIFY(!window.tacticalLargeMapMode());
    QVERIFY(tacticalPage->isHidden());

    // 生产策略把命中当秒计入遮挡，因此固定 10 秒的遮挡需经过
    // 11 次协议倒计时更新才归零。
    gameData->simulateGameTimeElapse(11);

    QVERIFY(!tacticalPage->isHidden());
    QVERIFY(window.tacticalLargeMapMode());
    QVERIFY(window.tacticalLargeMapRendered());

    // 自动恢复后，M 键仍可打开原有战术指令页。
    QKeyEvent showCommandScreen(QEvent::KeyPress, Qt::Key_M, Qt::NoModifier);
    QApplication::sendEvent(clientTarget, &showCommandScreen);
    QVERIFY(showCommandScreen.isAccepted());
    QVERIFY(!window.tacticalLargeMapMode());
  }

  void testTacticalLargeMapSitsBetweenPhasePopupAndTimedEventPopup() {
    RM::MainWindow window;
    window.resize(1920, 1080);
    window.show();
    QTest::qWait(50);

    QWidget *clientTarget = window.centralWidget();
    QVERIFY(clientTarget);

    auto *gameData = window.findChild<GameData *>();
    QVERIFY(gameData);

    auto *tacticalPage =
        window.findChild<QQuickWidget *>(QStringLiteral("tacticalPageWidget"));
    auto *officialPopup = window.findChild<QQuickWidget *>(
        QStringLiteral("officialEventPopupWidget"));
    auto *timedPopup = window.findChild<QQuickWidget *>(
        QStringLiteral("tacticalTimedEventPopup0"));
    auto *popupOverlay =
        window.findChild<QQuickWidget *>(QStringLiteral("popupOverlayWidget"));
    QVERIFY(tacticalPage);
    QVERIFY(officialPopup);
    QVERIFY(timedPopup);
    QVERIFY(popupOverlay);

    auto sendKeyPress = [&](int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
      QKeyEvent event(QEvent::KeyPress, key, modifiers);
      QApplication::sendEvent(clientTarget, &event);
      QVERIFY(event.isAccepted());
    };

    sendKeyPress(Qt::Key_M);
    QTRY_VERIFY(!tacticalPage->isHidden());

    const int officialDisplayToken =
        officialPopup->property("displayToken").toInt();
    sendKeyPress(Qt::Key_F9, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(officialPopup->property("displayToken").toInt() >
                officialDisplayToken);
    // 官方事件面板当前按产品策略保持隐藏，调试键只刷新事件内容。
    QVERIFY(officialPopup->isHidden());

    sendKeyPress(Qt::Key_F8, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(!timedPopup->isHidden());

    QTRY_VERIFY(childStackIndex(clientTarget, timedPopup) >
                childStackIndex(clientTarget, tacticalPage));

    sendKeyPress(Qt::Key_M);
    QTRY_VERIFY(window.tacticalLargeMapMode());
    QTRY_VERIFY(window.tacticalLargeMapRendered());
    QTRY_VERIFY(childStackIndex(clientTarget, tacticalPage) >
                childStackIndex(clientTarget, timedPopup));

    robomaster::GameStatus status;
    status.set_current_stage(1);
    status.set_stage_countdown_sec(30);
    status.set_stage_elapsed_sec(0);
    status.set_red_score(0);
    status.set_blue_score(0);
    status.set_current_round(1);
    status.set_total_rounds(1);
    status.set_is_paused(false);
    gameData->updateGameStatus(status);

    QTRY_VERIFY(!popupOverlay->isHidden());
    QTRY_VERIFY(childStackIndex(clientTarget, popupOverlay) >
                childStackIndex(clientTarget, tacticalPage));
    QTRY_VERIFY(childStackIndex(clientTarget, tacticalPage) >
                childStackIndex(clientTarget, timedPopup));
  }
};

QTEST_MAIN(TestMainWindowTacticalHotkeyIntegration)
#include "test_mainwindow_tactical_hotkey_integration.moc"
