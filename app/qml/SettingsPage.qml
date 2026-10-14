// File/layer: app/qml/SettingsPage.qml | Qt6 Quick, UI layer
// Testing: manual smoke — discovery list renders (Windows: real registry;
//          macOS/Linux: empty-state hint), picker row fills URI field,
//          manual URI connect/disconnect cycle, error label on bad URI

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

Pane {
    id: page
    background: Rectangle { color: Theme.bg }

    // Discovered Pass-Thru interfaces: [{ vendorName, dllPath }, ...].
    // Empty on non-Windows or when no drivers are installed.
    property var j2534Devices: []

    function refreshDevices() {
        j2534Devices = QtTune.discoverJ2534Devices()
    }

    function attemptConnect(uri) {
        uriField.text = uri
        const err = QtTune.connectSession(uri)
        errorLabel.text = err
        if (err === "")
            errorLabel.visible = false
    }

    Component.onCompleted: refreshDevices()

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
                  : qsTr("Not connected")
            color: QtTune.connected ? Theme.success : Theme.textDim
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            TextField {
                id: uriField
                Layout.fillWidth: true
                placeholderText: qsTr("URI — e.g. mock://demo or j2534://Vendor?baud=250000")
                enabled: !QtTune.connected && QtTune.coreInitialized
                color: Theme.text
                onAccepted: page.attemptConnect(text)
            }

            Button {
                text: QtTune.connected ? qsTr("Disconnect") : qsTr("Connect")
                enabled: QtTune.connected
                         ? true
                         : (QtTune.coreInitialized && uriField.text !== "")
                background: Rectangle {
                    radius: Theme.radius
                    color: QtTune.connected ? Theme.elevated : Theme.primary
                }
                onClicked: {
                    if (QtTune.connected) {
                        QtTune.disconnectSession()
                        errorLabel.visible = false
                    } else {
                        page.attemptConnect(uriField.text)
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
                    text: modelData === "" ? qsTr("j2534:// (first device)") : modelData
                    enabled: !QtTune.connected
                    flat: true
                    contentItem: Label {
                        text: parent.text
                        color: Theme.textDim
                    }
                    onClicked: uriField.text = modelData
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
                             + "(Windows) or enter a direct path below")
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
                    onClicked: page.attemptConnect("j2534://" + row.modelData.vendorName)
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacing
                    spacing: 2

                    Label {
                        text: row.modelData.vendorName
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
    }
}
