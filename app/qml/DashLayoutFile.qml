// File/layer: app/qml/DashLayoutFile.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard{,_pages,_panel}, QmlTest.dashedit and its parts (QmlTest.dashedit_*), QmlTest.phone_*,
//          QmlTest.linkhealth, QmlTest.driveview{,_sheet,_platform} (test_qml); BENCH, Manual smoke

import QtQuick

pragma ComponentBehavior: Bound

// The Dashboard's layout file: export, load and Reset, a part of DashboardPage

Item {
    id: part
    required property DashboardPage host
    visible: false

    // The layout file (ADR-0057's appendix on the layout file)

    // What the last export or load said, in words; layoutNoteBad when it
    // is a refusal
    property string layoutNote: ""
    property bool layoutNoteBad: false
    function noteLayout(words, bad) {
        layoutNote = words
        layoutNoteBad = bad === true
    }
    // A form's name in the export's note and the load's words, so the two agree
    function formWord(f) {
        return f === "phone" ? qsTr("phone") : qsTr("desktop")
    }
    // The picked vehicle's picks the store has, each as the file a layout
    // names (ADR-0064, "Dashboards") with the references its profile
    // gives: [{ name, sha256, from, profile, refs }]. sha256 is the
    // variant's written file's; from the hashes of the signal files it was
    // made from, which a layout saved by an earlier version names
    function vehicleFiles() {
        const v = VehicleBook.picked(part.host.vehicles)
        if (!v)
            return []
        return Bridge.pickFiles(v.picks).map(f => ({
            name: f.name, sha256: f.sha256, from: f.from, profile: f.profile,
            refs: Bridge.profileSignals("mock://?profile=" + f.profile).map(r => r.ref) }))
    }
    // The name a layout file is offered under
    function layoutFileName() {
        const v = VehicleBook.picked(part.host.vehicles)
        const slug = v ? v.label.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "") : ""
        return "dashboard-" + (slug !== "" ? slug + "-" : "") + part.host.form + ".json"
    }
    // Writes this form's layout, and the picks' files it needs by name,
    // to the file at url. A pick no plugin here has gives no references,
    // so the layout can't say it needs it: the note names the pick, and
    // counts the gauges no pick gives a key for
    function exportLayoutTo(url) {
        part.host.commitTyping()
        const files = vehicleFiles()
        const needed = DashboardBook.neededFiles(part.host.doc, files)
        const v = VehicleBook.picked(part.host.vehicles)
        const unloaded = (v ? v.picks : []).filter(p => p !== "obd2" && !files.some(f => f.profile === p))
        const unbound = DashboardBook.unboundKeyCells(part.host.doc, files)
        const text = DashboardBook.exportText(part.host.doc, part.host.form, needed)
        if (text === "") {
            Bridge.dropPicked(url)
            noteLayout(qsTr("Not saved: this layout can't be written as a file"), true)
            return false
        }
        const r = Bridge.saveLayoutFile(text, url)
        if (!r.ok) {
            noteLayout(qsTr("Not saved: %1").arg(r.words), true)
            return false
        }
        noteLayout(qsTr("Saved this layout to a file. It's a %1 layout: it loads on a %1, not on a %2. "
                        + "A layout isn't scaled between forms")
                       .arg(formWord(part.host.form)).arg(formWord(part.host.form === "phone" ? "desktop" : "phone"))
                   + (needed.length === 0 ? ""
                      : (needed.length === 1 ? qsTr(". It needs the plugin %1") : qsTr(". It needs the plugins %1"))
                            .arg(needed.map(f => f.name).join(", ")))
                   + (unloaded.length === 0 ? ""
                      : qsTr(". The vehicle's picks %1 aren't in the store, so a gauge reading them isn't listed as needing them")
                            .arg(unloaded.join(", ")))
                   + (unbound === 0 ? ""
                      : (unbound === 1 ? qsTr(". 1 gauge reads a signal none of the vehicle's picked plugins has, so the file can't name a plugin for it")
                                       : qsTr(". %1 gauges read signals none of the vehicle's picked plugins has, so the file can't name a plugin for them").arg(unbound)))
                   + ".")
        return true
    }
    // Loads a layout file as this form's layout, one undo step. Refused in
    // words for a file that can't be read, a newer one and one of the other
    // form; never while Reset's or Remove's question is open
    function loadLayoutFrom(url, adding) {
        if (part.host.confirmReset || part.host.confirmRemovePage || part.host.confirmReplaceKey !== "") {
            noteLayout(qsTr("Answer the question first"), true)
            return false
        }
        const r = Bridge.layoutFileText(url)
        if (!r.ok) {
            noteLayout(qsTr("Not loaded: %1").arg(r.words), true)
            return false
        }
        // A pick is found by its written file's hash, or by the hash of a
        // signal file it was made from
        const files = vehicleFiles()
        const have = []
        for (const f of files) {
            have.push(f.sha256)
            for (let i = 0; i < f.from.length; ++i)
                have.push(f.from[i])
        }
        const read = DashboardBook.importProblem(r.text, part.host.form, have)
        if (read.problem === "none") {
            noteLayout(qsTr("Not loaded: the file is empty"), true)
        } else if (read.problem === "damaged") {
            noteLayout(qsTr("Not loaded: the file isn't a dashboard layout this version can read"), true)
        } else if (read.problem === "newer") {
            noteLayout(qsTr("Not loaded: the file was made by a newer version of Havne"), true)
        } else if (read.problem === "otherform") {
            noteLayout(qsTr("Not loaded: the file is a %1 layout and this is the %2 form. "
                            + "A layout isn't scaled between forms")
                       .arg(formWord(read.form)).arg(formWord(part.host.form)), true)
        } else {
            part.host.commitTyping()
            part.host.cancelHold()
            const words = [adding === true ? qsTr("Added the file's pages") : qsTr("Loaded the layout")]
            if (read.problem === "dropped")
                words.push(qsTr("%1 left out because they couldn't be read").arg(part.host.droppedText(read.dropped)))
            if (read.notes.indexOf("no-form") >= 0)
                words.push(qsTr("the file named no form, so it was read as the %1 form").arg(formWord(part.host.form)))
            // A pick changed since the save (Replace, an edit in the form)
            // is picked still: only its file is another
            const changedPicks = DashboardBook.changedFiles(read.missing, files)
            const absent = read.missing.filter(m => changedPicks.indexOf(m) < 0)
            if (changedPicks.length > 0)
                words.push(qsTr("it was saved with another version of %1: a gauge reading a signal the "
                                + "picked version lacks reads not in profile")
                           .arg(changedPicks.map(f => f.name).join(", ")))
            if (absent.length > 0)
                words.push(qsTr("it needs plugins this vehicle doesn't pick: %1. Import or pick them under "
                                + "Signals; until then those gauges read not in profile")
                           .arg(absent.map(f => f.name).join(", ")))
            let changed
            if (adding === true) {
                part.host.presetList = ""
                const after = part.host.pageIndex
                changed = part.host.edit(DashboardBook.insertPages(part.host.doc, after, read.doc.pages),
                               part.host.historyPart.stepOf("", qsTr("added pages from a file"), -1, -1, after + 1))
                if (!changed) {
                    noteLayout(qsTr("Not added: a form holds %1 pages at most").arg(DashboardBook.maxPages), true)
                    return false
                }
                part.host.selected = -1
                part.host.panel = ""
                part.host.goTo(after + 1)
            } else {
                changed = part.host.edit(read.doc, part.host.historyPart.stepOf("", qsTr("load a layout"), -1, -1, 0))
                if (!changed)
                    words[0] = qsTr("The file holds the layout already shown")
                else {
                    part.host.selected = -1
                    part.host.panel = ""
                    part.host.goTo(0)
                }
            }
            noteLayout(words.join(". "))
            return changed
        }
        return false
    }

    // Back to the built-in layout: nothing stored, so a later version's
    // built-in layout reaches it. Undo stores the replaced pages again
    function resetForm() {
        if (part.host.layoutStore[part.host.formKey] !== "")
            part.host.historyPart.record(part.host.historyPart.shownText(), "", part.host.historyPart.stepOf("", qsTr("reset the layout"), -1, -1, 0))
        const texts = DashboardBook.saveTexts(part.host.problem, part.host.layoutStore[part.host.formKey], part.host.doc)
        if (texts.unread !== "")
            part.host.layoutStore[part.host.formKey + "Unread"] = texts.unread
        part.host.layoutStore[part.host.formKey] = ""
        part.host.historyPart.showStoredLayout()
        part.host.confirmReset = false
        part.host.moreShown = false
        part.host.selected = -1
        part.host.panel = ""
        part.host.goTo(0)
    }
}
