// File/layer: app/qml/SettingsPage.qml | Qt6 Quick, UI layer
// Testing: manual smoke — discovery list renders (Windows: real registry;
//          macOS/Linux: empty-state hint) sized to its rows, picker row
//          fills URI field only, Connect button drives connectSessionAsync
//          (UI stays responsive while a vendor blocks ~8 s), activity log
//          shows connect lifecycle and takes the remaining height, error
//          label on bad URI, Record sessions fixed during a session and
//          its folder shown relative to Documents; at 1280 × 650 the page
//          scrolls and the activity log keeps its minimum height;
//          the Display row steps Theme.zoom. The recording's
//          bridge side: BridgeTest.Recording*. The status label and the
//          activity log's link lines in each link state: test_qml
//          (tests/qml/tst_linkhealth.qml), by objectName.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

Pane {
    id: page
    padding: 0
    background: Rectangle { color: Theme.bg }

    readonly property int maxLogLines: 200
    readonly property int deviceRowHeight: 56
    readonly property int deviceRowsShown: 4   // the list scrolls past this

    // Discovered Pass-Thru interfaces: [{ vendorName, dllPath,
    // architecture }, ...] with architecture "x86", "x64" or "unknown".
    // Empty on non-Windows or when no drivers are installed.
    property var j2534Devices: []

    component Heading: Label {
        color: Theme.text
        font.pixelSize: 16
        font.weight: Font.DemiBold
        Layout.topMargin: Theme.spacing
    }

    function refreshDevices() {
        j2534Devices = Bridge.discoverJ2534Devices()
    }

    // Row click / preset click: selection is INTENT, never action. A row
    // that only fills the URI field can never accidentally cost 8 seconds.
    function fillUri(uri) {
        uriField.text = uri
    }

    function requestConnect(uri) {
        if (uri === "")
            return
        Bridge.connectSessionAsync(uri)
        // Connecting/Connected/failed lines arrive via logMessage.
    }

    // Each line is stamped with local time in the surrogate session
    // log's format (HH:MM:SS.mmm), so the two line up, and a disconnect
    // or a recovery can be timed from here.
    function appendLog(message) {
        activityLog.append(Qt.formatTime(new Date(), "hh:mm:ss.zzz") + "  " + message)
        // Bound the log: QML TextArea.append keeps growing otherwise.
        // Removes whole oldest LINES (split on "\n") — NOT visual
        // rows. lineCount counts wrapped rows, so a single long
        // wrapped message can exceed maxLogLines while the text has
        // no newline at all; indexOf("\n") then returns -1, and
        // remove(0, 0) would remove nothing and spin forever. The
        // guard breaks the loop instead: such a message stays until a
        // newline-bearing line can be trimmed.
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
        target: Bridge

        function onLogMessage(message) {
            page.appendLog(message)
        }

        function onConnectResult(uri, error) {
            errorLabel.text = error
            errorLabel.visible = (error !== "")
        }
    }

    // Scrolls when the content doesn't fit (a short laptop screen);
    // otherwise the activity log takes the height that's left
    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.height + 2 * Theme.pageMargin
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {
            policy: flick.contentHeight > flick.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
        }

        ColumnLayout {
            id: column
            x: Theme.pageMargin
            y: Theme.pageMargin
            width: flick.width - 2 * Theme.pageMargin
            // At least the page, so the activity log takes what's left;
            // taller when the content needs it, and the page scrolls
            height: Math.max(implicitHeight, flick.height - 2 * Theme.pageMargin)
            spacing: Theme.spacing * 2

            PageHeader { title: qsTr("Settings") }

            // ---- Connection ----
            Heading { text: qsTr("Connection") }

            Label {
                objectName: "sessionStatus"
                text: Bridge.connected
                      ? qsTr("Connected: %1").arg(Bridge.currentUri)
                      : Bridge.connecting ? qsTr("Connecting…")
                      : Bridge.disconnecting ? qsTr("Disconnecting…")
                      : qsTr("Not connected")
                color: Theme.linkColor(Bridge.linkState)
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing

                TextField {
                    id: uriField
                    Layout.fillWidth: true
                    // ?poll= replaces the profile's PID rotation and needs
                    // a channel. The raw profile declares none, so the
                    // example adds ?filter=; under the obd2 default ?poll=
                    // alone works and still decodes.
                    placeholderText: qsTr("URI — e.g. mock://demo or j2534://Vendor?profile=raw&filter=0x7E8&poll=0x0C")
                    enabled: !Bridge.connected && !Bridge.connecting && !Bridge.disconnecting
                             && Bridge.coreInitialized
                    color: Theme.text
                    onAccepted: page.requestConnect(text)
                }

                ThemedButton {
                    text: Bridge.connected ? qsTr("Disconnect")
                        : Bridge.connecting ? qsTr("Connecting…")
                        : Bridge.disconnecting ? qsTr("Disconnecting…")
                        : qsTr("Connect")
                    highlighted: !Bridge.connected
                    // Disabled while a connect or a teardown is in flight —
                    // the bridge refuses both anyway, but the UI must not
                    // offer them during a multi-second vendor block.
                    enabled: Bridge.connected
                             ? true
                             : (!Bridge.connecting
                                && !Bridge.disconnecting
                                && Bridge.coreInitialized
                                && uriField.text !== "")
                    onClicked: {
                        if (Bridge.connected) {
                            Bridge.disconnectSession()
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
                    ThemedButton {
                        required property string modelData
                        // A bare j2534:// opens the first discovered device
                        text: modelData === "j2534://"
                              ? qsTr("j2534:// (first device)")
                              : modelData
                        enabled: !Bridge.connected && !Bridge.connecting && !Bridge.disconnecting
                        onClicked: page.fillUri(modelData)
                    }
                }
            }

            // ---- Recording (ADR-0017) ----
            // Off by default. It takes effect at the next connect, so it's
            // fixed while a session is up and each recording covers a whole
            // session.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing

                CheckBox {
                    id: recordBox
                    text: qsTr("Record sessions")
                    checked: Bridge.recording
                    enabled: !Bridge.connected && !Bridge.connecting && !Bridge.disconnecting
                    // The bridge decides; the binding comes back either way
                    onToggled: {
                        Bridge.recording = checked
                        checked = Qt.binding(() => Bridge.recording)
                    }
                    contentItem: Label {
                        text: recordBox.text
                        color: recordBox.enabled ? Theme.text : Theme.textDim
                        leftPadding: recordBox.indicator.width + recordBox.spacing
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    color: Bridge.recordingActive ? Theme.success : Theme.textDim
                    text: Bridge.recordingActive
                          ? qsTr("Recording to %1").arg(Bridge.recordingFolder)
                          : Bridge.recordingFolder !== ""
                            ? qsTr("Last: %1").arg(Bridge.recordingFolder)
                          : Bridge.recording
                            ? qsTr("Frames, values and events, from the next connect")
                          : ""
                }
                ThemedButton {
                    text: qsTr("Open folder")
                    visible: Bridge.recordingFolder !== ""
                    onClicked: Qt.openUrlExternally(Bridge.recordingFolderUrl)
                }
            }

            // ---- Display ----
            // The whole UI's zoom; Main.qml applies and stores it
            RowLayout {
                spacing: Theme.spacing
                Label { text: qsTr("Display size:"); color: Theme.textDim }
                ThemedButton {
                    text: "−"
                    Accessible.name: qsTr("Smaller")
                    enabled: Theme.zoom > Theme.zoomSteps[0] + 0.001
                    onClicked: Theme.stepZoom(-1)
                }
                Label {
                    Layout.minimumWidth: 52
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("%1 %").arg(Math.round(Theme.zoom * 100))
                    color: Theme.text
                }
                ThemedButton {
                    text: "+"
                    Accessible.name: qsTr("Larger")
                    enabled: Theme.zoom < Theme.zoomSteps[Theme.zoomSteps.length - 1] - 0.001
                    onClicked: Theme.stepZoom(1)
                }
                ThemedButton {
                    text: qsTr("Reset")
                    enabled: Math.abs(Theme.zoom - 1) > 0.001
                    onClicked: Theme.zoom = 1
                }
                Label {
                    text: qsTr("or Ctrl + / Ctrl − / Ctrl 0")
                    color: Theme.textDim
                }
            }

            // ---- J2534 interfaces ----
            Heading { text: qsTr("J2534 Pass-Thru Interfaces") }

            RowLayout {
                spacing: Theme.spacing
                Label {
                    text: page.j2534Devices.length === 0
                          ? qsTr("None discovered — install a Pass-Thru driver "
                                 + "(Windows) or enter a direct path above")
                          : qsTr("%1 found — select one to put its URI in the field above, "
                                 + "then Connect").arg(page.j2534Devices.length)
                    color: page.j2534Devices.length === 0 ? Theme.textDim : Theme.text
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                ThemedButton {
                    text: qsTr("Refresh")
                    onClicked: page.refreshDevices()
                }
            }

            // Sized to its rows up to deviceRowsShown, then it scrolls; the
            // activity log takes the rest of the page
            ListView {
                id: deviceList
                Layout.fillWidth: true
                Layout.preferredHeight: contentHeight
                Layout.maximumHeight: page.deviceRowsShown * (page.deviceRowHeight + spacing) - spacing
                visible: count > 0
                clip: true
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds
                model: page.j2534Devices

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Rectangle {
                    id: row
                    required property var modelData
                    width: deviceList.width
                    height: page.deviceRowHeight
                    radius: Theme.radius
                    color: mouse.containsMouse ? Theme.elevatedLight : Theme.elevated

                    MouseArea {
                        id: mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        enabled: !Bridge.connected
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
                            color: Bridge.connected ? Theme.textDim : Theme.text
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
            Heading { text: qsTr("Activity") }

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                // Its share of the page's implicit height: about a dozen
                // lines. Without it, the text's own height would grow the
                // page instead of scrolling the log
                Layout.preferredHeight: 200
                Layout.minimumHeight: 200

                TextArea {
                    id: activityLog
                    objectName: "activityLog"
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
}
