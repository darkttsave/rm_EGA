import QtQuick 2.15

// 哨兵 0x0310 4 字节自定义消息的【临时】诊断组件。
//
// 只负责渲染 GameData.sentryStatusPreview 提供的字段名与原始数值：
// - 字段名与顺序来自状态层（最终源自 tools/protocol_specs/sentry_bt_status_v1.json），
//   因此本文件不需要重复任何协议定义；
// - 不解释、也不显示进攻/防守/撤退/待命等未经确认的业务语义；
// - 无声音、无动画、无按钮、无控制动作。
//
// 显隐跟随 GunnerOverlay（即 GameData.customUIEnabled / Ctrl+G）。
// 尺寸使用固定两档（未收到时更矮），避免依赖 Positioner 与 Repeater 的隐式尺寸传播。
Rectangle {
    id: root
    objectName: "sentryStatusPreview"

    width: 176
    height: root.hasData ? 96 : 46

    color: "#CC1A1A1A"
    radius: 5
    border.color: "#55FFAA33"
    border.width: 1

    // 由 GunnerOverlay 注入 GameData.sentryStatusPreview。
    property var sentryData: ({})
    property bool hasData: root.sentryData.received === true
    property var fieldRows: (root.hasData && root.sentryData.fields !== undefined)
                            ? root.sentryData.fields : []

    Column {
        id: content
        anchors.centerIn: parent
        spacing: 3

        Text {
            text: "SENTRY MSG (TEMP)"
            color: "#FFAA33"
            font.pixelSize: 10
            font.bold: true
        }

        Text {
            visible: !root.hasData
            text: "未收到"
            color: "#888888"
            font.pixelSize: 10
        }

        Repeater {
            // 与仓库既有写法一致：先把模型规范成真正的 JS 数组再交给 Repeater。
            model: Array.isArray(root.fieldRows) ? root.fieldRows.slice(0)
                                                 : root.fieldRows

            delegate: Row {
                id: fieldRow
                objectName: "sentryFieldRow"
                // 通过 delegate 的 id 限定访问，保持仓库 qmllint 零 unqualified 基线。
                required property var modelData

                spacing: 6

                Text {
                    width: 100
                    text: fieldRow.modelData.name
                    color: "#00CED1"
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }

                Text {
                    text: String(fieldRow.modelData.value)
                    color: "#FFFFFF"
                    font.pixelSize: 10
                    font.bold: true
                }
            }
        }

        Text {
            objectName: "sentryRawHex"
            visible: root.hasData
            text: "hex " + (root.sentryData.rawHex !== undefined
                            ? root.sentryData.rawHex : "-")
            color: "#999999"
            font.pixelSize: 8
        }
    }
}
