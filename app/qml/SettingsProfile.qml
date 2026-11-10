// File/layer: app/qml/SettingsProfile.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The profile buttons (what to read): one part of SettingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    property alias didField: didField

    // ---- Profile: the URI's query (ADR-0013, ADR-0020) ----
    // The chosen button is drawn as the primary color
    LabeledFlow {
        label: qsTr("Profile:")
        Repeater {
            model: root.host.profiles
            ThemedButton {
                required property var modelData
                objectName: "profile_" + modelData.name
                text: modelData.label
                checkable: true
                checked: root.host.profileChoice === modelData.name
                enabled: root.host.idle
                onClicked: root.host.chooseProfile(modelData.name)
            }
        }
    }

    Label {
        objectName: "profileHint"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        text: root.host.profileHint()
    }

    RowLayout {
        objectName: "didRow"
        Layout.fillWidth: true
        spacing: Theme.spacing
        visible: root.host.profileChoice === "uds-generic"
        Label { text: qsTr("DIDs:"); color: Theme.textDim }
        ThemedTextField {
            id: didField
            objectName: "didField"
            Layout.fillWidth: true
            // The ?did= grammar as core reads it (ADR-0020); the
            // text goes into the URI as typed, so core's words
            // name any mistake. The validator admits only the
            // grammar's characters, so a query metacharacter (&,
            // =, ?, #) can't split the URI into other keys.
            placeholderText: qsTr("1E1C:u16, F190:text, 7E9:1E1C, 1E00-1EFF; "
                                  + "[ecu:]DID[-DID][:hex|text|u8…u32|i8…i32]")
            validator: RegularExpressionValidator { regularExpression: /[0-9A-Za-z:,\-\s]*/ }
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            enabled: root.host.idle
            color: Theme.text
            onTextChanged: {
                if (!root.host.syncing && root.host.profileChoice === "uds-generic")
                    root.host.recompose()
            }
            onAccepted: root.host.requestConnect(root.host.uriField.text)
        }
    }
}
