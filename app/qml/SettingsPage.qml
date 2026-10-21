// File/layer: app/qml/SettingsPage.qml | Qt6 Quick, UI layer
// Testing: manual smoke — discovery list renders (Windows: real registry;
//          macOS/Linux: empty-state hint), picker row fills URI field only,
//          Connect button drives connectSessionAsync (UI stays responsive
//          while a vendor blocks ~8 s), activity log shows connect lifecycle,
//          error label on bad URI, fault states tint the status label

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

Pane {
    id: page
    background: Rectangle { color: Theme.bg }

    readonly property int maxLogLines: 200

    // Discovered Pass-Thru interfaces: [{ vendorName, dllPath,
    // architecture }, ...] with architecture "x86", "x64" or "unknown".
    // Empty on non-Windows or when no drivers are installed.
    property var j2534Devices: []

    function refreshDevices() {
        j2534Devices = QtTune.discoverJ2534Devices()
    }

    // Row click / preset click: selection is INTENT, never action. A row
    // that only fills the URI field can never accidentally cost 8 seconds.
    function fillUri(uri) {
        uriField.text = uri
    }

    function requestConnect(uri) {
        if (uri === "")
            return
        QtTune.connectSessionAsync(uri)
        // Connecting/Connected/failed lines arrive via logMessage.
    }

    function appendLog(message) {
        activityLog.append(message)
        // Bound the log: QML TextArea.append keeps growing otherwise.
        // Removes whole oldest LINES (split on "\n") — NOT visual
        // rows. lineCount counts wrapped rows, so a single long
        // wrapped message can exceed maxLogLines while the text has
        // no newline at all; indexOf("\n") then returns -1 and
        // remove(0, 0) removes nothing, spinning forever (the old
        // bug). The guard breaks the loop instead: such a message
        // stays until a newline-bearing line can be trimmed.
        while (activityLog.lineCount > page.maxLogLines) {
            const nl = activityLog.text.indexOf("\n")
            if (nl < 0)
                break
            activityLog.remove(0, nl + 1)
        }
        activityLog.cursorPosition = activityLog.length
    }

    Component.onCompleted: refreshDevices()

    Connections {
        target: QtTune

        function onLogMessage(message) {
            page.appendLog(message)
        }

        function onConnectResult(uri, error) {
            errorLabel.text = error
            errorLabel.visible = (error !== "")
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing * 2
        spacing: Theme.spacing * 2

        // ---- Connection ----
        Label {
            text: qsTr("Connection")
            color: Theme.text
            font.bold: true
        }

        Label {
            text: QtTune.connected
                  ? qsTr("Connected: %1").arg(QtTune.currentUri)
                  : (QtTune.connecting
                       ? qsTr("Connecting…")
                       : qsTr("Not connected"))
            color: Theme.linkColor(QtTune.linkState)
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            TextField {
                id: uriField
                Layout.fillWidth: true
                // ?poll= replaces the profile's PID rotation (finding
                // 12) and needs a channel. The raw profile declares
                // none, so the example adds ?filter=; under the obd2
                // default ?poll= alone works and still decodes.
                placeholderText: qsTr("URI — e.g. mock://demo or j2534://Vendor?profile=raw&filter=0x7E8&poll=0x0C")
                enabled: !QtTune.connected && !QtTune.connecting && QtTune.coreInitialized
                color: Theme.text
                onAccepted: page.requestConnect(text)
            }

            Button {
                text: QtTune.connected
                      ? qsTr("Disconnect")
                      : (QtTune.connecting ? qsTr("Connecting…") : qsTr("Connect"))
                // Disabled while an attempt is in flight — the bridge
                // guards double-starts anyway, but the UI must not
                // offer them during a multi-second vendor block.
                enabled: QtTune.connected
                         ? true
                         : (!QtTune.connecting
                            && QtTune.coreInitialized
                            && uriField.text !== "")
                background: Rectangle {
                    radius: Theme.radius
                    color: QtTune.connected ? Theme.elevated : Theme.primary
                }
                onClicked: {
                    if (QtTune.connected) {
                        QtTune.disconnectSession()
                        errorLabel.visible = false
                    } else {
                        page.requestConnect(uriField.text)
                    }
                }
            }
        }

        Label {
            id: errorLabel
            visible: false
            color: Theme.danger
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
            text: ""
        }

        // ---- Presets ----
        RowLayout {
            spacing: Theme.spacing
            Label { text: qsTr("Presets:"); color: Theme.textDim }
            Repeater {
                model: ["mock://demo", "j2534://"]
                Button {
                    required property string modelData
                    text: modelData === ""
                          ? qsTr("j2534:// (first device)")
                          : modelData
                    enabled: !QtTune.connected && !QtTune.connecting
                    onClicked: page.fillUri(modelData)
                    background: Rectangle {
                        radius: Theme.radius
                        color: parent.enabled ? Theme.elevatedLight : Qt.darker(Theme.elevatedLight, 1.2)
                    }
                }
            }
        }

        // ---- J2534 interfaces ----
        Label {
            text: qsTr("J2534 Pass-Thru Interfaces")
            color: Theme.text
            font.bold: true
        }

        RowLayout {
            spacing: Theme.spacing
            Label {
                text: page.j2534Devices.length === 0
                      ? qsTr("None discovered — install a Pass-Thru driver "
                             + "(Windows) or enter a direct path above")
                      : qsTr("%1 found").arg(page.j2534Devices.length)
                color: page.j2534Devices.length === 0 ? Theme.textDim : Theme.text
                Layout.fillWidth: true
            }
            Button {
                text: qsTr("Refresh")
                onClicked: page.refreshDevices()
            }
        }

        ListView {
            id: deviceList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: page.j2534Devices

            delegate: Rectangle {
                id: row
                required property var modelData
                width: deviceList.width
                height: 56
                radius: Theme.radius
                color: mouse.containsMouse ? Theme.elevatedLight : Theme.elevated

                MouseArea {
                    id: mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    enabled: !QtTune.connected
                    onClicked: page.fillUri("j2534://" + row.modelData.vendorName)
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacing
                    spacing: 2

                    Label {
                        text: row.modelData.vendorName
                              + (row.modelData.architecture
                                   ? "  (" + row.modelData.architecture + ")"
                                   : "")
                        color: QtTune.connected ? Theme.textDim : Theme.text
                    }
                    Label {
                        text: row.modelData.dllPath
                        color: Theme.textDim
                        font.pointSize: 8
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                }
            }
        }

        // ---- Activity log ----
        Label {
            text: qsTr("Activity")
            color: Theme.text
            font.bold: true
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 140

            TextArea {
                id: activityLog
                readOnly: true
                wrapMode: TextArea.WrapAnywhere
                font.pointSize: 8
                color: Theme.textDim
                background: Rectangle {
                    radius: Theme.radius
                    color: Theme.elevated
                }
                text: qsTr("Ready.")
            }
        }
    }
}
