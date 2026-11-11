// File/layer: app/qml/SettingsConnect.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The connect bar (the status line, the URI field and Connect): one part of SettingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    property alias uriField: uriField
    property alias errorLabel: errorLabel
    // Set where the words are a connect's refusal, which the Log explains
    property bool logExplains: false

    Label {
        objectName: "sessionStatus"
        text: Bridge.connected
              ? qsTr("Connected: %1").arg(Bridge.currentUri)
              : Bridge.connecting ? qsTr("Connecting…")
              : Bridge.disconnecting ? qsTr("Disconnecting…")
              : qsTr("No session")
        color: Theme.linkColor(Bridge.linkState)
        // A long URI breaks anywhere: unwrapped, it widened the
        // page past a phone's edge once connected
        wrapMode: Text.WrapAnywhere
        Layout.fillWidth: true
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing

        ThemedTextField {
            id: uriField
            objectName: "uriField"
            Layout.fillWidth: true
            // ?poll= replaces the profile's PID rotation. Raw on an
            // adapter listens to the engine ECU, so ?poll= alone
            // asks it; under the obd2 default it also decodes.
            // A WiFi adapter is typed: nothing lists it (ADR-0026's
            // appendix "WiFi over TCP")
            placeholderText: qsTr("Address, e.g. mock://demo, elm327://COM5, "
                                  + "elm327://192.168.0.10:35000 or "
                                  + "j2534://Vendor?profile=raw&poll=0x0C")
            enabled: root.host.idle
            color: Theme.text
            // A URI, not prose: a phone's keyboard capitalized
            // "Replay://" into a scheme core doesn't know
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            // Any text the page didn't compose is read back
            onTextChanged: {
                if (!root.host.composing)
                    root.host.readChoices()
            }
            onAccepted: root.host.requestConnect(text)
        }

        ThemedButton {
            objectName: "connectButton"
            text: Bridge.connected ? qsTr("Disconnect")
                : Bridge.connecting ? qsTr("Connecting…")
                : Bridge.disconnecting ? qsTr("Disconnecting…")
                : qsTr("Connect")
            highlighted: !Bridge.connected
            // Disconnect is always offered; Connect only while idle
            // with a URI to connect
            // and no relay holds the adapter (ADR-0047)
            enabled: Bridge.connected
                     || (root.host.idle && uriField.text !== "" && !Relay.running)
            onClicked: {
                if (Bridge.connected) {
                    Bridge.disconnectSession()
                    errorLabel.visible = false
                    root.logExplains = false
                    errorLabel.text = ""
                } else {
                    root.host.requestConnect(uriField.text)
                }
            }
        }
    }

    // A refusal's words, and the Log beside them: the moment it's wanted.
    // The row follows the words: a child's visible reads false in a hidden row
    RowLayout {
        visible: errorLabel.text !== ""
        Layout.fillWidth: true
        spacing: Theme.spacing
        Label {
            id: errorLabel
            objectName: "errorLabel"
            visible: false
            color: Theme.danger
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
            text: ""
        }
        ThemedButton {
            objectName: "errorShowLog"
            visible: root.logExplains
            flat: true
            text: qsTr("Show log")
            Layout.alignment: Qt.AlignTop
            onClicked: root.host.logRequested()
        }
    }
}
