// File/layer: app/qml/UnlockDialog.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.unlock (test_qml)
// The question before one unlock of a module the plugin talks to: core's
// words on the engine's state and on the lockout, shown in full, the level
// where the module declares more than one, and the word itself (ADR-0066).
// Drawn in place on the Diagnostics page, not as a Popup, which would not
// scale with the zoom

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

Rectangle {
    id: root
    // The module's conversation entry (Bridge.consent.conversations) and
    // core's two lines: [{ key, heading, text }]
    property var conversation: ({})
    property var lines: []
    // An unlock asked and not yet answered, and a refusal's words
    property bool asking: false
    property string note: ""
    signal unlockChosen(int level)
    signal cancelChosen()

    readonly property var levels: {
        const out = []
        for (const l of (conversation.levels ?? [])) {
            if (out.indexOf(l.level) < 0)
                out.push(l.level)
        }
        return out
    }
    // The level the user picked; the only one, or the first, until then
    property int picked: 0
    readonly property int level: levels.indexOf(picked) >= 0 ? picked : (levels.length > 0 ? levels[0] : 0)
    function hex(n) { return "0x" + n.toString(16).toUpperCase().padStart(2, "0") }

    implicitHeight: body.implicitHeight + 2 * Theme.pageMargin
    radius: Theme.radius
    // The buttons are Theme.elevated: a panel of that color would hide them
    color: Theme.surface
    border.width: 1
    border.color: Theme.warning

    ColumnLayout {
        id: body
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: 10

        Label {
            textFormat: Text.PlainText
            objectName: "unlockTitle"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Unlock ECU %1?").arg(root.conversation.ecu ?? "")
            font.weight: Font.DemiBold
            color: Theme.text
        }
        Repeater {
            model: root.lines
            ColumnLayout {
                id: line
                required property var modelData
                objectName: "unlockLine_" + modelData.key
                Layout.fillWidth: true
                spacing: 0
                Label {
                    textFormat: Text.PlainText
                    visible: line.modelData.heading !== ""
                    text: line.modelData.heading
                    font.weight: Font.DemiBold
                    color: Theme.text
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
                Label {
                    textFormat: Text.PlainText
                    objectName: "unlockLineText"
                    text: line.modelData.text
                    color: Theme.text
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
            }
        }
        // A choice only where the module declares more than one level
        Flow {
            objectName: "unlockLevels"
            visible: root.levels.length > 1
            Layout.fillWidth: true
            spacing: Theme.spacing
            Repeater {
                model: root.levels
                ThemedButton {
                    id: choice
                    required property int modelData
                    objectName: "unlockLevel_" + modelData
                    checkable: true
                    checked: root.level === modelData
                    text: qsTr("Level %1").arg(root.hex(modelData))
                    enabled: !root.asking
                    onClicked: root.picked = modelData
                }
            }
        }
        Label {
            textFormat: Text.PlainText
            objectName: "unlockNote"
            visible: text !== ""
            text: root.asking ? qsTr("Asking the module...") : root.note
            color: Theme.warning
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.minimumWidth: 0
        }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.spacing
            ThemedButton {
                objectName: "unlockConfirm"
                text: qsTr("Unlock this module")
                enabled: !root.asking && root.level > 0
                onClicked: root.unlockChosen(root.level)
            }
            ThemedButton {
                objectName: "unlockCancel"
                text: qsTr("Cancel")
                onClicked: root.cancelChosen()
            }
        }
    }
}
