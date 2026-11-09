// File/layer: app/qml/SettingsPage.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.linkhealth, QmlTest.log,
//          QmlTest.phone (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

Pane {
    id: page
    padding: 0
    background: Rectangle { color: Theme.bg }

    property alias uriField: settingsConnect.uriField
    property alias errorLabel: settingsConnect.errorLabel
    property alias vehicleLabelField: settingsVehiclePicker.vehicleLabelField
    property alias obdbYearField: settingsSignalFiles.obdbYearField
    property alias saveDialog: settingsSignalFiles.saveDialog
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
        useSignalFiles()
    }

    // ---- The vehicles' signal files (ADR-0051) ----
    // The store's word on each file the book references, by its sha256:
    // { ok, words, name, profile, count }. Every referenced file is
    // registered (VehicleBook.profileProblem keeps one file to a
    // profile), so a URI typed with any vehicle's profile connects.
    property var signalInfo: ({})
    property string signalProblem: ""
    property string signalNote: ""

    function useSignalFiles() {
        const info = {}
        for (const e of Bridge.useSignalFiles(VehicleBook.referencedFiles(vehicles)))
            info[e.sha256] = e
        signalInfo = info
    }

    function signalOk(sha) {
        const e = signalInfo[sha]
        return e !== undefined && e.ok
    }

    function signalFileText(f) {
        const e = signalInfo[f.sha256]
        if (e === undefined)
            return f.name
        if (!e.ok)
            return qsTr("%1: %2").arg(f.name, e.words)
        return e.extends === "obd2"
            ? qsTr("%1: obd2 and %2 of your own, profile %3").arg(f.name).arg(e.own).arg(f.profile)
            : e.count === 1 ? qsTr("%1: 1 signal, profile %2").arg(f.name).arg(f.profile)
            : qsTr("%1: %2 signals, profile %3").arg(f.name).arg(e.count).arg(f.profile)
    }

    // A file the user picked, into the picked vehicle: refused with the
    // loader's words, or the book's when another file makes its profile
    // (its copy then goes, unless a vehicle keeps it). True when added
    function importSignalFile(url) {
        signalNote = ""
        if (pickedVehicle === null)
            return false
        const r = Bridge.importSignalFile(url)
        if (!r.ok) {
            signalProblem = r.words
            return false
        }
        return addKeptFile(r)
    }

    // What a kept file's import said, as the notes put it
    function keptText(r) {
        return r.extends === "obd2"
            ? qsTr("obd2 and %1 of your own, profile %2").arg(r.own).arg(r.profile)
            : r.count === 1 ? qsTr("1 signal, profile %1").arg(r.profile)
            : qsTr("%1 signals, profile %2").arg(r.count).arg(r.profile)
    }

    // A file the store kept (importSignalFile's map, or an OBDb import's)
    // into the picked vehicle; true when added
    function addKeptFile(r) {
        const file = { sha256: r.sha256, name: r.name, profile: r.profile }
        const problem = VehicleBook.profileProblem(vehicles, file, "")
        if (problem !== "") {
            signalProblem = problem
            Bridge.deleteUnusedSignalFiles(VehicleBook.referencedFiles(vehicles))
            return false
        }
        signalProblem = ""
        if (pickedVehicle.signals.some(e => e.sha256 === r.sha256)) {
            signalNote = qsTr("%1 is this vehicle's already").arg(r.name)
            return false
        }
        saveVehicles(VehicleBook.addSignals(vehicles, file))
        signalNote = qsTr("%1 added: %2").arg(r.name).arg(keptText(r))
        // The field on an adapter reading Default, obd2, with no profile=
        // of its own: the file that now stands in for obd2 is read
        // instead, and the note says so. A profile the user picked stays
        if (standIn !== null && standIn.sha256 === r.sha256 && profileChoice === "default"
                && queryValue(splitUri(uriField.text).pairs, "profile") === undefined) {
            chooseProfile(r.profile)
            signalNote = qsTr("%1 added: now reading obd2 + %1, profile %2")
                             .arg(r.name).arg(r.profile)
        }
        return true
    }

    // ---- An OBDb set imported (ADR-0051's appendix of 2026-11-06) ----
    // The report of the set read and not yet kept (the bridge's
    // previewObdb map, null for none), and the vehicle's earlier import
    // it replaces ("" for none)
    property var obdbReport: null
    property string obdbReplaces: ""
    property string obdbVehicle: "" // the id of the vehicle it was read for
    // A list's rows shown at most, the rest counted: a set of thousands
    // of signals would otherwise lay out thousands of labels on a phone
    readonly property int obdbRowsShown: 50
    readonly property bool obdbShown: obdbReport !== null && pickedVehicle !== null
                                      && pickedVehicle.id === obdbVehicle

    // The connected car's model year once its VIN names the picked
    // vehicle (its VIN key), kept while the page lives, as the plugins'
    // matches stay once the session ends; the VIN itself stays in the
    // bridge, and the year is never stored
    property int obdbCarYear: 0
    property string obdbCarVehicle: ""

    function noteCarYear() {
        if (pickedVehicle === null || pickedVehicle.vinKey === ""
                || Bridge.vinKey(vehicles.salt) !== pickedVehicle.vinKey)
            return
        obdbCarYear = Bridge.vinModelYear("")
        obdbCarVehicle = pickedVehicle.id
    }

    // The model year a VIN gives: the lookup's typed one, else the
    // connected car's when it was the picked vehicle, else 0
    function obdbVinYear() {
        if (vinLookupField.text.trim() !== "")
            return Bridge.vinModelYear(vinLookupField.text)
        return pickedVehicle !== null && pickedVehicle.id === obdbCarVehicle ? obdbCarYear : 0
    }

    // The year field as a number: 0 when empty, -1 when it isn't a year
    function obdbYear() {
        const t = obdbYearField.text.trim()
        if (t === "")
            return 0
        const y = Number(t)
        return /^[0-9]{4}$/.test(t) && y >= 1980 && y <= 2039 ? y : -1
    }

    // The picked file read for the picked vehicle: its report shown, or
    // the bridge's words why not. True when there's a report
    function previewObdb(url) {
        signalNote = ""
        obdbReport = null
        if (pickedVehicle === null)
            return false
        const year = obdbYear()
        if (year < 0) {
            signalProblem = qsTr("A model year is 1980 to 2039, or empty for none")
            return false
        }
        const n = VehicleBook.obdbName(vehicles, pickedVehicle.label,
                                       sha => Bridge.isObdbImport(sha))
        const r = Bridge.previewObdb(url, year, n.name)
        if (!r.ok) {
            signalProblem = r.words
            return false
        }
        signalProblem = ""
        obdbReplaces = n.replaces
        obdbVehicle = pickedVehicle.id
        obdbReport = r
        return true
    }

    // The report's summary line
    function obdbSummary(r) {
        if (r === null)
            return ""
        const parts = [r.converted === 1 ? qsTr("1 signal converted")
                                         : qsTr("%1 signals converted").arg(r.converted),
                       r.changed.length === 1 ? qsTr("1 field differs")
                                              : qsTr("%1 fields differ").arg(r.changed.length),
                       qsTr("%1 left out").arg(r.skipped.length),
                       r.year > 0 ? qsTr("model year %1").arg(r.year)
                                  : qsTr("no model year, so the commands OBDb gives for some years only are left out")]
        let s = qsTr("%1 from OBDb: %2").arg(r.name).arg(parts.join(", "))
        if (obdbReplaces !== "")
            s += qsTr("; replaces the vehicle's earlier OBDb import")
        return s
    }

    // The report kept: the file into the picked vehicle, in its earlier
    // import's place when it replaces one. True when the vehicle has it
    function keepObdb() {
        signalNote = ""
        if (!obdbShown)
            return false
        const replaces = obdbReplaces
        const r = Bridge.keepObdb(replaces)
        if (!r.ok) {
            // A session reading the earlier import keeps the report up,
            // so Keep works once it has disconnected
            signalProblem = r.words
            return false
        }
        obdbReport = null
        obdbReplaces = ""
        if (replaces === "" || !pickedVehicle.signals.some(f => f.sha256 === replaces))
            return addKeptFile(r)
        if (r.sha256 === replaces) {
            signalProblem = ""
            signalNote = qsTr("%1 is this vehicle's already").arg(r.name)
            return true
        }
        const file = { sha256: r.sha256, name: r.name, profile: r.profile }
        const problem = VehicleBook.profileProblem(vehicles, file, replaces)
        if (problem !== "") {
            signalProblem = problem
            Bridge.deleteUnusedSignalFiles(VehicleBook.referencedFiles(vehicles))
            return false
        }
        signalProblem = ""
        saveVehicles(VehicleBook.replaceSignals(vehicles, replaces, file))
        Bridge.deleteUnusedSignalFiles(VehicleBook.referencedFiles(vehicles))
        signalNote = qsTr("%1 replaced the vehicle's earlier OBDb import: %2")
                         .arg(r.name).arg(keptText(r))
        return true
    }

    function discardObdb() {
        Bridge.dropObdb()
        obdbReport = null
        obdbReplaces = ""
        signalProblem = ""
        signalNote = ""
    }

    // ---- The app's plugins (ADR-0052) ----
    // The bridge's list (PluginStore): each loaded plugin with its
    // variants, or a copy that no longer loads; the variants its VIN
    // proposes for the car (the session's VIN or the one the lookup holds,
    // never stored); and what the last import or removal said
    property var pluginList: []
    property var pluginMatches: []
    property string pluginProblem: ""
    property string pluginNote: ""

    function loadPlugins() {
        pluginList = Bridge.plugins()
        refreshMatches()
    }

    function refreshMatches() {
        pluginMatches = Bridge.pluginMatches(vinLookupField.text)
    }

    function pluginText(p) {
        return p.ok ? qsTr("%1 %2: %3").arg(p.name, p.version, p.variants.join(", "))
                    : p.words
    }

    function importPlugin(url) {
        pluginNote = ""
        const r = Bridge.importPlugin(url)
        if (!r.ok) {
            pluginProblem = r.words
            return false
        }
        pluginProblem = ""
        pluginNote = r.replaced !== ""
            ? qsTr("%1 %2 replaces %3").arg(r.name, r.version, r.replaced)
            : qsTr("%1 %2 added: %3").arg(r.name, r.version, r.profiles.join(", "))
        loadPlugins()
        return true
    }

    // A removed plugin's variants are no vehicle's pick any more, and the
    // field reads Default when it named one
    function removePlugin(sha) {
        pluginNote = ""
        const entry = pluginList.find(p => p.sha256 === sha)
        const profiles = entry !== undefined && entry.ok ? entry.profiles : []
        const r = Bridge.removePlugin(sha)
        pluginProblem = r.ok ? "" : r.words
        if (r.ok && profiles.length > 0) {
            saveVehicles(VehicleBook.dropPlugin(vehicles, profiles))
            if (profiles.indexOf(profileChoice) >= 0)
                chooseProfile("default")
        }
        loadPlugins()
    }

    // The user's pick for this vehicle (ADR-0052 section 5: the VIN only
    // proposes): kept in its entry, and read through the URI's profile=.
    // The pick pressed again is undone
    function pickPluginVariant(profile) {
        if (pickedVehicle !== null && pickedVehicle.plugin === profile) {
            saveVehicles(VehicleBook.setPlugin(vehicles, ""))
            if (profileChoice === profile)
                chooseProfile("default")
            return
        }
        saveVehicles(VehicleBook.setPlugin(vehicles, profile))
        chooseProfile(profile)
    }

    // The Signals row's summary: a count of files, one or many (no
    // translation is loaded to pick a plural form)
    function filesSummary(n) {
        return n === 1 ? qsTr("1 file") : qsTr("%1 files").arg(n)
    }

    // The picked vehicle's variant as the Profile buttons offer it: null for
    // none, or one no plugin loaded registers
    function pluginChoice() {
        if (pickedVehicle === null || pickedVehicle.plugin === "")
            return null
        for (const p of pluginList) {
            const at = p.ok ? p.profiles.indexOf(pickedVehicle.plugin) : -1
            if (at >= 0)
                return { name: p.profiles[at], label: qsTr("%1: %2").arg(p.name, p.variants[at]),
                         hint: qsTr("The plugin %1 %2, its variant %3, picked for this vehicle")
                                   .arg(p.name, p.version, p.variants[at]) }
        }
        return null
    }

    // The vehicle's own file was written here and has no copy anywhere
    // else, so its removal asks first
    function removeSignalFile(sha) {
        if (pickedVehicle !== null && pickedVehicle.own === sha)
            confirming = "removeSignals:" + sha
        else
            dropSignalFile(sha)
    }

    function dropSignalFile(sha) {
        confirming = ""
        signalProblem = ""
        signalNote = ""
        saveVehicles(VehicleBook.removeSignals(vehicles, sha))
        Bridge.deleteUnusedSignalFiles(VehicleBook.referencedFiles(vehicles))
    }

    // A Save out of the app: the copy the vehicle has, byte for byte, where
    // the user chooses (the desktop's dialog, Android's document picker),
    // so it can be shared or given to havne-cli --signals. Its name to
    // start from is the profile's slug
    property string savingSha: ""

    function saveFileName(f) {
        const slug = String(f.profile).replace(/^user\./, "")
        return (slug !== "" ? slug : "signals") + ".json"
    }

    function saveSignalFileAs(f) {
        savingSha = f.sha256
        saveDialog.selectedFile = saveDialog.currentFolder.toString().replace(/\/$/, "")
                                  + "/" + saveFileName(f)
        saveDialog.open()
    }

    function saveSignalFileTo(sha, url) {
        const f = pickedVehicle !== null ? pickedVehicle.signals.find(e => e.sha256 === sha)
                                         : undefined
        const r = Bridge.saveSignalFile(sha, url)
        signalProblem = r.ok ? "" : r.words
        signalNote = r.ok ? qsTr("%1 saved to the file you chose")
                                .arg(f !== undefined ? f.name : qsTr("The signal file"))
                          : ""
        return r.ok
    }

    function confirmingFile() {
        if (!confirming.startsWith("removeSignals:") || pickedVehicle === null)
            return null
        const sha = confirming.substring("removeSignals:".length)
        return pickedVehicle.signals.find(f => f.sha256 === sha) || null
    }

    // ---- The vehicle's own signals (ADR-0051's form) ----
    // The own file as a document (SignalForm), null for none; the form's
    // choices that aren't text; what Read once last said
    readonly property var ownFile: pickedVehicle === null || pickedVehicle.own === "" ? null
        : SignalForm.parseFile(Bridge.signalFileText(pickedVehicle.own))
    readonly property var ownSignals: ownFile === null ? [] : ownFile.signals
    property string formService: "22"
    property string formOrder: "be"
    property string formKind: "number"
    property string formGroup: ""
    property string formProblem: ""
    property string formNote: ""
    property var readResult: null
    // The key of the signal Edit filled the form with, "" for a new one:
    // its Save replaces that signal, renamed or not, and ends the Edit
    property string editingKey: ""
    // The vehicle a Read once was asked for: its reply shows only there
    property string readOnceFor: ""

    // The form and the page's notes are the picked vehicle's: a pick,
    // New, Duplicate, Forget or a car recognized starts them afresh, and a
    // question asked about the last one goes with it
    readonly property string pickedId: pickedVehicle !== null ? pickedVehicle.id : ""
    onPickedIdChanged: {
        confirming = ""
        signalProblem = ""
        signalNote = ""
        clearForm()
        dropAnotherVehiclesProfile()
    }

    // A file's or a plugin's profile in the field is one vehicle's: once
    // another is picked (or none), the field reads Default, so a connect
    // never decodes with the last vehicle's signals and remembers them as
    // this one's. A vehicle with a connection of its own then sets the
    // field (pickVehicle); a session's URI is left as it is until the
    // session ends, since a VIN may pick another vehicle while it runs
    onIdleChanged: {
        if (idle)
            dropAnotherVehiclesProfile()
    }

    function dropAnotherVehiclesProfile() {
        if (!idle)
            return
        const p = profileChoice
        if (!p.startsWith("user.") && !p.startsWith("plugin."))
            return
        const mine = pickedVehicle === null ? []
            : pickedVehicle.signals.map(f => f.profile)
                  .concat(pickedVehicle.plugin !== "" ? [pickedVehicle.plugin] : [])
        if (mine.indexOf(p) < 0)
            chooseProfile("default")
    }

    function clearForm() {
        fillForm(SignalForm.emptyFields())
    }

    function readForm() {
        return { name: formName.text, rx: formRx.text, tx: formTx.text, service: formService,
                 number: formNumber.text, byte: formByte.text, bits: formBits.text,
                 bit: formBit.text, order: formOrder, signed: formSigned.checked,
                 bytes: formBytes.text, scale: formScale.text, offset: formOffset.text,
                 unit: formUnit.text, decimals: formDecimals.text, group: formGroup,
                 kind: formKind, values: formValues.text }
    }

    function fillForm(f) {
        formName.text = f.name
        formRx.text = f.rx
        formTx.text = f.tx
        formService = f.service
        formNumber.text = f.number
        formByte.text = f.byte
        formBits.text = f.bits
        formBit.text = f.bit
        formOrder = f.order
        formSigned.checked = f.signed
        formBytes.text = f.bytes
        formScale.text = f.scale
        formOffset.text = f.offset
        formUnit.text = f.unit
        formDecimals.text = f.decimals
        formGroup = f.group
        formKind = f.kind
        formValues.text = f.values
        formProblem = ""
        formNote = ""
        readResult = null
        editingKey = ""
    }

    // The adapter Read once asks on: the field's, when it names one, else
    // the vehicle's last
    function readOnceAdapter() {
        const typed = uriField.text.trim()
        if (/^(j2534|elm327|socketcan):\/\//i.test(typed))
            return typed
        return pickedVehicle !== null ? pickedVehicle.uri : ""
    }

    function readOnceDraft() {
        formProblem = ""
        formNote = ""
        readResult = null
        const f = readForm()
        readOnceFor = pickedId
        const why = Bridge.readOnce(readOnceAdapter(), SignalForm.draft(f), SignalForm.moduleOf(f))
        if (why !== "")
            formProblem = why
    }

    function readResultText(r) {
        if (r === null)
            return ""
        if (r.ok)
            return qsTr("Asked %1, got %2: %3").arg(r.request, r.reply, r.value)
        return r.reply !== "" ? qsTr("Asked %1, got %2: %3").arg(r.request, r.reply, r.words)
                              : qsTr("Asked %1: %2").arg(r.request, r.words)
    }

    // doc as the vehicle's own file, in place of the one before. The
    // loader's words when it refuses it, without a line and column of a
    // file the user never sees; the book's when another file makes its
    // profile. True when saved
    function saveOwn(doc, note) {
        const r = Bridge.importSignalText(JSON.stringify(doc, null, 2))
        if (!r.ok) {
            formProblem = String(r.words).replace(/^line \d+, column \d+: /, "")
            return false
        }
        const file = { sha256: r.sha256, name: r.name, profile: r.profile }
        const problem = VehicleBook.profileProblem(vehicles, file, pickedVehicle.own)
        if (problem !== "") {
            formProblem = problem
            Bridge.deleteUnusedSignalFiles(VehicleBook.referencedFiles(vehicles))
            return false
        }
        saveVehicles(VehicleBook.setOwn(vehicles, file))
        Bridge.deleteUnusedSignalFiles(VehicleBook.referencedFiles(vehicles))
        formProblem = ""
        formNote = note
        return true
    }

    // The form's signal into the own file, made with the first under a
    // name no other file's profile has; the signal Edit filled the form
    // with replaced, or a new one's of the same name. Another signal whose
    // name makes the same key is not overwritten: the form says so
    function saveDraft() {
        formNote = ""
        readResult = null
        if (pickedVehicle === null)
            return false
        const f = readForm()
        const signal = SignalForm.draft(f)
        const clash = ownSignals.find(s => s.key === signal.key && s.key !== editingKey
                                           && (editingKey !== "" || s.name !== signal.name))
        if (clash !== undefined) {
            formProblem = qsTr("%1, saved already, has the key %2 too: edit it, or give "
                               + "this one another name").arg(clash.name, signal.key)
            return false
        }
        const base = ownFile !== null ? ownFile
            : SignalForm.emptyFile(pickedVehicle.label,
                                   VehicleBook.ownName(vehicles, pickedVehicle.label))
        const module = SignalForm.moduleOf(f)
        // One request ID to a module: a typed one that differs moves the
        // module's other signals too, and the note names them
        const move = SignalForm.requestIdMove(base, signal, module, editingKey)
        const note = move === null ? qsTr("%1 saved in %2").arg(signal.name, base.name)
            : qsTr("%1 saved in %2; %3 asks on %4 for %5 too")
                  .arg(signal.name, base.name, move.rx, move.tx, move.names.join(", "))
        if (!saveOwn(SignalForm.withSignal(base, signal, module, editingKey), note))
            return false
        editingKey = ""   // saved: the Edit is over
        return true
    }

    function editOwnSignal(key) {
        const s = ownSignals.find(x => x.key === key)
        if (s !== undefined) {
            fillForm(SignalForm.fieldsOf(ownFile, s))
            editingKey = key
        }
    }

    // A copy of the picked vehicle; its own file is copied under a name of
    // its own, so each vehicle's own signals change apart (one file to a
    // profile). Its copy unreadable, the new vehicle starts with none
    function duplicateVehicle() {
        const from = pickedVehicle
        let doc = VehicleBook.duplicate(vehicles, copyLabel)
        if (from !== null && from.own !== "") {
            const own = SignalForm.parseFile(Bridge.signalFileText(from.own))
            const r = own === null ? { ok: false } : (() => {
                own.name = VehicleBook.ownName(doc, VehicleBook.picked(doc).label)
                return Bridge.importSignalText(JSON.stringify(own, null, 2))
            })()
            doc = r.ok ? VehicleBook.setOwn(doc, { sha256: r.sha256, name: r.name,
                                                   profile: r.profile })
                       : VehicleBook.removeSignals(doc, from.own)
        }
        changeVehicles(doc)
    }

    // The last signal's removal takes the file with it, once asked: it
    // was written here and has no copy elsewhere
    function removeOwnSignal(key) {
        const doc = SignalForm.withoutSignal(ownFile, key)
        if (doc.signals.length === 0) {
            removeSignalFile(pickedVehicle.own)
            return
        }
        saveOwn(doc, qsTr("Removed from %1").arg(doc.name))
    }

    // Forget and Forget all: the copies no vehicle keeps go with them
    function forgetVehicles(all) {
        changeVehicles(all ? VehicleBook.forgetAll(vehicles) : VehicleBook.forget(vehicles))
        Bridge.deleteUnusedSignalFiles(VehicleBook.referencedFiles(vehicles))
    }

    // Picking fills the field with the vehicle's connection, read back
    // like typed text so the profile row and the DID field follow
    function pickVehicle(id) {
        confirming = ""
        vinPickNote = ""
        saveVehicles(VehicleBook.pick(vehicles, id))
        if (pickedVehicle !== null && pickedVehicle.uri !== "")
            uriField.text = pickedVehicle.uri
    }

    // A change of the list from the page's buttons: the field empties
    // for the next label
    function changeVehicles(doc) {
        confirming = ""
        saveVehicles(doc)
        vehicleLabelField.text = ""
    }

    // Live's ticks and unticks, the picked vehicle's from now on
    function rememberPlotting() {
        saveVehicles(VehicleBook.rememberPlotting(vehicles, Bridge.signalModel.plottedNames(),
                                                  Bridge.signalModel.unplottedNames()))
    }

    // Said beside the vehicles while the car's VIN, not the user, made
    // the pick (VehicleBook.pickNote); "" once the user picks
    property string vinPickNote: ""

    // The VIN settled which vehicle this is (ADR-0027 section 4)
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
        vinPickNote = VehicleBook.pickNote(r)
        if (r.outcome === "new" && plottedApplied)
            rememberPlotting()
        else if (r.outcome === "switched" && plottedApplied)
            Bridge.signalModel.setPlotChoices(pickedVehicle.plotted, pickedVehicle.unplotted)
        Bridge.logEvent(VehicleBook.logLine(r))
    }

    // No session, and none starting or ending. Every control that chooses
    // or starts a session is enabled by this one value, so none can be
    // used during a multi-second vendor block: the bridge refuses a
    // connect then anyway, but a device row or a preset would otherwise
    // rewrite the grayed field under the connect in flight.
    readonly property bool idle: !Bridge.connected && !Bridge.connecting && !Bridge.disconnecting

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
          summary: (devices.length === 0 ? qsTr("none found") : qsTr("%1 found").arg(devices.length))
                   + " · " + profileLabel() },
        { key: "vehicle", title: qsTr("Vehicle"),
          summary: (pickedVehicle !== null ? pickedVehicle.label : qsTr("None"))
                   + (vehicles.list.length > 0 ? qsTr(" · %1 saved").arg(vehicles.list.length) : "") },
        { key: "signals", title: qsTr("Signals"),
          summary: pickedVehicle === null ? qsTr("pick a vehicle")
                   : filesSummary(pickedVehicle.signals.length + (pickedVehicle.own !== "" ? 1 : 0)) },
        { key: "recording", title: qsTr("Recording"),
          summary: Bridge.recordingActive ? qsTr("recording") : Bridge.recording ? qsTr("on") : qsTr("off") },
        { key: "display", title: qsTr("Display"),
          summary: qsTr("%1 %").arg(Math.round(Theme.zoom * 100))
                   + (Bridge.unitSystem === 1 ? qsTr(" · imperial") : qsTr(" · metric"))
                   + " · " + Theme.nightSummary() },
        { key: "advanced", title: qsTr("Advanced"),
          summary: Theme.developerMode ? qsTr("developer mode") : qsTr("off") }
    ]

    function open(key) {
        pageKey = key
    }
    // Back from a page: the form returns to Signals, the rest to the list
    function back() {
        pageKey = pageKey === "ownform" ? "signals" : ""
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

    // A best-effort back key: where Qt delivers it, it repeats the back
    // arrow. Nothing relies on it
    focus: true
    Keys.onBackPressed: event => {
        if (!twoPane && pageKey !== "") {
            back()
            event.accepted = true
        }
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
        const top = uriField.mapToItem(body, 0, 0).y + body.y - Theme.spacing
        if (top < connectionPage.contentY)
            connectionPage.contentY = Math.max(0, top)
    }

    // The URI is composed from three choices, so none of it needs typing:
    // a base (a preset or a discovered device), a profile, and
    // uds-generic's DIDs. The field stays editable both ways: a
    // hand-written URI connects as written and its profile= and did= set
    // the row and the DID field, and the next preset, device or profile
    // click recomposes it, keeping every other query key where it was.
    // The choices live only while the app runs.
    property string profileChoice: "default"

    // The profiles the row offers (ADR-0013, ADR-0020), then the picked
    // vehicle's signal files that loaded (ADR-0051). "default" is the
    // scheme's own profile, so it adds no profile= key. A typed name
    // outside the list highlights no button; core judges it at connect.
    readonly property var profiles: builtInProfiles.concat(vehicleProfiles, pluginProfiles)
    // The vehicle's plugin variant, when one is picked and loaded
    readonly property var pluginProfiles: {
        const choice = pluginChoice()
        return choice === null ? [] : [choice]
    }
    readonly property var vehicleProfiles: pickedVehicle === null ? []
        : pickedVehicle.signals.filter(f => signalOk(f.sha256)
                                            && (standIn === null || f.sha256 !== standIn.sha256))
              .map(f => ({
              name: f.profile, label: f.name,
              hint: signalInfo[f.sha256].extends === "obd2"
                    ? qsTr("Your signal file %1: obd2 and %2 of your own").arg(f.name)
                          .arg(signalInfo[f.sha256].own)
                    : signalInfo[f.sha256].count === 1
                    ? qsTr("Your signal file %1: 1 signal").arg(f.name)
                    : qsTr("Your signal file %1: %2 signals").arg(f.name)
                          .arg(signalInfo[f.sha256].count) }))

    // On an adapter, Default is obd2, and a file that extends obd2 reads
    // all of obd2 and more, so it takes obd2's place there (ADR-0052):
    // the first such file the picked vehicle has, null for none or on
    // another scheme, where Default isn't obd2. Default's button stays
    // only while the field reads it, so the row always shows the URI's
    readonly property bool adapterUri: VehicleBook.isAdapterUri(uriField.text)
    readonly property var standIn: !adapterUri || pickedVehicle === null ? null
        : pickedVehicle.signals.find(f => signalOk(f.sha256)
                                          && signalInfo[f.sha256].extends === "obd2") || null
    readonly property var defaultProfile: ({
        name: "default", label: qsTr("Default"),
        hint: qsTr("The scheme's own profile: obd2 on an adapter, raw on a replay or a "
                   + "CAN interface, the demo set on mock://") })
    // The stand-in's entry, built in the same binding as the list, so the
    // two never disagree mid-update
    function standInProfile(f) {
        return { name: f.profile, label: qsTr("obd2 + %1").arg(f.name),
                 hint: qsTr("obd2's signals and %1 of your own, from your signal file %2")
                           .arg(signalInfo[f.sha256].own).arg(f.name) }
    }
    readonly property var builtInProfiles: (standIn === null ? [defaultProfile]
        : profileChoice === "default" ? [defaultProfile, standInProfile(standIn)]
        : [standInProfile(standIn)]).concat([
        { name: "raw", label: qsTr("raw (log only)"),
          hint: qsTr("No decode: frames reach the Log page only. On an adapter it listens "
                     + "to the engine ECU (0x7E8); add ?filter= for another ID. An ELM327 "
                     + "hears only replies to its requests, so add ?poll= there") },
        { name: "uds-generic", label: qsTr("uds-generic (DIDs)"),
          hint: qsTr("Service 0x22 reads of the DIDs below, polled; a range is scanned "
                     + "once and the DIDs the ECU answers join the polling") }
    ])

    // What a page that works with no session lists its signals from: the
    // field's URI, which carries the chosen profile, or with no URI typed
    // the chosen profile alone
    readonly property string offlineUri: {
        const typed = uriField.text.trim()
        if (typed !== "")
            return typed
        return profileChoice === "default" ? "" : composeUri("mock://", [])
    }

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
        "mock": ["profile", "did", "diagnose"],
        "replay": ["profile", "did", "speed", "loop"],
        "j2534": ["baud", "flags", "filter", "txid", "poll", "probe", "identify", "diagnose",
                  "pack", "scan", "profile", "did"],
        "elm327": ["baud", "serial", "filter", "txid", "poll", "probe", "identify", "diagnose",
                   "pack", "scan", "profile", "did"],
        "socketcan": ["profile", "did", "baud", "filter", "txid", "poll", "probe", "identify",
                      "diagnose", "pack", "scan"]
    })
    // What a SocketCAN session that only listens takes: Default is raw
    // there, and core refuses it the keys that ask an ECU
    readonly property var socketcanListenKeys: ["profile", "did", "baud", "filter"]

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
            // SocketCAN asks an ECU through core's ISO-TP (ADR-0040) under a
            // profile that requests (uds-generic, a file's); one that only
            // listens takes no request key
            if (scheme === "socketcan" && ["default", "raw", "mock"].indexOf(profileChoice) >= 0)
                pairs = pairs.filter(p => socketcanListenKeys.indexOf(p[0]) >= 0)
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

    // The addresses that connected, newest first, for Dash's connect list
    readonly property int maxRecentUris: 5
    property var recentUris: []
    // The stored list, or `lastUri` alone when it holds none; a damaged
    // list reads as none, and anything but a non-empty string is left out
    function parseRecentUris(text, lastUri) {
        let list = []
        try {
            const read = JSON.parse(text)
            if (Array.isArray(read))
                list = read.filter(u => typeof u === "string" && u !== "")
        } catch (e) {
            // none, until the next connect writes one
        }
        if (list.length === 0 && lastUri !== "")
            list = [lastUri]
        return list.slice(0, maxRecentUris)
    }
    // `uri` first and only once, the list cut to its most recent
    function withRecentUri(list, uri) {
        return [uri].concat(list.filter(u => u !== uri)).slice(0, maxRecentUris)
    }
    // What Dash lists: the recent addresses, the picked vehicle's saved one
    // first when absent. Display only: recentUris holds what connected
    readonly property string vehicleUri: pickedVehicle !== null ? pickedVehicle.uri : ""
    function connectRows(recents, vehicle) {
        return vehicle === "" || recents.includes(vehicle) ? recents : [vehicle].concat(recents)
    }
    readonly property var connectUris: connectRows(recentUris, vehicleUri)
    // Dash's Connect: the field shows it as typed text, before the connect
    function connectFromList(uri) {
        uriField.text = uri
        requestConnect(uri)
    }

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
        useSignalFiles()
        loadPlugins()
        refreshDevices()
        Bluetooth.refresh(false)   // asked for when Connection first opens
        // Set like typed text, so the profile row and the DID field read
        // it back
        if (uriField.text === "")
            uriField.text = sessionSettings.uri
        recentUris = parseRecentUris(sessionSettings.recentUris, sessionSettings.uri)
        Bridge.recording = sessionSettings.recording
    }

    Connections {
        target: Bridge

        function onReadOnceResult(result) {
            if (page.readOnceFor === page.pickedId)
                page.readResult = result
        }
        function onConnectResult(uri, error) {
            page.errorLabel.text = error
            page.errorLabel.visible = (error !== "")
            if (error === "") {
                sessionSettings.uri = uri
                page.recentUris = page.withRecentUri(page.recentUris, uri)
                sessionSettings.recentUris = JSON.stringify(page.recentUris)
                page.rememberVersions(uri, Bridge.adapterVersions)
                // The picked vehicle's connection and plotted signals,
                // for an adapter's session only (ADR-0027 section 1, section 2)
                if (VehicleBook.isAdapterUri(uri) && page.pickedVehicle !== null) {
                    page.saveVehicles(VehicleBook.rememberUri(page.vehicles, uri))
                    Bridge.signalModel.setPlotChoices(page.pickedVehicle.plotted,
                                                      page.pickedVehicle.unplotted)
                }
                page.plottedApplied = true
            }
        }

        function onConnectedChanged() {
            if (!Bridge.connected)
                page.plottedApplied = false
            // A new session reads its VIN again; at its end the matches
            // the VIN proposed stay to be picked (the VIN goes with it)
            if (Bridge.connected)
                page.refreshMatches()
        }

        function onVehicleIdentified() {
            page.recognizeVehicle()
            page.refreshMatches()
            page.noteCarYear()
        }

        function onRecordingChanged() {
            sessionSettings.recording = Bridge.recording
        }

    }

    // A tick or an untick on Live is the picked vehicle's from now on
    Connections {
        target: Bridge.signalModel

        function onSubscribedCountChanged() {
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

                SettingsSignalFiles {
                    id: settingsSignalFiles
                    host: page
                }

                SettingsPlugins {
                    id: settingsPlugins
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
