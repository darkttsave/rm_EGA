// SPDX-License-Identifier: MIT
import QtQuick
import QtQuick.Controls

Rectangle {
    id: root
    objectName: "rm27DevPanelRoot"
    color: "#101923"
    property bool panelActive: false
    property int revision: 0
    property var dataSource: gameData

    // Re-read GameData, including updates that do not carry a dedicated QML notify.
    Timer { interval: 250; running: root.panelActive; repeat: true; onTriggered: root.revision++ }
    Connections {
        target: root.dataSource
        function onRobotDataUpdated() { root.revision++ }
        function onMyRobotUpdated() { root.revision++ }
        function onDataChanged() { root.revision++ }
    }
    ScrollView {
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        Column {
            width: Math.max(300, root.width - 64)
            x: 32
            spacing: 14
            Text { text: "RM27 开发者状态面板"; color: "white"; font.pixelSize: 28; topPadding: 20 }
            Text { text: "Ctrl+Shift+F10 或 Esc 关闭 · 键鼠机器人控制已禁用 · GameData（部分旧业务尚未适配）"; color: "#a8c4d8"; font.pixelSize: 16 }
            Text {
                objectName: "rm27GameState"
                color: "white"; font.pixelSize: 20
                width: parent.width; wrapMode: Text.Wrap
                text: !root.dataSource ? "" : "阶段: " + gameData.gamePhase + "    剩余: " + gameData.remainingTime + " 秒    暂停: " + (gameData.is_paused ? "是" : "否") + "    局数: " + gameData.currentRound
            }
            Text {
                objectName: "rm27Scores"
                color: "white"; font.pixelSize: 20
                width: parent.width; wrapMode: Text.Wrap
                text: !root.dataSource ? "" : "得分  红: " + gameData.redScore + "    蓝: " + gameData.blueScore + "    当前机器人 ID: " + gameData.currentRobotId
            }
            Text {
                objectName: "rm27Bases"
                color: "white"; font.pixelSize: 19
                width: parent.width; wrapMode: Text.Wrap
                text: !root.dataSource ? "" : "基地  红: " + gameData.redBaseHealth + "/" + gameData.redBaseMaxHealth + " (无敌: " + gameData.redBaseInvincible + ")    蓝: " + gameData.blueBaseHealth + "/" + gameData.blueBaseMaxHealth + " (无敌: " + gameData.blueBaseInvincible + ")"
            }
            Text {
                objectName: "rm27Outposts"
                color: "white"; font.pixelSize: 19
                width: parent.width; wrapMode: Text.Wrap
                text: !root.dataSource ? "" : "前哨站  红: " + gameData.redOutpostHealth + "/" + gameData.redOutpostMaxHealth + " (摧毁: " + gameData.redOutpostDestroyed + ")    蓝: " + gameData.blueOutpostHealth + "/" + gameData.blueOutpostMaxHealth + " (摧毁: " + gameData.blueOutpostDestroyed + ")"
            }
            Text {
                objectName: "rm27CurrentRobot"
                property var robot: { root.revision; return root.dataSource ? root.dataSource.getRobotInfo(root.dataSource.currentRobotId) : ({}) }
                color: "white"; font.pixelSize: 19
                text: "当前机器人血量: " + (robot.hp === undefined ? "未知" : robot.hp + "/" + robot.maxHp)
            }
            Text { text: "机器人状态（实际 GameData 值）"; color: "#8de0ff"; font.pixelSize: 21; topPadding: 10 }
            Repeater {
                model: [1, 3, 4, 5, 6, 7, 8, 9, 101, 103, 104, 105, 106, 107, 108, 109]
                delegate: Text {
                    required property int modelData
                    objectName: "rm27Robot" + modelData
                    property var robot: { root.revision; return root.dataSource ? root.dataSource.getRobotInfo(modelData) : ({}) }
                    color: "white"; font.pixelSize: 18
                    text: (robot.identityTeam || "未知阵营") + " " + (robot.identityName || "未知") + " (ID " + modelData + ")    血量: " + (robot.hp === undefined ? "未知" : robot.hp + "/" + robot.maxHp) + "    离线: " + (robot.isOffline === undefined ? "未知" : robot.isOffline)
                }
            }
            Text { text: "机器人模块状态（当前身份）"; color: "#8de0ff"; font.pixelSize: 21; topPadding: 12 }
            Text {
                objectName: "rm27ModuleSummary"
                property var snapshot: root.dataSource ? root.dataSource.robotModuleStatus : ({})
                color: "white"; font.pixelSize: 18; width: parent.width; wrapMode: Text.Wrap
                text: "模块状态已收到: " + (snapshot.received ? "是（机器人 ID " + snapshot.robotId + "）" : "否") +
                      "\n42mm 安装权限已确认: " + (snapshot.received && (snapshot.robotId === 1 || snapshot.robotId === 101) && snapshot.modular_42mm !== undefined && snapshot.modular_42mm <= 1 ? "是" : "否") +
                      "    当前权限值: " + (!snapshot.received || snapshot.modular_42mm === undefined ? "未收到" :
                          snapshot.robotId !== 1 && snapshot.robotId !== 101 ? "不适用（原始值 " + snapshot.modular_42mm + "）" :
                          snapshot.modular_42mm === 0 ? "0（未拥有）" : snapshot.modular_42mm === 1 ? "1（拥有）" : "非法值 " + snapshot.modular_42mm) +
                      "（仅适用于重装；不代表已安装或可射击）"
            }
            Repeater {
                model: [
                    {key:"power_manager", name:"电源管理"}, {key:"rfid", name:"RFID"},
                    {key:"light_strip", name:"灯条"}, {key:"small_shooter", name:"17mm 测速"},
                    {key:"big_shooter", name:"42mm 测速"}, {key:"uwb", name:"定位"},
                    {key:"armor", name:"装甲"}, {key:"video_transmission", name:"图传"},
                    {key:"capacitor", name:"电容"}, {key:"main_controller", name:"主控"}
                ]
                delegate: Text {
                    required property var modelData
                    property var snapshot: root.dataSource ? root.dataSource.robotModuleStatus : ({})
                    property var value: snapshot[modelData.key]
                    objectName: "rm27Module_" + modelData.key
                    color: "white"; font.pixelSize: 18
                    text: modelData.name + ": " + (value === undefined ? "未收到" : value === 0 ? "0（离线）" : value === 1 ? "1（在线）" : value === 2 ? "2（安装不规范，视为离线）" : "非法值 " + value)
                }
            }
            Text { text: "RMUL 装配事件观测"; color: "#8de0ff"; font.pixelSize: 21; topPadding: 12 }
            Text {
                objectName: "rm27Event16Status"
                width: parent.width; wrapMode: Text.Wrap
                color: "white"; font.pixelSize: 18
                text: !root.dataSource ? "" : "Event 16 已收到: " + (gameData.event16Received ? "是" : "否") +
                      "    最近接收: " + (gameData.event16Received ? Qt.formatDateTime(gameData.event16LastReceivedAt, "yyyy-MM-dd HH:mm:ss.zzz") : "无") +
                      "\n来源: GameData 协议事件（MQTT 或兼容入口） · 当前安装状态尚未独立确认"
            }
            Text { text: "2027 控制度 / 阵地控制状态：未接入"; color: "#ffca7a"; font.pixelSize: 20; topPadding: 12; bottomPadding: 32 }
        }
    }
}
