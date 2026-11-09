/**
 * @file TopInfoBar.qml
 * @brief 顶部信息栏组件
 * @details 主界面顶部栏按官方客户端结构对齐，禁止默认展示 mock UI。
 */

import QtQuick 2.15
import QtQuick.Layouts 1.15

Item {
    id: root
    width: parent ? parent.width : 1920
    height: 170
    readonly property string redTeamLogoSource: "qrc:/images/top_mid/red_team_logo.png"
    readonly property string blueTeamLogoSource: "qrc:/images/top_mid/blue_team_logo.png"
    readonly property int teamNameWidth: 360
    readonly property int teamNameOuterInset: 28

    // === 数据绑定属性 ===
    // gameData 由 MainWindow 注入，顶部栏统一从这个入口读取比赛状态。
    // qmllint disable unqualified
    readonly property var gameDataContext: typeof gameData !== "undefined" ? gameData : null
    readonly property var uiConfigContext: typeof uiConfig !== "undefined" ? uiConfig : null
    // qmllint enable unqualified
    readonly property string competitionUiMode: uiConfigContext ? uiConfigContext.competitionUiMode : "rmul"
    readonly property var officialGame: gameDataContext ? gameDataContext.gameStatusObservation : ({})
    readonly property var globalObservation: gameDataContext ? gameDataContext.globalHealthObservation : ({})
    readonly property var mqttMeta: gameDataContext ? gameDataContext.mqttReceptionMetadata : ({})
    readonly property var gameReceipt: (mqttMeta.topics || ({})).GameStatus || ({})
    readonly property var globalReceipt: (mqttMeta.topics || ({})).GlobalUnitStatus || ({})
    function officialField(fields, receipt, key, identityOk) {
        var field = (fields || ({}))[key] || ({})
        if (field.state === "invalid") return "无效"
        if (field.state !== "valid") return "未上报"
        if (!identityOk) return "身份不匹配"
        if (receipt.sessionState === "current") return String(field.value)
        if (receipt.sessionState === "historical" || receipt.sessionState === "disconnected") return String(field.value) + " (历史)"
        return "来源未确认"
    }
    function gameField(key) { return officialField(officialGame.fields, gameReceipt, key, true) }
    function globalField(key) { return officialField(globalObservation.rawFields, globalReceipt, key, globalObservation.identityMatches === true) }
    readonly property string rmulStage: gameField("current_stage")
    readonly property string rmulStageLabel: {
        var f = (officialGame.fields || ({})).current_stage || ({})
        if (f.state !== "valid" || (gameReceipt.sessionState !== "current" && gameReceipt.sessionState !== "historical" && gameReceipt.sessionState !== "disconnected")) return rmulStage
        var names = ["未开始", "准备阶段", "裁判自检", "五秒倒计时", "比赛中", "比赛结算"]
        return names[Number(f.value)] + (gameReceipt.sessionState === "current" ? "" : " (历史)")
    }
    readonly property string rmulRedScore: gameField("red_score")
    readonly property string rmulBlueScore: gameField("blue_score")
    readonly property string rmulAllyControl: globalField("total_cp_ally")
    readonly property string rmulEnemyControl: globalField("total_cp_enemy")
    readonly property int controlReceiver: Number(globalObservation.receiverId || 0)
    readonly property bool controlReceiverRed: controlReceiver >= 1 && controlReceiver <= 11
    readonly property bool controlReceiverBlue: controlReceiver >= 101 && controlReceiver <= 111
    readonly property string rmulRedControl: controlReceiverRed ? rmulAllyControl : controlReceiverBlue ? rmulEnemyControl : "未上报"
    readonly property string rmulBlueControl: controlReceiverRed ? rmulEnemyControl : controlReceiverBlue ? rmulAllyControl : "未上报"
    // RMUL rules specify 600 initial control points. The protocol's accumulated
    // field has no confirmed maximum; 600 is a visual reference, not a cap or
    // a conversion to remaining control degree. The numeric value stays raw.
    function controlReferenceRatio(key) {
        var item = (globalObservation.rawFields || ({}))[key] || ({})
        if (item.state !== "valid" || globalObservation.identityMatches !== true) return -1
        if (globalReceipt.sessionState !== "current" && globalReceipt.sessionState !== "historical" && globalReceipt.sessionState !== "disconnected") return -1
        return Math.max(0, Math.min(1, Number(item.value) / 600))
    }
    readonly property real redControlReferenceRatio: controlReferenceRatio(controlReceiverRed ? "total_cp_ally" : controlReceiverBlue ? "total_cp_enemy" : "")
    readonly property real blueControlReferenceRatio: controlReferenceRatio(controlReceiverRed ? "total_cp_enemy" : controlReceiverBlue ? "total_cp_ally" : "")
    readonly property string rmulTime: {
        var raw = gameField("stage_countdown_sec")
        var f = (officialGame.fields || ({})).stage_countdown_sec || ({})
        if (f.state !== "valid" || Number(f.value) < 0 || (gameReceipt.sessionState !== "current" && gameReceipt.sessionState !== "historical" && gameReceipt.sessionState !== "disconnected")) return raw
        var n = Number(f.value)
        return Math.floor(n / 60) + ":" + (n % 60 < 10 ? "0" : "") + (n % 60) + (gameReceipt.sessionState === "current" ? "" : " (历史)")
    }
    property int redBaseHP: gameDataContext ? gameDataContext.redBaseHealth : 2500
    property int redBaseMaxHP: gameDataContext ? gameDataContext.redBaseMaxHealth : 5000
    property int blueBaseHP: gameDataContext ? gameDataContext.blueBaseHealth : 3000
    property int blueBaseMaxHP: gameDataContext ? gameDataContext.blueBaseMaxHealth : 5000
    property int redOutpostHP: gameDataContext ? gameDataContext.redOutpostHealth : 1500
    property int redOutpostMaxHP: gameDataContext ? gameDataContext.redOutpostMaxHealth : 1500
    property int blueOutpostHP: gameDataContext ? gameDataContext.blueOutpostHealth : 1500
    property int blueOutpostMaxHP: gameDataContext ? gameDataContext.blueOutpostMaxHealth : 1500
    property int redOutpostRebuildCount: gameDataContext ? gameDataContext.redOutpostRebuildCount : 2
    property int blueOutpostRebuildCount: gameDataContext ? gameDataContext.blueOutpostRebuildCount : 2
    property int redOutpostMaxRebuildCount: gameDataContext ? gameDataContext.redOutpostMaxRebuildCount : 2
    property int blueOutpostMaxRebuildCount: gameDataContext ? gameDataContext.blueOutpostMaxRebuildCount : 2
    property bool showOutpostModule: true
    property int redScore: gameDataContext ? gameDataContext.redScore : 0
    property int blueScore: gameDataContext ? gameDataContext.blueScore : 0
    property int currentRound: gameDataContext ? gameDataContext.currentRound : 1
    property int gameTime: gameDataContext ? gameDataContext.remainingTime : 420
    property string gamePhase: gameDataContext ? gameDataContext.gamePhase : "未开始"
    property bool gamePaused: gameDataContext ? gameDataContext.is_paused : false
    property int redEconomy: gameDataContext ? gameDataContext.redEconomy : 550
    property int blueEconomy: gameDataContext ? gameDataContext.blueEconomy : 550
    property string redTeamName: gameDataContext ? gameDataContext.redTeamName : "复旦大学 星云EGA"
    property string blueTeamName: gameDataContext ? gameDataContext.blueTeamName : "上海交通大学 交龙"

    // 顶部栏只允许显示已有真实字段，禁止继续展示 mock 占领/固定 buff 值
    property int redDefenseBonus: gameDataContext ? gameDataContext.redDefenseBonus : 0
    property int blueDefenseBonus: gameDataContext ? gameDataContext.blueDefenseBonus : 0
    property bool redInvincible: gameDataContext ? (gameDataContext.redBaseInvincible || false) : false
    property bool blueInvincible: gameDataContext ? (gameDataContext.blueBaseInvincible || false) : false
    readonly property bool redOutpostDestroyed: redOutpostHP <= 0
    readonly property bool blueOutpostDestroyed: blueOutpostHP <= 0
    readonly property bool redInvincibleEffective: redInvincible || !redOutpostDestroyed
    readonly property bool blueInvincibleEffective: blueInvincible || !blueOutpostDestroyed

    property bool showPerformanceOverlay: false
    property bool showOccupationHints: false
    property double redOccupationProgress: 0.0
    property int redOccupationTime: 0
    property bool isRedOccupied: false

    property double blueOccupationProgress: 0.0
    property int blueOccupationTime: 0
    property bool isBlueOccupied: false

    property string formattedTime: {
        var m = Math.floor(root.gameTime / 60);
        var s = root.gameTime % 60;
        return (m < 10 ? "0" + m : m) + ":" + (s < 10 ? "0" + s : s);
    }

    // === 帧率显示 ===
    FpsDisplay {
        visible: root.showPerformanceOverlay
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: 10
        anchors.topMargin: 5
        z: 100 // 确保帧率显示位于最上层
    }

    Item {
        objectName: "rmulTopInfo"
        visible: root.competitionUiMode === "rmul"
        anchors.fill: parent
        anchors.topMargin: 5

        Item {
            id: compactBar
            objectName: "rmulCompactBar"
            width: Math.min(parent.width - 24, 1280)
            height: 76
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top

            Rectangle {
                anchors.fill: parent
                radius: 5
                color: "#a5162028"
                border.color: "#596a7480"
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12; anchors.rightMargin: 12
                spacing: 0
                Item {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    Rectangle {
                        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; height: 15; color: "#53615c5d"
                        Rectangle { objectName: "redControlReferenceBar"; anchors.left: parent.left; height: parent.height; width: parent.width * Math.max(0, root.redControlReferenceRatio); color: "#dc6860"; opacity: root.globalReceipt.sessionState === "current" ? 1 : 0.45 }
                    }
                    Column {
                        anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; spacing: 3
                        Text { text: "红方"; color: "#f18b82"; font.pixelSize: 13; font.bold: true }
                        Text { objectName: "rmulRedControl"; text: "累计控制度  " + root.rmulRedControl + "  /  600参考"; color: "#d4dee3"; font.pixelSize: 13 }
                    }
                    Text { objectName: "rmulRedScore"; anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter; text: root.rmulRedScore; color: "#fff1ef"; font.pixelSize: 26; font.bold: true }
                }
                Item {
                    Layout.preferredWidth: Math.min(260, compactBar.width * 0.28)
                    Layout.fillHeight: true
                    Column {
                        anchors.centerIn: parent; spacing: 1
                        Text { objectName: "rmulGameStage"; anchors.horizontalCenter: parent.horizontalCenter; text: root.rmulStageLabel + "  ·  第 " + root.gameField("current_round") + " 局"; color: "#a9d7e8"; font.pixelSize: 12 }
                        Text { objectName: "rmulGameTime"; anchors.horizontalCenter: parent.horizontalCenter; text: root.rmulTime; color: "white"; font.pixelSize: 28; font.bold: true }
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "红蓝比分  ·  官方观测"; color: "#a3b5bd"; font.pixelSize: 10 }
                    }
                }
                Item {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    Rectangle {
                        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; height: 15; color: "#53615c5d"
                        Rectangle { objectName: "blueControlReferenceBar"; anchors.right: parent.right; height: parent.height; width: parent.width * Math.max(0, root.blueControlReferenceRatio); color: "#59bce9"; opacity: root.globalReceipt.sessionState === "current" ? 1 : 0.45 }
                    }
                    Column {
                        anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter; spacing: 3
                        Text { anchors.right: parent.right; text: "蓝方"; color: "#83ccef"; font.pixelSize: 13; font.bold: true }
                        Text { objectName: "rmulBlueControl"; anchors.right: parent.right; text: "累计控制度  " + root.rmulBlueControl + "  /  600参考"; color: "#d4dee3"; font.pixelSize: 13 }
                    }
                    Text { objectName: "rmulBlueScore"; anchors.left: parent.left; anchors.leftMargin: 16; anchors.verticalCenter: parent.verticalCenter; text: root.rmulBlueScore; color: "#e9f8ff"; font.pixelSize: 26; font.bold: true }
                }
            }
        }
    }

    Item {
        objectName: "rmucLegacyTopInfo"
        visible: root.competitionUiMode === "rmuc"
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.topMargin: 0

        // ========================================
        // [中间] 统一背景面板
        // ========================================
        Item {
            id: centerPanel
            width: 340
            height: 86
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 4

            Image {
                anchors.fill: parent
                source: "qrc:/images/top_middle_background_v2.png"
                fillMode: Image.PreserveAspectFit
                opacity: 0.8
            }

            // --- 文字覆盖层 ---
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 13
                text: (root.gamePaused ? "比赛暂停" : root.gamePhase) + " · Round - " + root.currentRound
                color: root.gamePaused ? "#FFD54F" : "#66CCFF"
                font.pixelSize: 11
                font.bold: true
                font.family: "Arial"
                style: Text.Outline
                styleColor: "#000000"
            }

            Text {
                anchors.centerIn: parent
                anchors.verticalCenterOffset: 11
                text: root.formattedTime
                color: root.gamePaused ? "#FFD54F" : "#FF3333"
                font.family: "Arial"
                font.pixelSize: 26
                font.bold: true
                style: Text.Outline
                styleColor: "#000000"
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 72
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: 0
                text: root.redScore.toString()
                color: "#FFFFFF"
                font.pixelSize: 20
                font.bold: true
                font.family: "Arial"
                style: Text.Outline
                styleColor: "#000000"
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 72
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: 0
                text: root.blueScore.toString()
                color: "#FFFFFF"
                font.pixelSize: 20
                font.bold: true
                font.family: "Arial"
                style: Text.Outline
                styleColor: "#000000"
            }
        }

        // ========================================
        // [中间下方] 经济面板
        // ========================================
        Item {
            id: economyPanel
            width: 200
            height: 80
            anchors.top: centerPanel.bottom
            anchors.topMargin: -5
            anchors.horizontalCenter: parent.horizontalCenter

            Image {
                anchors.fill: parent
                source: "qrc:/images/BattleVolumn_Bg.png"
                fillMode: Image.Stretch
                opacity: 0.8
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 25
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: 2
                text: root.redEconomy.toString()
                color: "#FFFFFF"
                font.pixelSize: 24
                font.bold: true
                font.family: "Arial"
                style: Text.Outline
                styleColor: "#000000"
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 25
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: 2
                text: root.blueEconomy.toString()
                color: "#FFFFFF"
                font.pixelSize: 24
                font.bold: true
                font.family: "Arial"
                style: Text.Outline
                styleColor: "#000000"
            }
        }

        // ========================================
        // [左侧] 红方区域
        // ========================================
        Item {
            id: redArea
            anchors.right: centerPanel.left
            anchors.rightMargin: 7
            anchors.top: parent.top
            height: parent.height
            width: 800

            Text {
                id: redTeamNameText
                width: root.teamNameWidth
                anchors.right: redHealthContainer.right
                anchors.rightMargin: root.teamNameOuterInset
                anchors.top: parent.top
                anchors.topMargin: 0
                text: root.redTeamName
                color: "#FFFFFF"
                font.pixelSize: 17
                minimumPixelSize: 13
                fontSizeMode: Text.HorizontalFit
                font.bold: true
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                style: Text.Outline
                styleColor: "#101214"
                z: 20
            }

            Item {
                id: redHealthContainer
                width: 470
                height: 56
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 4

                BaseHealthBar {
                    id: redBar
                    width: 332
                    height: 30
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom

                    currentHealth: root.redBaseHP
                    maxHealth: root.redBaseMaxHP
                    isBlue: false
                    invincible: root.redInvincibleEffective
                }

                OutpostStatusBadge {
                    id: redOutpostModule
                    visible: root.showOutpostModule
                    width: 92
                    height: 52
                    anchors.right: redBar.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: redBar.verticalCenter
                    anchors.verticalCenterOffset: 0
                    opacity: root.redOutpostDestroyed ? 0.55 : 1.0
                    z: 12
                    isBlue: false
                    hp: root.redOutpostHP
                    maxHp: root.redOutpostMaxHP
                    rebuildCount: root.redOutpostRebuildCount
                    maxRebuildCount: root.redOutpostMaxRebuildCount
                }

                Row {
                    anchors.top: redBar.bottom
                    anchors.topMargin: 5
                    anchors.left: redBar.left
                    anchors.leftMargin: 20
                    spacing: 4
                    visible: root.redInvincible || root.redDefenseBonus > 0

                    Image {
                        source: "qrc:/images/buffs/shield.png"
                        width: 16
                        height: 16
                        fillMode: Image.PreserveAspectFit
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: root.redInvincible ? "护盾激活" : (root.redDefenseBonus + "%")
                        color: "#EEEEEE"
                        font.pixelSize: 10
                        font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: root.redInvincible ? "基地状态" : "防御增益"
                        color: "#AAAAAA"
                        font.pixelSize: 10
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                Item {
                    id: redLogoBadge
                    width: 72
                    height: 72
                    anchors.left: redBar.right
                    anchors.leftMargin: -28
                    anchors.verticalCenter: redBar.verticalCenter
                    anchors.verticalCenterOffset: 0
                    z: 14

                    Image {
                        anchors.fill: parent
                        source: root.redTeamLogoSource
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        mipmap: true
                    }
                }
            }

            // 红方底部栏
                RowLayout {
                    anchors.right: parent.right
                    anchors.rightMargin: -38
                    anchors.top: redHealthContainer.bottom
                    anchors.topMargin: 2
                    spacing: 8

                // 1. 机器人列表
                RowLayout {
                    spacing: 2
                    RobotStatusItem {
                        robotId: 7
                        avatarSource: "qrc:/images/robot_avatar.png"
                    }
                    RobotStatusItem {
                        robotId: 6
                        avatarSource: "qrc:/images/robot_avatar.png"
                    }
                    RobotStatusItem {
                        robotId: 4
                        avatarSource: "qrc:/images/robot_avatar.png"
                    }
                    RobotStatusItem {
                        robotId: 3
                        avatarSource: "qrc:/images/robot_avatar.png"
                    }
                    RobotStatusItem {
                        robotId: 1
                        avatarSource: "qrc:/images/robot_avatar.png"
                    }
                }

                // 2. 占领提示进度环
                ColumnLayout {
                    spacing: 4
                    visible: root.showOccupationHints && root.isRedOccupied

                    Item {
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        Layout.alignment: Qt.AlignHCenter

                        // 进度环背景占位圆
                        Rectangle {
                            anchors.fill: parent
                            radius: 16
                            color: "transparent"
                            border.color: "#60333333"    // 半透明边框
                            border.width: 2
                        }

                        Canvas {
                            id: redProgressCanvas
                            anchors.fill: parent
                            rotation: -90 // 从顶部开始绘制
                            property double progress: root.redOccupationProgress
                            property color progressColor: "#FF3333" // 红方颜色

                            onProgressChanged: requestPaint()

                            onPaint: {
                                var ctx = getContext("2d");
                                var centerX = width / 2;
                                var centerY = height / 2;
                                var radius = width / 2 - 2; // 为边框预留空间
                                var startAngle = 0;
                                var endAngle = progress * 2 * Math.PI;

                                ctx.reset();
                                ctx.beginPath();
                                ctx.arc(centerX, centerY, radius, startAngle, endAngle);
                                ctx.strokeStyle = progressColor;
                                ctx.lineWidth = 3;
                                ctx.stroke();
                            }
                        }

                        Image {
                            anchors.centerIn: parent
                            source: "qrc:/images/sundry_bullet.png"
                            width: 16
                            height: 16
                            fillMode: Image.PreserveAspectFit
                        }
                    }

                    Text {
                        text: "对方占领中 " + root.redOccupationTime + "s"
                        color: "#FFEB3B"
                        font.pixelSize: 10
                        visible: true
                        Layout.alignment: Qt.AlignHCenter
                    }
                }
            }
        }

        // ========================================
        // [右侧] 蓝方区域 (对称镜像)
        // ========================================
        Item {
            id: blueArea
            anchors.left: centerPanel.right
            anchors.leftMargin: 7
            anchors.top: parent.top
            height: parent.height
            width: 800

            Text {
                id: blueTeamNameText
                width: root.teamNameWidth
                anchors.left: blueHealthContainer.left
                anchors.leftMargin: root.teamNameOuterInset
                anchors.top: parent.top
                anchors.topMargin: 0
                text: root.blueTeamName
                color: "#FFFFFF"
                font.pixelSize: 17
                minimumPixelSize: 13
                fontSizeMode: Text.HorizontalFit
                font.bold: true
                horizontalAlignment: Text.AlignLeft
                elide: Text.ElideRight
                style: Text.Outline
                styleColor: "#101214"
                z: 20
            }

            Item {
                id: blueHealthContainer
                width: 470
                height: 56
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.topMargin: 4

                BaseHealthBar {
                    id: blueBar
                    width: 332
                    height: 30
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    currentHealth: root.blueBaseHP
                    maxHealth: root.blueBaseMaxHP
                    isBlue: true
                    invincible: root.blueInvincibleEffective
                }

                OutpostStatusBadge {
                    id: blueOutpostModule
                    visible: root.showOutpostModule
                    width: 92
                    height: 52
                    anchors.left: blueBar.right
                    anchors.leftMargin: 12
                    anchors.verticalCenter: blueBar.verticalCenter
                    anchors.verticalCenterOffset: 0
                    opacity: root.blueOutpostDestroyed ? 0.55 : 1.0
                    z: 12
                    isBlue: true
                    hp: root.blueOutpostHP
                    maxHp: root.blueOutpostMaxHP
                    rebuildCount: root.blueOutpostRebuildCount
                    maxRebuildCount: root.blueOutpostMaxRebuildCount
                }

                Row {
                    anchors.top: blueBar.bottom
                    anchors.topMargin: 5
                    anchors.right: blueBar.right
                    anchors.rightMargin: 20
                    spacing: 4
                    visible: root.blueInvincible || root.blueDefenseBonus > 0
                    layoutDirection: Qt.RightToLeft

                    Image {
                        source: "qrc:/images/buffs/shield.png"
                        width: 16
                        height: 16
                        fillMode: Image.PreserveAspectFit
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: root.blueInvincible ? "护盾激活" : (root.blueDefenseBonus + "%")
                        color: "#EEEEEE"
                        font.pixelSize: 10
                        font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: root.blueInvincible ? "基地状态" : "防御增益"
                        color: "#AAAAAA"
                        font.pixelSize: 10
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                Item {
                    id: blueLogoBadge
                    width: 72
                    height: 72
                    anchors.right: blueBar.left
                    anchors.rightMargin: -28
                    anchors.verticalCenter: blueBar.verticalCenter
                    anchors.verticalCenterOffset: 0
                    z: 14

                    Image {
                        anchors.fill: parent
                        source: root.blueTeamLogoSource
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        mipmap: true
                    }
                }
            }

            // 蓝方底部栏
                RowLayout {
                    anchors.left: parent.left
                    anchors.leftMargin: -38
                    anchors.top: blueHealthContainer.bottom
                    anchors.topMargin: 2
                    spacing: 8

                // 2. 占领提示
                ColumnLayout {
                    spacing: 4
                    visible: root.showOccupationHints && root.isBlueOccupied

                    Item {
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        Layout.alignment: Qt.AlignHCenter

                        // 进度环背景占位圆
                        Rectangle {
                            anchors.fill: parent
                            radius: 16
                            color: "transparent"
                            border.color: "#60333333"    // 半透明边框
                            border.width: 2
                        }

                        Canvas {
                            id: blueProgressCanvas
                            anchors.fill: parent
                            rotation: -90 // 从顶部开始绘制
                            property double progress: root.blueOccupationProgress
                            property color progressColor: "#3399FF" // 蓝方颜色

                            onProgressChanged: requestPaint()

                            onPaint: {
                                var ctx = getContext("2d");
                                var centerX = width / 2;
                                var centerY = height / 2;
                                var radius = width / 2 - 2;
                                var startAngle = 0;
                                var endAngle = progress * 2 * Math.PI;

                                ctx.reset();
                                ctx.beginPath();
                                ctx.arc(centerX, centerY, radius, startAngle, endAngle);
                                ctx.strokeStyle = progressColor;
                                ctx.lineWidth = 3;
                                ctx.stroke();
                            }
                        }

                        Image {
                            anchors.centerIn: parent
                            source: "qrc:/images/sundry_bullet.png"
                            width: 16
                            height: 16
                            fillMode: Image.PreserveAspectFit
                        }
                    }

                    Text {
                        text: "对方占领中 " + root.blueOccupationTime + "s"
                        color: "#FFEB3B"
                        font.pixelSize: 10
                        visible: true
                        Layout.alignment: Qt.AlignHCenter
                    }
                }

                // 1. 机器人列表
                RowLayout {
                    spacing: 2
                    RobotStatusItem {
                        robotId: 101
                        avatarSource: "qrc:/images/robot_avatar.png"
                    }
                    RobotStatusItem {
                        robotId: 103
                        avatarSource: "qrc:/images/robot_avatar.png"
                    }
                    RobotStatusItem {
                        robotId: 104
                        avatarSource: "qrc:/images/robot_avatar.png"
                    }
                    RobotStatusItem {
                        robotId: 106
                        avatarSource: "qrc:/images/robot_avatar.png"
                    }
                    RobotStatusItem {
                        robotId: 107
                        avatarSource: "qrc:/images/robot_avatar.png"
                    }
                }
            }
        }
    }
}
