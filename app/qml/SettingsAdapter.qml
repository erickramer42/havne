// File/layer: app/qml/SettingsAdapter.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The adapter list and the presets: one part of SettingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2


    // ---- Adapters: J2534 interfaces and serial ports ----
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing

        Label {
            objectName: "deviceCount"
            text: root.host.devices.length === 0
                  ? qsTr("None found: install a Pass-Thru driver (Windows), plug in "
                         + "or pair an ELM327 adapter, or pick one below")
                  : qsTr("J2534 interfaces: %1, serial ports: %2%3; select one to "
                         + "put its URI in the field above, then Connect")
                        .arg(root.host.j2534Devices.length).arg(root.host.serialPorts.length)
                        .arg((root.host.canInterfaces.length > 0
                              ? qsTr(", CAN interfaces: %1").arg(root.host.canInterfaces.length)
                              : "")
                             + (Bluetooth.available || root.host.bluetoothAdapters.length > 0
                                ? qsTr(", Bluetooth: %1").arg(root.host.bluetoothAdapters.length)
                                : ""))
            color: root.host.devices.length === 0 ? Theme.textDim : Theme.text
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        ThemedButton {
            text: qsTr("Refresh")
            onClicked: {
                root.host.refreshDevices()
                Bluetooth.refresh(true)
            }
        }
    }

    // ---- Bluetooth (Android): why the list is short, and a
    // scan for BLE adapters, which aren't paired ----
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing
        visible: Bluetooth.available

        Label {
            objectName: "bluetoothStatus"
            text: Bluetooth.status !== ""
                  ? Bluetooth.status
                  : qsTr("Paired adapters are listed; pair one in Android's "
                         + "Bluetooth settings")
            color: Bluetooth.statusWarns ? Theme.warning : Theme.textDim
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        ThemedButton {
            objectName: "bleScanButton"
            text: Bluetooth.scanning ? qsTr("Scanning…") : qsTr("Scan for BLE")
            enabled: root.host.idle && !Bluetooth.scanning
            onClicked: Bluetooth.scan()
        }
    }

    // Sized to its rows up to deviceRowsShown, then it scrolls
    ListView {
        id: deviceList
        objectName: "deviceList"
        // No current row: ListView gives a current one a default
        // highlight, which animates to its size and, after the
        // section opened at a phone's width, lay past the edge
        currentIndex: -1
        highlightFollowsCurrentItem: false
        Layout.fillWidth: true
        Layout.preferredHeight: contentHeight
        Layout.maximumHeight: root.host.deviceRowsShown * (root.host.deviceRowHeight + spacing) - spacing
        visible: count > 0
        clip: true
        spacing: 2
        boundsBehavior: Flickable.StopAtBounds
        model: root.host.devices

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        delegate: Rectangle {
            id: row
            required property var modelData
            width: deviceList.width
            height: root.host.deviceRowHeight
            radius: Theme.radius
            color: mouse.containsMouse ? Theme.elevatedLight : Theme.elevated
            border.width: root.host.isChosen(row.modelData.base) ? 2 : 0
            border.color: Theme.primary

            MouseArea {
                id: mouse
                objectName: "deviceRowArea"
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                enabled: root.host.idle
                onClicked: {
                    root.host.fillUri(row.modelData.base)
                    root.host.showUriField()
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spacing
                spacing: 2

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    Label {
                        objectName: "deviceTitle"
                        text: row.modelData.title
                        color: root.host.idle ? Theme.text : Theme.textDim
                    }
                    Label {
                        objectName: "deviceVersions"
                        visible: text !== ""
                        text: row.modelData.versions === ""
                              ? "" : qsTr("last seen: %1").arg(row.modelData.versions)
                        color: Theme.textDim
                        font.pointSize: 8
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignRight
                        rightPadding: Theme.spacing   // clear of the list's scroll bar
                        Layout.fillWidth: true
                    }
                }
                Label {
                    objectName: "deviceDetail"
                    text: row.modelData.detail
                    color: Theme.textDim
                    font.pointSize: 8
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }
            }
        }
    }

    // ---- Relay (the phone, developer mode; ADR-0047): the
    // selected Bluetooth adapter relayed to a computer over USB,
    // for havne-cli there ----
    ColumnLayout {
        objectName: "relayBox"
        visible: Relay.available && Theme.developerMode
        Layout.fillWidth: true
        spacing: Theme.spacing

        Label {
            objectName: "relayStatus"
            text: Relay.running
                  ? Relay.status
                  : qsTr("Relay to a computer over USB: select a paired adapter above, "
                         + "Relay, then on the computer adb forward tcp:%1 tcp:%1 "
                         + "and havne-cli on elm327://127.0.0.1:%1")
                        .arg(Relay.defaultPort)
            color: Relay.running ? Theme.text : Theme.textDim
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Label {
                objectName: "relayTraffic"
                visible: Relay.running
                text: qsTr("%1 from the adapter, %2 to it")
                      .arg(root.host.byteText(Relay.bytesIn))
                      .arg(root.host.byteText(Relay.bytesOut))
                color: Theme.textDim
                // Beside the button on a phone's width
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            ThemedButton {
                objectName: "relayButton"
                text: Relay.running ? qsTr("Stop relaying") : qsTr("Relay")
                // Not beside a session: the adapter takes one link
                enabled: Relay.running
                         || (root.host.idle && /^elm327:\/\/bt:/i.test(root.host.uriField.text))
                onClicked: {
                    if (Relay.running) {
                        Relay.stop()
                        return
                    }
                    const why = Relay.start(root.host.uriField.text)
                    root.host.errorLabel.text = why
                    root.host.errorLabel.visible = why !== ""
                }
            }
        }
    }

    // ---- Presets: the URI's base, for what the list doesn't show ----
    LabeledFlow {
        label: qsTr("Not listed:")
        Repeater {
            model: [
                { name: "mock", uri: "mock://demo", label: "mock://demo" },
                // A bare j2534:// opens the first discovered device
                { name: "j2534", uri: "j2534://", label: qsTr("j2534:// (first device)") },
                // elm327:// takes no first port: the user picks one
                // from the list above or types a WiFi adapter's
                // address (core's refusal says so)
                { name: "elm327", uri: "elm327://", label: qsTr("elm327:// (a port above, or an address)") },
                // WiFi adapters aren't listed (ADR-0026's appendix
                // "WiFi over TCP"): the two common addresses, so a
                // phone needn't type one
                { name: "wifi_obdx", uri: "elm327://192.168.4.1:23",
                  label: qsTr("WiFi: OBDX (192.168.4.1:23)") },
                { name: "wifi_elm", uri: "elm327://192.168.0.10:35000",
                  label: qsTr("WiFi: most others (192.168.0.10:35000)") }
            ]
            ThemedButton {
                required property var modelData
                objectName: "preset_" + modelData.name
                text: modelData.label
                // Wraps rather than run past a narrow page's edge
                // (Linux's font at 320 px did)
                width: Math.min(implicitWidth, parent.width)
                // The mock is a developer's tool, not a device
                visible: modelData.name !== "mock" || Theme.developerMode
                enabled: root.host.idle
                onClicked: {
                    root.host.fillUri(modelData.uri)
                    root.host.showUriField()
                }
            }
        }
    }
}
