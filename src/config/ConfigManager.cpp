/**
 * @file ConfigManager.cpp
 * @brief 配置管理器实现
 * @author Clear
 * @date 2025-11-30
 */

#include "ConfigManager.h"
#include "../core/RobotIdentity.h"
#include <QCoreApplication>
#include <QByteArray>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonValue>
#include <QTimer>
#include <QSaveFile>

QMutex ConfigManager::m_mutex;

namespace {
bool isValidClientRobotId(int robotId) {
  return RM::resolveRobotIdentity(robotId).rmulSelectable;
}
} // namespace

ConfigManager &ConfigManager::instance() {
  static ConfigManager instance;
  return instance;
}

ConfigManager::ConfigManager(QObject *parent)
    : QObject(parent), m_watcher(new QFileSystemWatcher(this)) {
  connect(this, &ConfigManager::configReloaded, this, &ConfigManager::competitionUiModeChanged);
  connect(m_watcher, &QFileSystemWatcher::fileChanged, this,
          &ConfigManager::onFileChanged);
  // 默认加载
  loadConfig();
}

QString ConfigManager::competitionUiMode() const {
  const QString mode = m_config.value("ui").toObject().value("competition_mode").toString();
  return mode == QLatin1String("rmuc") ? QStringLiteral("rmuc") : QStringLiteral("rmul");
}

bool ConfigManager::setCompetitionUiMode(const QString &mode) {
  if (mode != QLatin1String("rmul") && mode != QLatin1String("rmuc")) return false;
  {
    QMutexLocker locker(&m_mutex);
    if (m_currentConfigPath.isEmpty()) return false;
    QFile source(m_currentConfigPath);
    if (!source.open(QIODevice::ReadOnly)) return false;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(source.readAll(), &error);
    source.close();
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
    QJsonObject updated = doc.object();
    QJsonObject ui = updated.value("ui").toObject();
    ui["competition_mode"] = mode;
    updated["ui"] = ui;
    QSaveFile destination(m_currentConfigPath);
    const QByteArray bytes = QJsonDocument(updated).toJson();
    if (!destination.open(QIODevice::WriteOnly) || destination.write(bytes) != bytes.size() || !destination.commit()) return false;
    m_config = updated;
  }
  emit competitionUiModeChanged();
  return true;
}

ConfigManager::~ConfigManager() {}

bool ConfigManager::loadConfig(const QString &configPath) {
  QMutexLocker locker(&m_mutex);

  QString foundPath;
  QJsonObject newConfig = ConfigLoader::load(configPath, &foundPath);

  if (newConfig.isEmpty()) {
    qWarning() << "配置加载失败或配置为空";
    return false;
  }

  QString errorMsg;
  if (!ConfigValidator::validate(newConfig, errorMsg)) {
    qWarning() << "配置验证失败:" << errorMsg;
    return false;
  }

  m_config = newConfig;
  qDebug() << "配置文件加载成功";

  foundPath = QFileInfo(foundPath).absoluteFilePath();
  // 更新文件监控
  if (m_currentConfigPath != foundPath) {
    if (!m_currentConfigPath.isEmpty()) {
      m_watcher->removePath(m_currentConfigPath);
    }
    m_currentConfigPath = foundPath;
    if (!m_currentConfigPath.isEmpty()) {
      m_watcher->addPath(m_currentConfigPath);
    }
  } else if (!m_currentConfigPath.isEmpty() &&
             !m_watcher->files().contains(m_currentConfigPath)) {
    m_watcher->addPath(m_currentConfigPath);
  }

  emit configReloaded();
  return true;
}

void ConfigManager::onFileChanged(const QString &path) {
  qDebug() << "配置文件发生变化:" << path;
  if (QFileInfo(path).absoluteFilePath() != m_currentConfigPath) return;
  // 重新加载配置
  // 注意：某些编辑器可能会先删除再创建文件，导致 watcher 失效
  // 这里简单处理，如果文件存在则重新加载
  if (QFile::exists(path)) {
    loadConfig(path);
  } else {
    // 如果文件被删除，可能需要重新添加监控（如果它再次出现）
    // 这里暂时不做复杂处理，只是尝试重新加载默认路径
    QTimer::singleShot(100, this, [this, path]() {
      if (QFileInfo(path).absoluteFilePath() == m_currentConfigPath)
        loadConfig(path);
    });
  }
}

QString ConfigManager::getAppName() const {
  return m_config["app_settings"].toObject()["client_name"].toString(
      "RoboMaster Client");
}

QString ConfigManager::getAppVersion() const {
  return m_config["app_settings"].toObject()["version"].toString("1.0.0");
}

QString ConfigManager::getUIText(const QString &key) const {
  return m_config["ui_text"].toObject()[key].toString(key);
}

QString ConfigManager::getServerIP() const {
  return getEnvOrConfigString("RM_SERVER_IP", "network", "server_ip",
                              "127.0.0.1");
}

