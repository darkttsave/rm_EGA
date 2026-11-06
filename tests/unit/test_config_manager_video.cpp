#include "config/ConfigManager.h"
#include <QFileInfo>
#include <QTemporaryDir>

#include <QtTest>

class TestConfigManagerVideo : public QObject {
  Q_OBJECT

private slots:
  void optionalArModelIsNotAssumed() {
    QCOMPARE(ConfigManager::instance().getARModelPath(), QString());
    QVERIFY(!ConfigManager::instance().getAROverlayEnabled());
  }

  void industrialCameraGridEnvironmentOverride() {
    const QByteArray previousValue = qgetenv("RM_HERO_CAMERA_GRID");

    qputenv("RM_HERO_CAMERA_GRID", "true");
    QVERIFY(ConfigManager::instance().getIndustrialCameraGridEnabled());

    qputenv("RM_HERO_CAMERA_GRID", "0");
    QVERIFY(!ConfigManager::instance().getIndustrialCameraGridEnabled());

    if (previousValue.isNull()) {
      qunsetenv("RM_HERO_CAMERA_GRID");
    } else {
      qputenv("RM_HERO_CAMERA_GRID", previousValue);
    }
  }

  void clientRobotIdEnvironmentRange() {
    const QByteArray previousValue = qgetenv("RM_CLIENT_ROBOT_ID");

    qputenv("RM_CLIENT_ROBOT_ID", "105");
    QCOMPARE(ConfigManager::instance().getClientRobotId(), 0);

    qputenv("RM_CLIENT_ROBOT_ID", "8");
    QVERIFY(ConfigManager::instance().getClientRobotId() != 8);

    if (previousValue.isNull()) {
      qunsetenv("RM_CLIENT_ROBOT_ID");
    } else {
      qputenv("RM_CLIENT_ROBOT_ID", previousValue);
    }
  }

  void robotIdentityPersistenceAndInvalidValues() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString example = QFileInfo(QStringLiteral(__FILE__)).absolutePath() + "/../../config.example.json";
    const QString path = dir.filePath("config.json");
    QVERIFY(QFile::copy(example, path));
    auto &config = ConfigManager::instance();
    QVERIFY(config.loadConfig(path));
    const QByteArray previous = qgetenv("RM_CLIENT_ROBOT_ID");
    qunsetenv("RM_CLIENT_ROBOT_ID");
    for (int id : {1, 3, 7, 101, 103, 107}) {
      QString error;
      QVERIFY2(config.setClientRobotId(id, &error), qPrintable(error));
      QVERIFY(config.loadConfig(path));
      QCOMPARE(config.getClientRobotId(), id);
    }
    for (int id : {0, 2, 4, 6, 102, 105, 201, 257}) {
      QString error;
      QVERIFY(!config.setClientRobotId(id, &error));
      QVERIFY(!error.isEmpty());
      QCOMPARE(config.getClientRobotId(), 107);
    }
    for (int id : {2, 102, 201, 257}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::ReadOnly));
      QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
      file.close();
      QJsonObject network = object.value("network").toObject();
      network["client_robot_id"] = id;
      object["network"] = network;
      QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
      file.write(QJsonDocument(object).toJson());
      file.close();
      QVERIFY(config.loadConfig(path));
      QCOMPARE(config.getClientRobotId(), 0);
    }
    qputenv("RM_CLIENT_ROBOT_ID", "2");
    QCOMPARE(config.getClientRobotId(), 0);
    QString error;
    QVERIFY(!config.setClientRobotId(1, &error));
    qputenv("RM_CLIENT_ROBOT_ID", "101");
    QCOMPARE(config.getClientRobotId(), 101);
    QVERIFY(!config.setClientRobotId(3, &error));
    if (previous.isNull()) qunsetenv("RM_CLIENT_ROBOT_ID");
    else qputenv("RM_CLIENT_ROBOT_ID", previous);
  }
};

QTEST_MAIN(TestConfigManagerVideo)
#include "test_config_manager_video.moc"
