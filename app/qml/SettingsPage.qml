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
//          serial ports and Bluetooth adapters, a serial port's and a
//          Bluetooth adapter's row and the elm327:// preset
//          filling the field, each row's versions last seen at a connect
//          (kept across a new page; the fake's on Windows); the status label
//          in each link state (tests/qml/tst_linkhealth.qml), by
//          objectName; the developer box and the mock:// preset that
//          follows it (tests/qml/tst_log.qml); the Vehicle section
//          (ADR-0027): New, Rename, Duplicate and their defaults, Forget
//          and Forget all asking once, kept across a new page, picking
//          that fills the field, locked during a session, a mock session
//          left alone, and (Windows, the fake) an adapter's connect
//          keeping its URI and plotting, its VIN learned and logged;
//          every item inside the page's width at a phone's (320 px); a
//          recording that ends listed, its row filling the field with a
//          replay that connects under the recorded profile and DIDs; the
//          five sections folded at first, each heading's summary,
//          chevron and fold; Connect… elsewhere opening Adapter; the
//          Record line saying a
//          replay isn't recorded; a preset of another scheme dropping
//          the keys it doesn't take and the mock profile on an adapter.
//          Manual smoke: the live discovery list renders (Windows: the
//          registry's interfaces and ports; macOS/Linux: the /dev ports or
//          the empty-state hint) sized to its rows, a device row
//          fills the URI field only, Connect button drives
//          connectSessionAsync (UI stays responsive while a vendor blocks
//          ~8 s), Record sessions fixed during a session and its folder
//          shown relative to Documents; at 1280 × 650 the page scrolls;
//          the Display row steps Theme.zoom; developer mode kept between
//          runs (Main.qml). The recording's bridge side:
//          BridgeTest.Recording*. The Units row sets Bridge.unitSystem
//          (tst_settings.qml); its conversions: SignalsTest.DisplayConvert*,
//          BridgeTest.UnitSystem*.

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes

pragma ComponentBehavior: Bound

