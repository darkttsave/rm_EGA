// SPDX-License-Identifier: MIT
/**
 * @file test_sentry_bt_status_qml.cpp
 * @brief 临时诊断 QML 组件的加载、实例化与绑定测试。
 * @details 在真实 GameData 上、通过真实 QQuickView 场景加载 GunnerOverlay.qml，验证：
 *          - 组件能被加载与实例化（无“组件未找到/属性不存在/绑定错误”类警告）；
 *          - 诊断面板确实读取 GameData.sentryStatusPreview 并随 NOTIFY 刷新；
 *          - 三个原始字段确实被实例化为可见行；
 *          - 面板显隐跟随 customUIEnabled（即现有 Ctrl+G 统一隐显）。
 */

#include "core/GameData.h"
#include "network/SentryBtStatusParser.h"

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QScopedPointer>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtTest>

namespace {

QStringList *g_qmlMessages = nullptr;
QtMessageHandler g_forwardHandler = nullptr;

void captureQmlMessages(QtMsgType type, const QMessageLogContext &context,
                        const QString &message) {
  if (g_qmlMessages != nullptr && type != QtDebugMsg && type != QtInfoMsg) {
    g_qmlMessages->append(message);
  }
  // 继续转发，避免吞掉 QTest 自身的失败输出
  if (g_forwardHandler != nullptr) {
    g_forwardHandler(type, context, message);
  }
}

/// Repeater 生成的 delegate 不在 QObject::findChildren 可达范围内（其 QObject
/// 父对象由 DelegateModel 内部持有），因此必须遍历可视项树。
QList<QQuickItem *> collectSentryFieldRows(QQuickItem *root) {
  QList<QQuickItem *> rows;
  if (root == nullptr) {
    return rows;
  }
  QList<QQuickItem *> pending{root};
  while (!pending.isEmpty()) {
    QQuickItem *item = pending.takeFirst();
    if (item->objectName() == QLatin1String("sentryFieldRow")) {
      rows.append(item);
    }
    pending.append(item->childItems());
  }
  return rows;
}

}  // namespace

