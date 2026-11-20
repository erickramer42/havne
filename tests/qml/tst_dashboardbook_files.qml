// File/layer: tests/qml/tst_dashboardbook_files.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashboardbook_files (test_qml)

import QtQuick
import QtTest
import App
import AppTest

DashboardBookKit {
    name: "DashboardBook"

    // ---- The layout file (ADR-0057's appendix on the layout file)

    readonly property string sha: "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

    function pagesAndCells(doc) {
        return [doc.pages.length, doc.pages.reduce((n, p) => n + p.cells.length, 0)]
    }

    function test_exportThenImportReturnsTheDocument_data() {
        return [{ tag: "phone", form: "phone" }, { tag: "desktop", form: "desktop" }]
    }

    function test_exportThenImportReturnsTheDocument(data) {
        const doc = book()
        doc.unknownKey = { kept: true }
        const files = [{ name: "Boost.json", sha256: sha }]
        const text = DashboardBook.exportText(doc, data.form, files)
        verify(text !== "", "the document exports")
        const raw = JSON.parse(text)
        compare(raw.form, data.form)
        compare(raw.files, files)
        compare(Object.keys(raw).slice(0, 3), ["format", "form", "files"])
        compare(Object.keys(raw)[Object.keys(raw).length - 1], "pages", "the pages come last")
        const r = DashboardBook.importProblem(text, data.form, [sha])
        compare(r.problem, "")
        compare(r.doc, doc, "the form and files keys never reach the document")
        compare(r.form, data.form)
        compare(r.files, files)
        compare(r.missing, [])
        compare(r.notes, [])
        compare(DashboardBook.importProblem(text, data.form, []).missing, files, "a file the vehicle lacks")
        compare(DashboardBook.importProblem(text, data.form, undefined).missing, files, "no vehicle: every file")
    }

    function test_theOtherFormIsRefusedInTheWordsOfItsProblem() {
        const text = DashboardBook.exportText(book(), "phone", [])
        const r = DashboardBook.importProblem(text, "desktop", [])
        compare(r.problem, "otherform")
        compare(r.form, "phone")
        compare(r.doc, DashboardBook.builtIn("desktop"), "the built-in layout stands in")
    }

    function test_anExportOfADocumentThatDoesNotReadBackIsRefused() {
        const doc = book()
        doc.pages[0].cells[0].w = 0
        compare(DashboardBook.exportText(doc, "desktop", []), "")
        compare(DashboardBook.exportText(book(), "tablet", []), "")
        compare(DashboardBook.exportText(book(), "desktop", "x"), "")
        compare(DashboardBook.exportText(book(), "desktop", [{ name: "a", sha256: "short" }]), "")
        const many = []
        for (let i = 0; i < DashboardBook.maxFiles + 1; ++i)
            many.push({ name: "f" + i, sha256: sha })
        compare(DashboardBook.exportText(book(), "desktop", many), "")
    }

    function test_aFullDocumentWithItsFilesStillLoads() {
        const doc = { format: DashboardBook.format, pages: [] }
        for (let p = 0; p < DashboardBook.maxPages; ++p) {
            const cells = []
            for (let c = 0; c < DashboardBook.maxCells; ++c)
                cells.push(cell("C" + c, { label: "x".repeat(DashboardBook.maxLabel),
                                           refs: ["id:1", "id:2", "id:3", "id:4", "id:5", "id:6", "id:7", "id:8"] }))
            doc.pages.push({ name: "P" + p, cells: cells })
        }
        verify(DashboardBook.documentProblem(doc) === "", "the fullest document is within its bound")
        const files = []
        for (let i = 0; i < DashboardBook.maxFiles; ++i)
            files.push({ name: "n".repeat(DashboardBook.maxFileName), sha256: sha })
        const text = DashboardBook.exportText(doc, "desktop", files)
        verify(text !== "")
        const r = DashboardBook.importProblem(text, "desktop", [sha])
        compare(r.problem, "", "the keys' own bytes are allowed for")
        compare(r.files.length, DashboardBook.maxFiles)
    }

    function test_theFilesALayoutNeeds() {
        const doc = { format: DashboardBook.format, pages: [
            { name: "A", cells: [cell("a", { refs: ["id:1005", "key:boost"] })] },
            { name: "B", cells: [cell("b", { refs: ["key:oil"] })] }] }
        const vehicle = [
            { name: "Boost.json", sha256: sha, refs: ["id:30000", "key:boost"] },
            { name: "Other.json", sha256: "b".repeat(64), refs: ["key:other", "id:1005"] },
            { name: "Oil.json", sha256: "c".repeat(64), refs: ["key:oil"] }]
        compare(DashboardBook.neededFiles(doc, vehicle).map(f => f.name), ["Boost.json", "Oil.json"],
                "a file is needed by a key: reference in any cell of any page")
        compare(DashboardBook.neededFiles(doc, vehicle)[0], { name: "Boost.json", sha256: sha },
                "its name and sha256 only")
        compare(DashboardBook.neededFiles(doc, []), [])
    }

    // A file's name is cut to its bound in code points, a variant's part
    // whole, and a needed file is named so
    function test_aFilesNameIsCutToItsBoundWithItsVariantWhole() {
        const most = DashboardBook.maxFileName
        const variant = ": " + "v".repeat(24)
        compare(DashboardBook.fittedFileName("Boost"), "Boost")
        compare(DashboardBook.fittedFileName("n".repeat(most)), "n".repeat(most), "a name at the bound is whole")
        compare(DashboardBook.fittedFileName("n".repeat(most + 1)), "n".repeat(most - 3) + "...")
        compare(DashboardBook.fittedFileName("n".repeat(63) + variant), "n".repeat(most - 29) + "..." + variant,
                "the plugin's name gives way, the variant stays whole")
        compare(DashboardBook.fittedFileName("a: " + "n".repeat(60) + variant),
                "a: " + "n".repeat(most - 32) + "..." + variant, "the last \": \" is the variant's")
        const wide = String.fromCodePoint(0x1F600)
        compare(DashboardBook.fittedFileName(wide.repeat(most)), wide.repeat(most), "counted in code points")
        compare(DashboardBook.fittedFileName(wide.repeat(most + 1)), wide.repeat(most - 3) + "...",
                "and cut between them")
        compare(DashboardBook.fittedFileName("a: " + "v".repeat(most)), "a: " + "v".repeat(most - 6) + "...",
                "a part after \": \" too long to keep is cut with the rest")

        const doc = { format: DashboardBook.format, pages: [
            { name: "A", cells: [cell("a", { refs: ["key:boost"] })] }] }
        const needed = DashboardBook.neededFiles(
            doc, [{ name: "n".repeat(63) + variant, sha256: sha, refs: ["key:boost"] }])
        compare(needed, [{ name: "n".repeat(most - 29) + "..." + variant, sha256: sha }])
        verify(DashboardBook.exportText(doc, "desktop", needed) !== "", "so the layout can be written")
    }

    // A cell reading a "key:" signal no file of the vehicle gives is one a
    // layout can't name a plugin for; one gauge counts once
    function test_aKeyCellNoFileGivesIsCounted() {
        const doc = { format: DashboardBook.format, pages: [
            { name: "A", cells: [
                cell("own", { refs: ["key:boost"] }),
                cell("two keys", { refs: ["key:gone", "key:also_gone", "id:7001"] }),
                cell("built in", { refs: ["id:7001"] }),
                cell("blank", { refs: [] })] },
            { name: "B", cells: [cell("again", { refs: ["key:gone"] })] }] }
        const files = [{ name: "Plugin", sha256: sha, refs: ["key:boost", "id:7001"] }]
        compare(DashboardBook.unboundKeyCells(doc, files), 2, "two cells, the one with two keys once")
        compare(DashboardBook.unboundKeyCells(doc, []), 3, "no vehicle file: every key cell")
        compare(DashboardBook.unboundKeyCells(doc, [{ name: "More", sha256: sha,
                                                      refs: ["key:boost", "key:gone"] }]), 0,
                "a key of any of a cell's refs is enough")
        compare(DashboardBook.unboundKeyCells({ format: DashboardBook.format, pages: [] }, files), 0)
    }

    // A missing file a pick is named as is a pick that changed, not one
    // the vehicle lacks
    function test_aMissingFileNamedAsAPickIsAChangedPick() {
        const variant = ": " + "v".repeat(24)
        const long = "n".repeat(63) + variant
        const picks = [{ name: "Boosty" }, { name: "Shared: base" }, { name: long }]
        const missing = names => names.map(n => ({ name: n, sha256: sha }))
        const changed = names => DashboardBook.changedFiles(missing(names), picks).map(f => f.name)
        compare(changed(["Boosty"]), ["Boosty"], "the pick's own name")
        compare(changed(["Shared"]), ["Shared"], "the plugin's name, where the pick names its variant too")
        compare(changed([DashboardBook.fittedFileName(long)]), [DashboardBook.fittedFileName(long)],
                "a name the layout cut")
        compare(changed(["Boost", "Shared: other", "Share", "Else"]), [], "no other name")
        compare(changed(["Else", "Boosty"]), ["Boosty"])
        compare(DashboardBook.changedFiles(missing(["Boosty"]), []), [])
    }

    function test_anExportPastTheFilesBoundIsRefused() {
        // A control character escapes to 6 bytes: each name fits fileFits
        const files = []
        for (let i = 0; i < DashboardBook.maxFiles; ++i)
            files.push({ name: String.fromCharCode(1).repeat(DashboardBook.maxFileName - 2) + (i % 10) + (i % 7), sha256: sha })
        verify(files.every(DashboardBook.fileFits))
        compare(DashboardBook.exportText(book(), "desktop", files), "")
        verify(DashboardBook.exportText(book(), "desktop", files.slice(0, 4)) !== "", "a few of them fit")
    }

    function test_aTextPastTheFilesBoundIsDamaged() {
        const text = DashboardBook.exportText(book(), "desktop", [])
        const padded = text.slice(0, -1) + ',"pad":"' + "x".repeat(DashboardBook.maxBytes
                                                                 + DashboardBook.fileKeysBytes) + '"}'
        compare(DashboardBook.importProblem(padded, "desktop", []).problem, "damaged")
        compare(DashboardBook.importProblem(text, "desktop", []).problem, "")
        compare(DashboardBook.importProblem("", "desktop", []).problem, "none")
    }

    // The bridge reads a layout file up to the bound the book judges by
    function test_theBridgesFileBoundIsTheBooks() {
        compare(Bridge.layoutFileMaxBytes, DashboardBook.maxBytes + DashboardBook.fileKeysBytes)
    }

    // havne-cli's `layout check` is a C++ port of importProblem: both read
    // every case of tests/data/layouts to the verdicts beside it
    function test_theLayoutCorpus_data() {
        return Harness.layoutCases().map(n => ({ tag: n, name: n }))
    }

    function test_theLayoutCorpus(data) {
        const file = Bridge.layoutFileText(Harness.layoutUrl(data.name, "json"))
        verify(file.ok, "the case reads: " + file.words)
        const expected = Bridge.layoutFileText(Harness.layoutUrl(data.name, "expect.json"))
        verify(expected.ok, "its verdicts read: " + expected.words)
        const runs = JSON.parse(expected.text)
        verify(runs.length > 0)
        for (const run of runs) {
            const where = data.name + " as " + run.as
            const r = DashboardBook.importProblem(file.text, run.as, run.vehicle ?? [])
            compare(r.problem, run.problem, where + ": problem")
            compare(r.form, run.form, where + ": form")
            if (run.problem !== "" && run.problem !== "dropped")
                continue
            const counts = pagesAndCells(r.doc)
            compare(counts[0], run.pages, where + ": pages")
            compare(counts[1], run.cells, where + ": cells")
            compare(r.dropped, run.dropped, where + ": dropped")
            compare(r.files, run.files, where + ": files")
            compare(r.notes, run.notes, where + ": notes")
            if (run.first_cell !== undefined) {
                compare(r.doc.pages[0].cells[0].w, run.first_cell.w, where + ": first cell's width")
                compare(r.doc.pages[0].cells[0].h, run.first_cell.h, where + ": first cell's height")
            }
            if (run.missing !== undefined)
                compare(r.missing.map(f => f.name), run.missing, where + ": missing")
        }
    }

    // ---- Presets (the Dashboard's Standard, Temperatures and Showcase)

    function canon(v) {
        return DashboardBook.canonical(v)
    }

    // Each form's three files read, hold one page of their own name and
    // fit the form's bounds, and Standard is the built-in page's cells
    function test_eachPresetFileReadsAsOnePage_data() {
        const rows = []
        for (const form of ["phone", "desktop"]) {
            for (const p of DashboardBook.presets)
                rows.push({ tag: form + " " + p.key, form: form, key: p.key, label: p.label })
        }
        return rows
    }

    function test_eachPresetFileReadsAsOnePage(data) {
        const text = DashboardBook.presetText(data.key, data.form)
        verify(text !== "", "the file is in the resources")
        const r = DashboardBook.importProblem(text, data.form, [])
        compare(r.problem, "", "reads whole, nothing dropped")
        compare(r.form, data.form)
        compare(r.files, [], "needs no plugin's file")
        compare(r.doc.pages.length, 1)
        compare(r.doc.pages[0].name, data.label)
        compare(DashboardBook.documentProblem(r.doc), "")
        verify(DashboardBook.presetPage(data.key, data.form) !== null)
    }

    function test_theStandardPresetIsTheBuiltInPage_data() {
        return [{ tag: "phone", form: "phone" }, { tag: "desktop", form: "desktop" }]
    }

    function test_theStandardPresetIsTheBuiltInPage(data) {
        compare(canon(DashboardBook.presetPage("standard", data.form).cells),
                canon(DashboardBook.builtIn(data.form).pages[0].cells))
    }

    function test_aPresetNamesNoPluginsSignal() {
        for (const form of ["phone", "desktop"]) {
            for (const p of DashboardBook.presets) {
                for (const c of DashboardBook.presetPage(p.key, form).cells)
                    verify(c.refs.every(r => /^id:[0-9]+$/.test(r)), p.key + ": " + c.refs)
            }
        }
    }

    function test_theTemperaturesPresetHasOutsideAirAndCatalyst() {
        for (const form of ["phone", "desktop"]) {
            const labels = DashboardBook.presetPage("temperatures", form).cells.map(c => c.label)
            verify(labels.indexOf("Outside air") >= 0 && labels.indexOf("Catalyst") >= 0, form)
        }
    }

    function test_theShowcaseShowsEveryDrawingOnce() {
        for (const form of ["phone", "desktop"]) {
            const kinds = DashboardBook.presetPage("showcase", form).cells.map(c => c.kind)
            for (const k of ["shift-light", "analog", "ring", "bar", "segments", "sparkline",
                             "seven-segment", "digital"])
                verify(kinds.indexOf(k) >= 0, form + " has " + k)
        }
    }

    function test_aPresetThatIsntOneReadsNull() {
        compare(DashboardBook.presetText("nope", "phone"), "")
        compare(DashboardBook.presetText("standard", "tablet"), "")
        compare(DashboardBook.presetPage("../standard", "phone"), null)
    }

    function test_insertPagesPutsThemAfterThePlace() {
        const d = book()
        const page = { name: "New", cells: [cell("N")] }
        compare(DashboardBook.insertPages(d, 0, [page]).pages.map(p => p.name), ["Engine", "New", "Track"])
        compare(DashboardBook.insertPages(d, 1, [page]).pages.map(p => p.name), ["Engine", "Track", "New"])
        compare(DashboardBook.insertPages(d, -1, [page]).pages.map(p => p.name), ["New", "Engine", "Track"])
        compare(d.pages.length, 2, "the argument is untouched")
    }

    function test_insertPagesIsRefusedWholeOrNotAtAll() {
        const d = book()
        const page = { name: "New", cells: [] }
        compare(DashboardBook.insertPages(d, 2, [page]), d, "no such place")
        compare(DashboardBook.insertPages(d, 0, []), d, "no pages")
        compare(DashboardBook.insertPages(d, 0, [{ name: "", cells: [] }]), d, "a page that doesn't read back")
        let full = d
        while (full.pages.length < DashboardBook.maxPages)
            full = DashboardEdit.addPage(full, "P" + full.pages.length)
        compare(DashboardBook.insertPages(full, 0, [page]), full, "past the form's pages")
        compare(DashboardBook.insertPages(d, 0, [page, page]).pages.length, 4, "several at once")
    }

    function test_replacePageKeepsItsNameAndTakesTheCells() {
        const d = book()
        const out = DashboardBook.replacePage(d, 1, { name: "Other", cells: [cell("Z")] })
        compare(out.pages[1].name, "Track")
        compare(out.pages[1].cells.map(c => c.label), ["Z"])
        compare(out.pages[0], d.pages[0])
        compare(DashboardBook.replacePage(d, 5, { name: "x", cells: [] }), d)
        compare(DashboardBook.replacePage(d, 0, { name: "x" }), d)
    }

    function test_aPageMatchesAPresetByItsCellsAlone() {
        for (const form of ["phone", "desktop"]) {
            for (const p of DashboardBook.presets) {
                const page = DashboardBook.presetPage(p.key, form)
                verify(DashboardBook.matchesPreset(page, form), p.key)
                page.name = "Mine"
                verify(DashboardBook.matchesPreset(page, form), "a rename doesn't matter")
                page.cells[0].label = "Edited"
                verify(!DashboardBook.matchesPreset(page, form), "an edited gauge does")
            }
            verify(!DashboardBook.matchesPreset({ name: "x", cells: [] }, form))
            verify(!DashboardBook.matchesPreset(null, form))
        }
    }

    function test_aPresetOfTheOtherFormIsNotThisFormsPage() {
        verify(!DashboardBook.matchesPreset(DashboardBook.presetPage("standard", "phone"), "desktop"))
    }

    function test_anEmptyPageLosesNothingToAPreset() {
        for (const form of ["phone", "desktop"]) {
            verify(DashboardBook.losesNothing({ name: "x", cells: [] }, form))
            verify(DashboardBook.losesNothing(DashboardBook.presetPage("showcase", form), form))
            verify(!DashboardBook.losesNothing({ name: "x", cells: [cell("A")] }, form))
        }
        verify(!DashboardBook.losesNothing(null, "phone"))
    }

    // The presets come from the app's resources through the bridge (an
    // XMLHttpRequest on qrc: is refused without QML_XHR_ALLOW_FILE_READ):
    // only the six files, never a path
    function test_thePresetsComeFromTheResourcesAndOnlyThose() {
        verify(Bridge.presetLayoutText("standard", "phone").length > 0)
        for (const [name, form] of [["../standard", "phone"], ["standard", "../phone"], ["nope", "phone"],
                                    ["standard", "tablet"], ["../desktop/standard", "phone"], ["", ""], ["standard.json", "desktop"]])
            compare(Bridge.presetLayoutText(name, form), "", name + " " + form)
    }
}