quint16 ConfigManager::getServerPort() const {
  return getEnvOrConfigPort("RM_SERVER_PORT", "network", "server_port", 20000);
}

quint16 ConfigManager::getClientPort() const {
  return getEnvOrConfigPort("RM_CLIENT_PORT", "network", "client_port", 10000);
}

quint16 ConfigManager::getVideoPort() const {
  return getEnvOrConfigPort("RM_VIDEO_PORT", "network", "video_port", 3334);
}

QString ConfigManager::getMqttBroker() const {
  return getEnvOrConfigString("RM_MQTT_BROKER", "network", "mqtt_broker",
                              "127.0.0.1");
}

quint16 ConfigManager::getMqttPort() const {
  return getEnvOrConfigPort("RM_MQTT_PORT", "network", "mqtt_port", 1883);
}

int ConfigManager::getClientRobotId() const {
  bool ok = false;
  const QByteArray envValue = qgetenv("RM_CLIENT_ROBOT_ID");
  if (!envValue.trimmed().isEmpty()) {
    const int parsed = QString::fromUtf8(envValue).toInt(&ok);
    if (ok && isValidClientRobotId(parsed)) {
      qInfo() << "ConfigManager: client_robot_id=" << parsed
              << "(from env RM_CLIENT_ROBOT_ID)";
      return parsed;
    }
    qWarning() << "ConfigManager: 无效的 RM_CLIENT_ROBOT_ID，请重新选择:" << envValue;
    return 0;
  }

  const QJsonValue configValue =
      m_config["network"].toObject()["client_robot_id"];
  if (configValue.isDouble()) {
    const int configured = configValue.toInt(0);
    if (isValidClientRobotId(configured)) {
      qInfo() << "ConfigManager: client_robot_id=" << configured
              << "(from config.json)";
      qInfo() << "ConfigManager: To override, set RM_CLIENT_ROBOT_ID=<id>"
              << "(PowerShell: $env:RM_CLIENT_ROBOT_ID=<id>)";
      return configured;
    }
    qWarning() << "ConfigManager: 无效的 network.client_robot_id，请重新选择:" << configured;
  }
  return 0;
}

