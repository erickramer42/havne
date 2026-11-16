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

    // ---- The plugin store (ADR-0064): every import is a plugin, and a
    // vehicle picks variants of them ----
    // Raised after each change of the store and when the session's VIN
    // comes or goes, so what the bridge answered is asked again
    property int storeRev: 0
    property int vinRev: 0
    // The bridge's list: each loaded plugin with its variants, or a copy
    // that no longer loads
    readonly property var pluginList: storeRev >= 0 ? Bridge.plugins() : []
    readonly property var pluginBySha: {
        const map = {}
        for (const p of pluginList) {
            if (p.ok)
                map[p.sha256] = p
        }
        return map
    }
    // The user's own plugins, which take a variant
    readonly property var ownPlugins: pluginList.filter(p => p.ok && p.mine)
    // The copies that no longer load, each with the loader's words
    readonly property var refusedPlugins: pluginList.filter(p => !p.ok)
    readonly property var picks: picksNow()
    // The picked vehicle's picks read from the vehicle itself: a handler
    // of the pick's change runs before the bindings on it are evaluated
    function picksNow() {
        return pickedVehicle !== null && pickedVehicle.picks !== undefined ? pickedVehicle.picks : []
    }
    // The picks as the bridge reads them: each one's plugin, whether the
    // store still has it and, on an adapter, the one standing in for Default
    readonly property var pickRows: storeRev >= 0 ? Bridge.pickRow(picks, adapterUri) : []
    // What the last import, pick, save or removal said
    property string signalProblem: ""
    property string signalNote: ""

    // The loaded plugin and variant a profile names: { plugin, variant },
    // null for none
    function placeOf(profile) {
        for (const p of pluginList) {
            const at = p.ok ? p.profiles.indexOf(profile) : -1
            if (at >= 0)
                return { plugin: p, variant: p.variants[at] }
        }
        return null
    }

    // A variant as the user knows it: its plugin's name, with the variant
    // only where the plugin has several
    function variantName(plugin, variant) {
        return plugin.variants.length > 1 ? qsTr("%1: %2").arg(plugin.name, variant) : plugin.name
    }
    function profileName(profile) {
        const place = placeOf(profile)
        return place === null ? profile : variantName(place.plugin, place.variant)
    }

    // The list's rows, one for each variant of each plugin. First, whole,
    // the plugins with a variant of this car's: one a VIN matches (the
    // lookup's, else the session's and those kept on the vehicle), one the
    // vehicle picks, or any of a plugin imported for the vehicle.
    // caption heads a group; unused marks a plugin's last row when no
    // vehicle picks any of its variants
    readonly property var variantRows: {
        const offered = storeRev >= 0 && vinRev >= 0
            ? Bridge.offeredVariants(pickedId, vinLookupField.text, picks) : []
        const kept = pickedVehicle !== null && pickedVehicle.vinMatches !== undefined
                     && vinLookupField.text.trim() === "" ? pickedVehicle.vinMatches : []
        const rows = []
        for (const o of offered) {
            const p = pluginBySha[o.sha256]
            if (p === undefined)
                continue
            const at = p.profiles.indexOf(o.profile)
            rows.push({ sha256: o.sha256, id: o.id, name: o.name, version: o.version,
                        variant: o.variant, profile: o.profile, mine: o.mine === true,
                        obd2: o.extends === "obd2", count: at >= 0 ? p.counts[at] : 0,
                        own: at >= 0 ? p.owns[at] : 0, several: p.variants.length > 1,
                        forCar: pickedVehicle !== null
                                && (o.forThisCar === true || kept.indexOf(o.profile) >= 0
                                    || picks.indexOf(o.profile) >= 0),
                        picked: picks.indexOf(o.profile) >= 0, caption: "", unused: false })
        }
        // Whole: a plugin's variants never split across the two groups
        const carPlugins = rows.filter(r => r.forCar).map(r => r.sha256)
        rows.forEach(r => r.forCar = carPlugins.indexOf(r.sha256) >= 0)
        const forCar = rows.filter(r => r.forCar)
        const all = forCar.concat(rows.filter(r => !r.forCar))
        const last = {}
        all.forEach((r, i) => last[r.sha256] = i)
        all.forEach((r, i) => {
            if (forCar.length > 0 && i === 0)
                r.caption = qsTr("For this car:")
            else if (forCar.length > 0 && i === forCar.length)
                r.caption = qsTr("Other plugins:")
            r.unused = last[r.sha256] === i
                       && !VehicleBook.pickedBy(vehicles, pluginBySha[r.sha256].profiles)
        })
        return all
    }

    function rowTitle(r) {
        return r.several ? qsTr("%1 %2: %3").arg(r.name, r.version, r.variant)
                         : qsTr("%1 %2").arg(r.name, r.version)
    }
    // A profile by its row's title; the profile itself when no plugin has it
    function profileTitle(profile) {
        const place = placeOf(profile)
        return place === null ? profile
             : rowTitle({ name: place.plugin.name, version: place.plugin.version,
                          variant: place.variant, several: place.plugin.variants.length > 1 })
    }
    // What a row reads, and whose it is
    function rowReads(r) {
        const n = r.obd2 ? r.own : r.count
        const signals = n === 1 ? qsTr("1 signal") : qsTr("%1 signals").arg(n)
        return (r.obd2 ? qsTr("obd2 + %1").arg(signals) : signals)
               + (r.mine ? qsTr(" · yours") : "")
    }

    // The picks no plugin here has (a version that dropped the variant, a
    // plugin removed elsewhere), so each can be seen and undone
    readonly property var missingPicks: pickRows.filter(r => r.missing).map(r => {
        const m = /^plugin\.([^.]+)\.(.+)$/.exec(r.profile)
        const p = m === null ? undefined : pluginList.find(e => e.ok && e.id === m[1])
        return { profile: r.profile,
                 text: p !== undefined ? qsTr("%1 is no longer in %2").arg(m[2], p.name)
                                       : qsTr("%1: not in the store").arg(r.profile) }
    })

    // The Signals row's summary: the plugins counted, then the picked
    // vehicle's first pick by name (no translation is loaded to pick a
    // plural form)
    function signalsSummary() {
        const n = pluginList.filter(p => p.ok).length
        const plugins = n === 1 ? qsTr("1 plugin") : qsTr("%1 plugins").arg(n)
        if (pickedVehicle === null)
            return n === 0 ? qsTr("none yet") : plugins
        const named = pickRows.filter(r => !r.missing)
        const missing = pickRows.length - named.length
        const first = named.length > 0 ? profileName(named[0].profile) : ""
        const said = named.length === 0 ? (n === 0 ? qsTr("none yet") : qsTr("%1, none picked").arg(plugins))
                   : named.length === 1 ? qsTr("%1: %2").arg(plugins, first)
                   : qsTr("%1: %2 and %3 more").arg(plugins, first).arg(named.length - 1)
        return said + (missing === 0 ? "" : missing === 1 ? qsTr(", 1 pick missing")
                                                           : qsTr(", %1 picks missing").arg(missing))
    }

    // A press on a row: the variant picked for the vehicle, or its pick
    // undone. A pick made while the field reads Default is read at once
    function togglePick(profile) {
        if (pickedVehicle === null || !idle)
            return
        signalProblem = ""
        signalNote = ""
        if (picks.indexOf(profile) >= 0) {
            saveVehicles(VehicleBook.setPicks(vehicles, Bridge.unpickVariant(picks, profile)))
            if (profileChoice === profile)
                chooseProfile("default")
            return
        }
        pickProfile(profile)
    }

    // profile into the picked vehicle's picks, last: "" when refused (the
    // words in signalProblem), "read" when the field, reading Default,
    // now reads it, else "picked"
    function pickProfile(profile) {
        const r = Bridge.pickVariant(picks, profile)
        if (!r.ok) {
            signalProblem = r.words
            return ""
        }
        const readsDefault = profileChoice === "default"
            && queryValue(splitUri(uriField.text).pairs, "profile") === undefined
        saveVehicles(VehicleBook.setPicks(vehicles, r.picks))
        if (!readsDefault)
            return "picked"
        chooseProfile(profile)
        return "read"
    }

    // ---- One Import (ADR-0064 Q4): a package goes in as it is, a signal
    // file waits for As a new plugin or Into one of the user's own, an
    // OBDb set shows its report first ----
    // The file read and not yet kept: null, { kind: "signals", name, id,
    // variant } or { kind: "obdb" }
    property var pendingImport: null
    // The user's own plugin a waiting signal file goes into, by its
    // sha256; "" while none is chosen. A removal or a rewrite (a variant
    // dropped gives the plugin a new sha256) ends the choice
    property string importInto: ""
    onOwnPluginsChanged: {
        if (importInto !== "" && !ownPlugins.some(p => p.sha256 === importInto))
            importInto = ""
    }
    // Replace...'s variant, which the next file read takes the place of:
    // { sha256, variant, name }, null for none
    property var replaceTarget: null

    // The plugin's own words for a kept import, after the page's
    function saidToo(r) {
        const parts = [r.notes, r.words].filter(t => t !== undefined && t !== "")
        return parts.length === 0 ? "" : qsTr(" (%1)").arg(parts.join("; "))
    }

    // An import that gave one variant is picked for the picked vehicle at
    // once; the note ends with what happened to it
    function pickKept(profile, note) {
        if (pickedVehicle === null || !idle || picks.indexOf(profile) >= 0) {
            signalNote = note
            return
        }
        const how = pickProfile(profile)
        // The row's word where it says more than the name: obd2 + <name>
        const shown = profiles.find(p => p.name === profile)
        const label = shown !== undefined ? shown.label : ""
        signalNote = how === "" ? note
                   : how === "read" && label !== "" && label !== profileName(profile)
                     ? qsTr("%1: picked for %2, now reading %3").arg(note, pickedVehicle.label, label)
                   : qsTr("%1: picked for %2").arg(note, pickedVehicle.label)
    }

    // The file the user chose. True when it was kept or is waiting
    function importFile(url) {
        signalProblem = ""
        signalNote = ""
        const target = replaceTarget
        discardImport()
        const r = Bridge.readImport(url, target !== null ? "signals" : "")
        if (!r.ok) {
            signalProblem = r.words
            return false
        }
        if (r.kind === "plugin") {
            notePackage(r)
            return true
        }
        if (r.kind === "signals") {
            pendingImport = { kind: "signals", name: r.name, id: r.id, variant: r.variant }
            if (target !== null)
                return keepPending(target.sha256, target.variant, true)
            importInto = ""
            importVariantField.text = r.variant
            return true
        }
        // The field first: its change converts a set that is waiting
        const y = obdbVinYear()
        obdbYearField.text = y > 0 ? String(y) : ""
        obdbYearFromVin = y > 0
        pendingImport = { kind: "obdb" }
        obdbVehicle = pickedId
        return previewObdb()
    }

    // A package that went in: added, updated in place, or in an earlier
    // version's place, whose VIN matches go with it (the new version's
    // match may not cover the car)
    function notePackage(r) {
        const before = r.replacedProfiles !== undefined ? r.replacedProfiles : []
        if (before.length > 0)
            saveVehicles(VehicleBook.dropVinMatches(vehicles, before))
        ++storeRev
        const note = r.updated !== "" ? qsTr("%1 updated from %2 to %3").arg(r.name, r.updated, r.version)
                   : r.replaced !== "" ? qsTr("%1 %2 replaces %3").arg(r.name, r.version, r.replaced)
                   : r.kept ? qsTr("%1 %2 is here already").arg(r.name, r.version)
                   : qsTr("%1 %2 added").arg(r.name, r.version)
        if (r.profiles.length === 1 && !r.kept)
            pickKept(r.profiles[0], note)
        else
            signalNote = r.kept || r.updated !== "" || r.replaced !== "" || pickedVehicle === null
                       ? note : qsTr("%1: pick a variant below").arg(note)
    }

    // The variant id Into's field names: the typed one, else the id core
    // makes from the waiting file's name
    function importVariantId(typed) {
        const id = String(typed).trim()
        return id !== "" || pendingImport === null || pendingImport.kind !== "signals"
            ? id : pendingImport.variant
    }

    // The waiting signal file kept: as a new plugin (into ""), imported
    // for the picked vehicle, or as the variant named of one of the
    // user's own, picked unless it is Replace's (replacing). A refusal
    // leaves it waiting. True when kept
    function keepPending(into, variant, replacing) {
        signalNote = ""
        if (pendingImport === null || pendingImport.kind !== "signals")
            return false
        const before = pluginBySha[into]
        const id = importVariantId(variant)
        const had = before !== undefined && before.variants.indexOf(id) >= 0
        const r = Bridge.keepSignals(into, variant, into === "" ? pickedId : "")
        if (!r.ok) {
            signalProblem = r.words
            return false
        }
        signalProblem = ""
        pendingImport = null
        importInto = ""
        ++storeRev
        const at = into === "" ? 0 : r.variants.indexOf(id)
        const profile = at >= 0 && at < r.profiles.length ? r.profiles[at] : ""
        const made = at >= 0 && at < r.variants.length ? r.variants[at] : id
        const note = (had ? qsTr("%1 %2: variant %3 replaced").arg(r.name, r.version, made)
                      : into !== "" ? qsTr("%1 %2 now has the variant %3").arg(r.name, r.version, made)
                      : r.kept ? qsTr("%1 %2 is here already").arg(r.name, r.version)
                      : qsTr("%1 %2 added").arg(r.name, r.version)) + saidToo(r)
        if (profile !== "" && replacing !== true)
            pickKept(profile, note)
        else
            signalNote = note
        return true
    }

    function discardImport() {
        Bridge.dropImport()
        pendingImport = null
        importInto = ""
        replaceTarget = null
        obdbReport = null
        obdbYearField.text = ""   // the set's year, and with it the VIN's mark
        signalProblem = ""
        signalNote = ""
    }

    // Replace... on a variant of the user's own: the next file read
    // takes its place
    function startReplace(row) {
        replaceTarget = { sha256: row.sha256, variant: row.variant, name: profileName(row.profile) }
    }

    // What Into's field would do in the chosen plugin: "" for a new
    // variant, else the words for one it takes the place of
    function importReplaces(variant) {
        const p = pluginBySha[importInto]
        const id = importVariantId(variant)
        return p !== undefined && p.variants.indexOf(id) >= 0 ? qsTr("replaces %1").arg(id) : ""
    }

    // ---- An OBDb set imported (ADR-0051's appendix "the OBDb import in
    // the app", ADR-0064 Q3) ----
    // The report of the set read and not yet kept (the bridge's map, null
    // for none) and the vehicle it was read for ("" for none)
    property var obdbReport: null
    property string obdbVehicle: ""
    // The year in the field came from a VIN, not the user: such a year
    // never names the variant (ADR-0064 Q2)
    property bool obdbYearFromVin: false
    // A list's rows shown at most, the rest counted: a set of thousands
    // of signals would otherwise lay out thousands of labels on a phone
    readonly property int obdbRowsShown: 50
    readonly property bool obdbShown: obdbReport !== null && pickedId === obdbVehicle

    // The name an OBDb import takes: the picked vehicle's, so its next
    // import finds this one
    function obdbName() {
        return pickedVehicle !== null ? qsTr("%1 OBDb").arg(pickedVehicle.label) : qsTr("OBDb set")
    }
    // The picked vehicle's earlier conversion of that name, which Keep
    // takes the place of while it has one variant: its entry, null for none
    readonly property var obdbEarlier: pickedVehicle === null ? null
        : pluginList.find(p => p.ok && p.obdb === true && p.importedFor === pickedVehicle.id
                               && p.name === obdbName()) || null
    readonly property bool obdbReplaces: obdbEarlier !== null && obdbEarlier.variants.length === 1

    // The connected car's model year once its VIN names the picked
    // vehicle (its VIN key), kept while the page lives; the VIN itself
    // stays in the bridge, and the year is never stored
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

    // The held set converted for the field's year: its report shown, or
    // the bridge's words why not. True when there's a report. While typing,
    // one to three digits are a year on its way: no report and no refusal
    function previewObdb(typing) {
        signalNote = ""
        obdbReport = null
        if (pendingImport === null || pendingImport.kind !== "obdb")
            return false
        const year = obdbYear()
        if (year < 0) {
            signalProblem = typing === true && /^[0-9]{1,3}$/.test(obdbYearField.text.trim())
                ? "" : qsTr("A model year is 1980 to 2039, or empty for none")
            return false
        }
        const r = Bridge.previewObdbImport(year, obdbName())
        if (!r.ok) {
            signalProblem = r.words
            return false
        }
        signalProblem = ""
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
        if (obdbReplaces)
            s += qsTr("; replaces the vehicle's earlier OBDb import")
        return s
    }

    // The report kept as a plugin of the user's own, imported for the
    // vehicle it was read for; picks of the import it took the place of
    // follow it. A refusal (a session reading the earlier import) keeps
    // the report up. True when kept
    function keepObdb() {
        signalNote = ""
        if (!obdbShown)
            return false
        const typed = obdbYearField.text.trim() !== "" && !obdbYearFromVin
        const r = Bridge.keepObdbPlugin(obdbVehicle, typed)
        if (!r.ok) {
            signalProblem = r.words
            return false
        }
        signalProblem = ""
        pendingImport = null
        obdbReport = null
        const gone = (r.replacedProfiles !== undefined ? r.replacedProfiles : [])
            .filter(p => r.profiles.indexOf(p) < 0)
        if (gone.length > 0 && r.profiles.length === 1) {
            if (gone.indexOf(profileChoice) >= 0)
                chooseProfile(r.profiles[0])
            saveVehicles(VehicleBook.movePicks(vehicles, gone, r.profiles[0]))
        }
        ++storeRev
        const made = r.merged === true ? r.profiles[r.profiles.length - 1] : r.profiles[0]
        const note = (r.merged === true ? qsTr("%1 %2: kept as a variant of the earlier OBDb import").arg(r.name, r.version)
                      : r.replaced !== undefined && r.replaced !== ""
                        ? qsTr("%1 replaced the vehicle's earlier OBDb import").arg(r.name)
                      : r.kept ? qsTr("%1 is here already").arg(r.name)
                      : qsTr("%1 %2 added").arg(r.name, r.version)) + saidToo(r)
        pickKept(made, note)
        return true
    }

    // ---- A row's actions ----
    // The row whose actions show (its profile, "" for none), and within
    // it Copy's choice of where and Export's share-alike note
    property string openRow: ""
    property bool copyShown: false
    property string exportNote: ""
    onOpenRowChanged: {
        copyShown = false
        exportNote = ""
    }

    function markMine(sha) {
        signalNote = ""
        const p = pluginBySha[sha]
        const r = Bridge.markPluginMine(sha)
        signalProblem = r.ok ? "" : r.words
        if (r.ok) {
            copyShown = false   // Copy is for someone else's
            signalNote = qsTr("%1 is yours now").arg(p !== undefined ? p.name : qsTr("The plugin"))
                         + saidToo(r)
        }
        ++storeRev
    }

    // Copy to my plugin (ADR-0064 Q8): the variant into a new plugin of
    // the user's own (into "") or one of theirs
    function copyVariantTo(row, into) {
        signalNote = ""
        const r = Bridge.copyVariant(row.sha256, row.variant, into, "")
        signalProblem = r.ok ? "" : r.words
        if (!r.ok)
            return false
        copyShown = false
        ++storeRev
        signalNote = qsTr("%1 copied into %2 %3").arg(profileName(row.profile), r.name, r.version)
                     + saidToo(r)
        return true
    }

    // A variant out of a plugin of the user's own, every vehicle's pick
    // of it undone
    function removeVariant(row) {
        signalNote = ""
        const r = Bridge.dropPluginVariant(row.sha256, row.variant)
        signalProblem = r.ok ? "" : r.words
        if (!r.ok)
            return false
        saveVehicles(VehicleBook.dropProfiles(vehicles, [row.profile]))
        if (profileChoice === row.profile)
            chooseProfile("default")
        openRow = ""
        ++storeRev
        return true
    }

    // A variant of the user's own has no copy anywhere else either: its
    // Remove asks first, as the whole plugin's does
    function askRemoveVariant(row) {
        confirming = "removeVariant:" + row.profile
    }

    // A plugin of the user's own has no copy anywhere else, so its
    // removal asks first
    function askRemovePlugin(sha) {
        const p = pluginBySha[sha]
        if (p !== undefined && p.mine)
            confirming = "removePlugin:" + sha
        else
            removePlugin(sha)
    }

    function confirmingPlugin() {
        for (const head of ["removePlugin:", "removeOwn:"]) {
            if (confirming.startsWith(head))
                return pluginBySha[confirming.substring(head.length)] || null
        }
        return null
    }
    function pluginQuestion(p) {
        return qsTr("Remove %1? It is yours, and no other copy exists").arg(p.name)
    }
    // The question a row of the list asks, "" for none: a plugin's in its
    // open row, where its Remove was pressed, and a variant's in its own
    function rowQuestion(entry, open) {
        if (open && confirming === "removePlugin:" + entry.sha256 && confirmingPlugin() !== null)
            return pluginQuestion(confirmingPlugin())
        if (confirming === "removeVariant:" + entry.profile)
            return qsTr("Remove the variant %1 of %2? It is yours, and no other copy exists")
                       .arg(entry.variant, entry.name)
        return ""
    }
    function ownQuestion() {
        const p = confirmingPlugin()
        if (!confirming.startsWith("removeOwn:") || p === null || ownPlace === null)
            return ""
        return p.variants.length > 1
            ? qsTr("Remove the variant %1 of %2? It is yours, and no other copy exists")
                  .arg(ownPlace.variant, p.name)
            : pluginQuestion(p)
    }
    // The question's Remove: what it asked about goes
    function confirmRemoval() {
        const asked = confirming
        const p = confirmingPlugin()
        confirming = ""
        if (asked.startsWith("removeVariant:")) {
            const row = variantRows.find(r => "removeVariant:" + r.profile === asked)
            return row !== undefined && removeVariant(row)
        }
        if (asked.startsWith("removeUnmigrated:")) {
            const record = unmigratedRows.find(r => "removeUnmigrated:" + unmigratedKey(r) === asked)
            if (record !== undefined)
                removeUnmigrated(record)
            return record !== undefined
        }
        if (p !== null && asked.startsWith("removeOwn:") && p.variants.length > 1 && ownPlace !== null)
            return removeVariant({ sha256: p.sha256, variant: ownPlace.variant,
                                   profile: pickedVehicle.own })
        return p !== null && removePlugin(p.sha256)
    }

    // A removed plugin's variants are no vehicle's pick or own variant any
    // more, and the field reads Default when it named one
    function removePlugin(sha) {
        confirming = ""
        signalNote = ""
        const p = pluginBySha[sha]
        const profiles = p !== undefined ? p.profiles : []
        const r = Bridge.removePlugin(sha)
        signalProblem = r.ok ? "" : r.words
        if (r.ok && profiles.length > 0) {
            saveVehicles(VehicleBook.dropProfiles(vehicles, profiles))
            if (profiles.indexOf(profileChoice) >= 0)
                chooseProfile("default")
        }
        if (r.ok)
            openRow = ""
        ++storeRev
        return r.ok
    }

    // ---- Saves out of the app, through the one save dialog: a variant
    // as a signal file, a plugin's packed copy, a file that didn't move.
    // { kind: "variant" | "plugin" | "unmigrated", sha256, variant, name,
    // file }, null for none
    property var saving: null

    function openSave(what) {
        saving = what
        saveDialog.title = what.kind === "plugin" ? qsTr("Export the plugin")
                                                  : qsTr("Save as a signal file")
        saveDialog.nameFilters = what.kind === "plugin"
            ? [qsTr("Plugins (*.havne-plugin.json)"), qsTr("All files (*)")]
            : [qsTr("Signal files (*.json)"), qsTr("All files (*)")]
        saveDialog.selectedFile = saveDialog.currentFolder.toString().replace(/\/$/, "")
                                  + "/" + what.file
        saveDialog.open()
    }

    // What each save writes and the name it starts from: a variant's
    // signal file by its plugin's id, with the variant where the plugin
    // has several; a plugin's packed copy; a file that didn't move
    function variantSave(row) {
        return { kind: "variant", sha256: row.sha256, variant: row.variant,
                 name: profileName(row.profile),
                 file: (row.several ? row.id + "-" + row.variant : row.id) + ".json" }
    }
    function pluginSave(sha) {
        const p = pluginBySha[sha]
        return p === undefined ? null
             : { kind: "plugin", sha256: sha, variant: "", name: p.name,
                 file: p.id + ".havne-plugin.json" }
    }
    function unmigratedSave(record) {
        return { kind: "unmigrated", sha256: record.sha256, variant: "", name: record.name,
                 file: "signals.json" }
    }

    function saveVariantAs(row) {
        openSave(variantSave(row))
    }

    // Export plugin...: its share-alike note first, when it has one
    function exportPluginAs(sha) {
        if (!exportWaits(sha))
            openExport(sha)
    }

    // True when the export waits: on its note's Save..., or refused
    function exportWaits(sha) {
        signalNote = ""
        const r = Bridge.pluginExportNote(sha)
        signalProblem = r.ok ? "" : r.words
        exportNote = r.ok ? r.note : ""
        return !r.ok || r.note !== ""
    }

    function openExport(sha) {
        const what = pluginSave(sha)
        exportNote = ""
        if (what !== null)
            openSave(what)
    }

    function saveUnmigratedAs(record) {
        openSave(unmigratedSave(record))
    }

    // The save the dialog was opened for, to the file chosen
    function saveTo(url) {
        const what = saving
        saving = null
        if (what === null) {
            Bridge.dropPicked(url)
            return false
        }
        const r = what.kind === "variant" ? Bridge.saveVariantFile(what.sha256, what.variant, url)
                : what.kind === "plugin" ? Bridge.exportPlugin(what.sha256, url)
                : Bridge.saveUnmigratedFile(what.sha256, url)
        signalProblem = r.ok ? "" : r.words
        signalNote = !r.ok ? ""
                   : what.kind === "plugin" ? qsTr("%1 exported to the file you chose").arg(what.name)
                   : qsTr("%1 saved to the file you chose").arg(what.name)
        return r.ok
    }

    // ---- The signal files the move into the plugin store left behind
    // (ADR-0064 Q6): the picked vehicle's, then the book's own, each
    // { sha256, name, words, kept, loose } ----
    readonly property var unmigratedRows:
        (pickedVehicle !== null && pickedVehicle.unmigrated !== undefined
         ? pickedVehicle.unmigrated.map(r => ({ sha256: r.sha256, name: r.name, words: r.words,
                                                kept: r.kept !== false, loose: false })) : [])
        .concat(vehicles.unmigrated !== undefined
                ? vehicles.unmigrated.map(r => ({ sha256: r.sha256, name: r.name, words: r.words,
                                                  kept: r.kept !== false, loose: true })) : [])

    function unmigratedText(r) {
        return qsTr("%1: %2").arg(r.name, r.words)
               + (r.kept ? "" : qsTr(" (its copy is still in the earlier version's folder)"))
    }

    function unmigratedKey(r) {
        return (r.loose ? "book:" : "vehicle:") + r.sha256
    }
    // A removal that deletes the kept bytes (no other record lists them)
    // asks first: they are the file's one copy here
    function askRemoveUnmigrated(record) {
        const after = record.loose ? VehicleBook.dropLooseUnmigrated(vehicles, record.sha256)
                                   : VehicleBook.dropUnmigrated(vehicles, record.sha256)
        if (record.kept && !VehicleBook.hashListed(after, record.sha256))
            confirming = "removeUnmigrated:" + unmigratedKey(record)
        else
            removeUnmigrated(record)
    }
    function unmigratedQuestion(record) {
        return confirming === "removeUnmigrated:" + unmigratedKey(record)
            ? qsTr("Remove %1? Its kept copy goes with it").arg(record.name) : ""
    }

    // The record out of the book; its kept bytes go once nothing lists
    // them. The earlier version's folder is never touched
    function removeUnmigrated(record) {
        signalProblem = ""
        signalNote = ""
        saveVehicles(record.loose ? VehicleBook.dropLooseUnmigrated(vehicles, record.sha256)
                                  : VehicleBook.dropUnmigrated(vehicles, record.sha256))
        if (!VehicleBook.hashListed(vehicles, record.sha256))
            Bridge.removeUnmigratedFile(record.sha256)
    }

    // ---- The VIN's matches ----
    // The session's matches kept on the vehicle its VIN identified, so
    // they are marked with no session (ADR-0052's appendix of 2026-11-13);
    // only that vehicle's: a replay of another car names none
    function noteVinMatches() {
        if (pickedVehicle === null || pickedVehicle.vinKey === ""
                || Bridge.vinKey(vehicles.salt) !== pickedVehicle.vinKey)
            return
        const profiles = Bridge.pluginMatches("").map(m => m.profile)
        if (JSON.stringify(profiles) !== JSON.stringify(pickedVehicle.vinMatches))
            saveVehicles(VehicleBook.setVinMatches(vehicles, profiles))
    }

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
        discardImport()   // a file waiting was read for the last vehicle
        openRow = ""
        clearForm()
        dropAnotherVehiclesProfile()
    }

    // A plugin's profile in the field is one vehicle's pick: once another
    // vehicle is picked (or none), the field reads Default, so a connect
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
        if (picksNow().indexOf(p) < 0)
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

    // The loader's words without a line and column of a file the user
    // never sees
    function formWords(words) {
        return String(words).replace(/^line \d+, column \d+: /, "")
    }

    // doc as the vehicle's own variant: put in its place, or with none
    // yet made a plugin of the user's own, picked for the vehicle. The
    // loader's words when it refuses it. True when saved
    function saveOwn(doc, note) {
        const file = Object.assign({}, doc)
        delete file.notes   // the variant's written header line, not the file's
        const place = ownPlace
        const r = Bridge.keepSignalText(JSON.stringify(file, null, 2),
                                        place !== null ? place.plugin.sha256 : "",
                                        place !== null ? place.variant : "", pickedId)
        if (!r.ok) {
            formProblem = formWords(r.words)
            return false
        }
        ++storeRev
        if (place === null) {
            const profile = r.profiles[0]
            let next = VehicleBook.setOwn(vehicles, profile)
            const picked = Bridge.pickVariant(VehicleBook.picked(next).picks, profile)
            if (picked.ok)
                next = VehicleBook.setPicks(next, picked.picks)
            saveVehicles(next)
        }
        formProblem = ""
        formNote = note
        return true
    }

    // The name the own plugin is made under: "<label> signals"
    function ownName(label) {
        return qsTr("%1 signals").arg(String(label).trim())
    }

    // The form's signal into the own variant, made with the first; the
    // signal Edit filled the form with replaced under the key it has
    // (SignalForm.keyFor), or a new one's of the same name. An Edit can't
    // take another saved signal's name: the lists tell signals by name
    function saveDraft() {
        formNote = ""
        readResult = null
        if (pickedVehicle === null)
            return false
        const f = readForm()
        const signal = SignalForm.draft(f)
        const named = ownSignals.find(s => s.name === signal.name && s.key !== editingKey)
        if (editingKey !== "" && named !== undefined) {
            formProblem = qsTr("%1 is saved already: give this one another name").arg(named.name)
            return false
        }
        signal.key = SignalForm.keyFor(ownSignals, signal.name, editingKey)
        const base = ownFile !== null ? ownFile
            : SignalForm.emptyFile(pickedVehicle.label, ownName(pickedVehicle.label))
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

    // A copy of the picked vehicle. Its own signals are copied into a
    // plugin of its own, picked in the place of the original's, so each
    // vehicle's own signals change apart; that plugin not made, the copy
    // starts with none
    function duplicateVehicle() {
        const from = pickedVehicle
        const text = ownFile === null ? "" : (() => {
            const file = Object.assign({}, ownFile)
            delete file.notes
            file.name = ownName(copyLabel)
            return JSON.stringify(file, null, 2)
        })()
        let doc = VehicleBook.duplicate(vehicles, copyLabel)
        if (from !== null && from.own !== "") {
            const id = VehicleBook.picked(doc).id
            const r = text === "" ? { ok: false } : Bridge.keepSignalText(text, "", "", id)
            doc = VehicleBook.setPicks(doc, VehicleBook.picked(doc).picks.map(
                      p => p === from.own ? (r.ok ? r.profiles[0] : "") : p).filter(p => p !== ""))
            if (r.ok)
                doc = VehicleBook.setOwn(doc, r.profiles[0])
            ++storeRev
        }
        changeVehicles(doc)
    }

    // The last signal's removal takes the variant with it, once asked: it
    // was written here and has no copy elsewhere
    function removeOwnSignal(key) {
        const doc = SignalForm.withoutSignal(ownFile, key)
        if (doc.signals.length > 0) {
            saveOwn(doc, qsTr("Removed from %1").arg(doc.name))
            return
        }
        confirming = "removeOwn:" + ownPlace.plugin.sha256
    }

    // Forget and Forget all: what the store holds as imported for the
    // vehicles goes with them; the plugins stay
    function forgetVehicles(all) {
        const id = pickedId
        changeVehicles(all ? VehicleBook.forgetAll(vehicles) : VehicleBook.forget(vehicles))
        if (all)
            Bridge.forgetAllVehicleImports()
        else if (id !== "")
            Bridge.forgetVehicleImports(id)
        ++storeRev
    }

    // Picking fills the field with the vehicle's connection as a list
    // would connect it, read back like typed text so the profile row and
    // the DID field follow
    function pickVehicle(id) {
        confirming = ""
        vinPickNote = ""
        saveVehicles(VehicleBook.pick(vehicles, id))
        if (pickedVehicle !== null && pickedVehicle.uri !== "")
            uriField.text = listConnectUri(pickedVehicle.uri)
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

    // The URI is composed from three choices, so none of it needs typing:
    // a base (a preset or a discovered device), a profile, and
    // uds-generic's DIDs. The field stays editable both ways: a
    // hand-written URI connects as written and its profile= and did= set
    // the row and the DID field, and the next preset, device or profile
    // click recomposes it, keeping every other query key where it was.
    // The choices live only while the app runs.
    property string profileChoice: "default"
    // uds-generic's extended session (session=03), composed as did= is
    property bool extSession: false

    // The profiles the row offers (ADR-0013, ADR-0020), then the picked
    // vehicle's picks the store has (ADR-0064 Q5). "default" is the
    // scheme's own profile, so it adds no profile= key. A typed name
    // outside the list highlights no button; core judges it at connect.
    readonly property var profiles: builtInProfiles.concat(pickProfiles)

    // On an adapter, Default is obd2, and a variant that extends obd2
    // reads all of obd2 and more, so the first such pick takes obd2's
    // place there (ADR-0064 Q5): null for none or on another scheme,
    // where Default isn't obd2. Default's button stays only while the
    // field reads it, so the row always shows the URI's
    readonly property bool adapterUri: VehicleBook.isAdapterUri(uriField.text)
    readonly property var standIn: pickRows.find(r => r.standIn) || null
    readonly property var defaultProfile: ({
        name: "default", label: qsTr("Default"),
        hint: qsTr("Default reads OBD-II values on an adapter; a recording or a "
                   + "CAN interface shows its frames") })

    // A pick as the row's entry: one that extends obd2 says so on an
    // adapter, where obd2 is what Default reads
    function pickEntry(r) {
        const name = profileName(r.profile)
        const obd2 = r.extends === "obd2"
        return { name: r.profile,
                 label: obd2 && adapterUri ? qsTr("obd2 + %1").arg(name) : name,
                 hint: obd2 ? qsTr("obd2's signals and those of %1, picked for this vehicle")
                                  .arg(profileTitle(r.profile))
                            : qsTr("The signals of %1, picked for this vehicle")
                                  .arg(profileTitle(r.profile)) }
    }
    // The picks but the stand-in, which sits where Default does
    readonly property var pickProfiles: pickRows.filter(r => !r.missing && !r.standIn)
                                                .map(r => pickEntry(r))
    readonly property var builtInProfiles: (standIn === null ? [defaultProfile]
        : profileChoice === "default" ? [defaultProfile, pickEntry(standIn)]
        : [pickEntry(standIn)]).concat([
        { name: "raw", label: qsTr("raw (log only)"),
          hint: qsTr("Raw (log only): no decode, frames reach the Activity page only. On an adapter it listens "
                     + "to the engine ECU (0x7E8); add ?filter= for another ID. An ELM327 "
                     + "hears only replies to its requests, so add ?poll= there") },
        { name: "uds-generic", label: qsTr("uds-generic (DIDs)"),
          hint: qsTr("UDS DIDs (uds-generic): Service 0x22 reads of the DIDs below, polled; a range is scanned "
                     + "once and the DIDs the ECU answers join the polling") }
    ])

    // The picked vehicle's sets as [{ profile, name, variant }]: its picks
    // the store has, in their order
    readonly property var pickedSets: pickRows.filter(r => !r.missing)
        .map(r => ({ profile: r.profile, name: r.name, variant: r.variant }))
    // A profile's name as the user knows it: every loaded plugin's
    // variants by the plugin's name
    readonly property var profileNames: {
        const names = {}
        for (const p of pluginList) {
            for (const profile of (p.ok ? p.profiles : []))
                names[profile] = p.name
        }
        return names
    }

    // The first picked set that reads all of obd2, which stands in for
    // Default on an adapter (ADR-0064)
    readonly property var obd2PickedSet: {
        const first = pickRows.find(r => !r.missing && r.extends === "obd2")
        return first === undefined ? null
             : { profile: first.profile, name: first.name, variant: first.variant }
    }

    // What a page that works with no session lists its signals from: the
    // field's URI, which carries the chosen profile, except an adapter's
    // read under Default, where the vehicle's obd2 stand-in is listed;
    // with no URI typed the chosen profile alone, or under Default the
    // picked vehicle's first set, which a restart doesn't forget as it
    // does the choice
    readonly property string offlineUri: {
        const typed = uriField.text.trim()
        if (typed !== "") {
            return profileChoice === "default" && VehicleBook.isAdapterUri(typed) && obd2PickedSet !== null
                   ? "mock://?profile=" + obd2PickedSet.profile : typed
        }
        if (profileChoice !== "default")
            return composeUri("mock://", [])
        return pickedSets.length > 0 ? "mock://?profile=" + pickedSets[0].profile : ""
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
        return qsTr("The address names profile '%1'; core judges it at connect").arg(profileChoice)
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
        // Core refuses session= under any other profile; extSession keeps the
        // choice for the way back to uds-generic
        if (profileChoice !== "uds-generic")
            setQuery(out, "session", null)
        else if (sessionOffered(base))
            setQuery(out, "session", extSession ? "03" : null)
        return joinUri(base, out)
    }

    // The extended session is requested over an adapter or a CAN interface;
    // core refuses session= on mock:// and replay://
    function sessionOffered(base) {
        const scheme = schemeOf(base)
        return scheme !== "mock" && scheme !== "replay"
    }

    // The query keys each scheme takes, as core lists them
    // (core/src/mock_transport.cpp, replay_transport.cpp,
    // j2534_transport.cpp, elm327_transport.cpp)
    readonly property var schemeKeys: ({
        "mock": ["profile", "did", "diagnose"],
        "replay": ["profile", "did", "speed", "loop"],
        "j2534": ["baud", "flags", "filter", "txid", "poll", "probe", "identify", "diagnose",
                  "pack", "scan", "gap", "session", "keepalive_ms", "profile", "did"],
        "elm327": ["baud", "serial", "filter", "txid", "poll", "probe", "identify", "diagnose",
                   "pack", "scan", "gap", "session", "keepalive_ms", "profile", "did"],
        "socketcan": ["profile", "did", "baud", "filter", "txid", "poll", "probe", "identify",
                      "diagnose", "pack", "scan", "gap", "session", "keepalive_ms"]
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
        if (profileChoice === "uds-generic") {
            didField.text = did === undefined || did === null ? "" : did
            const session = queryValue(pairs, "session")
            extSession = session !== undefined && session !== null && Number(session) === 3
        }
        else if (did !== undefined && did !== null)
            didField.text = did
        syncing = false
    }

    // kept: the address to keep once uri connects, where it isn't uri
    // itself (a list's row, whose profile the page added)
    property string connectKeeps: ""
    function requestConnect(uri, kept) {
        if (uri === "")
            return
        connectKeeps = kept === undefined ? "" : kept
        Bridge.connectSessionAsync(uri)
        // Connecting/Connected/failed lines arrive via logMessage.
    }

    // The addresses kept from the connects that succeeded, newest first,
    // for Dash's connect list
    readonly property int maxRecentUris: 5
    property var recentUris: []
    // A replay or the mock is never kept: Recordings is a replay's way back
    function keptUri(uri) {
        const u = String(uri).trim().toLowerCase()
        return !u.startsWith("replay://") && !u.startsWith("mock://")
    }
    // The stored list, or `lastUri` alone when it holds none; a damaged
    // list reads as none, and anything but a non-empty kept string is left out
    function parseRecentUris(text, lastUri) {
        let list = []
        try {
            const read = JSON.parse(text)
            if (Array.isArray(read))
                list = read.filter(u => typeof u === "string" && u !== "" && keptUri(u))
        } catch (e) {
            // none, until the next connect writes one
        }
        if (list.length === 0 && lastUri !== "" && keptUri(lastUri))
            list = [lastUri]
        return list.slice(0, maxRecentUris)
    }
    // `uri` first and only once, the list cut to its most recent
    function withRecentUri(list, uri) {
        return [uri].concat(list.filter(u => u !== uri)).slice(0, maxRecentUris)
    }
    // What Dash lists: the recent addresses, the picked vehicle's saved one
    // first when absent. Display only: recentUris holds what was kept
    readonly property string vehicleUri: pickedVehicle !== null ? pickedVehicle.uri : ""
    function connectRows(recents, vehicle) {
        return vehicle === "" || recents.includes(vehicle) ? recents : [vehicle].concat(recents)
    }
    readonly property var connectUris: connectRows(recentUris, vehicleUri)
    // What the connect lists offer besides the recent addresses: the J2534
    // interfaces, the serial ports, the CAN interfaces and the phone's
    // Bluetooth adapters, as Connection's device list has them
    readonly property var connectAdapters: devices.map(d => ({
            uri: d.base, title: d.title, kind: d.kind, other: d.other === true
        }))
    // ---- A connect from a list (Home's, Dash's): an address that names
    // no profile reads the picked vehicle's plugin where obd2 is asked
    // (ADR-0064 Q5) ----
    // The first pick that loads and reads all of obd2, null for none
    readonly property var listStandIn:
        (storeRev >= 0 ? Bridge.pickRow(picks, true) : []).find(r => r.standIn) || null

    // Whether a pick that reads obd2 is read at the address: a J2534 or
    // ELM327 adapter's, or a CAN interface that sends requests (a virtual
    // bus, or a controller up in one-shot mode). A listening interface
    // is left to listen: a pick there would start sending
    function takesStandIn(uri) {
        if (VehicleBook.isAdapterUri(uri))
            return true
        const found = /^socketcan:\/\/([^?]+)/.exec(uri)
        const c = found === null ? undefined : canInterfaces.find(i => i.name === found[1])
        return c !== undefined && c.up === true
               && (c.kind === "vcan" || (c.modes || []).indexOf("one-shot") >= 0)
    }

    // A row's address as the picked vehicle reads it: a plugin's or a
    // signal file's profile the vehicle doesn't pick is dropped, as the
    // field drops it, so no connect reads another vehicle's signals
    function listAddress(uri) {
        const named = queryValue(splitUri(uri).pairs, "profile")
        return typeof named === "string" && (named.startsWith("plugin.") || named.startsWith("user."))
               && picksNow().indexOf(named) < 0 ? VehicleBook.withoutFileProfile(uri) : uri
    }

    // The address a list's row connects: with the stand-in's profile
    // where the rule applies, else as the vehicle reads it (one that
    // names a profile, the demo, a recording, no vehicle, no such pick)
    function listConnectUri(uri) {
        const plain = listAddress(uri)
        return listStandIn !== null && takesStandIn(plain)
            ? VehicleBook.withProfile(plain, listStandIn.profile) : plain
    }

    // What a list's row reads, where its kind doesn't say: "obd2 + <name>"
    // for a row the rule composes, a plugin's name for an address that
    // names its profile; "" otherwise
    function listProfileWord(uri) {
        const plain = listAddress(uri)
        const named = queryValue(splitUri(plain).pairs, "profile")
        if (typeof named === "string" && named.startsWith("plugin."))
            return placeOf(named) !== null ? profileName(named) : qsTr("a plugin not in the store")
        return listConnectUri(uri) !== plain ? qsTr("obd2 + %1").arg(profileName(listStandIn.profile)) : ""
    }
    // The lists' rows by address, for those with a word
    readonly property var connectProfileWords: {
        const words = {}
        for (const uri of connectUris.concat(connectAdapters.map(a => a.uri))) {
            const word = listProfileWord(uri)
            if (word !== "")
                words[uri] = word
        }
        return words
    }

    // A list's Connect: the field shows the address as typed text, with
    // the profile it reads, before the connect. The row's address is what
    // is kept: the profile added is the vehicle's pick of this moment
    function connectFromList(uri) {
        const composed = listConnectUri(uri)
        uriField.text = composed
        requestConnect(composed, listAddress(uri))
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
