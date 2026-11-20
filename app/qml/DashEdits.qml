// File/layer: app/qml/DashEdits.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit and its parts (QmlTest.dashedit_*), QmlTest.phone,
//          QmlTest.linkhealth, QmlTest.driveview (test_qml); BENCH, Manual smoke

import QtQuick

pragma ComponentBehavior: Bound

// The Dashboard's edits of a cell and of the pages, a part of DashboardPage

Item {
    id: part
    required property DashboardPage host
    visible: false

    // The cell's panel

    // The selected cell's options, on `base` when given (a write already
    // composed for the same step). A run of a stepped option on one cell
    // is one undo step; a typed one never groups, since a field commits once
    function setOptions(changes, base) {
        const names = Object.keys(changes).sort()
        const key = names.every(n => part.host.historyPart.steppedOptions.indexOf(n) >= 0)
            ? "opt/" + part.host.pageIndex + "/" + part.host.selected + "/" + names.join(",") : ""
        const word = part.host.historyPart.optionWords[names[0]] ?? names[0]
        return part.host.edit(DashboardEdit.setOptions(base ?? part.host.doc, part.host.pageIndex, part.host.selected, changes),
                    part.host.historyPart.stepOf(key, qsTr("changed %1's %2").arg(part.host.cellName(part.host.selectedCell)).arg(word),
                           part.host.selected, part.host.selected))
    }
    function setLabel(text) {
        return part.host.edit(DashboardEdit.setCell(part.host.doc, part.host.pageIndex, part.host.selected,
                                          { label: text === "" ? null : text }),
                    part.host.historyPart.stepOf("", qsTr("changed %1's label").arg(part.host.cellName(part.host.selectedCell)), part.host.selected, part.host.selected))
    }
    function resize(w, h) {
        return part.host.edit(DashboardEdit.resizeCell(part.host.doc, part.host.pageIndex, part.host.selected, w, h),
                    part.host.historyPart.stepOf("size/" + part.host.pageIndex + "/" + part.host.selected,
                           qsTr("resized %1").arg(part.host.cellName(part.host.selectedCell)), part.host.selected, part.host.selected))
    }
    // Earlier, later, to the start and to the end: the cell's place in
    // the order, which both forms' grids follow
    function moveTo(to) {
        const t = Math.max(0, Math.min(part.host.shownCells.length - 1, to))
        if (part.host.edit(DashboardEdit.moveCell(part.host.doc, part.host.pageIndex, part.host.selected, t),
                 part.host.historyPart.stepOf("", qsTr("moved %1").arg(part.host.cellName(part.host.selectedCell)), part.host.selected, t)))
            part.host.selected = t
    }
    function removeCell(i) {
        part.host.commitTyping()
        if (!part.host.edit(DashboardEdit.removeCell(part.host.doc, part.host.pageIndex, i),
                  part.host.historyPart.stepOf("", qsTr("removed %1").arg(part.host.cellName(part.host.shownCells[i])), i, -1)))
            return
        if (part.host.selected === i) {
            part.host.selected = -1
            part.host.panel = ""
        } else if (part.host.selected > i) {
            --part.host.selected
        }
    }

    // The pages' edits

    function addPage() {
        if (part.host.edit(DashboardEdit.addPage(part.host.doc, qsTr("Page %1").arg(part.host.pages.length + 1)),
                 part.host.historyPart.stepOf("", qsTr("added a page"), -1, -1, part.host.pages.length))) {
            part.host.goTo(part.host.pages.length - 1)
            askForPageName()
        }
    }
    // The new page's name is the first thing to type: the Page row opens on it
    function askForPageName() {
        part.host.moreShown = true
        part.host.pageNameWanted()
    }
    // The preset list (the "+" tab, Add page, Replace this page with...)
    function openPresets(mode) {
        part.host.commitTyping()
        part.host.confirmReset = false
        part.host.confirmRemovePage = false
        part.host.confirmReplaceKey = ""
        part.host.presetList = mode
        loadPluginPages()
    }
    // The loaded plugins' pages of this form (ADR-0052's appendix on pages),
    // read when the list opens: [{ id, name, picked, pages: [{ index, name }] }],
    // the picked vehicle's plugins first. Plain copies, never the bridge's maps
    property var pluginPageList: []
    function loadPluginPages() {
        const picked = {}
        for (const s of part.host.pickedSets)
            picked[s.profile] = true
        const out = []
        for (const p of Bridge.plugins()) {
            if (!p.ok)
                continue
            const pages = []
            for (const g of p.pages) {
                if (g.form !== part.host.form)
                    continue
                const r = DashboardBook.importProblem(Bridge.pluginPageText(p.id, g.index), part.host.form, [])
                // A refused text answers with the built-in page, so only a read one gives a name
                if ((r.problem !== "" && r.problem !== "dropped") || r.doc.pages.length !== 1)
                    continue
                pages.push({ index: g.index, name: r.doc.pages[0].name })
            }
            if (pages.length > 0)
                out.push({ id: String(p.id), name: String(p.name), pages: pages,
                           picked: Array.from(p.profiles).some(q => picked[q] === true) })
        }
        pluginPageList = out.filter(e => e.picked).concat(out.filter(e => !e.picked))
    }
    // A plugin's page as the user's own copy after the shown page; one undo
    // step. Whole whether or not the vehicle's picks have its signals: those
    // gauges read not in profile, and the note counts them
    function addPluginPage(id, index) {
        const entry = pluginPageList.find(e => e.id === id)
        const plugin = entry ? entry.name : id
        const r = DashboardBook.importProblem(Bridge.pluginPageText(id, index), part.host.form, [])
        part.host.presetList = ""
        if (!DashboardEdit.canAddPage(part.host.doc)) {
            part.host.layoutFilePart.noteLayout(qsTr("Not added: a form holds %1 pages at most").arg(DashboardBook.maxPages), true)
            return false
        }
        if ((r.problem !== "" && r.problem !== "dropped") || r.doc.pages.length !== 1) {
            part.host.layoutFilePart.noteLayout(qsTr("The page couldn't be read"), true)
            return false
        }
        const page = r.doc.pages[0]
        const after = part.host.pageIndex
        if (!part.host.edit(DashboardBook.insertPages(part.host.doc, after, [page]),
                  part.host.historyPart.stepOf("", qsTr("added the page %1").arg(page.name), -1, -1, after + 1))) {
            part.host.layoutFilePart.noteLayout(qsTr("The page couldn't be added"), true)
            return false
        }
        part.host.goTo(after + 1)
        askForPageName()
        const unbound = DashboardBook.unboundKeyCells({ pages: [page] }, part.host.layoutFilePart.vehicleFiles())
        const words = [qsTr("Added the page %1 from the plugin %2").arg(page.name).arg(plugin)]
        if (r.problem === "dropped")
            words.push(qsTr("%1 left out because they couldn't be read").arg(part.host.droppedText(r.dropped)))
        if (unbound > 0)
            words.push(unbound === 1
                       ? qsTr("1 gauge reads a signal none of the vehicle's picked plugins has: pick the plugin's variant under Signals")
                       : qsTr("%1 gauges read signals none of the vehicle's picked plugins has: pick the plugin's variant under Signals").arg(unbound))
        part.host.layoutFilePart.noteLayout(words.join(". "))
        return true
    }
    function closePresets() {
        part.host.presetList = ""
        part.host.confirmReplaceKey = ""
    }
    function addBlankPage() {
        part.host.presetList = ""
        addPage()
    }
    // A preset as a new page after the shown one; one undo step
    function addPresetPage(key) {
        const page = DashboardBook.presetPage(key, part.host.formKey)
        part.host.presetList = ""
        if (page === null) {
            part.host.layoutFilePart.noteLayout(qsTr("The preset couldn't be read"), true)
            return false
        }
        const after = part.host.pageIndex
        if (!part.host.edit(DashboardBook.insertPages(part.host.doc, after, [page]),
                  part.host.historyPart.stepOf("", qsTr("added the page %1").arg(page.name), -1, -1, after + 1)))
            return false
        part.host.goTo(after + 1)
        askForPageName()
        return true
    }
    // True when the shown page's gauges are one preset's: replacing them
    // loses nothing a preset doesn't give back
    function shownPageIsPreset() {
        return DashboardBook.losesNothing(part.host.pages[part.host.pageIndex], part.host.formKey)
    }
    function replaceChosen(key) {
        part.host.presetList = ""
        if (DashboardBook.presetPage(key, part.host.formKey) === null)
            part.host.layoutFilePart.noteLayout(qsTr("The preset couldn't be read"), true)
        else if (shownPageIsPreset())
            replaceWithPreset(key)
        else
            part.host.confirmReplaceKey = key
    }
    function replaceWithPreset(key) {
        part.host.confirmReplaceKey = ""
        part.host.presetList = ""
        const page = DashboardBook.presetPage(key, part.host.formKey)
        if (page === null) {
            part.host.layoutFilePart.noteLayout(qsTr("The preset couldn't be read"), true)
            return false
        }
        part.host.selected = -1
        part.host.panel = ""
        part.host.addKind = ""
        return part.host.edit(DashboardBook.replacePage(part.host.doc, part.host.pageIndex, page),
                    part.host.historyPart.stepOf("", qsTr("replaced the page with %1").arg(page.name), -1, -1))
    }
    function presetLabel(key) {
        const p = DashboardBook.presets.find(q => q.key === key)
        return p ? p.label : key
    }
    function movePage(by) {
        const to = part.host.pageIndex + by
        if (part.host.edit(DashboardEdit.movePage(part.host.doc, part.host.pageIndex, to), part.host.historyPart.stepOf("", qsTr("moved the page"), -1, -1, to)))
            part.host.goTo(to)
    }
    function renamePage(name) {
        part.host.edit(DashboardEdit.renamePage(part.host.doc, part.host.pageIndex, name), part.host.historyPart.stepOf("", qsTr("renamed the page"), -1, -1))
    }
    // Another page takes this one's index, so nothing stays selected
    function removePage() {
        part.host.confirmRemovePage = false
        part.host.selected = -1
        part.host.panel = ""
        part.host.addKind = ""
        const p = part.host.pages[part.host.pageIndex]
        const name = p && p.name ? p.name : qsTr("Page %1").arg(part.host.pageIndex + 1)
        part.host.edit(DashboardEdit.removePage(part.host.doc, part.host.pageIndex),
             part.host.historyPart.stepOf("", qsTr("removed the page %1").arg(name), -1, -1,
                    Math.max(0, Math.min(part.host.pageIndex, part.host.pages.length - 2))))
    }
}
