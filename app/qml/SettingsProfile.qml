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
    // On the tab-bar form the three common profiles are one segmented row
    // (a closed set, a tap takes effect), the others buttons under it
    readonly property var segmentNames: ["default", "raw", "uds-generic"]
    readonly property var segmentWords: ({ "default": qsTr("Default"), "raw": qsTr("Raw"),
                                           "uds-generic": qsTr("UDS DIDs") })
    readonly property var segmented: Theme.compact
        ? root.host.profiles.filter(p => segmentNames.indexOf(p.name) >= 0) : []
    readonly property var loose: Theme.compact
        ? root.host.profiles.filter(p => segmentNames.indexOf(p.name) < 0) : root.host.profiles

    Label {
        visible: root.segmented.length > 0
        text: qsTr("Profile:")
        color: Theme.textDim
    }
    SegmentRow {
        objectName: "profileSegments"
        visible: root.segmented.length > 0
        Layout.fillWidth: true
        Repeater {
            model: root.segmented
            SegmentButton {
                required property var modelData
                objectName: "profile_" + modelData.name
                share: true
                tapHeight: Theme.touchTarget
                labelSize: Theme.fontBody
                text: root.segmentWords[modelData.name]
                Accessible.name: modelData.label
                selected: root.host.profileChoice === modelData.name
                checked: selected
                enabled: root.host.idle
                onClicked: root.host.chooseProfile(modelData.name)
            }
        }
    }
    LabeledFlow {
        visible: root.loose.length > 0
        label: root.segmented.length > 0 ? "" : qsTr("Profile:")
        Repeater {
            model: root.loose
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

    ThemedCheckBox {
        id: extBox
        objectName: "extSessionBox"
        Layout.fillWidth: true
        visible: root.host.profileChoice === "uds-generic" && root.host.sessionOffered(root.host.uriBase())
        text: qsTr("Extended session (10 03)")
        checked: root.host.extSession
        enabled: root.host.idle
        onToggled: {
            root.host.extSession = checked
            checked = Qt.binding(() => root.host.extSession)
            root.host.recompose()
        }
        contentItem: Label {
            text: extBox.text
            color: extBox.enabled ? Theme.text : Theme.textDim
            leftPadding: extBox.indicator.width + extBox.spacing
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
        }
    }
    Label {
        objectName: "optionsNote"
        Layout.fillWidth: true
        visible: root.host.profileChoice === "uds-generic"
        wrapMode: Text.WordWrap
        color: Theme.textDim
        text: qsTr("More options go after ?: see the address reference")
    }
}