bool ConfigManager::setClientRobotId(int robotId, QString *error) {
  auto fail = [error](const QString &message) {
    if (error) *error = message;
    return false;
  };
  if (!isValidClientRobotId(robotId))
    return fail(QStringLiteral("机器人身份无效，请重新选择"));
  const QByteArray envValue = qgetenv("RM_CLIENT_ROBOT_ID").trimmed();
  if (!envValue.isEmpty()) {
    bool ok = false;
    const int overrideId = QString::fromUtf8(envValue).toInt(&ok);
    if (!ok || overrideId != robotId)
      return fail(QStringLiteral("启动环境已锁定机器人身份，请移除 RM_CLIENT_ROBOT_ID 后重新启动"));
  }
  QMutexLocker locker(&m_mutex);
  if (m_currentConfigPath.isEmpty())
    return fail(QStringLiteral("未找到可写配置文件"));
  QFile source(m_currentConfigPath);
  if (!source.open(QIODevice::ReadOnly))
    return fail(QStringLiteral("无法读取配置文件"));
  QJsonParseError parseError;
  const QByteArray original = source.readAll();
  source.close();
  const QJsonDocument document = QJsonDocument::fromJson(original, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
    return fail(QStringLiteral("配置文件格式错误"));
  QJsonObject updated = document.object();
  QJsonObject network = updated.value("network").toObject();
  network["client_robot_id"] = robotId;
  updated["network"] = network;
  QSaveFile destination(m_currentConfigPath);
  const QByteArray serialized = QJsonDocument(updated).toJson();
  if (!destination.open(QIODevice::WriteOnly) ||
      destination.write(serialized) != serialized.size() || !destination.commit())
    return fail(QStringLiteral("无法保存机器人身份"));
  m_config = updated;
  return true;
}

QString ConfigManager::getDefaultVideoPath() const {
  return m_config["video"].toObject()["default_path"].toString(
      "resources/videos/demo.mp4");
}

QString ConfigManager::getVideoStreamUrl() const {
  const QString envValue =
      qEnvironmentVariable("RM_VIDEO_STREAM_URL").trimmed();
  if (!envValue.isEmpty()) {
    return envValue;
  }

  const QJsonValue configValue = m_config["video"].toObject()["stream_url"];
  if (configValue.isString() && !configValue.toString().trimmed().isEmpty()) {
    return configValue.toString();
  }

  return QStringLiteral("udp://0.0.0.0:%1").arg(getVideoPort());
}

QString ConfigManager::getVideoResolution() const {
  return m_config["video"].toObject()["resolution"].toString("1280x720");
}

bool ConfigManager::getIndustrialCameraGridEnabled() const {
  const QString envValue =
      qEnvironmentVariable("RM_HERO_CAMERA_GRID").trimmed().toLower();
  if (!envValue.isEmpty()) {
    return envValue == QLatin1String("1") || envValue == QLatin1String("true") ||
           envValue == QLatin1String("yes") || envValue == QLatin1String("on");
  }
  return m_config["video"]
      .toObject()["industrial_camera_grid"]
      .toBool(false);
}

// --- 窗口配置访问器 ---
int ConfigManager::getWindowMinWidth() const {
  return m_config["window"].toObject()["min_width"].toInt(1280);
}

int ConfigManager::getWindowMinHeight() const {
  return m_config["window"].toObject()["min_height"].toInt(720);
}

int ConfigManager::getWindowDefaultWidth() const {
  return m_config["window"].toObject()["default_width"].toInt(1920);
}

int ConfigManager::getWindowDefaultHeight() const {
  return m_config["window"].toObject()["default_height"].toInt(1080);
}

bool ConfigManager::getWindowFullscreen() const {
  return m_config["window"].toObject()["fullscreen"].toBool(true);
}

QJsonObject ConfigManager::getRobotConfig(const QString &robotType) const {
  return m_config["robots"].toObject()[robotType].toObject();
}

QString ConfigManager::getRobotLayout(const QString &robotType) const {
  return getRobotConfig(robotType)["ui_layout"].toString("standard");
}

QJsonObject ConfigManager::getMapConfig() const {
  return m_config["map"].toObject();
}

QString ConfigManager::getPositionSource() const {
  QString value = qEnvironmentVariable("RM_POSITION_SOURCE").trimmed().toLower();
  if (value.isEmpty()) {
    value = m_config["position_source"].toString("official").trimmed().toLower();
  }
  if (value != QStringLiteral("official") &&
      value != QStringLiteral("ros_bridge")) {
    qWarning() << "ConfigManager: 未知 position_source=" << value
               << "，按 official 处理";
    return QStringLiteral("official");
  }
  return value;
}

QString ConfigManager::getRosRadarTopic() const {
  const QString topic =
      m_config["ros_bridge"].toObject()["topic"].toString(
          QStringLiteral("RosRadarPositions"));
  return topic.trimmed().isEmpty() ? QStringLiteral("RosRadarPositions")
                                   : topic.trimmed();
}

// --- AR 叠加配置访问器 ---

bool ConfigManager::getAROverlayEnabled() const {
  return m_config["ar_overlay"].toObject()["enabled"].toBool(false);
}

QString ConfigManager::getARModelPath() const {
  return m_config["ar_overlay"].toObject()["model_path"].toString();
}

float ConfigManager::getARConfidenceThreshold() const {
  return static_cast<float>(
      m_config["ar_overlay"].toObject()["confidence_threshold"].toDouble(0.5));
}

float ConfigManager::getARNMSThreshold() const {
  return static_cast<float>(
      m_config["ar_overlay"].toObject()["nms_threshold"].toDouble(0.4));
}

float ConfigManager::getARSmoothingFactor() const {
  return static_cast<float>(
      m_config["ar_overlay"].toObject()["smoothing_factor"].toDouble(0.3));
}

int ConfigManager::getARMaxMissedFrames() const {
  return m_config["ar_overlay"].toObject()["max_missed_frames"].toInt(10);
}

int ConfigManager::getARDetectionInterval() const {
  return m_config["ar_overlay"].toObject()["detection_interval_ms"].toInt(33);
}

void ConfigManager::setAROverlayEnabled(bool enabled) {
  QMutexLocker locker(&m_mutex);
  QJsonObject arConfig = m_config["ar_overlay"].toObject();
  if (arConfig["enabled"].toBool() != enabled) {
    arConfig["enabled"] = enabled;
    m_config["ar_overlay"] = arConfig;

    // 发送配置变更信号
    emit configReloaded();
  }
}

QString ConfigManager::getEnvOrConfigString(const char *envName,
                                            const QString &section,
                                            const QString &key,
                                            const QString &defaultValue) const {
  const QString envValue = qEnvironmentVariable(envName).trimmed();
  if (!envValue.isEmpty()) {
    return envValue;
  }

  const QJsonObject sectionObject = m_config[section].toObject();
  const QJsonValue value = sectionObject.value(key);
  if (value.isString()) {
    const QString configValue = value.toString().trimmed();
    if (!configValue.isEmpty()) {
      return configValue;
    }
  }

  return defaultValue;
}

quint16 ConfigManager::getEnvOrConfigPort(const char *envName,
                                          const QString &section,
                                          const QString &key,
                                          quint16 defaultValue) const {
  bool ok = false;
  const QByteArray envValue = qgetenv(envName).trimmed();
  if (!envValue.isEmpty()) {
    const uint port = envValue.toUInt(&ok);
    if (ok && port <= 65535U) {
      return static_cast<quint16>(port);
    }
    qWarning() << "环境变量端口无效，忽略:" << envName << envValue;
  }

  const int configPort = m_config[section].toObject()[key].toInt(-1);
  if (configPort >= 0 && configPort <= 65535) {
    return static_cast<quint16>(configPort);
  }

  return defaultValue;
}
