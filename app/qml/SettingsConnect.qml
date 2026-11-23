// File/layer: app/qml/SettingsConnect.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone_* (test_qml)
// The connect bar (the status line, the URI field and Connect) and the keep-awake switch: one part of SettingsPage, which holds the state

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
        textFormat: Text.PlainText
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
            enabled: root.host.ownPart.idle
            color: Theme.text
            // A URI, not prose: a phone's keyboard capitalized
            // "Replay://" into a scheme core doesn't know
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            // Any text the page didn't compose is read back
            onTextChanged: {
                if (!root.host.uriPart.composing)
                    root.host.uriPart.readChoices()
            }
            onAccepted: root.host.connectFromPart.requestConnect(text)
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
                     || (root.host.ownPart.idle && uriField.text !== "" && !Relay.running)
            onClicked: {
                if (Bridge.connected) {
                    Bridge.disconnectSession()
                    errorLabel.visible = false
                    root.logExplains = false
                    errorLabel.text = ""
                } else {
                    root.host.connectFromPart.requestConnect(uriField.text)
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
            textFormat: Text.PlainText
            id: errorLabel
            objectName: "errorLabel"
            visible: false
            color: Theme.danger
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            text: ""
        }
        ThemedButton {
            objectName: "errorShowLog"
            visible: root.logExplains
            flat: true
            text: qsTr("Show activity")
            Layout.alignment: Qt.AlignTop
            onClicked: root.host.logRequested()
        }
    }

    // Keep-awake (ADR-0033's appendix "the host kept awake while a
    // session reads"): on by default and kept between runs; it changes
    // during a session too (off releases at once, on holds from the next
    // connect). On a phone it gates the session service's wake lock,
    // which a relay holds whatever it says
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0
        ThemedCheckBox {
            id: awakeBox
            objectName: "keepAwakeBox"
            Layout.fillWidth: true
            text: Theme.mobile ? qsTr("Keep the phone awake while connected")
                               : qsTr("Keep the computer awake while connected")
            checked: Bridge.keepAwake
            onToggled: {
                Bridge.keepAwake = checked
                checked = Qt.binding(() => Bridge.keepAwake)
            }
            contentItem: Label {
                textFormat: Text.PlainText
                text: awakeBox.text
                color: Theme.text
                wrapMode: Text.Wrap
                leftPadding: awakeBox.indicator.width + awakeBox.spacing
                verticalAlignment: Text.AlignVCenter
            }
        }
        Label {
            textFormat: Text.PlainText
            objectName: "keepAwakeNote"
            text: !Theme.mobile ? qsTr("For an adapter. A closed lid still sleeps it.")
                  : Relay.running ? qsTr("A relay keeps the phone awake whatever this says.")
                  : qsTr("Reads on with the screen off")
            color: Theme.textDim
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }
}
