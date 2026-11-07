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
            Text { text: "本机 MQTT 接收与会话"; color: "#8de0ff"; font.pixelSize: 21 }
            Text {
                objectName: "rm27MqttSession"
                property var meta: root.dataSource ? root.dataSource.mqttReceptionMetadata : ({})
                color: "white"; font.pixelSize: 18; width: parent.width; wrapMode: Text.Wrap
                text: "MQTT: " + (meta.connected ? "已连接" : "未连接") + "    当前会话: " + (meta.session || 0) +
                      "    客户端身份: " + (meta.clientId || "未连接") + "    当前机器人: " + (meta.configuredIdentity || "无效") +
                      "\n时间为本机接收时间；当前会话不代表实时有效。"
            }
            Repeater {
                model: ["GameStatus", "RobotDynamicStatus", "RobotStaticStatus", "RobotModuleStatus", "Event16"]
                delegate: Text {
                    required property string modelData
                    property var meta: { root.revision; return root.dataSource ? root.dataSource.mqttReceptionMetadata : ({}) }
                    property var record: modelData === "RobotStaticStatus" ? ((meta.staticByRobot || ({}))[String(meta.configuredIdentity)] || ({})) : ((meta.topics || ({}))[modelData] || ({}))
                    property string state: record.sessionState || "never"
                    objectName: "rm27Reception_" + modelData
                    color: "white"; font.pixelSize: 18; width: parent.width; wrapMode: Text.Wrap
                    text: modelData + ": " + (state === "never" ? "从未收到" :
                          (state === "current" ? "当前会话观测" : state === "disconnected" ? "断连前历史观测" : state === "identity_changed" ? "旧身份历史观测" : "历史会话观测") +
                          "    本机接收: " + Qt.formatDateTime(record.receivedAt, "yyyy-MM-dd HH:mm:ss.zzz") +
                          "    距接收: " + Math.floor(record.ageMs / 1000) + " 秒    会话: " + record.session + (record.identity ? "    机器人 ID: " + record.identity : "（全局或身份未确定）"))
                }
            }
            Text { text: "全局比赛状态（官方 GameStatus 观测）"; color: "#8de0ff"; font.pixelSize: 21 }
            Text {
                objectName: "rm27GameStatusSummary"
                property var snapshot: root.dataSource ? root.dataSource.gameStatusObservation : ({})
                text: "比赛状态消息已收到: " + (snapshot.received ? "是" : "否")
                color: "white"; font.pixelSize: 18
            }
            Repeater {
                model: [
                    {key:"current_round", name:"当前局号"}, {key:"total_rounds", name:"总局数"},
                    {key:"red_score", name:"红方得分"}, {key:"blue_score", name:"蓝方得分"},
                    {key:"current_stage", name:"比赛阶段"}, {key:"stage_countdown_sec", name:"阶段剩余时间（秒）"},
                    {key:"stage_elapsed_sec", name:"阶段已过时间（秒）"}, {key:"is_paused", name:"暂停"},
                    {key:"game_result", name:"当局结果"}, {key:"end_reason", name:"结束原因"}
                ]
                delegate: Text {
                    required property var modelData
                    property var snapshot: root.dataSource ? root.dataSource.gameStatusObservation : ({})
                    property var item: snapshot.fields ? snapshot.fields[modelData.key] : undefined
                    property var last: snapshot.accepted ? snapshot.accepted[modelData.key] : undefined
                    property var receipt: root.dataSource ? ((root.dataSource.mqttReceptionMetadata.topics || ({})).GameStatus || ({})) : ({})
                    property var lastReceipt: (receipt.fieldUpdates || ({}))[modelData.key]
                    function lastStamp() { return lastReceipt ? "；本机接收 " + Qt.formatDateTime(lastReceipt.receivedAt, "HH:mm:ss.zzz") + " 会话 " + lastReceipt.session : "；接收来源未确认" }
                    function describe(value) {
                        if (modelData.key === "current_stage") return ["未开始", "准备", "十五秒自检", "五秒倒计时", "比赛中", "比赛结算中"][value] + " (" + value + ")"
                        if (modelData.key === "game_result") return ["平局", "红方胜利", "蓝方胜利"][value] + " (" + value + ")"
                        if (modelData.key === "end_reason") return ["", "基地被摧毁", "控制度", "全队总攻击伤害", "全队机器人总剩余血量", "平局", "异常终止", "主裁判手动判定胜负"][value] + " (" + value + ")"
                        return String(value)
                    }
                    objectName: "rm27GameStatus_" + modelData.key
                    color: "white"; font.pixelSize: 18; width: parent.width; wrapMode: Text.Wrap; textFormat: Text.RichText
                    text: modelData.name + ": " + (!item || item.state === "unreported" ? "未上报" + (last === undefined || modelData.key === "game_result" || modelData.key === "end_reason" ? "" : "（上次: " + describe(last) + "&nbsp;&nbsp;&nbsp;&nbsp;<font color='#60D8B7'>valid</font>" + lastStamp() + "）") :
                          item.state === "not_applicable" ? "当前阶段不适用（原始值 " + item.value + "）" :
                          item.state === "invalid" ? "非法或未识别（原始值 " + item.value + "）" : describe(item.value) + "&nbsp;&nbsp;&nbsp;&nbsp;<font color='#60D8B7'>valid</font>" + (item.legacyProjection === "out_of_range" ? "（超出旧状态可表示范围）" : ""))
                }
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
            Text { text: "当前机器人静态性能（最近上报）"; color: "#8de0ff"; font.pixelSize: 21; topPadding: 10 }
            Text {
                objectName: "rm27StaticPerformance"
                property var robot: { root.revision; return root.dataSource ? root.dataSource.getRobotInfo(root.dataSource.currentRobotId) : ({}) }
                property var reported: robot.staticPerformance || ({})
                function mode(key, first, second) {
                    let v = reported[key]
                    return v === undefined ? "未上报" : v === 1 ? first : v === 2 ? second : "非法值 " + v
                }
                function numberValue(key, min) {
                    let v = reported[key]
                    return v === undefined ? "未上报" : !isFinite(v) || v < min || v > 65535 ? "非法值 " + v : String(v)
                }
                color: "white"; font.pixelSize: 18; width: parent.width; wrapMode: Text.Wrap
                text: "当前身份: " + (robot.identityTeam || "未知阵营") + " " + (robot.identityName || "未知") + " (ID " + (robot.robotId || 0) + ")" +
                      "\n发射机构性能体系: " + mode("performance_system_shooter", "冷却优先", "爆发优先") +
                      "    底盘性能体系: " + mode("performance_system_chassis", "血量优先", "功率优先") +
                      "\n最大血量: " + numberValue("max_health", 1) + "    最大热量: " + numberValue("max_heat", 0) +
                      "    冷却速率: " + numberValue("heat_cooldown_rate", 0) +
                      "\n最大功率: " + numberValue("max_power", 0) + "    最大缓冲能量: " + numberValue("max_buffer_energy", 0) +
                      "    最大底盘能量: " + numberValue("max_chassis_energy", 0) +
                      "\n缺失或非法字段不会覆盖上次有效值；上报值不代表安装状态。"
            }
            Text {
                objectName: "rm27LastEventObservation"
                property var event: root.dataSource ? root.dataSource.lastEventObservation : ({})
                text: event.eventId === undefined ? "最近原始事件：未收到" :
                      "最近原始事件：ID " + event.eventId + "，参数 " + event.param
                color: "white"; font.pixelSize: 18; wrapMode: Text.Wrap; width: parent.width
            }
            Text { text: "当前机器人动态状态（官方消息观测）"; color: "#8de0ff"; font.pixelSize: 21; topPadding: 10 }
            Text {
                objectName: "rm27DynamicSummary"
                property var snapshot: root.dataSource ? root.dataSource.robotDynamicStatus : ({})
                color: "white"; font.pixelSize: 18
                text: "动态消息已收到: " + (snapshot.received ? "是（机器人 ID " + snapshot.robotId + "，按终端关联）" : "否")
            }
            Repeater {
                model: [
                    {key:"current_health", name:"当前血量"}, {key:"current_heat", name:"当前热量"},
                    {key:"last_projectile_fire_rate", name:"上一发弹丸射速（m/s）"}, {key:"reserved", name:"保留字段"},
                    {key:"current_buffer_energy", name:"当前缓冲能量"}, {key:"current_experience", name:"当前经验"},
                    {key:"experience_for_upgrade", name:"距离升级经验"}, {key:"total_projectiles_fired", name:"累计已发弹量"},
                    {key:"remaining_ammo", name:"剩余允许发弹量（原始值）"}, {key:"is_out_of_combat", name:"脱战状态"},
                    {key:"out_of_combat_countdown", name:"脱战倒计时"}, {key:"can_remote_heal", name:"远程补血资格"},
                    {key:"can_remote_ammo", name:"远程补弹资格"}
                ]
                delegate: Text {
                    required property var modelData
                    property var snapshot: root.dataSource ? root.dataSource.robotDynamicStatus : ({})
                    property var item: snapshot.fields ? snapshot.fields[modelData.key] : undefined
                    property var last: snapshot.accepted ? snapshot.accepted[modelData.key] : undefined
                    property var receipt: root.dataSource ? ((root.dataSource.mqttReceptionMetadata.topics || ({})).RobotDynamicStatus || ({})) : ({})
                    property var lastReceipt: (receipt.fieldUpdates || ({}))[modelData.key]
                    function lastStamp() { return lastReceipt ? "；本机接收 " + Qt.formatDateTime(lastReceipt.receivedAt, "HH:mm:ss.zzz") + " 会话 " + lastReceipt.session : "；接收来源未确认" }
                    objectName: "rm27Dynamic_" + modelData.key
                    color: "white"; font.pixelSize: 18; textFormat: Text.RichText
                    text: modelData.name + ": " + (!item || item.state === "unreported" ? "未上报" + (last === undefined ? "" : "（上次: " + last + "&nbsp;&nbsp;&nbsp;&nbsp;<font color='#60D8B7'>valid</font>" + lastStamp() + "）") :
                          item.state === "invalid" ? "非法值 " + item.value : item.state === "not_applicable" ? "不适用（协议保留位）" : item.value + "&nbsp;&nbsp;&nbsp;&nbsp;<font color='#60D8B7'>valid</font>" + (item.legacyProjection === "out_of_range" ? "（超出旧状态可表示范围）" : ""))
                }
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
                property var receipt: root.dataSource ? ((root.dataSource.mqttReceptionMetadata.topics || ({})).RobotModuleStatus || ({})) : ({})
                color: "white"; font.pixelSize: 18; width: parent.width; wrapMode: Text.Wrap
                text: "模块状态已收到: " + (snapshot.received ? "是（机器人 ID " + snapshot.robotId + "）" : "否") +
                      "\n42mm 安装权限本会话已上报: " + (receipt.sessionState === "current" && snapshot.received && (snapshot.robotId === 1 || snapshot.robotId === 101) && snapshot.modular_42mm !== undefined && snapshot.modular_42mm <= 1 ? "是" : "否") +
                      "    最近权限原值: " + (!snapshot.received || snapshot.modular_42mm === undefined ? "未收到" :
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
