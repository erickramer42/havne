// File/layer: app/qml/SettingsPage.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.linkhealth, QmlTest.log,
//          QmlTest.phone, QmlTest.driveview (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

Pane {
    id: page
    padding: 0
    background: Rectangle { color: Theme.bg }

    readonly property int driveHideMs: settingsDisplay.driveHideMs
    readonly property string driveOrientation: settingsDisplay.driveOrientation
    property alias uriField: settingsConnect.uriField
    property alias errorLabel: settingsConnect.errorLabel
    property alias logExplains: settingsConnect.logExplains
    property alias vehicleLabelField: settingsVehiclePicker.vehicleLabelField
    property alias obdbYearField: settingsSignals.obdbYearField
    property alias importVariantField: settingsSignals.importVariantField
    property alias saveDialog: settingsSignals.saveDialog
    property alias formName: settingsOwnForm.formName
    property alias formRx: settingsOwnForm.formRx
    property alias formTx: settingsOwnForm.formTx
    property alias formNumber: settingsOwnForm.formNumber
    property alias formByte: settingsOwnForm.formByte
    property alias formBits: settingsOwnForm.formBits
    property alias formBit: settingsOwnForm.formBit
    property alias formBytes: settingsOwnForm.formBytes
    property alias formSigned: settingsOwnForm.formSigned
    property alias formScale: settingsOwnForm.formScale
    property alias formOffset: settingsOwnForm.formOffset
    property alias formUnit: settingsOwnForm.formUnit
    property alias formDecimals: settingsOwnForm.formDecimals
    property alias formValues: settingsOwnForm.formValues
    property alias exprEditor: settingsOwnForm.computedPart
    property alias vinLookupField: settingsVinLookup.vinLookupField
    property alias didField: settingsProfile.didField

    readonly property int deviceRowHeight: 56
    readonly property int deviceRowsShown: 4   // the list scrolls past this

    // Discovered Pass-Thru interfaces: [{ vendorName, dllPath,
    // architecture }, ...] with architecture "x86", "x64" or "unknown".
    // Empty on non-Windows or when no drivers are installed.
    property var j2534Devices: []

    // The machine's serial ports, for elm327:// (ADR-0026 section 7): [{ name,
    // device }, ...], device the driver's on Windows, "" elsewhere. Every
    // port is listed, whatever is behind it: none is opened until Connect
    property var serialPorts: []

    // The machine's CAN interfaces, for socketcan:// (ADR-0032 section 7): [{
    // name, kind, up, listenOnly, fd, bitrate, state, modes, modesKnown,
    // modesSupported }, ...]. Empty off Linux. Listed whatever their mode;
    // core refuses a controller in neither listen-only nor one-shot mode,
    // and the row says so first
    property var canInterfaces: []

    // The phone's Bluetooth adapters (ADR-0028 section 2): [{ name, address,
    // kind, other }], kind "bt" (classic, paired) or "ble", other true for
    // audio, wearable and health gear. Empty off Android
    property var bluetoothAdapters: Bluetooth.adapters

    // Each device's versions as last seen at a connect, { base: text }
    // (Bridge.adapterVersions). The list can't ask for them without
    // opening the adapter, which the VX and VT refuse while unattached
    property var seenVersions: ({})

    // The device list's rows, the interfaces, the ports, then the
    // Bluetooth adapters: { title, detail, kind, base, versions }, base being
    // what a click puts in the URI field and kind the connect lists' word
    readonly property var devices: j2534Devices.map(d => withVersions({
            title: d.vendorName + (d.architecture ? "  (" + d.architecture + ")" : ""),
            detail: d.dllPath,
            kind: qsTr("J2534 adapter"),
            base: "j2534://" + d.vendorName
        })).concat(serialPorts.map(p => withVersions({
            title: p.name,
            detail: p.device !== "" ? qsTr("Serial port, for elm327:// · %1").arg(p.device)
                                    : qsTr("Serial port, for elm327://"),
            kind: qsTr("Serial port"),
            base: "elm327://" + p.name
        }))).concat(canInterfaces.map(c => withVersions({
            title: c.name,
            detail: canDetail(c),
            kind: qsTr("CAN interface"),
            base: "socketcan://" + c.name
        }))).concat(bluetoothAdapters.map(a => withVersions({
            title: a.name !== "" ? a.name : a.address,
            detail: a.kind === "ble" ? qsTr("Bluetooth LE · %1").arg(a.address)
                                     : qsTr("Bluetooth, paired · %1").arg(a.address),
            kind: a.kind === "ble" ? qsTr("Bluetooth LE") : qsTr("Bluetooth"),
            base: "elm327://" + a.kind + ":" + a.address,
            other: a.other === true
        })))

    // The rows the connect list shows: a paired headset, watch or health
    // device is folded under "Other paired devices" there and isn't counted
    readonly property int devicesShown: devices.filter(d => d.other !== true).length

    // A CAN interface's row: what it is, its state past error-active, and
    // why a connect would be refused before one is tried: listen-only to
    // listen, one-shot to send requests (ADR-0040 section 2), naming only
    // the modes its driver offers when the kernel says which
    function canDetail(c) {
        if (c.kind === "vcan")
            return c.up ? qsTr("Virtual CAN bus, for socketcan://")
                        : qsTr("Virtual CAN bus, down: sudo ip link set %1 up").arg(c.name)
        const modes = c.modes || []
        const offers = m => !c.modesKnown || (c.modesSupported || []).indexOf(m) >= 0
        const listens = offers("listen-only")
        const sends = offers("one-shot")
        if (!listens && !sends)
            return qsTr("CAN interface: its driver offers neither listen-only nor one-shot mode, which socketcan:// needs")
        if (!c.up) {
            if (!sends)
                return qsTr("CAN interface, down: bring it up listen-only with ip link (its driver offers no one-shot mode, so it only listens)")
            if (!listens)
                return qsTr("CAN interface, down: bring it up one-shot with ip link (its driver offers no listen-only mode, so it only sends requests)")
            return qsTr("CAN interface, down: bring it up with ip link, listen-only to listen or one-shot to send requests")
        }
        const state = c.state && c.state !== "error-active" ? qsTr(", %1").arg(c.state) : ""
        if (modes.indexOf("listen-only") >= 0)
            return qsTr("CAN interface, listen-only at %1 bit/s%2, for listening").arg(c.bitrate).arg(state)
        if (modes.indexOf("one-shot") >= 0)
            return qsTr("CAN interface, one-shot at %1 bit/s%2, for requests").arg(c.bitrate).arg(state)
        if (!sends)
            return qsTr("CAN interface at %1 bit/s%2, not listen-only: its driver offers no one-shot mode, so bring it up listen-only to listen").arg(c.bitrate).arg(state)
        return qsTr("CAN interface at %1 bit/s%2, neither listen-only nor one-shot: socketcan:// refuses it until it is one").arg(c.bitrate).arg(state)
    }

    // A byte count in words: 512 B, 3.4 KB, 1.2 MB
    function byteText(n) {
        if (n < 1024)
            return qsTr("%1 B").arg(n)
        if (n < 1024 * 1024)
            return qsTr("%1 KB").arg((n / 1024).toFixed(1))
        return qsTr("%1 MB").arg((n / (1024 * 1024)).toFixed(1))
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

    // ---- Vehicles (ADR-0027): VehicleBook's document, stored as JSON
    // under vehicles/book (format 2, ADR-0064). A stored text that doesn't
    // parse leaves no vehicles, and is overwritten only by the next
    // change. The earlier format's key, vehicles/document, is an older
    // Havne's to read and is never written
    property var vehicles: VehicleBook.empty()
    readonly property var pickedVehicle: VehicleBook.picked(vehicles)
    // A book a later Havne saved is left as it is: none shown, none saved
    readonly property bool bookLater: VehicleBook.isLater(vehicles)
    // A removal waiting for its second press, asked where it was pressed:
    // "", "forget", "forgetAll", "removePlugin:<sha256>" (in the plugin's
    // open row), "removeOwn:<sha256>" (under the vehicle's own signals),
    // "removeVariant:<profile>" (in its row) or "removeUnmigrated:<key>"
    // (unmigratedKey, in the file's row)
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
        if (bookLater)
            return
        vehicles = doc
        vehicleStore.book = VehicleBook.serialize(doc)
        Bridge.vehicleLabel = pickedVehicle !== null ? pickedVehicle.label : ""
    }

    SettingsPicks { id: picker; host: page }

    // SettingsPicks's members others use
    property alias storeRev: picker.storeRev
    property alias vinRev: picker.vinRev
    property alias pluginList: picker.pluginList
    property alias pluginBySha: picker.pluginBySha
    property alias ownPlugins: picker.ownPlugins
    property alias refusedPlugins: picker.refusedPlugins
    property alias picks: picker.picks
    function picksNow() { return picker.picksNow() }
    property alias pickRows: picker.pickRows
    property alias signalProblem: picker.signalProblem
    property alias signalNote: picker.signalNote
    function placeOf(profile) { return picker.placeOf(profile) }
    function profileName(profile) { return picker.profileName(profile) }
    property alias variantRows: picker.variantRows
    function rowTitle(r) { return picker.rowTitle(r) }
    function profileTitle(profile) { return picker.profileTitle(profile) }
    function rowReads(r) { return picker.rowReads(r) }
    property alias missingPicks: picker.missingPicks
    function signalsSummary() { return picker.signalsSummary() }
    function togglePick(profile) { return picker.togglePick(profile) }
    function pickProfile(profile) { return picker.pickProfile(profile) }

    SettingsImport { id: importer; host: page }

    onOwnPluginsChanged: {
        if (importInto !== "" && !ownPlugins.some(p => p.sha256 === importInto))
            importInto = ""
    }

    // SettingsImport's members others use
    property alias pendingImport: importer.pendingImport
    property alias importInto: importer.importInto
    property alias replaceTarget: importer.replaceTarget
    function saidToo(r) { return importer.saidToo(r) }
    function importFile(url) { return importer.importFile(url) }
    function keepPending(into, variant, replacing) { return importer.keepPending(into, variant, replacing) }
    function discardImport() { return importer.discardImport() }
    function startReplace(row) { return importer.startReplace(row) }
    function importReplaces(variant) { return importer.importReplaces(variant) }
    property alias obdbReport: importer.obdbReport
    property alias obdbYearFromVin: importer.obdbYearFromVin
    property alias obdbRowsShown: importer.obdbRowsShown
    property alias obdbShown: importer.obdbShown
    property alias obdbReplaces: importer.obdbReplaces
    function noteCarYear() { return importer.noteCarYear() }
    function obdbVinYear() { return importer.obdbVinYear() }
    function previewObdb(typing) { return importer.previewObdb(typing) }
    function obdbSummary(r) { return importer.obdbSummary(r) }
    function keepObdb() { return importer.keepObdb() }

    SettingsRowActions { id: rowActions; host: page }

    // SettingsRowActions's members others use
    property alias openRow: rowActions.openRow
    property alias copyShown: rowActions.copyShown
    property alias exportNote: rowActions.exportNote
    function markMine(sha) { return rowActions.markMine(sha) }
    function copyVariantTo(row, into) { return rowActions.copyVariantTo(row, into) }
    function removeVariant(row) { return rowActions.removeVariant(row) }
    function askRemoveVariant(row) { return rowActions.askRemoveVariant(row) }
    function askRemovePlugin(sha) { return rowActions.askRemovePlugin(sha) }
    function confirmingPlugin() { return rowActions.confirmingPlugin() }
    function rowQuestion(entry, open) { return rowActions.rowQuestion(entry, open) }
    function ownQuestion() { return rowActions.ownQuestion() }
    function confirmRemoval() { return rowActions.confirmRemoval() }
    function removePlugin(sha) { return rowActions.removePlugin(sha) }
    property alias saving: rowActions.saving
    function variantSave(row) { return rowActions.variantSave(row) }
    function pluginSave(sha) { return rowActions.pluginSave(sha) }
    function unmigratedSave(record) { return rowActions.unmigratedSave(record) }
    function saveVariantAs(row) { return rowActions.saveVariantAs(row) }
    function exportPluginAs(sha) { return rowActions.exportPluginAs(sha) }
    function exportWaits(sha) { return rowActions.exportWaits(sha) }
    function openExport(sha) { return rowActions.openExport(sha) }
    function saveUnmigratedAs(record) { return rowActions.saveUnmigratedAs(record) }
    function saveTo(url) { return rowActions.saveTo(url) }
    property alias unmigratedRows: rowActions.unmigratedRows
    function unmigratedText(r) { return rowActions.unmigratedText(r) }
    function askRemoveUnmigrated(record) { return rowActions.askRemoveUnmigrated(record) }
    function unmigratedQuestion(record) { return rowActions.unmigratedQuestion(record) }
    function noteVinMatches() { return rowActions.noteVinMatches() }

    SettingsOwn { id: own; host: page }

    // ---- The vehicle's own signals (ADR-0051's form, ADR-0064 Q7): the
    // variant the form writes, in a plugin of the user's own ----
    // Where the own variant is, null for none or one the store lost
    readonly property var ownPlace: pickedVehicle === null || pickedVehicle.own === "" ? null
        : placeOf(pickedVehicle.own)

    // Its signal file as a document (SignalForm), null for none; the
    // form's choices that aren't text; what Read once last said
    readonly property var ownFile: {
        if (ownPlace === null)
            return null
        const r = Bridge.pluginVariantText(ownPlace.plugin.sha256, ownPlace.variant)
        return r.ok ? SignalForm.parseFile(r.text) : null
    }

    readonly property var ownSignals: ownFile === null ? [] : ownFile.signals

    // SettingsOwn's members others use
    property alias formService: own.formService
    property alias formOrder: own.formOrder
    property alias formKind: own.formKind
    property alias formGroup: own.formGroup
    property alias formProblem: own.formProblem
    property alias formNote: own.formNote
    property alias readResult: own.readResult
    property alias editingKey: own.editingKey
    property alias readOnceFor: own.readOnceFor
    property alias pickedId: own.pickedId
    function clearForm() { return own.clearForm() }
    function fillForm(f) { return own.fillForm(f) }
    function readOnceDraft() { return own.readOnceDraft() }
    function readResultText(r) { return own.readResultText(r) }
    function saveOwn(doc, note) { return own.saveOwn(doc, note) }
    function ownName(label) { return own.ownName(label) }
    function saveDraft() { return own.saveDraft() }
    function editOwnSignal(key) { return own.editOwnSignal(key) }
    function duplicateVehicle() { return own.duplicateVehicle() }
    function removeOwnSignal(key) { return own.removeOwnSignal(key) }
    function forgetVehicles(all) { return own.forgetVehicles(all) }
    function pickVehicle(id) { return own.pickVehicle(id) }
    function changeVehicles(doc) { return own.changeVehicles(doc) }
    function rememberPlotting() { return own.rememberPlotting() }
    property alias vinPickNote: own.vinPickNote
    function recognizeVehicle() { return own.recognizeVehicle() }
    property alias idle: own.idle

    // ---- Navigation: the list of pages, and the page shown. A phone shows
    // one at a time (the list is the root, a page pushes over it); 900 and
    // wider shows the list at the left and a page beside it. The pages are
    // all made once and shown or hidden, so the state in them (a typed
    // URI, a half-filled form) survives a visit elsewhere ----
    property string pageKey: ""    // "" is the list, on a phone
    readonly property bool twoPane: width >= 900
    readonly property string shownKey: twoPane && pageKey === "" ? "connection" : pageKey
    readonly property var pageList: [
        { key: "connection", title: qsTr("Connection"),
          summary: (devicesShown === 0 ? qsTr("none found") : qsTr("%1 found").arg(devicesShown))
                   + " · " + profileLabel() },
        { key: "vehicle", title: qsTr("Vehicle"),
          summary: (pickedVehicle !== null ? pickedVehicle.label : qsTr("None"))
                   + (vehicles.list.length > 0 ? qsTr(" · %1 saved").arg(vehicles.list.length) : "") },
        { key: "signals", title: qsTr("Signals"),
          summary: signalsSummary(),
          warn: missingPicks.length > 0 || refusedPlugins.length > 0 },
        { key: "alerts", title: qsTr("Alerts"),
          summary: Bridge.conditionsProblem !== "" ? qsTr("doesn't load")
                   : settingsAlerts.conditions.length === 0 ? qsTr("none")
                   : settingsAlerts.conditions.length === 1 ? qsTr("1 condition")
                   : qsTr("%1 conditions").arg(settingsAlerts.conditions.length),
          warn: Bridge.conditionsProblem !== "" },
        { key: "recording", title: qsTr("Recording"),
          summary: !Bridge.recording ? qsTr("off")
                   : Bridge.armedActive ? qsTr("on · armed")
                   : Bridge.recordingActive && !Bridge.logActive ? qsTr("on · recording") : qsTr("on") },
        { key: "display", title: qsTr("Display"),
          summary: qsTr("%1 %").arg(Math.round(Theme.zoom * 100))
                   + (Bridge.unitSystem === 1 ? qsTr(" · imperial") : qsTr(" · metric"))
                   + (Theme.lookSummary() !== "" ? " · " + Theme.lookSummary() : "")
                   + " · " + Theme.nightSummary() },
        { key: "advanced", title: qsTr("Advanced"),
          summary: Theme.developerMode ? qsTr("developer mode") : qsTr("off") }
    ].concat(Theme.compact ? [{ key: "log", title: qsTr("Activity"), summary: "" }] : [])

    // The tab-bar form has no sidebar to list the Log: its row, last, opens
    // the Log page itself, as does Show log beside a failed connect
    signal logRequested()
    // Back from Connection while Other address... sent the user here: the
    // page that sent them, as Android's Back goes
    signal leaveRequested()
    // The newest session event, the Log row's summary: the Log's first line
    // until one comes. The display's inset lines are about the screen, not
    // the session, so they don't take it
    property string lastActivity: qsTr("Ready.")
    Connections {
        target: Bridge
        function onLogMessage(message) {
            if (!message.startsWith("Display:"))
                page.lastActivity = message
        }
    }

    function open(key) {
        if (key === "log") {
            logRequested()
            return
        }
        pageKey = key
    }
    // Back from a page: the form returns to Signals, an alert's editor to
    // Alerts, the rest to the list
    function back() {
        if (backLeaves && pageKey === "connection") {
            leaveRequested()
            return
        }
        pageKey = pageKey === "ownform" ? "signals" : pageKey === "alertedit" ? "alerts" : ""
    }
    // A gauge's Alert (ADR-0063 section 9): the editor on a new condition,
    // a copy of the gauge's levels; "" when it opened, else why not (an
    // edit waiting in the editor, no room, a file that doesn't load)
    function openAlertDraft(c) {
        if (pageKey === "alertedit" && settingsAlertEditor.changed())
            return qsTr("An alert's edit waits in Settings > Alerts: save or cancel it first.")
        return settingsAlerts.openDraft(c)
    }
    // The form for a new signal (Add a signal), as a page of its own
    function openOwnForm() {
        open("ownform")
    }
    // The phone's Bluetooth permission is asked for when what to connect to
    // is seen: Connection is the page shown and Settings is on screen. Not
    // at launch, and not when the width crosses 900 while Settings is
    // hidden. bluetoothAsks counts the asks
    property int bluetoothAsks: 0
    function askBluetoothIfSeen() {
        if (visible && shownKey === "connection") {
            ++bluetoothAsks
            Bluetooth.refresh(true)
        }
    }
    // Back from Android's settings (or any other app) with Connection
    // shown: the permission may have changed there, so the message and the
    // Open settings button follow it with no tap. Reads the answer only:
    // never an ask, never a scan. bluetoothRechecks counts them
    property int bluetoothRechecks: 0
    function recheckBluetoothIfSeen() {
        if (visible && shownKey === "connection") {
            ++bluetoothRechecks
            Bluetooth.refresh(false)
        }
    }
    // qmllint disable missing-property
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state === Qt.ApplicationActive)
                page.recheckBluetoothIfSeen()
        }
    }
    // qmllint enable missing-property
    onShownKeyChanged: askBluetoothIfSeen()
    onVisibleChanged: askBluetoothIfSeen()

    // Back from Connection opened through Dash's "Other address..." or a
    // refused Replay: Main goes back to that page instead of this list, and
    // clears it when another page than the Log is shown
    property bool backLeaves: false
    onPageKeyChanged: if (pageKey !== "connection") backLeaves = false
    // The back key repeats the back arrow, and answers whether it had a page to close
    function closeShown() {
        if (twoPane || pageKey === "")
            return false
        back()
        return true
    }
    focus: true
    Keys.onBackPressed: event => {
        event.accepted = !backLeaves && closeShown()
    }

    // Connect... on Home, Live or the header lands here: what to connect to
    function showAdapters() {
        open("connection")
        connectionPage.contentY = 0
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

    // A recording's replay:// URI from the Recordings page (Main's
    // replayRecording), under the profile it was recorded with, so a phone
    // needs nothing typed. Set like typed text, so the profile row and the
    // DID field read it
    function pickRecording(uri) {
        // The field is on Connection, which a phone may not be showing
        open("connection")
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
        const body = connectionPage.body
        // Under the pinned title row, not behind it
        const top = uriField.mapToItem(body, 0, 0).y + body.y - Theme.spacing
                    - connectionPage.headerHeight
        if (top < connectionPage.contentY)
            connectionPage.contentY = Math.max(0, top)
    }

    SettingsUri { id: uriPart; host: page }

    // SettingsUri's members others use
    property alias profileChoice: uriPart.profileChoice
    property alias extSession: uriPart.extSession
    property alias profiles: uriPart.profiles
    property alias adapterUri: uriPart.adapterUri
    property alias standIn: uriPart.standIn
    property alias pickedSets: uriPart.pickedSets
    property alias profileNames: uriPart.profileNames
    property alias offlineUri: uriPart.offlineUri
    property alias syncing: uriPart.syncing
    property alias composing: uriPart.composing
    function profileHint() { return uriPart.profileHint() }
    function splitUri(text) { return uriPart.splitUri(text) }
    function queryValue(pairs, key) { return uriPart.queryValue(pairs, key) }
    function uriBase() { return uriPart.uriBase() }
    function sessionOffered(base) { return uriPart.sessionOffered(base) }
    function fillUri(base) { return uriPart.fillUri(base) }
    function chooseProfile(name) { return uriPart.chooseProfile(name) }
    function recompose() { return uriPart.recompose() }
    function readChoices() { return uriPart.readChoices() }

    SettingsConnectFrom { id: connectFrom; host: page }

    // SettingsConnectFrom's members others use
    property alias connectKeeps: connectFrom.connectKeeps
    function requestConnect(uri, kept) { return connectFrom.requestConnect(uri, kept) }
    property alias recentUris: connectFrom.recentUris
    function keptUri(uri) { return connectFrom.keptUri(uri) }
    function parseRecentUris(text, lastUri) { return connectFrom.parseRecentUris(text, lastUri) }
    function withRecentUri(list, uri) { return connectFrom.withRecentUri(list, uri) }
    property alias vehicleUri: connectFrom.vehicleUri
    property alias connectUris: connectFrom.connectUris
    property alias connectAdapters: connectFrom.connectAdapters
    function listConnectUri(uri) { return connectFrom.listConnectUri(uri) }
    function listProfileWord(uri) { return connectFrom.listProfileWord(uri) }
    property alias connectProfileWords: connectFrom.connectProfileWords
    function connectFromList(uri) { return connectFrom.connectFromList(uri) }

    // ---- The last URI that connected, the recent ones, the Record switch
    // and each device's versions (seenVersions, as JSON), kept between runs
    // as Main.qml keeps the zoom and units. Only a URI that connected is
    // stored, so a typo or a refused query doesn't come back
    Settings {
        id: sessionSettings
        category: "session"
        property string uri: ""
        property string recentUris: "[]"
        property bool recording: false
        property string adapterVersions: "{}"
    }
    Settings {
        id: vehicleStore
        category: "vehicles"
        property string book: ""
    }

    Component.onCompleted: {
        try {
            const seen = JSON.parse(sessionSettings.adapterVersions)
            if (seen !== null && typeof seen === "object" && !Array.isArray(seen))
                seenVersions = seen
        } catch (e) {
            // a damaged store shows no versions until the next connect
        }
        vehicles = VehicleBook.parse(vehicleStore.book)
        Bridge.vehicleLabel = pickedVehicle !== null ? pickedVehicle.label : ""
        refreshDevices()
        Bluetooth.refresh(false)   // asked for when Connection first opens
        // A replay or mock address an earlier version stored is dropped
        if (!keptUri(sessionSettings.uri))
            sessionSettings.uri = ""
        // Set like typed text, so the profile row and the DID field read
        // it back; as a list would connect it, like a picked vehicle's
        if (uriField.text === "")
            uriField.text = listConnectUri(sessionSettings.uri)
        recentUris = parseRecentUris(sessionSettings.recentUris, sessionSettings.uri)
        if (JSON.stringify(recentUris) !== sessionSettings.recentUris)
            sessionSettings.recentUris = JSON.stringify(recentUris)
        Bridge.recording = sessionSettings.recording
    }

    Connections {
        target: Bridge

        function onReadOnceResult(result) {
            if (page.readOnceFor === page.pickedId)
                page.readResult = result
        }
        function onConnectResult(uri, error) {
            const keeps = page.connectKeeps !== "" ? page.connectKeeps : uri
            page.errorLabel.text = error
            page.errorLabel.visible = (error !== "")
            page.logExplains = (error !== "")
            if (error === "") {
                if (page.keptUri(keeps)) {
                    sessionSettings.uri = keeps
                    page.recentUris = page.withRecentUri(page.recentUris, keeps)
                    sessionSettings.recentUris = JSON.stringify(page.recentUris)
                }
                page.rememberVersions(uri, Bridge.adapterVersions)
                // The picked vehicle's connection and plotted signals,
                // for an adapter's session only (ADR-0027 section 1, section 2)
                if (VehicleBook.isAdapterUri(uri) && page.pickedVehicle !== null) {
                    page.saveVehicles(VehicleBook.rememberUri(page.vehicles, keeps))
                    Bridge.signalModel.setPlotChoices(page.pickedVehicle.plotted,
                                                      page.pickedVehicle.unplotted)
                }
                page.plottedApplied = true
            }
        }

        function onConnectedChanged() {
            if (!Bridge.connected)
                page.plottedApplied = false
            // The session's VIN, read again at each, goes at its end; the
            // matches it made stay on the vehicle it identified
            ++page.vinRev
        }

        function onVehicleIdentified() {
            page.recognizeVehicle()
            ++page.vinRev
            page.noteVinMatches()
            page.noteCarYear()
        }

        function onRecordingChanged() {
            sessionSettings.recording = Bridge.recording
        }

    }

    // A tick or an untick on Live is the picked vehicle's from now on
    Connections {
        target: Bridge.signalModel

        // Once for each change of choices, a text row's tick included
        function onTicksChanged() {
            if (page.plottedApplied && page.pickedVehicle !== null
                    && VehicleBook.isAdapterUri(Bridge.currentUri))
                page.rememberPlotting()
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        SettingsList {
            objectName: "settingsSideList"
            stacked: true
            visible: page.twoPane
            Layout.preferredWidth: 200
            // A nested layout fills its row by default, which would leave
            // the pages beside it no width
            Layout.fillWidth: false
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignTop
            Layout.margins: Theme.pageMargin
            pages: page.pageList
            current: page.shownKey
            onChosen: key => page.open(key)
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // The list, a phone's root
            SettingsSubpage {
                objectName: "settingsListPage"
                anchors.fill: parent
                visible: !page.twoPane && page.pageKey === ""
                title: qsTr("Settings")

                SettingsList {
                    objectName: "settingsList"
                    Layout.fillWidth: true
                    pages: page.pageList
                    logSummary: page.lastActivity
                    onChosen: key => page.open(key)
                }

                // The phone has no Home page to show it, and a bug report needs it
                Label {
                    objectName: "settingsCoreVersion"
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.spacing
                    font.pixelSize: Theme.fontCaption
                    color: Theme.textDim
                    text: qsTr("core %1").arg(Bridge.coreVersion)
                }
            }

            SettingsSubpage {
                id: connectionPage
                objectName: "settingsConnectionPage"
                anchors.fill: parent
                visible: page.shownKey === "connection"
                title: qsTr("Connection")
                backShown: !page.twoPane
                onBackRequested: page.back()

                SettingsConnect {
                    id: settingsConnect
                    host: page
                }

                SettingsAdapter {
                    id: settingsAdapter
                    host: page
                }

                SettingsProfile {
                    id: settingsProfile
                    host: page
                }
            }

            SettingsSubpage {
                objectName: "settingsVehiclePage"
                anchors.fill: parent
                visible: page.shownKey === "vehicle"
                title: qsTr("Vehicle")
                backShown: !page.twoPane
                onBackRequested: page.back()

                SettingsVehiclePicker {
                    id: settingsVehiclePicker
                    host: page
                }

                SettingsVinLookup {
                    id: settingsVinLookup
                    host: page
                }
            }

            SettingsSubpage {
                objectName: "settingsSignalsPage"
                anchors.fill: parent
                visible: page.shownKey === "signals"
                title: qsTr("Signals")
                backShown: !page.twoPane
                onBackRequested: page.back()

                SettingsSignals {
                    id: settingsSignals
                    host: page
                }

                SettingsOwnSignals {
                    id: settingsOwnSignals
                    host: page
                }
            }

            SettingsSubpage {
                objectName: "settingsOwnFormPage"
                anchors.fill: parent
                visible: page.shownKey === "ownform"
                title: qsTr("Add a signal")
                backName: qsTr("Back to Signals")
                backShown: true
                onBackRequested: page.back()

                SettingsOwnForm {
                    id: settingsOwnForm
                    host: page
                }
            }

            SettingsSubpage {
                objectName: "settingsAlertsPage"
                anchors.fill: parent
                visible: page.shownKey === "alerts"
                title: qsTr("Alerts")
                backShown: !page.twoPane
                onBackRequested: page.back()

                SettingsAlerts {
                    id: settingsAlerts
                    host: page
                }
            }

            SettingsSubpage {
                objectName: "settingsAlertEditPage"
                anchors.fill: parent
                visible: page.shownKey === "alertedit"
                title: settingsAlertEditor.isNew ? qsTr("New alert") : qsTr("Edit alert")
                backName: qsTr("Back to Alerts")
                backShown: true
                onBackRequested: page.back()

                SettingsAlertEditor {
                    id: settingsAlertEditor
                    host: page
                    alerts: settingsAlerts
                }
            }

            SettingsSubpage {
                objectName: "settingsRecordingPage"
                anchors.fill: parent
                visible: page.shownKey === "recording"
                title: qsTr("Recording")
                backShown: !page.twoPane
                onBackRequested: page.back()

                SettingsRecording {
                    id: settingsRecording
                    host: page
                }
            }

            SettingsSubpage {
                objectName: "settingsDisplayPage"
                anchors.fill: parent
                visible: page.shownKey === "display"
                title: qsTr("Display")
                backShown: !page.twoPane
                onBackRequested: page.back()

                SettingsDisplay {
                    id: settingsDisplay
                    host: page
                }
            }

            SettingsSubpage {
                objectName: "settingsAdvancedPage"
                anchors.fill: parent
                visible: page.shownKey === "advanced"
                title: qsTr("Advanced")
                backShown: !page.twoPane
                onBackRequested: page.back()

                SettingsAdvanced {
                    id: settingsAdvanced
                    host: page
                }
            }
        }
    }
}
