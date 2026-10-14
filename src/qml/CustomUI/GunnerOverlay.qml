import QtQuick 2.15

Item {
    id: root
    anchors.fill: parent

    // gameData 由 MainWindow 注入 QML 上下文，统一在叠加层入口适配。
    // qmllint disable unqualified
    readonly property var gameDataContext: typeof gameData !== "undefined" ? gameData : null
    // qmllint enable unqualified
    property var customData: root.gameDataContext ? root.gameDataContext.myRobotCustomData : ({})
    // 哨兵 0x0310 临时诊断状态；属性带 NOTIFY，GameData 更新后自动刷新。
    property var sentryPreview: root.gameDataContext ? root.gameDataContext.sentryStatusPreview : ({})
    property bool uiEnabled: root.gameDataContext ? root.gameDataContext.customUIEnabled : false
    visible: root.uiEnabled

    // 临时诊断面板：放在顶部中央，避免遮挡左侧摩擦轮、右侧底盘/超级电容
    // 以及中心准星与主要视频区域。
    SentryStatusPreview {
        id: sentryPanel
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 15
        sentryData: root.sentryPreview
    }

    FrictionWheelStatus {
        id: frictionPanel
        anchors.left: parent.left
        anchors.leftMargin: 15
        anchors.top: parent.top
        anchors.topMargin: 15
        customData: root.customData
    }

    ChassisModePanel {
        id: chassisPanel
        anchors.right: parent.right
        anchors.rightMargin: 15
        anchors.top: parent.top
        anchors.topMargin: 15
        customData: root.customData
    }

    SuperCapacitorBar {
        id: capPanel
        anchors.right: parent.right
        anchors.rightMargin: 15
        anchors.verticalCenter: parent.verticalCenter
        customData: root.customData
    }

    GimbalAngleIndicator {
        id: anglePanel
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        customData: root.customData
    }
}