class TestSentryBtStatusQml : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() {
    // 静态库中的 Qt 资源需要显式初始化，qrc:/qml/... 才可解析。
    Q_INIT_RESOURCE(qml);
    Q_INIT_RESOURCE(resources);
  }

  void cleanupTestCase() { Q_CLEANUP_RESOURCE(resources); }

  void init() {
    m_messages.clear();
    g_qmlMessages = &m_messages;
    g_forwardHandler = qInstallMessageHandler(captureQmlMessages);

    m_gameData = new GameData(this);
    m_view.reset(new QQuickView());
    m_view->rootContext()->setContextProperty(QStringLiteral("gameData"),
                                              m_gameData);
    m_view->setResizeMode(QQuickView::SizeRootObjectToView);
    m_view->resize(800, 600);
    m_view->setSource(QUrl(QStringLiteral("qrc:/qml/CustomUI/GunnerOverlay.qml")));

    if (m_view->status() != QQuickView::Ready) {
      const QList<QQmlError> errors = m_view->errors();
      const QString detail = errors.isEmpty()
                                 ? QStringLiteral("unknown QML error")
                                 : errors.first().toString();
      QFAIL(qPrintable(detail));
    }

    m_view->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_view.data()));

    m_root = m_view->rootObject();
    QVERIFY(m_root != nullptr);

    m_panel = m_root->findChild<QObject *>(QStringLiteral("sentryStatusPreview"));
    QVERIFY2(m_panel != nullptr, "GunnerOverlay 中未实例化 SentryStatusPreview");
  }

  void cleanup() {
    qInstallMessageHandler(g_forwardHandler);
    g_forwardHandler = nullptr;
    g_qmlMessages = nullptr;
    m_panel = nullptr;
    m_root = nullptr;
    m_view.reset();
    m_gameData = nullptr;
  }

  /// 组件能加载并实例化，诊断面板存在，且不产生绑定类警告
  void overlayLoadsAndInstantiatesPreviewPanel() {
    QVERIFY(m_view->errors().isEmpty());

    for (const QString &message : m_messages) {
      QVERIFY2(!message.contains(QStringLiteral("SentryStatusPreview")),
               qPrintable(message));
      QVERIFY2(!message.contains(QStringLiteral("GunnerOverlay")),
               qPrintable(message));
    }
  }

  /// 未收到消息时显示“未收到”状态
  void panelStartsWithoutData() {
    QCOMPARE(m_panel->property("hasData").toBool(), false);
    QVERIFY(m_panel->property("fieldRows").toList().isEmpty());
    QCOMPARE(m_panel->property("sentryData")
                 .toMap()
                 .value(QStringLiteral("received"))
                 .toBool(),
             false);
    QCOMPARE(collectSentryFieldRows(qobject_cast<QQuickItem *>(m_panel)).size(), 0);
  }

  /// 面板读取 GameData.sentryStatusPreview，并随 NOTIFY 自动刷新
  void panelReflectsGameDataPreviewUpdates() {
    RM::SentryBtStatus status;
    status.currentStance = 0x07;
    status.btRemainTime = 0x1234;
    status.btGameStatus = 0x09;
    m_gameData->updateSentryBtStatusPreview(status, QStringLiteral("07 34 12 09"),
                                           1234567);

    QCOMPARE(m_panel->property("hasData").toBool(), true);

    const QVariantMap data = m_panel->property("sentryData").toMap();
    QCOMPARE(data.value(QStringLiteral("current_stance")).toInt(), 0x07);
    QCOMPARE(data.value(QStringLiteral("bt_remain_time")).toInt(), 0x1234);
    QCOMPARE(data.value(QStringLiteral("bt_game_status")).toInt(), 0x09);

    // 渲染用的字段行来自状态层，QML 不重复协议字段定义
    const QVariantList rows = m_panel->property("fieldRows").toList();
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("current_stance"));
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("value")).toInt(), 0x07);
    QCOMPARE(rows.at(1).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("bt_remain_time"));
    QCOMPARE(rows.at(2).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("bt_game_status"));
  }

  /// 三个字段确实被实例化为可见行（而不是只存在于模型中）
  void fieldRowsAreActuallyInstantiated() {
    RM::SentryBtStatus status;
    status.currentStance = 5;
    status.btRemainTime = 321;
    status.btGameStatus = 2;
    m_gameData->updateSentryBtStatusPreview(status, QStringLiteral("05 41 01 02"), 7);

    // 三个字段必须真的被实例化为可见行（而不是只存在于模型中）
    const QList<QQuickItem *> rows =
        collectSentryFieldRows(qobject_cast<QQuickItem *>(m_panel));
    QCOMPARE(rows.size(), 3);

    // 每一行都绑定了对应的字段名与数值，且确实可见
    QStringList names;
    for (QQuickItem *row : rows) {
      QVERIFY(row->isVisible());
      const QVariantMap modelData = row->property("modelData").toMap();
      names.append(modelData.value(QStringLiteral("name")).toString());
      QVERIFY(!modelData.value(QStringLiteral("value")).isNull());
    }
    QCOMPARE(names, QStringList({QStringLiteral("current_stance"),
                                 QStringLiteral("bt_remain_time"),
                                 QStringLiteral("bt_game_status")}));
  }

  /// 面板显隐跟随 customUIEnabled（现有 Ctrl+G 统一隐显）
  void panelVisibilityFollowsCustomUIEnabled() {
    QCOMPARE(m_root->property("visible").toBool(), true);

    m_gameData->setCustomUIEnabled(false);
    QCOMPARE(m_root->property("visible").toBool(), false);

    m_gameData->setCustomUIEnabled(true);
    QCOMPARE(m_root->property("visible").toBool(), true);
  }

  /// 停止发送后面板保持最后一条合法状态，不自行编造新状态
  void panelKeepsLastStateWithoutNewMessages() {
    RM::SentryBtStatus status;
    status.currentStance = 3;
    status.btRemainTime = 250;
    status.btGameStatus = 1;
    m_gameData->updateSentryBtStatusPreview(status, QStringLiteral("03 fa 00 01"), 99);

    const QVariantMap before = m_panel->property("sentryData").toMap();
    QTest::qWait(30);  // 期间没有任何新输入
    const QVariantMap after = m_panel->property("sentryData").toMap();

    QCOMPARE(after.value(QStringLiteral("current_stance")).toInt(), 3);
    QCOMPARE(after.value(QStringLiteral("bt_remain_time")).toInt(), 250);
    QCOMPARE(before.value(QStringLiteral("receivedAtMs")).toLongLong(),
             after.value(QStringLiteral("receivedAtMs")).toLongLong());
  }

  /// 面板在未收到数据时保持低强调，收到数据后展示字段行并增高
  void panelStaysCompactUntilDataArrives() {
    const qreal emptyHeight = m_panel->property("height").toReal();
    QVERIFY(emptyHeight > 0.0);

    RM::SentryBtStatus status;
    m_gameData->updateSentryBtStatusPreview(status, QStringLiteral("00 00 00 00"), 1);

    QVERIFY2(m_panel->property("height").toReal() > emptyHeight,
             "收到数据后面板应展示字段行并增高");
  }

private:
  GameData *m_gameData = nullptr;
  QScopedPointer<QQuickView> m_view;
  QQuickItem *m_root = nullptr;
  QObject *m_panel = nullptr;
  QStringList m_messages;
};

QTEST_MAIN(TestSentryBtStatusQml)
#include "test_sentry_bt_status_qml.moc"
