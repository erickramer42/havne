// File/layer: app/qml/SettingsRowActions.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.settings_*  (test_qml)

import QtQuick

pragma ComponentBehavior: Bound

Item {
    required property SettingsPage host

    // A row's actions
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
        host.picksPart.signalNote = ""
        const p = host.picksPart.pluginBySha[sha]
        const r = Bridge.markPluginMine(sha)
        host.picksPart.signalProblem = r.ok ? "" : r.words
        if (r.ok) {
            copyShown = false   // Copy is for someone else's
            host.picksPart.signalNote = qsTr("%1 is yours now").arg(p !== undefined ? p.name : qsTr("The plugin"))
                         + host.importPart.saidToo(r)
        }
        ++host.picksPart.storeRev
    }

    // Copy to my plugin (ADR-0064 Q8): the variant into a new plugin of
    // the user's own (into "") or one of theirs
    function copyVariantTo(row, into) {
        host.picksPart.signalNote = ""
        const r = Bridge.copyVariant(row.sha256, row.variant, into, "")
        host.picksPart.signalProblem = r.ok ? "" : r.words
        if (!r.ok)
            return false
        copyShown = false
        ++host.picksPart.storeRev
        host.picksPart.signalNote = qsTr("%1 copied into %2 %3").arg(host.picksPart.profileName(row.profile), r.name, r.version)
                     + host.importPart.saidToo(r)
        return true
    }

    // A variant out of a plugin of the user's own, every vehicle's pick
    // of it undone
    function removeVariant(row) {
        host.picksPart.signalNote = ""
        const r = Bridge.dropPluginVariant(row.sha256, row.variant)
        host.picksPart.signalProblem = r.ok ? "" : r.words
        if (!r.ok)
            return false
        host.saveVehicles(VehicleBook.dropProfiles(host.vehicles, [row.profile]))
        if (host.uriPart.profileChoice === row.profile)
            host.uriPart.chooseProfile("default")
        openRow = ""
        ++host.picksPart.storeRev
        return true
    }

    // A variant of the user's own has no copy anywhere else either: its
    // Remove asks first, as the whole plugin's does
    function askRemoveVariant(row) {
        host.confirming = "removeVariant:" + row.profile
    }

    // A plugin of the user's own has no copy anywhere else, so its
    // removal asks first
    function askRemovePlugin(sha) {
        const p = host.picksPart.pluginBySha[sha]
        if (p !== undefined && p.mine)
            host.confirming = "removePlugin:" + sha
        else
            removePlugin(sha)
    }

    function confirmingPlugin() {
        for (const head of ["removePlugin:", "removeOwn:"]) {
            if (host.confirming.startsWith(head))
                return host.picksPart.pluginBySha[host.confirming.substring(head.length)] || null
        }
        return null
    }
    function pluginQuestion(p) {
        return qsTr("Remove %1? It is yours, and no other copy exists").arg(p.name)
    }
    // The question a row of the list asks, "" for none: a plugin's in its
    // open row, where its Remove was pressed, and a variant's in its own
    function rowQuestion(entry, open) {
        if (open && host.confirming === "removePlugin:" + entry.sha256 && confirmingPlugin() !== null)
            return pluginQuestion(confirmingPlugin())
        if (host.confirming === "removeVariant:" + entry.profile)
            return qsTr("Remove the variant %1 of %2? It is yours, and no other copy exists")
                       .arg(entry.variant, entry.name)
        return ""
    }
    function ownQuestion() {
        const p = confirmingPlugin()
        if (!host.confirming.startsWith("removeOwn:") || p === null || host.ownPlace === null)
            return ""
        return p.variants.length > 1
            ? qsTr("Remove the variant %1 of %2? It is yours, and no other copy exists")
                  .arg(host.ownPlace.variant, p.name)
            : pluginQuestion(p)
    }
    // The question's Remove: what it asked about goes
    function confirmRemoval() {
        const asked = host.confirming
        const p = confirmingPlugin()
        host.confirming = ""
        if (asked.startsWith("removeVariant:")) {
            const row = host.picksPart.variantRows.find(r => "removeVariant:" + r.profile === asked)
            return row !== undefined && removeVariant(row)
        }
        if (asked.startsWith("removeUnmigrated:")) {
            const record = unmigratedRows.find(r => "removeUnmigrated:" + unmigratedKey(r) === asked)
            if (record !== undefined)
                removeUnmigrated(record)
            return record !== undefined
        }
        if (p !== null && asked.startsWith("removeOwn:") && p.variants.length > 1 && host.ownPlace !== null)
            return removeVariant({ sha256: p.sha256, variant: host.ownPlace.variant,
                                   profile: host.pickedVehicle.own })
        return p !== null && removePlugin(p.sha256)
    }

    // A removed plugin's variants are no vehicle's pick or own variant any
    // more, and the field reads Default when it named one
    function removePlugin(sha) {
        host.confirming = ""
        host.picksPart.signalNote = ""
        const p = host.picksPart.pluginBySha[sha]
        const profiles = p !== undefined ? p.profiles : []
        const r = Bridge.removePlugin(sha)
        host.picksPart.signalProblem = r.ok ? "" : r.words
        if (r.ok && profiles.length > 0) {
            host.saveVehicles(VehicleBook.dropProfiles(host.vehicles, profiles))
            if (profiles.indexOf(host.uriPart.profileChoice) >= 0)
                host.uriPart.chooseProfile("default")
        }
        if (r.ok)
            openRow = ""
        ++host.picksPart.storeRev
        return r.ok
    }

    // Saves out of the app, through the one save dialog: a variant
    // as a signal file, a plugin's packed copy, a file that didn't move.
    // { kind: "variant" | "plugin" | "unmigrated", sha256, variant, name,
    // file }, null for none
    property var saving: null

    function openSave(what) {
        saving = what
        host.signalsPart.saveDialog.title = what.kind === "plugin" ? qsTr("Export the plugin")
                                                  : qsTr("Save as a signal file")
        host.signalsPart.saveDialog.nameFilters = what.kind === "plugin"
            ? [qsTr("Plugins (*.havne-plugin.json)"), qsTr("All files (*)")]
            : [qsTr("Signal files (*.json)"), qsTr("All files (*)")]
        host.signalsPart.saveDialog.selectedFile = host.signalsPart.saveDialog.currentFolder.toString().replace(/\/$/, "")
                                  + "/" + what.file
        host.signalsPart.saveDialog.open()
    }

    // What each save writes and the name it starts from: a variant's
    // signal file by its plugin's id, with the variant where the plugin
    // has several; a plugin's packed copy; a file that didn't move
    function variantSave(row) {
        return { kind: "variant", sha256: row.sha256, variant: row.variant,
                 name: host.picksPart.profileName(row.profile),
                 file: (row.several ? row.id + "-" + row.variant : row.id) + ".json" }
    }
    function pluginSave(sha) {
        const p = host.picksPart.pluginBySha[sha]
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
        host.picksPart.signalNote = ""
        const r = Bridge.pluginExportNote(sha)
        host.picksPart.signalProblem = r.ok ? "" : r.words
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
        host.picksPart.signalProblem = r.ok ? "" : r.words
        host.picksPart.signalNote = !r.ok ? ""
                   : what.kind === "plugin" ? qsTr("%1 exported to the file you chose").arg(what.name)
                   : qsTr("%1 saved to the file you chose").arg(what.name)
        return r.ok
    }

    // The signal files the move into the plugin store left behind
    // (ADR-0064 Q6): the picked vehicle's, then the book's own, each
    // { sha256, name, words, kept, loose }
    readonly property var unmigratedRows:
        (host.pickedVehicle !== null && host.pickedVehicle.unmigrated !== undefined
         ? host.pickedVehicle.unmigrated.map(r => ({ sha256: r.sha256, name: unmigratedName(r.name), words: r.words,
                                                kept: r.kept !== false, loose: false })) : [])
        .concat(host.vehicles.unmigrated !== undefined
                ? host.vehicles.unmigrated.map(r => ({ sha256: r.sha256, name: unmigratedName(r.name), words: r.words,
                                                  kept: r.kept !== false, loose: true })) : [])

    // A record may carry no name (a copy no vehicle lists): the row says so
    // in words instead of leading with a lone colon
    function unmigratedName(name) {
        return typeof name === "string" && name.trim() !== "" ? name : qsTr("A signal file")
    }

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
        const after = record.loose ? VehicleBook.dropLooseUnmigrated(host.vehicles, record.sha256)
                                   : VehicleBook.dropUnmigrated(host.vehicles, record.sha256)
        if (record.kept && !VehicleBook.hashListed(after, record.sha256))
            host.confirming = "removeUnmigrated:" + unmigratedKey(record)
        else
            removeUnmigrated(record)
    }
    function unmigratedQuestion(record) {
        return host.confirming === "removeUnmigrated:" + unmigratedKey(record)
            ? qsTr("Remove %1? Its kept copy goes with it").arg(record.name) : ""
    }

    // The record out of the book; its kept bytes go once nothing lists
    // them. The earlier version's folder is never touched
    function removeUnmigrated(record) {
        host.picksPart.signalProblem = ""
        host.picksPart.signalNote = ""
        host.saveVehicles(record.loose ? VehicleBook.dropLooseUnmigrated(host.vehicles, record.sha256)
                                  : VehicleBook.dropUnmigrated(host.vehicles, record.sha256))
        if (!VehicleBook.hashListed(host.vehicles, record.sha256))
            Bridge.removeUnmigratedFile(record.sha256)
    }

    // The VIN's matches
    // The session's matches kept on the vehicle its VIN identified, so
    // they are marked with no session (ADR-0052's appendix of 2026-11-13);
    // only that vehicle's: a replay of another car names none
    function noteVinMatches() {
        if (host.pickedVehicle === null || host.pickedVehicle.vinKey === ""
                || Bridge.vinKey(host.vehicles.salt) !== host.pickedVehicle.vinKey)
            return
        const profiles = Bridge.pluginMatches("").map(m => m.profile)
        if (JSON.stringify(profiles) !== JSON.stringify(host.pickedVehicle.vinMatches))
            host.saveVehicles(VehicleBook.setVinMatches(host.vehicles, profiles))
    }
}
