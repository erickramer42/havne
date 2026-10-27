// File/layer: app/qml/SettingsPage.qml | Qt6 Quick, UI layer
// Testing: test_qml (tests/qml/tst_settings.qml): the URI composed from a
//          preset, the profile row and the DID field, a typed URI read
//          back into the row and the field with its other keys kept, no
//          scheme's keys added or taken (a raw replay composes no filter),
//          the DID field refusing query characters, the field left
//          editable, core's refusal in the error line, a composed
//          uds-generic URI connecting through the mock, every control
//          locked while a connect is in flight, the last URI that
//          connected and the Record switch back in a new page (a refused
//          URI not kept), the device list's rows for interfaces and
//          serial ports, a serial port's row and the elm327:// preset
//          filling the field; the status label
//          and the activity log's link lines in each link state
//          (tests/qml/tst_linkhealth.qml), by objectName.
//          Manual smoke — the live discovery list renders (Windows: the
//          registry's interfaces and ports; macOS/Linux: the /dev ports or
//          the empty-state hint) sized to its rows, a device row
//          fills the URI field only, Connect button drives
//          connectSessionAsync (UI stays responsive while a vendor blocks
//          ~8 s), activity log shows connect lifecycle and takes the
//          remaining height, Record sessions fixed during a session and
//          its folder shown relative to Documents; at 1280 × 650 the page
//          scrolls and the activity log keeps its minimum height; the
//          Display row steps Theme.zoom. The recording's bridge side:
//          BridgeTest.Recording*. The Units row sets Bridge.unitSystem
//          (tst_settings.qml); its conversions: SignalsTest.DisplayConvert*,
//          BridgeTest.UnitSystem*.

