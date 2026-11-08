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

    component SourceCard: Rectangle {
        id: card
        property string title: ""
        default property alias content: cardBody.data
        width: parent.width
        height: cardLayout.implicitHeight + 28
        radius: 8
        color: "#172632"
        border.color: "#2b4453"
        Column {
            id: cardLayout
            x: 16; y: 14
            width: parent.width - 32
            spacing: 8
            Text { text: card.title; color: "#8de0ff"; font.pixelSize: 17; font.bold: true }
            Column { id: cardBody; width: parent.width; spacing: 6 }
        }
    }

    component SourceRow: Item {
        id: row
        property string primary: ""
        property string detail: ""
        property bool sideBySide: width >= 680
        width: parent.width
        height: Math.max(primaryText.height, sideBySide ? detailText.height : 0) +
                (!sideBySide && detail.length ? detailText.height + 2 : 0)
        Text {
            id: primaryText
            width: row.sideBySide ? row.width * 0.48 : row.width
            text: row.primary; color: "white"; font.pixelSize: 18; wrapMode: Text.Wrap
        }
        Text {
            id: detailText
            x: row.sideBySide ? row.width * 0.5 : 0
            y: row.sideBySide ? 0 : primaryText.height + 2
            width: row.sideBySide ? row.width * 0.5 : row.width
            visible: row.detail.length > 0
            text: row.detail; color: row.detail.indexOf("valid") === 0 ? "#60D8B7" : "#a8c4d8"; font.pixelSize: 16; wrapMode: Text.Wrap
        }
    }

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
                model: ["GameStatus", "GlobalUnitStatus", "RobotDynamicStatus", "RobotStaticStatus", "RobotModuleStatus", "PenaltyInfo", "RobotInjuryStat", "Event16"]
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
            Text { text: "来源对照（不决定业务优先级）"; color: "#8de0ff"; font.pixelSize: 21 }
            SourceCard {
                title: "血量 · 当前机器人"
                Column {
                    id: healthSources
                    objectName: "rm27HealthSources"
                    width: parent.width; spacing: 6
                    property var global: root.dataSource ? root.dataSource.globalHealthObservation : ({})
                    property var dynamic: root.dataSource ? root.dataSource.robotDynamicStatus : ({})
                    property var meta: root.dataSource ? root.dataSource.mqttReceptionMetadata : ({})
                    property string robotId: String(root.dataSource ? root.dataSource.currentRobotId : 0)
                    property var gf: (global.fields || ({}))[robotId]
                    property var globalLast: (global.accepted || ({}))[robotId]
                    property var df: (dynamic.fields || ({})).current_health
                    property var gr: global.source === "official_mqtt" ? ((meta.topics || ({})).GlobalUnitStatus || ({})) : ({})
                    property var dr: (meta.topics || ({})).RobotDynamicStatus || ({})
                    property string text: healthGlobal.primary + " " + healthDynamic.primary
                    function currentField(field) { return !field || field.state === "unreported" ? "未上报" : field.state !== "valid" ? "非法" : String(field.value) + (field.legacyProjection ? "（超出旧类型范围）" : "") }
                    function stamp(record, key, source) {
                        if (source && source !== "official_mqtt") return "接收来源未确认"
                        let entry = (record.fieldUpdates || ({}))[key]
                        return entry ? "本机接收 " + Qt.formatDateTime(entry.receivedAt, "HH:mm:ss.zzz") + " · 会话 " + entry.session : "接收来源未确认"
                    }
                    SourceRow { primary: "机器人 ID " + healthSources.robotId }
                    SourceRow {
                        id: healthGlobal
                        primary: "Global 血量: " + healthSources.currentField(healthSources.gf) + ((!healthSources.gf || healthSources.gf.state === "unreported") && healthSources.globalLast !== undefined ? "（上次: " + healthSources.globalLast + "）" : "") + (healthSources.global.received && !healthSources.global.identityMatches ? "（旧身份观测）" : "")
                        detail: "来源: " + (healthSources.global.source || "未收到") + " · " + (healthSources.gr.sessionState || "never") + " · " + healthSources.stamp(healthSources.gr, healthSources.robotId, (healthSources.global.acceptedSource || ({}))[healthSources.robotId])
                    }
                    SourceRow {
                        id: healthDynamic
                        primary: "Dynamic 血量: " + healthSources.currentField(healthSources.df)
                        detail: (healthSources.dr.sessionState || "never") + " · " + healthSources.stamp(healthSources.dr, "current_health")
                    }
                }
            }
            SourceCard {
                title: "热量"
                Column {
                    id: heatSources
                    objectName: "rm27HeatSources"
                    width: parent.width; spacing: 6
                    property var robot: { root.revision; return root.dataSource ? root.dataSource.getRobotInfo(root.dataSource.currentRobotId) : ({}) }
                    property var dynamic: root.dataSource ? root.dataSource.robotDynamicStatus : ({})
                    property var local: root.dataSource ? root.dataSource.localHeatObservation : ({})
                    property var field: (dynamic.fields || ({})).current_heat
                    property var receipt: root.dataSource ? ((root.dataSource.mqttReceptionMetadata.topics || ({})).RobotDynamicStatus || ({})) : ({})
                    property string text: localHeat.primary + " " + localHeat.detail
                    SourceRow {
                        primary: "Dynamic 官方热量: " + (!heatSources.field || heatSources.field.state === "unreported" ? "未上报" : heatSources.field.state !== "valid" ? "非法" : String(heatSources.field.value))
                        detail: "会话: " + (heatSources.receipt.sessionState || "never")
                    }
                    SourceRow {
                        primary: "旧兼容热量: " + (heatSources.robot.currentHeat === undefined ? "未知" : heatSources.robot.currentHeat)
                        detail: "来源可能混合"
                    }
                    SourceRow {
                        id: localHeat
                        property bool matches: heatSources.local.robotId === (root.dataSource ? root.dataSource.currentRobotId : 0)
                        primary: "最近本地输入: " + (matches ? heatSources.local.value : "无当前身份记录")
                        detail: matches ? "来源: " + heatSources.local.source : ""
                    }
                }
            }
            SourceCard {
                title: "连接状态"
                Column {
                    id: connectionSources
                    objectName: "rm27ConnectionSources"
                    width: parent.width; spacing: 6
                    property var meta: root.dataSource ? root.dataSource.mqttReceptionMetadata : ({})
                    property var referee: { root.revision; return root.dataSource ? root.dataSource.refereeRobotObservation(root.dataSource.currentRobotId) : ({}) }
                    property var staticReceipt: (meta.staticByRobot || ({}))[String(meta.configuredIdentity)] || ({})
                    property int video: root.dataSource ? root.dataSource.videoReceptionState : -1
                    property string text: refereeConnection.primary + " " + videoConnection.primary + " " + controllerConnection.primary
                    function val(v, labels) { return v === undefined ? "未上报" : v < labels.length ? labels[v] + " (" + v + ")" : "未识别 (" + v + ")" }
                    SourceRow { primary: "MQTT: " + (connectionSources.meta.connected ? "已连接" : "未连接") }
                    SourceRow {
                        id: refereeConnection
                        primary: "裁判机器人连接: " + connectionSources.val(connectionSources.referee.connection_state, ["未连接", "已连接"])
                        detail: "来源: " + (connectionSources.referee.source || "未收到") + " · " + (connectionSources.referee.source === "official_mqtt" ? (connectionSources.staticReceipt.sessionState || "never") : "来源未确认")
                    }
                    SourceRow { primary: "上场: " + connectionSources.val(connectionSources.referee.field_state, ["已上场", "未上场"]) + "    存活: " + connectionSources.val(connectionSources.referee.alive_state, ["未知", "存活", "战亡"]) }
                    SourceRow { id: videoConnection; primary: "视频接收: " + (connectionSources.video === 1 ? "近期收到帧" : connectionSources.video === 0 ? "帧接收超时" : "未知") }
                    SourceRow { id: controllerConnection; primary: "实体控制器连接: 未知"; detail: "无独立可靠来源" }
                }
            }
            SourceCard {
                title: "比赛状态对照"
                Column {
                    id: gameProjection
                    objectName: "rm27GameProjection"
                    width: parent.width; spacing: 6
                    property var old: { root.revision; return root.dataSource ? root.dataSource.legacyGameProjection() : ({}) }
                    property var official: root.dataSource ? root.dataSource.gameStatusObservation : ({})
                    property var meta: root.dataSource ? ((root.dataSource.mqttReceptionMetadata.topics || ({})).GameStatus || ({})) : ({})
                    property string text: stageProjection.primary + " " + countdownProjection.primary
                    function diff(key) { let v = (official.fields || ({}))[key]; return !v || v.state !== "valid" ? "未确认" : v.value === old[key] ? "一致" : "不同" }
                    SourceRow { primary: "GameStatus 官方会话: " + (gameProjection.meta.sessionState || "never") }
                    SourceRow { id: stageProjection; primary: "旧兼容阶段 " + gameProjection.old.current_stage + "（" + gameProjection.diff("current_stage") + "）" }
                    SourceRow { id: countdownProjection; primary: "剩余 " + gameProjection.old.stage_countdown_sec + "（" + gameProjection.diff("stage_countdown_sec") + "）" }
                    SourceRow { primary: "旧兼容比分 " + gameProjection.old.red_score + ":" + gameProjection.old.blue_score + "（红 " + gameProjection.diff("red_score") + " / 蓝 " + gameProjection.diff("blue_score") + "）" }
                    SourceRow { primary: "暂停 " + gameProjection.old.is_paused + "（" + gameProjection.diff("is_paused") + "）"; detail: "差异只表示数值对照，不代表实时性或来源权威。" }
                }
            }
            SourceCard {
                title: "判罚信息 · 终端关联身份（非消息自带身份）"
                Column {
                    width: parent.width; spacing: 6
                    property var snapshot: root.dataSource ? root.dataSource.penaltyObservation : ({})
                    property var receipt: root.dataSource ? ((root.dataSource.mqttReceptionMetadata.topics || ({})).PenaltyInfo || ({})) : ({})
                    SourceRow {
                        objectName: "rm27PenaltySession"
                        primary: "观测身份: " + (parent.snapshot.received ? parent.snapshot.identity : "未收到") + (parent.snapshot.received && !parent.snapshot.identityMatches ? "（旧身份）" : "")
                        detail: parent.snapshot.source === "official_mqtt" ? "会话: " + (parent.receipt.sessionState || "never") + (parent.receipt.receivedAt ? " · 本机接收 " + Qt.formatDateTime(parent.receipt.receivedAt, "HH:mm:ss.zzz") : "") : "来源未确认"
                    }
                    Repeater {
                        model: [{key:"penalty_type", name:"判罚类型"}, {key:"penalty_effect_sec", name:"效果时长（秒）"}, {key:"total_penalty_num", name:"累计判罚数量"}]
                        delegate: SourceRow {
                            required property var modelData
                            property var item: (parent.snapshot.fields || ({}))[modelData.key]
                            property var names: ["", "黄牌", "双方黄牌", "红牌", "超功率", "超热量", "超射速"]
                            objectName: "rm27Penalty_" + modelData.key
                            primary: modelData.name + ": " + (!item || item.state === "unreported" ? "未上报" : String(item.value) + (modelData.key === "penalty_type" ? "（" + (item.value >= 1 && item.value <= 6 ? names[item.value] : "未知类型") + "）" : ""))
                            detail: !item || item.state === "unreported" ? "未上报" : item.state === "unknown" ? "未知枚举" : "valid"
                        }
                    }
                }
            }
            SourceCard {
                title: "受伤统计 · 一次存活期间累计（终端关联身份）"
                Column {
                    width: parent.width; spacing: 6
                    property var snapshot: root.dataSource ? root.dataSource.injuryObservation : ({})
                    property var receipt: root.dataSource ? ((root.dataSource.mqttReceptionMetadata.topics || ({})).RobotInjuryStat || ({})) : ({})
                    SourceRow {
                        objectName: "rm27InjurySession"
                        primary: "观测身份: " + (parent.snapshot.received ? parent.snapshot.identity : "未收到") + (parent.snapshot.received && !parent.snapshot.identityMatches ? "（旧身份）" : "")
                        detail: parent.snapshot.source === "official_mqtt" ? "会话: " + (parent.receipt.sessionState || "never") + (parent.receipt.receivedAt ? " · 本机接收 " + Qt.formatDateTime(parent.receipt.receivedAt, "HH:mm:ss.zzz") : "") : "来源未确认"
                    }
                    Repeater {
                        model: [
                            {key:"total_damage", name:"累计受伤总计"}, {key:"collision_damage", name:"撞击伤害"},
                            {key:"small_projectile_damage", name:"17mm 弹丸伤害"}, {key:"large_projectile_damage", name:"42mm 弹丸伤害"},
                            {key:"dart_splash_damage", name:"飞镖溅射伤害"}, {key:"module_offline_damage", name:"模块离线扣血"},
                            {key:"offline_damage", name:"异常离线扣血"}, {key:"penalty_damage", name:"判罚扣血"},
                            {key:"server_kill_damage", name:"服务器强制战亡扣血"}, {key:"killer_id", name:"击杀者 ID"}
                        ]
                        delegate: SourceRow {
                            required property var modelData
                            property var item: (parent.snapshot.fields || ({}))[modelData.key]
                            objectName: "rm27Injury_" + modelData.key
                            primary: modelData.name + ": " + (!item || item.state === "unreported" ? "未上报" : String(item.value))
                            detail: !item || item.state === "unreported" ? "未上报" : modelData.key === "killer_id" ? "valid · " + (item.identityKind === "unknown" ? "未知身份" : item.identityName) : "valid"
                        }
                    }
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