Pane {
    id: page
    padding: 0
    background: Rectangle { color: Theme.bg }

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

    // The machine's CAN interfaces, for socketcan:// (ADR-0032 §7): [{
    // name, kind, up, listenOnly, fd, bitrate }, ...]. Empty off Linux.
    // Listed whatever their mode; core refuses a controller that isn't
    // listen-only, and the row says so first
    property var canInterfaces: []

    // The phone's Bluetooth adapters (ADR-0028 §2): [{ name, address,
    // kind }], kind "bt" (classic, paired) or "ble". Empty off Android
    property var bluetoothAdapters: Bluetooth.adapters

    // Each device's versions as last seen at a connect, { base: text }
    // (Bridge.adapterVersions). The list can't ask for them without
    // opening the adapter, which the VX and VT refuse while unattached
    property var seenVersions: ({})

    // The device list's rows, the interfaces, the ports, then the
    // Bluetooth adapters: { title, detail, base, versions }, base being
    // what a click puts in the URI field
    readonly property var devices: j2534Devices.map(d => withVersions({
            title: d.vendorName + (d.architecture ? "  (" + d.architecture + ")" : ""),
            detail: d.dllPath,
            base: "j2534://" + d.vendorName
        })).concat(serialPorts.map(p => withVersions({
            title: p.name,
            detail: p.device !== "" ? qsTr("Serial port, for elm327:// · %1").arg(p.device)
                                    : qsTr("Serial port, for elm327://"),
            base: "elm327://" + p.name
        }))).concat(canInterfaces.map(c => withVersions({
            title: c.name,
            detail: canDetail(c),
            base: "socketcan://" + c.name
        }))).concat(bluetoothAdapters.map(a => withVersions({
            title: a.name !== "" ? a.name : a.address,
            detail: a.kind === "ble" ? qsTr("Bluetooth LE · %1").arg(a.address)
                                     : qsTr("Bluetooth, paired · %1").arg(a.address),
            base: "elm327://" + a.kind + ":" + a.address
        })))

    // A CAN interface's row: what it is, and why a connect would be
    // refused before one is tried
    function canDetail(c) {
        if (c.kind === "vcan")
            return c.up ? qsTr("Virtual CAN bus, for socketcan://")
                        : qsTr("Virtual CAN bus, down: sudo ip link set %1 up").arg(c.name)
        if (!c.up)
            return qsTr("CAN interface, down: bring it up listen-only with ip link")
        if (!c.listenOnly)
            return qsTr("CAN interface, not listen-only: socketcan:// refuses it until it is")
        return qsTr("CAN interface, listen-only at %1 bit/s, for socketcan://").arg(c.bitrate)
    }

    function withVersions(row) {
        row.versions = page.seenVersions[row.base] || ""
        return row
    }

    // A port names no adapter: whatever answered on it last is what it
    // shows, until another connect there says otherwise
    function rememberVersions(uri, versions) {
        if (versions === "")
            return
        const base = splitUri(uri).base
        if (seenVersions[base] === versions)
            return
        const next = Object.assign({}, seenVersions)
        next[base] = versions
        seenVersions = next
        sessionSettings.adapterVersions = JSON.stringify(next)
    }

    // ---- Vehicles (ADR-0027): VehicleBook's document, stored as JSON in
    // the "vehicles" category. A stored text that doesn't parse leaves no
    // vehicles, and is overwritten only by the next change
    property var vehicles: VehicleBook.empty()
    readonly property var pickedVehicle: VehicleBook.picked(vehicles)
    // A removal waiting for its second press: "", "forget" or "forgetAll"
    property string confirming: ""
    // The picked vehicle's plotted signals reach Live once this session's
    // rows exist; a change of rows before that isn't the user's
    property bool plottedApplied: false
    readonly property string typedLabel: vehicleLabelField.text.trim()
    readonly property string newLabel: typedLabel !== "" ? typedLabel
                                                         : VehicleBook.defaultNewLabel(vehicles)
    readonly property string copyLabel: typedLabel !== "" || pickedVehicle === null
                                        ? typedLabel
                                        : VehicleBook.defaultCopyLabel(vehicles, pickedVehicle.label)

    function saveVehicles(doc) {
        vehicles = doc
        vehicleStore.document = VehicleBook.serialize(doc)
        Bridge.vehicleLabel = pickedVehicle !== null ? pickedVehicle.label : ""
    }

    // Picking fills the field with the vehicle's connection, read back
    // like typed text so the profile row and the DID field follow
    function pickVehicle(id) {
        confirming = ""
        saveVehicles(VehicleBook.pick(vehicles, id))
        if (pickedVehicle !== null && pickedVehicle.uri !== "")
            uriField.text = pickedVehicle.uri
    }

    // A change of the list from the section's buttons: the field empties
    // for the next label
    function changeVehicles(doc) {
        confirming = ""
        saveVehicles(doc)
        vehicleLabelField.text = ""
    }

    // The VIN settled which vehicle this is (ADR-0027 §4)
    function recognizeVehicle() {
        if (!VehicleBook.isAdapterUri(Bridge.currentUri))
            return
        let doc = vehicles
        if (doc.salt === "") {
            doc = VehicleBook.copy(doc)
            doc.salt = Bridge.newSalt()
        }
        const r = VehicleBook.recognize(doc, Bridge.vinKey(doc.salt), Bridge.currentUri)
        saveVehicles(r.doc)
        if (r.outcome === "new" && plottedApplied)
            saveVehicles(VehicleBook.rememberUnplotted(vehicles, Bridge.signalModel.unplottedNames()))
        else if (r.outcome === "switched" && plottedApplied)
            Bridge.signalModel.setUnplotted(pickedVehicle.unplotted)
        Bridge.logEvent(VehicleBook.logLine(r))
    }

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

    // A caption, then buttons that wrap: a row of them would hold the
    // page at its natural width, past a phone's. The caption is the
    // flow's first item, so a wrapped button has the whole width
    component LabeledFlow: Flow {
        property alias label: caption.text
        Layout.fillWidth: true
        spacing: Theme.spacing

        Label {
            id: caption
            visible: text !== ""
            color: Theme.textDim
            topPadding: 8
        }
    }

    // The page is the connect bar, then five sections, each folded
    // until its heading is pressed: what to connect to, the vehicle, what
    // to read, recording and the display. Unfolded, the page ran to
    // several screens on a phone and an adapter sat at its bottom. Each
    // heading says what its section holds now, so the page reads folded
    // (docs/bench-sessions/2026-10-28-android-5.md)
    component Section: ColumnLayout {
        id: section
        property string title
        property string summary
        property bool open: false
        default property alias content: body.data
        Layout.fillWidth: true
        spacing: Theme.spacing * 2

        ThemedButton {
            objectName: section.objectName + "Heading"
            Layout.fillWidth: true
            Accessible.name: section.title
            onClicked: section.open = !section.open
            contentItem: RowLayout {
                spacing: 10
                // Drawn, not a glyph: Android's fonts have no ▸ or ▾,
                // which showed as a box
                Shape {
                    objectName: "sectionChevron"
                    Layout.preferredWidth: 10
                    Layout.preferredHeight: 10
                    rotation: section.open ? 90 : 0
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath {
                        strokeColor: Theme.text
                        strokeWidth: 1.5
                        fillColor: "transparent"
                        capStyle: ShapePath.RoundCap
                        joinStyle: ShapePath.RoundJoin
                        startX: 3.5; startY: 1
                        PathLine { x: 7.5; y: 5 }
                        PathLine { x: 3.5; y: 9 }
                    }
                }
                Label {
                    text: section.title
                    color: Theme.text
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Label {
                    objectName: "sectionSummary"
                    text: section.summary
                    color: Theme.textDim
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                    Layout.fillWidth: true
                }
            }
        }

        ColumnLayout {
            id: body
            visible: section.open
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing
            Layout.bottomMargin: Theme.spacing
            spacing: Theme.spacing * 2
        }
    }

    // Connect… on Home, Live or the header lands here: what to connect to
    // is open, folded or not before
    function showAdapters() {
        adapterSection.open = true
        flick.contentY = 0
    }

    function profileLabel() {
        for (const p of profiles) {
            if (p.name === profileChoice)
                return p.label
        }
        return profileChoice
    }

    // A replay plays a recording already, so the bridge doesn't record
    // one; the Record switch keeps its setting for the next live session
    readonly property bool replayInField: /^\s*replay:\/\//i.test(uriField.text)

    function refreshDevices() {
        j2534Devices = Bridge.discoverJ2534Devices()
        serialPorts = Bridge.discoverSerialPorts()
        canInterfaces = Bridge.discoverCanInterfaces()
    }

    // The newest recordings, [{ title, uri, source, profile }, ...]
    // (Bridge.recentRecordings): read at start and after each recording
    // ends. A row fills the field with its replay:// URI, under the
    // profile it was recorded with, so a phone needs nothing typed
    readonly property int recordingsListed: 20
    property var recordings: []

    function refreshRecordings() {
        recordings = Bridge.recentRecordings(recordingsListed)
    }

    // Set like typed text, so the profile row and the DID field read it
    function pickRecording(uri) {
        uriField.text = uri
        showUriField()
    }

    // The row a device or recording list shows outlined: its URI is in
    // the field, whatever the query
    function isChosen(uri) {
        return uri !== "" && splitUri(uriField.text).base === splitUri(uri).base
    }

    // A row or preset fills the field: scrolls the page back to the field,
    // Connect and the error line under them when they're above the view,
    // as on a phone, where the pick otherwise seems to do nothing
    function showUriField() {
        const top = uriField.mapToItem(column, 0, 0).y + column.y - Theme.spacing
        if (top < flick.contentY)
            flick.contentY = Math.max(0, top)
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
          hint: qsTr("The scheme's own profile: obd2 on an adapter, raw on a replay or a "
                     + "CAN interface, the demo set on mock://") },
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

    // The query keys each scheme takes, as core lists them
    // (core/src/mock_transport.cpp, replay_transport.cpp,
    // j2534_transport.cpp, elm327_transport.cpp)
    readonly property var schemeKeys: ({
        "mock": ["profile", "did"],
        "replay": ["profile", "did", "speed", "loop"],
        "j2534": ["baud", "flags", "filter", "txid", "poll", "probe", "identify", "profile", "did"],
        "elm327": ["baud", "serial", "filter", "txid", "poll", "probe", "identify", "profile", "did"],
        "socketcan": ["profile", "did", "baud", "filter"]
    })

    function schemeOf(text) {
        const i = text.indexOf("://")
        return i < 0 ? "" : text.substring(0, i).trim().toLowerCase()
    }

    // Row click / preset click: selection is INTENT, never action. A row
    // that only fills the URI field can never accidentally cost 8 seconds.
    // The query the field holds comes along, less what another scheme
    // can't use: keys it doesn't take (a replay's speed= on an adapter),
    // and the mock profile on an adapter, where no frame is the mock's
    // and it asks the ECU nothing. Kept, they failed the next connect
    // (docs/bench-sessions/2026-10-28-android-5.md); the profile row
    // shows Default then
    function fillUri(base) {
        let pairs = splitUri(uriField.text).pairs
        const scheme = schemeOf(base)
        const keys = schemeKeys[scheme]
        if (keys !== undefined && scheme !== schemeOf(uriField.text)) {
            pairs = pairs.filter(p => keys.indexOf(p[0]) >= 0)
            if (profileChoice === "mock" && (scheme === "j2534" || scheme === "elm327"))
                profileChoice = "default"
            // SocketCAN only listens: a profile that requests is refused there
            if (profileChoice === "uds-generic" && scheme === "socketcan")
                profileChoice = "default"
        }
        setUri(composeUri(base, pairs))
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

    // ---- The last URI that connected, the Record switch and each
    // device's versions (seenVersions, as JSON), kept between runs as
    // Main.qml keeps the zoom and units. Only a URI that connected is
    // stored, so a typo or a refused query doesn't come back
    Settings {
        id: sessionSettings
        category: "session"
        property string uri: ""
        property bool recording: false
        property string adapterVersions: "{}"
    }
    Settings {
        id: vehicleStore
        category: "vehicles"
        property string document: ""
    }

    Component.onCompleted: {
        try {
            const seen = JSON.parse(sessionSettings.adapterVersions)
            if (seen !== null && typeof seen === "object" && !Array.isArray(seen))
                seenVersions = seen
        } catch (e) {
            // a damaged store shows no versions until the next connect
        }
        vehicles = VehicleBook.parse(vehicleStore.document)
        Bridge.vehicleLabel = pickedVehicle !== null ? pickedVehicle.label : ""
        refreshDevices()
        Bluetooth.refresh(false)   // asked for when Adapter first opens
        refreshRecordings()
        // Set like typed text, so the profile row and the DID field read
        // it back
        if (uriField.text === "")
            uriField.text = sessionSettings.uri
        Bridge.recording = sessionSettings.recording
    }

    Connections {
        target: Bridge

        function onConnectResult(uri, error) {
            errorLabel.text = error
            errorLabel.visible = (error !== "")
            if (error === "") {
                sessionSettings.uri = uri
                page.rememberVersions(uri, Bridge.adapterVersions)
                // The picked vehicle's connection and plotted signals,
                // for an adapter's session only (ADR-0027 §1, §2)
                if (VehicleBook.isAdapterUri(uri) && page.pickedVehicle !== null) {
                    page.saveVehicles(VehicleBook.rememberUri(page.vehicles, uri))
                    Bridge.signalModel.setUnplotted(page.pickedVehicle.unplotted)
                }
                page.plottedApplied = true
            }
        }

        function onConnectedChanged() {
            if (!Bridge.connected)
                page.plottedApplied = false
        }

        function onVehicleIdentified() {
            page.recognizeVehicle()
        }

        function onRecordingChanged() {
            sessionSettings.recording = Bridge.recording
        }

        // A recording that ended joins the list
        function onRecordingStateChanged() {
            if (!Bridge.recordingActive)
                page.refreshRecordings()
        }
    }

    // A tick or an untick on Live is the picked vehicle's from now on
    Connections {
        target: Bridge.signalModel

        function onSubscribedCountChanged() {
            if (page.plottedApplied && page.pickedVehicle !== null
                    && VehicleBook.isAdapterUri(Bridge.currentUri))
                page.saveVehicles(VehicleBook.rememberUnplotted(
                    page.vehicles, Bridge.signalModel.unplottedNames()))
        }
    }

    // Scrolls when the content doesn't fit (a short laptop screen);
    // otherwise the activity log takes the height that's left
    Flickable {
        id: flick
        objectName: "settingsFlick"
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
            objectName: "settingsColumn"
            x: Theme.pageMargin
            y: Theme.pageMargin
            width: flick.width - 2 * Theme.pageMargin
            // Its content's height: stretched to the page, a layout spreads
            // the spare height between the folded headings
            height: implicitHeight
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
                // A long URI breaks anywhere: unwrapped, it widened the
                // page past a phone's edge once connected
                wrapMode: Text.WrapAnywhere
                Layout.fillWidth: true
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
                    // A WiFi adapter is typed: nothing lists it (ADR-0026's
                    // appendix "WiFi over TCP")
                    placeholderText: qsTr("URI, e.g. mock://demo, elm327://COM5, "
                                          + "elm327://192.168.0.10:35000 or "
                                          + "j2534://Vendor?profile=raw&poll=0x0C")
                    enabled: page.idle
                    color: Theme.text
                    // A URI, not prose: a phone's keyboard capitalized
                    // "Replay://" into a scheme core doesn't know
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
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

            // ---- Adapter: what to connect to, found or typed ----
            Section {
                id: adapterSection
                objectName: "adapterSection"
                title: qsTr("Adapter")
                summary: page.devices.length === 0 ? qsTr("none found")
                                                   : qsTr("%1 found").arg(page.devices.length)
                // The phone's Bluetooth permission is asked for here, when
                // the list is first wanted, not at launch
                onOpenChanged: if (open) Bluetooth.refresh(true)

                // ---- Adapters: J2534 interfaces and serial ports ----
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    Label {
                        objectName: "deviceCount"
                        text: page.devices.length === 0
                              ? qsTr("None found: install a Pass-Thru driver (Windows), plug in "
                                     + "or pair an ELM327 adapter, or pick one below")
                              : qsTr("J2534 interfaces: %1, serial ports: %2%3; select one to "
                                     + "put its URI in the field above, then Connect")
                                    .arg(page.j2534Devices.length).arg(page.serialPorts.length)
                                    .arg((page.canInterfaces.length > 0
                                          ? qsTr(", CAN interfaces: %1").arg(page.canInterfaces.length)
                                          : "")
                                         + (Bluetooth.available || page.bluetoothAdapters.length > 0
                                            ? qsTr(", Bluetooth: %1").arg(page.bluetoothAdapters.length)
                                            : ""))
                        color: page.devices.length === 0 ? Theme.textDim : Theme.text
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    ThemedButton {
                        text: qsTr("Refresh")
                        onClicked: {
                            page.refreshDevices()
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
                        enabled: page.idle && !Bluetooth.scanning
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
                        border.width: page.isChosen(row.modelData.base) ? 2 : 0
                        border.color: Theme.primary

                        MouseArea {
                            id: mouse
                            objectName: "deviceRowArea"
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            enabled: page.idle
                            onClicked: {
                                page.fillUri(row.modelData.base)
                                page.showUriField()
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
                                    color: page.idle ? Theme.text : Theme.textDim
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
                            enabled: page.idle
                            onClicked: {
                                page.fillUri(modelData.uri)
                                page.showUriField()
                            }
                        }
                    }
                }
            }

            // ---- Vehicle ----
            Section {
                id: vehicleSection
                objectName: "vehicleSection"
                title: qsTr("Vehicle")
                summary: (page.pickedVehicle !== null ? page.pickedVehicle.label : qsTr("None"))
                         + (page.vehicles.list.length > 0
                            ? qsTr(" · %1 saved").arg(page.vehicles.list.length) : "")

                // ---- Vehicle (ADR-0027): picked before a connect, its
                // connection filling the field; every change to the list is
                // made here. Locked while a session is up or in flight ----
                LabeledFlow {
                    label: qsTr("Vehicle:")

                    ThemedButton {
                        objectName: "vehicle_none"
                        text: qsTr("None")
                        highlighted: page.pickedVehicle === null
                        enabled: page.idle
                        onClicked: page.pickVehicle("")
                    }
                    // Keyed by the count, not the list: each change makes a
                    // new list, and a list model would rebuild every
                    // button (losing focus) for a pick or a rename
                    Repeater {
                        model: page.vehicles.list.length
                        ThemedButton {
                            required property int index
                            // Undefined for a moment when the list shrinks,
                            // before the button goes
                            readonly property var vehicle: page.vehicles.list[index]
                            objectName: "vehicle_" + (vehicle?.id ?? "")
                            text: vehicle?.label ?? ""
                            // A long label wraps rather than run past a phone's edge
                            width: Math.min(implicitWidth, parent.width)
                            highlighted: vehicle !== undefined && page.vehicles.picked === vehicle.id
                            enabled: page.idle
                            onClicked: page.pickVehicle(vehicle.id)
                        }
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.spacing
                    visible: page.confirming === ""

                    TextField {
                        id: vehicleLabelField
                        objectName: "vehicleLabelField"
                        width: Math.min(300, parent.width)
                        enabled: page.idle
                        placeholderText: qsTr("Label")
                    }
                    ThemedButton {
                        objectName: "vehicleNew"
                        text: qsTr("New")
                        enabled: page.idle && VehicleBook.labelProblem(page.vehicles, page.newLabel, "") === ""
                        onClicked: page.changeVehicles(VehicleBook.add(page.vehicles, page.newLabel))
                    }
                    ThemedButton {
                        objectName: "vehicleDuplicate"
                        text: qsTr("Duplicate")
                        enabled: page.idle && page.pickedVehicle !== null
                                 && VehicleBook.labelProblem(page.vehicles, page.copyLabel, "") === ""
                        onClicked: page.changeVehicles(VehicleBook.duplicate(page.vehicles, page.copyLabel))
                    }
                    ThemedButton {
                        objectName: "vehicleRename"
                        text: qsTr("Rename")
                        enabled: page.idle && page.pickedVehicle !== null && page.typedLabel !== ""
                                 && page.typedLabel !== page.pickedVehicle.label
                                 && VehicleBook.labelProblem(page.vehicles, page.typedLabel,
                                                             page.pickedVehicle.id) === ""
                        onClicked: page.changeVehicles(VehicleBook.rename(page.vehicles, page.typedLabel))
                    }
                    ThemedButton {
                        objectName: "vehicleForgetVin"
                        text: qsTr("Forget the VIN")
                        enabled: page.idle && page.pickedVehicle !== null && page.pickedVehicle.vinKey !== ""
                        onClicked: page.changeVehicles(VehicleBook.forgetVin(page.vehicles))
                    }
                    ThemedButton {
                        objectName: "vehicleForget"
                        text: qsTr("Forget")
                        enabled: page.idle && page.pickedVehicle !== null
                        onClicked: page.confirming = "forget"
                    }
                    ThemedButton {
                        objectName: "vehicleForgetAll"
                        text: qsTr("Forget all vehicles")
                        enabled: page.idle && page.vehicles.list.length > 0
                        onClicked: page.confirming = "forgetAll"
                    }
                }

                // With the field empty, the defaults New and Duplicate take
                Label {
                    objectName: "vehicleLabelHint"
                    visible: page.typedLabel === "" && page.confirming === ""
                    text: page.pickedVehicle === null
                        ? qsTr("Left empty, New makes “%1”").arg(VehicleBook.defaultNewLabel(page.vehicles))
                        : qsTr("Left empty, New makes “%1” and Duplicate “%2”")
                              .arg(VehicleBook.defaultNewLabel(page.vehicles),
                                   VehicleBook.defaultCopyLabel(page.vehicles, page.pickedVehicle.label))
                    color: Theme.textDim
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                // Why a typed label can't be used
                Label {
                    objectName: "vehicleLabelProblem"
                    visible: text !== "" && page.confirming === ""
                    text: page.typedLabel === "" ? ""
                        : VehicleBook.labelProblem(page.vehicles, page.typedLabel,
                                                   page.pickedVehicle !== null ? page.pickedVehicle.id : "")
                    color: Theme.warning
                }

                // A removal asks once, in place
                Flow {
                    visible: page.confirming !== ""
                    Layout.fillWidth: true
                    spacing: Theme.spacing

                    Label {
                        objectName: "vehicleConfirmText"
                        text: page.confirming === "forgetAll" ? qsTr("Forget all vehicles?")
                            : page.pickedVehicle !== null ? qsTr("Forget %1?").arg(page.pickedVehicle.label)
                            : ""
                        color: Theme.text
                    }
                    ThemedButton {
                        objectName: "vehicleConfirm"
                        text: qsTr("Forget")
                        highlighted: true
                        enabled: page.idle
                        onClicked: page.changeVehicles(page.confirming === "forgetAll"
                                                       ? VehicleBook.forgetAll(page.vehicles)
                                                       : VehicleBook.forget(page.vehicles))
                    }
                    ThemedButton {
                        objectName: "vehicleCancel"
                        text: qsTr("Cancel")
                        onClicked: page.confirming = ""
                    }
                }
            }

            // ---- What to read: the profile ----
            Section {
                id: profileSection
                objectName: "profileSection"
                title: qsTr("What to read")
                summary: page.profileLabel()

                // ---- Profile: the URI's query (ADR-0013, ADR-0020) ----
                // The chosen button is drawn as the primary color
                LabeledFlow {
                    label: qsTr("Profile:")
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
                        placeholderText: qsTr("1E1C:u16, F190:text, 7E9:1E1C, 1E00-1EFF; "
                                              + "[ecu:]DID[-DID][:hex|text|u8…u32|i8…i32]")
                        validator: RegularExpressionValidator { regularExpression: /[0-9A-Za-z:,\-\s]*/ }
                        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                        enabled: page.idle
                        color: Theme.text
                        onTextChanged: {
                            if (!page.syncing && page.profileChoice === "uds-generic")
                                page.recompose()
                        }
                        onAccepted: page.requestConnect(uriField.text)
                    }
                }
            }

            // ---- Recording ----
            Section {
                id: recordingSection
                objectName: "recordingSection"
                title: qsTr("Recording")
                summary: (Bridge.recordingActive ? qsTr("recording") : Bridge.recording ? qsTr("on") : qsTr("off"))
                         + (page.recordings.length > 0
                            ? qsTr(" · %1 saved").arg(page.recordings.length) : "")

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
                        objectName: "recordStatus"
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        color: Bridge.recordingActive ? Theme.success : Theme.textDim
                        text: Bridge.recordingActive
                              ? qsTr("Recording to %1").arg(Bridge.recordingFolder)
                              : Bridge.recording && page.replayInField
                                ? qsTr("Replays aren't recorded")
                              : Bridge.recordingFolder !== ""
                                ? qsTr("Last: %1").arg(Bridge.recordingFolder)
                              : Bridge.recording
                                ? qsTr("Frames, values and events, from the next connect")
                              : ""
                    }
                    // A phone has no app that opens the app's own folders
                    ThemedButton {
                        text: qsTr("Open folder")
                        visible: Bridge.recordingFolder !== "" && !Theme.mobile
                        onClicked: Qt.openUrlExternally(Bridge.recordingFolderUrl)
                    }
                }

                Label {
                    objectName: "recordingsCaption"
                    visible: page.recordings.length > 0
                    text: qsTr("Recent recordings: select one to put its replay in the field above")
                    color: Theme.textDim
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                // Sized like the device list: up to deviceRowsShown rows, then
                // it scrolls
                ListView {
                    id: recordingList
                    objectName: "recordingList"
                    // No current row: ListView gives a current one a default
                    // highlight, which animates to its size and, after the
                    // section opened at a phone's width, lay past the edge
                    currentIndex: -1
                    highlightFollowsCurrentItem: false
                    Layout.fillWidth: true
                    Layout.preferredHeight: contentHeight
                    Layout.maximumHeight: page.deviceRowsShown * (page.deviceRowHeight + spacing) - spacing
                    visible: count > 0
                    clip: true
                    spacing: 2
                    boundsBehavior: Flickable.StopAtBounds
                    model: page.recordings

                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    delegate: Rectangle {
                        id: recordingRow
                        required property var modelData
                        width: recordingList.width
                        height: page.deviceRowHeight
                        radius: Theme.radius
                        color: recordingMouse.containsMouse ? Theme.elevatedLight : Theme.elevated
                        border.width: page.isChosen(recordingRow.modelData.uri) ? 2 : 0
                        border.color: Theme.primary

                        MouseArea {
                            id: recordingMouse
                            objectName: "recordingRowArea"
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            enabled: page.idle
                            onClicked: page.pickRecording(recordingRow.modelData.uri)
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: Theme.spacing
                            spacing: 2

                            Label {
                                objectName: "recordingTitle"
                                text: recordingRow.modelData.title
                                color: page.idle ? Theme.text : Theme.textDim
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Label {
                                objectName: "recordingDetail"
                                text: recordingRow.modelData.source !== ""
                                      ? qsTr("%1 · replays as %2").arg(recordingRow.modelData.source)
                                                                  .arg(recordingRow.modelData.profile)
                                      : qsTr("replays as %1").arg(recordingRow.modelData.profile)
                                color: Theme.textDim
                                font.pointSize: 8
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }

            // ---- Display ----
            Section {
                id: displaySection
                objectName: "displaySection"
                title: qsTr("Display")
                summary: qsTr("%1 %").arg(Math.round(Theme.zoom * 100))
                         + (Bridge.unitSystem === 1 ? qsTr(" · imperial") : qsTr(" · metric"))
                         + (Theme.developerMode ? qsTr(" · developer mode") : "")

                // The whole UI's zoom; Main.qml applies and stores it
                LabeledFlow {
                    label: qsTr("Display size:")
                    ThemedButton {
                        id: smaller
                        text: "−"
                        Accessible.name: qsTr("Smaller")
                        enabled: Theme.zoom > Theme.zoomSteps[0] + 0.001
                        onClicked: Theme.stepZoom(-1)
                    }
                    Label {
                        width: Math.max(52, implicitWidth)
                        height: smaller.height
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
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
                }
                // A phone has no keyboard to zoom with
                Label {
                    visible: !Theme.mobile
                    text: qsTr("or Ctrl + / Ctrl − / Ctrl 0")
                    color: Theme.textDim
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                // Units: what the pages show, converted in core (ADR-0022);
                // recordings keep the decoded values. Main.qml stores it.
                // Takes effect at once, connected or not.
                LabeledFlow {
                    label: qsTr("Units:")
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

                // Developer mode (Theme.developerMode); Main.qml stores it
                CheckBox {
                    id: developerBox
                    objectName: "developerBox"
                    text: qsTr("Developer mode: the mock:// preset, and the raw frames on the Log page")
                    checked: Theme.developerMode
                    Layout.fillWidth: true
                    onToggled: {
                        Theme.developerMode = checked
                        checked = Qt.binding(() => Theme.developerMode)
                    }
                    contentItem: Label {
                        text: developerBox.text
                        color: Theme.text
                        leftPadding: developerBox.indicator.width + developerBox.spacing
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}