import QtCore
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

    // The machine's serial ports, for elm327:// (ADR-0026 §7): [{ name,
    // device }, ...], device the driver's on Windows, "" elsewhere. Every
    // port is listed, whatever is behind it: none is opened until Connect
    property var serialPorts: []

    // The device list's rows, the interfaces then the ports: { title,
    // detail, base }, base being what a click puts in the URI field
    readonly property var devices: j2534Devices.map(d => ({
            title: d.vendorName + (d.architecture ? "  (" + d.architecture + ")" : ""),
            detail: d.dllPath,
            base: "j2534://" + d.vendorName
        })).concat(serialPorts.map(p => ({
            title: p.name,
            detail: p.device !== "" ? qsTr("Serial port, for elm327:// · %1").arg(p.device)
                                    : qsTr("Serial port, for elm327://"),
            base: "elm327://" + p.name
        })))

    // No session, and none starting or ending. Every control that chooses
    // or starts a session is enabled by this one value, so none can be
    // used during a multi-second vendor block: the bridge refuses a
    // connect then anyway, but a device row or a preset would otherwise
    // rewrite the grayed field under the connect in flight.
    readonly property bool idle: !Bridge.connected && !Bridge.connecting && !Bridge.disconnecting

    component Heading: Label {
        color: Theme.text
        font.pixelSize: 16
        font.weight: Font.DemiBold
        Layout.topMargin: Theme.spacing
    }

    function refreshDevices() {
        j2534Devices = Bridge.discoverJ2534Devices()
        serialPorts = Bridge.discoverSerialPorts()
    }

    // The URI is composed from three choices, so none of it needs typing:
    // a base (a preset or a discovered device), a profile, and
    // uds-generic's DIDs. The field stays editable both ways: a
    // hand-written URI connects as written and its profile= and did= set
    // the row and the DID field, and the next preset, device or profile
    // click recomposes it, keeping every other query key where it was.
    // The choices live only while the app runs.
    property string profileChoice: "default"

    // The profiles the row offers (ADR-0013, ADR-0020). "default" is the
    // scheme's own profile, so it adds no profile= key. A typed name
    // outside the list highlights no button; core judges it at connect.
    readonly property var profiles: [
        { name: "default", label: qsTr("Default"),
          hint: qsTr("The scheme's own profile: obd2 on an adapter, the demo set on mock://") },
        { name: "raw", label: qsTr("raw (log only)"),
          hint: qsTr("No decode: frames reach the Log page only. On an adapter it listens "
                     + "to the engine ECU (0x7E8); add ?filter= for another ID. An ELM327 "
                     + "hears only replies to its requests, so add ?poll= there") },
        { name: "uds-generic", label: qsTr("uds-generic (DIDs)"),
          hint: qsTr("Service 0x22 reads of the DIDs below, polled; a range is scanned "
                     + "once and the DIDs the ECU answers join the polling") }
    ]

    // True while a typed URI is being read into the choices, so the DID
    // field's change doesn't recompose the URI that set it; and true
    // while the page writes the field from the choices, so that text
    // isn't read back into them
    property bool syncing: false
    property bool composing: false

    function setUri(text) {
        composing = true
        uriField.text = text
        composing = false
    }

    function profileHint() {
        for (const p of profiles) {
            if (p.name === profileChoice)
                return p.hint
        }
        return qsTr("The URI names profile '%1'; core judges it at connect").arg(profileChoice)
    }

    // A URI as its base and its query's [key, value] pairs, in order
    // (a key with no "=" keeps null; core will name it)
    function splitUri(text) {
        const q = text.indexOf("?")
        const pairs = []
        if (q >= 0) {
            for (const part of text.substring(q + 1).split("&")) {
                if (part === "")
                    continue
                const eq = part.indexOf("=")
                pairs.push(eq < 0 ? [part, null] : [part.substring(0, eq), part.substring(eq + 1)])
            }
        }
        return { base: q < 0 ? text : text.substring(0, q), pairs: pairs }
    }

    function joinUri(base, pairs) {
        if (pairs.length === 0)
            return base
        return base + "?" + pairs.map(p => p[1] === null ? p[0] : p[0] + "=" + p[1]).join("&")
    }

    // The first value of key, or undefined
    function queryValue(pairs, key) {
        for (const p of pairs) {
            if (p[0] === key)
                return p[1]
        }
        return undefined
    }

    // Sets key in place, appends it, or with null removes it
    function setQuery(pairs, key, value) {
        for (let i = 0; i < pairs.length; ++i) {
            if (pairs[i][0] !== key)
                continue
            if (value === null)
                pairs.splice(i, 1)
            else
                pairs[i] = [key, value]
            return
        }
        if (value !== null)
            pairs.push([key, value])
    }

    // The field's text before its query
    function uriBase() {
        return splitUri(uriField.text).base
    }

    // base plus the query, with profile= and did= following the choices
    // and every other key kept in place. uds-generic's DIDs go in as
    // typed, minus whitespace; core judges them (ADR-0020) and the error
    // line shows its words.
    function composeUri(base, pairs) {
        if (base === "")
            return ""
        const out = pairs.slice()
        setQuery(out, "profile", profileChoice === "default" ? null : profileChoice)
        const dids = profileChoice === "uds-generic" ? didField.text.replace(/\s+/g, "") : ""
        setQuery(out, "did", dids === "" ? null : dids)
        return joinUri(base, out)
    }

    // Row click / preset click: selection is INTENT, never action. A row
    // that only fills the URI field can never accidentally cost 8 seconds.
    // The query the field holds comes along.
    function fillUri(base) {
        setUri(composeUri(base, splitUri(uriField.text).pairs))
    }

    function chooseProfile(name) {
        const uri = splitUri(uriField.text)
        profileChoice = name
        setUri(composeUri(uri.base, uri.pairs))
    }

    function recompose() {
        const uri = splitUri(uriField.text)
        setUri(composeUri(uri.base, uri.pairs))
    }

    // A URI the page didn't compose (typed, pasted, or set through UI
    // Automation) sets the choices, never the other way round: the
    // field's text is left as it is. The DID field follows did= whenever
    // it's shown (uds-generic), so it never shows DIDs the URI lacks;
    // under another profile it keeps what was typed for later.
    function readChoices() {
        const pairs = splitUri(uriField.text).pairs
        const profile = queryValue(pairs, "profile")
        const did = queryValue(pairs, "did")
        syncing = true
        profileChoice = (profile === undefined || profile === null || profile === "")
                        ? "default" : profile
        if (profileChoice === "uds-generic")
            didField.text = did === undefined || did === null ? "" : did
        else if (did !== undefined && did !== null)
            didField.text = did
        syncing = false
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

    // ---- The last URI that connected and the Record switch, kept between
    // runs as Main.qml keeps the zoom and units. Only a URI that connected
    // is stored, so a typo or a refused query doesn't come back
    Settings {
        id: sessionSettings
        category: "session"
        property string uri: ""
        property bool recording: false
    }

    Component.onCompleted: {
        refreshDevices()
        // Set like typed text, so the profile row and the DID field read
        // it back
        if (uriField.text === "")
            uriField.text = sessionSettings.uri
        Bridge.recording = sessionSettings.recording
    }

    Connections {
        target: Bridge

        function onLogMessage(message) {
            page.appendLog(message)
        }

        function onConnectResult(uri, error) {
            errorLabel.text = error
            errorLabel.visible = (error !== "")
            if (error === "")
                sessionSettings.uri = uri
        }

        function onRecordingChanged() {
            sessionSettings.recording = Bridge.recording
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
                    objectName: "uriField"
                    Layout.fillWidth: true
                    // ?poll= replaces the profile's PID rotation. Raw on an
                    // adapter listens to the engine ECU, so ?poll= alone
                    // asks it; under the obd2 default it also decodes.
                    placeholderText: qsTr("URI — e.g. mock://demo, elm327://COM5 or "
                                          + "j2534://Vendor?profile=raw&poll=0x0C")
                    enabled: page.idle
                    color: Theme.text
                    // Any text the page didn't compose is read back
                    onTextChanged: {
                        if (!page.composing)
                            page.readChoices()
                    }
                    onAccepted: page.requestConnect(text)
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
                    enabled: Bridge.connected || (page.idle && uriField.text !== "")
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
                objectName: "errorLabel"
                visible: false
                color: Theme.danger
                wrapMode: Text.WrapAnywhere
                Layout.fillWidth: true
                text: ""
            }

            // ---- Presets: the URI's base ----
            RowLayout {
                spacing: Theme.spacing
                Label { text: qsTr("Presets:"); color: Theme.textDim }
                Repeater {
                    model: [
                        { name: "mock", uri: "mock://demo", label: "mock://demo" },
                        // A bare j2534:// opens the first discovered device
                        { name: "j2534", uri: "j2534://", label: qsTr("j2534:// (first device)") },
                        // elm327:// takes no first port: the user picks one
                        // from the list below (core's refusal says so)
                        { name: "elm327", uri: "elm327://", label: qsTr("elm327:// (a port below)") }
                    ]
                    ThemedButton {
                        required property var modelData
                        objectName: "preset_" + modelData.name
                        text: modelData.label
                        enabled: page.idle
                        onClicked: page.fillUri(modelData.uri)
                    }
                }
            }

            // ---- Profile: the URI's query (ADR-0013, ADR-0020) ----
            // The chosen button is drawn as the primary color
            RowLayout {
                spacing: Theme.spacing
                Label { text: qsTr("Profile:"); color: Theme.textDim }
                Repeater {
                    model: page.profiles
                    ThemedButton {
                        required property var modelData
                        objectName: "profile_" + modelData.name
                        text: modelData.label
                        highlighted: page.profileChoice === modelData.name
                        enabled: page.idle
                        onClicked: page.chooseProfile(modelData.name)
                    }
                }
            }

            Label {
                objectName: "profileHint"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textDim
                text: page.profileHint()
            }

            RowLayout {
                objectName: "didRow"
                Layout.fillWidth: true
                spacing: Theme.spacing
                visible: page.profileChoice === "uds-generic"
                Label { text: qsTr("DIDs:"); color: Theme.textDim }
                TextField {
                    id: didField
                    objectName: "didField"
                    Layout.fillWidth: true
                    // The ?did= grammar as core reads it (ADR-0020); the
                    // text goes into the URI as typed, so core's words
                    // name any mistake. The validator admits only the
                    // grammar's characters, so a query metacharacter (&,
                    // =, ?, #) can't split the URI into other keys.
                    placeholderText: qsTr("1E1C:u16, F190:text, 7E9:1E1C, 1E00-1EFF — "
                                          + "[ecu:]DID[-DID][:hex|text|u8…u32|i8…i32]")
                    validator: RegularExpressionValidator { regularExpression: /[0-9A-Za-z:,\-\s]*/ }
                    enabled: page.idle
                    color: Theme.text
                    onTextChanged: {
                        if (!page.syncing && page.profileChoice === "uds-generic")
                            page.recompose()
                    }
                    onAccepted: page.requestConnect(uriField.text)
                }
            }

            // ---- Recording (ADR-0017) ----
            // Off until switched on, and kept between runs. It takes effect
            // at the next connect, so it's fixed while a session is up and
            // each recording covers a whole session.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing

                CheckBox {
                    id: recordBox
                    objectName: "recordBox"
                    text: qsTr("Record sessions")
                    checked: Bridge.recording
                    enabled: page.idle
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

            // Units: what the pages show, converted in core (ADR-0022);
            // recordings keep the decoded values. Main.qml stores it.
            // Takes effect at once, connected or not.
            RowLayout {
                spacing: Theme.spacing
                Label { text: qsTr("Units:"); color: Theme.textDim }
                Repeater {
                    model: [
                        { system: 0, name: "metric", label: qsTr("Metric (km/h, °C)") },
                        { system: 1, name: "imperial", label: qsTr("Imperial (mph, °F)") }
                    ]
                    ThemedButton {
                        required property var modelData
                        objectName: "units_" + modelData.name
                        text: modelData.label
                        highlighted: Bridge.unitSystem === modelData.system
                        onClicked: Bridge.unitSystem = modelData.system
                    }
                }
            }

            // ---- Adapters: J2534 interfaces and serial ports ----
            Heading { text: qsTr("Adapters") }

            RowLayout {
                spacing: Theme.spacing
                Label {
                    objectName: "deviceCount"
                    text: page.devices.length === 0
                          ? qsTr("None found — install a Pass-Thru driver (Windows), plug in "
                                 + "or pair an ELM327 adapter, or enter a URI above")
                          : qsTr("J2534 interfaces: %1, serial ports: %2 — select one to put "
                                 + "its URI in the field above, then Connect")
                                .arg(page.j2534Devices.length).arg(page.serialPorts.length)
                    color: page.devices.length === 0 ? Theme.textDim : Theme.text
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
                objectName: "deviceList"
                Layout.fillWidth: true
                Layout.preferredHeight: contentHeight
                Layout.maximumHeight: page.deviceRowsShown * (page.deviceRowHeight + spacing) - spacing
                visible: count > 0
                clip: true
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds
                model: page.devices

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
                        objectName: "deviceRowArea"
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        enabled: page.idle
                        onClicked: page.fillUri(row.modelData.base)
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing
                        spacing: 2

                        Label {
                            objectName: "deviceTitle"
                            text: row.modelData.title
                            color: page.idle ? Theme.text : Theme.textDim
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
