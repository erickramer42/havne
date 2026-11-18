// File/layer: app/qml/SettingsImport.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.settings_*  (test_qml)

import QtQuick

pragma ComponentBehavior: Bound

Item {
    required property SettingsPage host

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
        if (host.pickedVehicle === null || !host.ownPart.idle || host.picksPart.picks.indexOf(profile) >= 0) {
            host.picksPart.signalNote = note
            return
        }
        const how = host.picksPart.pickProfile(profile)
        // The row's word where it says more than the name: obd2 + <name>
        const shown = host.uriPart.profiles.find(p => p.name === profile)
        const label = shown !== undefined ? shown.label : ""
        host.picksPart.signalNote = how === "" ? note
                   : how === "read" && label !== "" && label !== host.picksPart.profileName(profile)
                     ? qsTr("%1: picked for %2, now reading %3").arg(note, host.pickedVehicle.label, label)
                   : qsTr("%1: picked for %2").arg(note, host.pickedVehicle.label)
    }

    // The file the user chose. True when it was kept or is waiting
    function importFile(url) {
        host.picksPart.signalProblem = ""
        host.picksPart.signalNote = ""
        const target = replaceTarget
        discardImport()
        const r = Bridge.readImport(url, target !== null ? "signals" : "")
        if (!r.ok) {
            host.picksPart.signalProblem = r.words
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
            host.importVariantField.text = r.variant
            return true
        }
        // The field first: its change converts a set that is waiting
        const y = obdbVinYear()
        host.obdbYearField.text = y > 0 ? String(y) : ""
        obdbYearFromVin = y > 0
        pendingImport = { kind: "obdb" }
        obdbVehicle = host.ownPart.pickedId
        return previewObdb()
    }

    // A package that went in: added, updated in place, or in an earlier
    // version's place, whose VIN matches go with it (the new version's
    // match may not cover the car)
    function notePackage(r) {
        const before = r.replacedProfiles !== undefined ? r.replacedProfiles : []
        if (before.length > 0)
            host.saveVehicles(VehicleBook.dropVinMatches(host.vehicles, before))
        ++host.picksPart.storeRev
        const note = r.updated !== "" ? qsTr("%1 updated from %2 to %3").arg(r.name, r.updated, r.version)
                   : r.replaced !== "" ? qsTr("%1 %2 replaces %3").arg(r.name, r.version, r.replaced)
                   : r.kept ? qsTr("%1 %2 is here already").arg(r.name, r.version)
                   : qsTr("%1 %2 added").arg(r.name, r.version)
        if (r.profiles.length === 1 && !r.kept)
            pickKept(r.profiles[0], note)
        else
            host.picksPart.signalNote = r.kept || r.updated !== "" || r.replaced !== "" || host.pickedVehicle === null
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
        host.picksPart.signalNote = ""
        if (pendingImport === null || pendingImport.kind !== "signals")
            return false
        const before = host.picksPart.pluginBySha[into]
        const id = importVariantId(variant)
        const had = before !== undefined && before.variants.indexOf(id) >= 0
        const r = Bridge.keepSignals(into, variant, into === "" ? host.ownPart.pickedId : "")
        if (!r.ok) {
            host.picksPart.signalProblem = r.words
            return false
        }
        host.picksPart.signalProblem = ""
        pendingImport = null
        importInto = ""
        ++host.picksPart.storeRev
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
            host.picksPart.signalNote = note
        return true
    }

    function discardImport() {
        Bridge.dropImport()
        pendingImport = null
        importInto = ""
        replaceTarget = null
        obdbReport = null
        host.obdbYearField.text = ""   // the set's year, and with it the VIN's mark
        host.picksPart.signalProblem = ""
        host.picksPart.signalNote = ""
    }

    // Replace... on a variant of the user's own: the next file read
    // takes its place
    function startReplace(row) {
        replaceTarget = { sha256: row.sha256, variant: row.variant, name: host.picksPart.profileName(row.profile) }
    }

    // What Into's field would do in the chosen plugin: "" for a new
    // variant, else the words for one it takes the place of
    function importReplaces(variant) {
        const p = host.picksPart.pluginBySha[importInto]
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
    readonly property bool obdbShown: obdbReport !== null && host.ownPart.pickedId === obdbVehicle

    // The name an OBDb import takes: the picked vehicle's, so its next
    // import finds this one
    function obdbName() {
        return host.pickedVehicle !== null ? qsTr("%1 OBDb").arg(host.pickedVehicle.label) : qsTr("OBDb set")
    }
    // The picked vehicle's earlier conversion of that name, which Keep
    // takes the place of while it has one variant: its entry, null for none
    readonly property var obdbEarlier: host.pickedVehicle === null ? null
        : host.picksPart.pluginList.find(p => p.ok && p.obdb === true && p.importedFor === host.pickedVehicle.id
                               && p.name === obdbName()) || null
    readonly property bool obdbReplaces: obdbEarlier !== null && obdbEarlier.variants.length === 1

    // The connected car's model year once its VIN names the picked
    // vehicle (its VIN key), kept while the page lives; the VIN itself
    // stays in the bridge, and the year is never stored
    property int obdbCarYear: 0
    property string obdbCarVehicle: ""

    function noteCarYear() {
        if (host.pickedVehicle === null || host.pickedVehicle.vinKey === ""
                || Bridge.vinKey(host.vehicles.salt) !== host.pickedVehicle.vinKey)
            return
        obdbCarYear = Bridge.vinModelYear("")
        obdbCarVehicle = host.pickedVehicle.id
    }

    // The model year a VIN gives: the lookup's typed one, else the
    // connected car's when it was the picked vehicle, else 0
    function obdbVinYear() {
        if (host.vinLookupField.text.trim() !== "")
            return Bridge.vinModelYear(host.vinLookupField.text)
        return host.pickedVehicle !== null && host.pickedVehicle.id === obdbCarVehicle ? obdbCarYear : 0
    }

    // The year field as a number: 0 when empty, -1 when it isn't a year
    function obdbYear() {
        const t = host.obdbYearField.text.trim()
        if (t === "")
            return 0
        const y = Number(t)
        return /^[0-9]{4}$/.test(t) && y >= 1980 && y <= 2039 ? y : -1
    }

    // The held set converted for the field's year: its report shown, or
    // the bridge's words why not. True when there's a report. While typing,
    // one to three digits are a year on its way: no report and no refusal
    function previewObdb(typing) {
        host.picksPart.signalNote = ""
        obdbReport = null
        if (pendingImport === null || pendingImport.kind !== "obdb")
            return false
        const year = obdbYear()
        if (year < 0) {
            host.picksPart.signalProblem = typing === true && /^[0-9]{1,3}$/.test(host.obdbYearField.text.trim())
                ? "" : qsTr("A model year is 1980 to 2039, or empty for none")
            return false
        }
        const r = Bridge.previewObdbImport(year, obdbName())
        if (!r.ok) {
            host.picksPart.signalProblem = r.words
            return false
        }
        host.picksPart.signalProblem = ""
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
        host.picksPart.signalNote = ""
        if (!obdbShown)
            return false
        const typed = host.obdbYearField.text.trim() !== "" && !obdbYearFromVin
        const r = Bridge.keepObdbPlugin(obdbVehicle, typed)
        if (!r.ok) {
            host.picksPart.signalProblem = r.words
            return false
        }
        host.picksPart.signalProblem = ""
        pendingImport = null
        obdbReport = null
        const gone = (r.replacedProfiles !== undefined ? r.replacedProfiles : [])
            .filter(p => r.profiles.indexOf(p) < 0)
        if (gone.length > 0 && r.profiles.length === 1) {
            if (gone.indexOf(host.uriPart.profileChoice) >= 0)
                host.uriPart.chooseProfile(r.profiles[0])
            host.saveVehicles(VehicleBook.movePicks(host.vehicles, gone, r.profiles[0]))
        }
        ++host.picksPart.storeRev
        const made = r.merged === true ? r.profiles[r.profiles.length - 1] : r.profiles[0]
        const note = (r.merged === true ? qsTr("%1 %2: kept as a variant of the earlier OBDb import").arg(r.name, r.version)
                      : r.replaced !== undefined && r.replaced !== ""
                        ? qsTr("%1 replaced the vehicle's earlier OBDb import").arg(r.name)
                      : r.kept ? qsTr("%1 is here already").arg(r.name)
                      : qsTr("%1 %2 added").arg(r.name, r.version)) + saidToo(r)
        pickKept(made, note)
        return true
    }
}
