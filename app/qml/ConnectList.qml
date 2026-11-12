// File/layer: app/qml/ConnectList.qml | Qt6 Quick + Shapes, UI layer
// Testing: QML: QmlTest.phone, QmlTest.home (test_qml)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes

pragma ComponentBehavior: Bound

// With no session, what can be connected to: the addresses that connected
// (Recent), then the adapters the machine or the phone lists (Adapters),
// one row chosen and the one Connect. Dash's card on the phone and the
// desktop's Home. Connecting stays Settings' (the list signals)
Rectangle {
    id: list
    objectName: "connectList"

    // The addresses that connected, newest first, with the picked
    // vehicle's saved one first when absent (Settings' connectUris)
    property var recents: []
    property string vehicleUri: ""
    // Says where a recording is played back; Home's own foot line says it
    property bool replayHint: true
    // [{ uri, title, kind }]: Settings' connectAdapters
    property var adapters: []
    // The phone's Bluetooth, read from the singleton; a test sets them
    property bool bluetoothAvailable: Bluetooth.available
    property string bluetoothPermission: Bluetooth.permission
    property bool bluetoothNeedsSettings: Bluetooth.needsSettings
    property bool bluetoothScanning: Bluetooth.scanning
    // The rows are a touch's size; a list can set another height
    property real rowHeight: Theme.touchTarget
    // More adapters than this go behind Other address...
    readonly property int maxAdapterRows: 6

    signal connectTo(string uri)
    signal otherAddress()
    signal showLog()
    // On screen again: Main lists the adapters anew
    signal shown()
    // A row tapped: the owner keeps it in `tapped`, since Dash's Loader
    // makes the list again each time it shows
    signal picked(string uri)
    property string tapped: ""
    onVisibleChanged: if (visible) shown()
    Component.onCompleted: if (visible) shown()

    // Every row and button a touch's size (ThemedButton reads this)
    readonly property real touchMin: rowHeight
    readonly property bool busy: Bridge.connecting || Bridge.disconnecting

    // The bridge's words for the last connect that failed
    property string errorText: ""
    Connections {
        target: Bridge
        function onConnectResult(uri, error) { list.errorText = error }
    }
    // Allow's asks: never one at the list's first show
    property int bluetoothAsks: 0

    // What a recent address is, in words a driver knows
    function kindOf(uri) {
        return uri.startsWith("mock://") ? qsTr("Demo data")
             : uri.startsWith("replay://") ? qsTr("Recording")
             : uri.startsWith("j2534://") ? qsTr("J2534 adapter")
             : uri.startsWith("elm327://") ? qsTr("ELM327 adapter")
             : uri.startsWith("socketcan://") ? qsTr("CAN interface")
             : ""
    }
    function baseOf(uri) {
        const query = uri.indexOf("?")
        return query < 0 ? uri : uri.substring(0, query)
    }

    // A recent address whose base is a listed device shows the device's
    // name, the address as its detail
    function deviceOf(uri) {
        const base = baseOf(uri)
        return adapters.find(a => a.uri === base)
    }
    // The profile an address asks for, in the words Settings uses
    function profileWordOf(uri) {
        const query = uri.indexOf("?")
        const pairs = query < 0 ? [] : uri.substring(query + 1).split("&")
        const pair = pairs.find(p => p.startsWith("profile="))
        const name = pair === undefined ? "" : pair.substring(8)
        return name === "" ? qsTr("Default")
             : name === "raw" ? qsTr("Raw")
             : name === "uds-generic" ? qsTr("UDS DIDs")
             : name
    }
    // Two recents on one adapter read the same but for their profile, which
    // the compact form puts where the kind goes
    readonly property var recentRows: recents.map(u => {
        const device = deviceOf(u)
        if (device === undefined)
            return { uri: u, title: Bridge.uriLabelFor(u), kind: kindOf(u), adapter: false, detail: "" }
        const shared = recents.filter(r => baseOf(r) === baseOf(u)).length > 1
        return { uri: u, title: device.title, kind: device.kind, adapter: false,
                 detail: Bridge.uriLabelFor(u), profileWord: shared ? profileWordOf(u) : "" }
    })
    // An adapter that is also a recent address shows once, under Recent
    readonly property var adapterRows: {
        const taken = recents.map(baseOf)
        const rows = adapters.filter(a => taken.indexOf(a.uri) < 0).slice(0, maxAdapterRows)
            .map(a => ({ uri: a.uri, title: a.title, kind: a.kind, adapter: true }))
        rows.push({ uri: "mock://demo", title: Bridge.uriLabelFor("mock://demo"),
                   kind: qsTr("Demo data"), adapter: true, demo: true })
        return rows
    }
    readonly property var rows: recentRows.concat(adapterRows)
    // A tapped row; else the vehicle's saved address; else the newest
    // recent; else the one adapter when there is exactly one; else none
    readonly property string chosen: {
        const uris = rows.map(r => r.uri)
        if (tapped !== "" && uris.indexOf(tapped) >= 0)
            return tapped
        if (vehicleUri !== "" && uris.indexOf(vehicleUri) >= 0)
            return vehicleUri
        if (recents.length > 0)
            return recents[0]
        const real = adapterRows.filter(r => r.demo !== true)
        return real.length === 1 ? real[0].uri : ""
    }

    // Bluetooth's line in Adapters: the permission first, asked only on
    // a tap; then, with none paired or found, where to pair one
    readonly property bool bluetoothAsk: bluetoothAvailable && bluetoothPermission !== "granted"
    readonly property bool bluetoothNone: bluetoothAvailable && bluetoothPermission === "granted"
        && !adapters.some(a => a.uri.startsWith("elm327://bt:") || a.uri.startsWith("elm327://ble:"))
    function askBluetooth() {
        ++bluetoothAsks
        if (bluetoothNeedsSettings)
            Bluetooth.openSettings()
        else
            Bluetooth.userRefresh()
    }

    implicitHeight: column.implicitHeight + 24
    color: Theme.surface
    radius: Theme.radiusCard
    border.width: 1
    border.color: Theme.hairline

    // A row: a check mark when chosen, the name, its kind
    Component {
        id: rowDelegate
        ThemedButton {
            id: row
            required property var modelData
            required property int index
            readonly property bool isChosen: list.chosen === modelData.uri
            objectName: "connectRow_" + (modelData.adapter ? list.recentRows.length + index : index)
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredHeight: list.rowHeight
            enabled: !list.busy
            checkable: true
            checked: isChosen
            leftPadding: 12
            rightPadding: 12
            readonly property string profileWord: modelData.profileWord !== undefined ? modelData.profileWord : ""
            Accessible.name: (modelData.kind !== "" ? qsTr("%1, %2").arg(modelData.title).arg(modelData.kind)
                                                    : modelData.title)
                             + (profileWord !== "" ? ", " + profileWord : "")
            onClicked: list.picked(modelData.uri)
            contentItem: RowLayout {
                spacing: 10
                // A mark, so the choice doesn't rest on the fill alone
                Item {
                    implicitWidth: 18; implicitHeight: 18   // layout-managed
                    Shape {
                        objectName: "connectRowMark"
                        visible: row.isChosen
                        anchors.centerIn: parent
                        width: 24; height: 24
                        scale: 0.75
                        preferredRendererType: Shape.CurveRenderer
                        ShapePath {
                            strokeColor: Theme.text
                            strokeWidth: 2.5
                            fillColor: "transparent"
                            capStyle: ShapePath.SquareCap
                            startX: 5; startY: 12
                            PathLine { x: 10; y: 17 }
                            PathLine { x: 19; y: 7 }
                        }
                    }
                }
                Label {
                    objectName: "connectRowAddress"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                    font.pixelSize: Theme.fontEmphasis
                    color: Theme.text
                    text: row.modelData.title
                }
                Label {
                    objectName: "connectRowDetail"
                    visible: text !== "" && !Theme.compact
                    Layout.maximumWidth: row.width * 0.4
                    elide: Text.ElideMiddle
                    font.pixelSize: Theme.fontCaption
                    color: Theme.textDim
                    text: row.modelData.detail !== undefined ? row.modelData.detail : ""
                }
                Label {
                    objectName: "connectRowKind"
                    visible: text !== ""
                    font.pixelSize: Theme.fontCaption
                    color: Theme.textDim
                    text: Theme.compact && row.profileWord !== "" ? row.profileWord : row.modelData.kind
                }
            }
        }
    }

    ColumnLayout {
        id: column
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
        spacing: 10

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                text: qsTr("No session")
                font.pixelSize: Theme.fontEmphasis
                font.weight: Font.DemiBold
                color: Theme.text
            }
            Label {
                objectName: "connectHint"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontCaption
                color: Theme.textDim
                text: list.replayHint
                      ? qsTr("Pick a source, then connect. To play a recording back, open Recordings.")
                      : qsTr("The gauges fill when a session runs. Pick a source, then connect.")
            }
        }

        ColumnLayout {
            objectName: "connectRecent"
            Layout.fillWidth: true
            spacing: 4
            visible: list.recentRows.length > 0
            Label {
                text: qsTr("Recent")
                font.pixelSize: Theme.fontCaption
                color: Theme.textDim
            }
            Repeater {
                model: list.recentRows
                delegate: rowDelegate
            }
        }

        ColumnLayout {
            objectName: "connectAdapters"
            Layout.fillWidth: true
            spacing: 4
            visible: list.adapterRows.length > 0 || list.bluetoothAsk || list.bluetoothNone
            Label {
                text: qsTr("Adapters")
                font.pixelSize: Theme.fontCaption
                color: Theme.textDim
            }
            Repeater {
                model: list.adapterRows
                delegate: rowDelegate
            }
            // Never asked at launch: a tap on Allow asks
            RowLayout {
                visible: list.bluetoothAsk
                Layout.fillWidth: true
                spacing: 8
                Label {
                    objectName: "connectBluetoothWhy"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontCaption
                    color: Theme.textDim
                    text: qsTr("Havne needs Android's permission to list paired adapters")
                }
                ThemedButton {
                    objectName: "connectAllow"
                    text: list.bluetoothNeedsSettings ? qsTr("Open settings") : qsTr("Allow")
                    enabled: !list.busy
                    onClicked: list.askBluetooth()
                }
            }
            RowLayout {
                visible: list.bluetoothNone
                Layout.fillWidth: true
                spacing: 8
                Label {
                    objectName: "connectBluetoothNone"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontCaption
                    color: Theme.textDim
                    text: qsTr("Pair an adapter in Android's Bluetooth settings, or scan")
                }
                ThemedButton {
                    objectName: "connectScan"
                    flat: true
                    enabled: !list.bluetoothScanning && !list.busy
                    text: list.bluetoothScanning ? qsTr("Scanning…") : qsTr("Scan")
                    onClicked: Bluetooth.scan()
                }
            }
        }

        Label {
            objectName: "connectPickHint"
            visible: list.chosen === ""
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontCaption
            color: Theme.textDim
            text: qsTr("Pick a row, then connect")
        }

        // A relay holds the adapter, so Connect waits for it to stop
        Label {
            objectName: "connectRelayNote"
            visible: list.chosen !== "" && Relay.running
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontCaption
            color: Theme.textDim
            text: qsTr("Stop the relay to connect")
        }

        // The bridge's words for a connect that failed, and the Log beside
        // them, which has the rest
        RowLayout {
            visible: list.errorText !== ""
            Layout.fillWidth: true
            spacing: 8
            Label {
                objectName: "connectError"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                maximumLineCount: 3
                elide: Text.ElideRight
                font.pixelSize: Theme.fontCaption
                color: Theme.danger
                text: list.errorText
            }
            ThemedButton {
                objectName: "connectShowLog"
                flat: true
                text: qsTr("Show log")
                Layout.alignment: Qt.AlignTop
                onClicked: list.showLog()
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8
            ThemedButton {
                objectName: "connectOther"
                flat: true
                enabled: !list.busy
                text: qsTr("Other address…")
                onClicked: list.otherAddress()
            }
            // Primary with a row chosen; with none chosen it waits
            ThemedButton {
                objectName: "connectGo"
                highlighted: list.chosen !== ""
                // A relay holds the adapter (ADR-0047)
                enabled: !list.busy && list.chosen !== "" && !Relay.running
                text: Bridge.connecting ? qsTr("Connecting…")
                    : Bridge.disconnecting ? qsTr("Disconnecting…")
                    : qsTr("Connect")
                onClicked: {
                    list.errorText = ""
                    list.connectTo(list.chosen)
                }
            }
        }
    }
}
