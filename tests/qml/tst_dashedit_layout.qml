// File/layer: tests/qml/tst_dashedit_layout.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashedit_layout (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

// The Dashboard's edit mode (ADR-0057 section 5), the layout file and the presets:
// each control's write and what's stored
DashEditKit {
    id: testCase
    name: "DashboardEditLayout"

    // Edit shows the bar and each cell's Remove; Done leaves edit mode,
    // and nothing was waiting to be saved
    // ---- The layout file (ADR-0057's appendix on the layout file)

    // A scratch file a layout is written to and read back from
    function layoutUrl(name) {
        return Harness.writeScratchFile(name, "")
    }

    function test_aLayoutRoundTripsAndLoadsAsOneUndoStep() {
        const first = docOf([cellOf("A", "analog"), cellOf("B")])
        const url = layoutUrl("layout-roundtrip.json")
        let page = storedPageOf({}, first)
        verify(page.layoutFilePart.exportLayoutTo(url), page.layoutFilePart.layoutNote)
        const raw = JSON.parse(Bridge.layoutFileText(url).text)
        compare(raw.form, "desktop")
        compare(raw.files, [])
        compare(page.layoutFilePart.layoutNote, "Saved this layout to a file. It's a desktop layout: it loads on a desktop, "
                + "not on a phone. A layout isn't scaled between forms.")
        page.destroy()

        const second = docOf([cellOf("X")])
        page = storedPageOf({}, second)
        page.startEditing()
        verify(page.layoutFilePart.loadLayoutFrom(url), page.layoutFilePart.layoutNote)
        compare(page.doc, first, "the file's layout, with no form or files key")
        compare(storedText(page), DashboardBook.serialize(first), "stored as an edit would store it")
        compare(page.historyPart.undoWords, "load a layout")
        page.historyPart.undo()
        compare(page.doc, second, "one Undo takes the whole load back")
        verify(!page.historyPart.canUndo, "and it was one step")
    }

    // Load layout through the phone's picker: a pick of
    // "name (1).json" after a plain one loads the second file, never the
    // first, and a picker with no document says so
    function test_thePickersSecondPickLoadsTheSecondFile() {
        const plainUrl = layoutUrl("layout-picked.json")
        const parenUrl = layoutUrl("layout-picked (1).json")
        let src = storedPageOf({}, docOf([cellOf("Plain")]))
        verify(src.layoutFilePart.exportLayoutTo(plainUrl), src.layoutFilePart.layoutNote)
        src.destroy()
        src = storedPageOf({}, docOf([cellOf("Paren")]))
        verify(src.layoutFilePart.exportLayoutTo(parenUrl), src.layoutFilePart.layoutNote)
        src.destroy()

        const page = storedPageOf({}, docOf([cellOf("X")]))
        const dialog = find(page, "layoutOpenDialog")
        dialog.usePicker = true
        DocumentPicker.setPickerForTesting(true)
        dialog.open()
        DocumentPicker.deliver(dialog.request, -1, plainUrl.toString(), "layout-picked.json")
        compare(labels(page), ["Plain"])
        dialog.open()
        DocumentPicker.deliver(dialog.request, -1, parenUrl.toString(), "layout-picked (1).json")
        compare(labels(page), ["Paren"], "the second pick's file")
        dialog.open()
        DocumentPicker.deliver(dialog.request, -1, "", "")
        compare(page.layoutFilePart.layoutNote, "The picker returned no document")
        verify(page.layoutFilePart.layoutNoteBad)
        compare(labels(page), ["Paren"], "nothing read")
    }

    // Export layout through the save picker: the pick's document holds the
    // layout, each save offers the page's own name (Downloads names its
    // document by an id, which a later save must never offer), and a picker
    // with no document says so
    function test_theSavePickerWritesTheLayoutAndOffersItsNameEachTime() {
        const page = storedPageOf({}, docOf([cellOf("Saved")]))
        const dialog = find(page, "layoutSaveDialog")
        dialog.usePicker = true
        DocumentPicker.setPickerForTesting(true)
        const url = layoutUrl("layout-save-picked.json")
        dialog.open()
        compare(DocumentPicker.titleForTesting(), page.layoutFilePart.layoutFileName())
        compare(DocumentPicker.mimeTypeForTesting(), "application/json")
        DocumentPicker.deliver(dialog.request, -1, url.toString(), "89", -1)
        verify(!page.layoutFilePart.layoutNoteBad, page.layoutFilePart.layoutNote)
        const reader = storedPageOf({}, docOf([cellOf("X")]))
        reader.layoutFilePart.loadLayoutFrom(url)
        compare(labels(reader), ["Saved"], "the picked document holds the layout")

        dialog.open()
        compare(DocumentPicker.titleForTesting(), page.layoutFilePart.layoutFileName(),
                "the page's name again, never the last answer's")
        DocumentPicker.deliver(dialog.request, -1, "", "")
        compare(page.layoutFilePart.layoutNote, "Not saved: The picker returned no document")
        verify(page.layoutFilePart.layoutNoteBad)
    }

    // A layout the page can't write as a file is refused before the bridge's
    // save: the pick's own document goes all the same (its verdict taken)
    function test_aLayoutThatCantBeWrittenDropsThePicksDocument() {
        const page = storedPageOf({}, docOf([cellOf("Saved")]))
        const bad = JSON.parse(JSON.stringify(page.doc))
        bad.format = 99
        page.doc = bad
        const uri = "content://com.android.externalstorage.documents/document/"
                    + "primary%3ADownload%2Fbad-layout.json"
        const dialog = find(page, "layoutSaveDialog")
        dialog.usePicker = true
        DocumentPicker.setPickerForTesting(true)
        dialog.open()
        DocumentPicker.deliver(dialog.request, -1, uri, "bad-layout.json",
                               DocumentPicker.launchedMsForTesting() + 50)
        compare(page.layoutFilePart.layoutNote, "Not saved: this layout can't be written as a file")
        compare(DocumentPicker.verdictOfForTesting(uri), 0, "the refusal took the verdict")
    }

    function test_twoQuickLoadsAreTwoUndoSteps() {
        const a = docOf([cellOf("A")])
        const b = docOf([cellOf("B")])
        const urlA = layoutUrl("layout-quick-a.json")
        const urlB = layoutUrl("layout-quick-b.json")
        let src = storedPageOf({}, a)
        verify(src.layoutFilePart.exportLayoutTo(urlA), src.layoutFilePart.layoutNote)
        src.destroy()
        src = storedPageOf({}, b)
        verify(src.layoutFilePart.exportLayoutTo(urlB), src.layoutFilePart.layoutNote)
        src.destroy()
        const start = docOf([cellOf("S")])
        const page = storedPageOf({}, start)
        page.startEditing()
        verify(page.layoutFilePart.loadLayoutFrom(urlA), page.layoutFilePart.layoutNote)
        verify(page.layoutFilePart.loadLayoutFrom(urlB), page.layoutFilePart.layoutNote)
        page.historyPart.undo()
        compare(page.doc, a, "the first Undo takes back the second load only")
        page.historyPart.undo()
        compare(page.doc, start, "the second takes back the first")
        verify(!page.historyPart.canUndo)
    }

    function test_aPhoneLayoutIsNotLoadedOnTheDesktopForm() {
        const url = layoutUrl("layout-phone.json")
        const phone = storedPageOf({ form: "phone" }, docOf([cellOf("P")]))
        verify(phone.layoutFilePart.exportLayoutTo(url), phone.layoutFilePart.layoutNote)
        compare(JSON.parse(Bridge.layoutFileText(url).text).form, "phone")
        phone.destroy()
        const desktop = docOf([cellOf("D")])
        const page = storedPageOf({}, desktop)
        page.startEditing()
        verify(!page.layoutFilePart.loadLayoutFrom(url))
        verify(page.layoutFilePart.layoutNoteBad)
        verify(page.layoutFilePart.layoutNote.indexOf("phone layout") >= 0 && page.layoutFilePart.layoutNote.indexOf("desktop form") >= 0,
               page.layoutFilePart.layoutNote)
        compare(page.doc, desktop, "nothing changed")
        verify(!page.historyPart.canUndo)
        compare(find(page, "dashStore").desktopUnread, "", "the unread text is never touched")
    }

    // The export's note names the form the file loads on, in the words the
    // other form's refusal gives the same file; on the narrowest phone it
    // wraps inside the page
    function test_theExportsNoteNamesTheFormItLoadsOn_data() {
        return [
            { tag: "desktop", form: "desktop", other: "phone", props: {} },
            { tag: "phone", form: "phone", other: "desktop", props: { width: 320, height: 832 } }
        ]
    }
    function test_theExportsNoteNamesTheFormItLoadsOn(data) {
        const url = layoutUrl("layout-form-" + data.tag + ".json")
        const props = Object.assign({ form: data.form }, data.props)
        const src = storedPageOf(props, docOf([cellOf("A")]))
        src.startEditing()
        click(src, "dashMore")
        verify(src.layoutFilePart.exportLayoutTo(url), src.layoutFilePart.layoutNote)
        verify(!src.layoutFilePart.layoutNoteBad)
        compare(src.layoutFilePart.layoutNote, "Saved this layout to a file. It's a " + data.form + " layout: it loads on a "
                + data.form + ", not on a " + data.other + ". A layout isn't scaled between forms.")
        const note = find(src, "dashLayoutNote")
        verify(note.visible, "the note shows in the More row")
        verify(waitForPolish(note), "the note laid out")
        verify(note.mapToItem(src, note.width, 0).x <= src.width + 0.5 && note.contentWidth <= note.width + 0.5,
               "inside the page: " + note.contentWidth + " of " + note.width + " in " + src.width)
        src.destroy()
        const page = storedPageOf({ form: data.other }, docOf([cellOf("B")]))
        page.startEditing()
        verify(!page.layoutFilePart.loadLayoutFrom(url))
        verify(page.layoutFilePart.layoutNote.indexOf("is a " + data.form + " layout") >= 0
               && page.layoutFilePart.layoutNote.indexOf("A layout isn't scaled between forms") >= 0, page.layoutFilePart.layoutNote)
    }

    function test_aDamagedOrNewerOrEmptyFileIsRefusedInWords_data() {
        return [
            { tag: "damaged", text: "{ nope", words: "isn't a dashboard layout" },
            { tag: "newer", text: JSON.stringify({ format: 9, pages: [] }), words: "newer version" },
            { tag: "empty", text: "", words: "the file is empty" },
            { tag: "no pages", text: JSON.stringify({ format: 2, form: "desktop", pages: [] }),
              words: "isn't a dashboard layout" }
        ]
    }
    function test_aDamagedOrNewerOrEmptyFileIsRefusedInWords(data) {
        const url = Harness.writeScratchFile("layout-bad-" + data.tag.replace(" ", "-") + ".json", data.text)
        const own = docOf([cellOf("A")])
        const page = storedPageOf({}, own)
        page.startEditing()
        verify(!page.layoutFilePart.loadLayoutFrom(url))
        verify(page.layoutFilePart.layoutNoteBad)
        verify(page.layoutFilePart.layoutNote.indexOf(data.words) >= 0, page.layoutFilePart.layoutNote)
        compare(page.doc, own)
        compare(storedText(page), DashboardBook.serialize(own))
        verify(!page.historyPart.canUndo)
    }

    function test_aMissingFileIsRefusedInWords() {
        const page = storedPageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        verify(!page.layoutFilePart.loadLayoutFrom(Qt.resolvedUrl("no-such-layout.json")))
        verify(page.layoutFilePart.layoutNote.indexOf("Couldn't read the file") >= 0, page.layoutFilePart.layoutNote)
    }

    function test_aFileWithDroppedGaugesSaysSoAndLoadsTheRest() {
        const text = JSON.stringify({ format: 2, form: "desktop", pages: [
            { name: "T", cells: [cellOf("Good"), { kind: "digital", w: 0, h: 4, refs: [] }] }] })
        const url = Harness.writeScratchFile("layout-dropped.json", text)
        const page = storedPageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        verify(page.layoutFilePart.loadLayoutFrom(url), page.layoutFilePart.layoutNote)
        compare(labels(page), ["Good"])
        verify(page.layoutFilePart.layoutNote.indexOf("1 gauge left out") >= 0, page.layoutFilePart.layoutNote)
        verify(!page.layoutFilePart.layoutNoteBad)
    }

    function test_aFileWithNoFormIsReadAsThisFormAndSaysSo() {
        const url = Harness.writeScratchFile("layout-noform.json",
                                             JSON.stringify({ format: 2, pages: [{ name: "T", cells: [cellOf("Good")] }] }))
        const page = storedPageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        verify(page.layoutFilePart.loadLayoutFrom(url), page.layoutFilePart.layoutNote)
        verify(page.layoutFilePart.layoutNote.indexOf("named no form") >= 0 && page.layoutFilePart.layoutNote.indexOf("desktop form") >= 0,
               page.layoutFilePart.layoutNote)
    }

    function test_theSameLayoutLoadedAgainChangesNothing() {
        const own = docOf([cellOf("A")])
        const url = layoutUrl("layout-same.json")
        const page = storedPageOf({}, own)
        page.startEditing()
        verify(page.layoutFilePart.exportLayoutTo(url))
        verify(!page.layoutFilePart.loadLayoutFrom(url))
        verify(page.layoutFilePart.layoutNote.indexOf("already shown") >= 0, page.layoutFilePart.layoutNote)
        verify(!page.historyPart.canUndo)
    }

    function test_noLoadPastAnOpenQuestion() {
        const url = layoutUrl("layout-question.json")
        const own = docOf([cellOf("A")])
        const page = storedPageOf({}, own)
        page.startEditing()
        verify(page.layoutFilePart.exportLayoutTo(url))
        page.doc = docOf([cellOf("Other")])
        page.confirmReset = true
        verify(!page.layoutFilePart.loadLayoutFrom(url))
        verify(page.layoutFilePart.layoutNoteBad)
        compare(labels(page), ["Other"], "the question stands and nothing loaded")
        page.confirmReset = false
        page.confirmRemovePage = true
        verify(!page.layoutFilePart.loadLayoutFrom(url))
        compare(labels(page), ["Other"])
    }

    // A signal file kept as a plugin of the user's own: its keepSignalText answer
    // A signal file kept as a plugin of its own, or (into, a plugin's
    // hash) as that plugin's variant, added or put in its place
    function keptPlugin(name, key, did, into, variant) {
        const file = { havne_signals: 1, name: name, source: { name: "t" },
                       ecus: [{ id: "pcm", rx: "7E8" }],
                       signals: [{ key: key, name: name.substring(0, 24),
                                   read: { service: "22", number: did },
                                   field: { byte: 0, bits: 8 }, unit: "kPa" }] }
        const r = Bridge.keepSignalText(JSON.stringify(file), into || "", variant || "", "")
        verify(r.ok, r.words)
        return r
    }

    // A layout file names the picks it needs, each by its name and its
    // written file's hash. Loaded for a vehicle that doesn't pick one, the
    // load names it; a file an earlier version saved names the signal file
    // the plugin was made from, and that is found under the pick too
    function test_thePluginsALayoutNeedsAreNamedAndTheLoadSaysWhichAreMissing() {
        const profile = keptPlugin("Boosty", "boosty", "D100").profiles[0]
        const vehicles = VehicleBook.setPicks(VehicleBook.add(VehicleBook.empty(), "Test car"), [profile])
        const picked = Bridge.pickFiles([profile])
        compare(picked.length, 1)
        compare(picked[0].name, "Boosty")

        const own = docOf([cellOf("A"), Object.assign(cellOf("Boosty"), { refs: ["key:boosty"] })])
        const url = layoutUrl("layout-needs.json")
        let page = storedPageOf({ vehicles: vehicles }, own)
        verify(page.layoutFilePart.exportLayoutTo(url), page.layoutFilePart.layoutNote)
        const raw = JSON.parse(Bridge.layoutFileText(url).text)
        compare(raw.files, [{ name: "Boosty", sha256: picked[0].sha256 }],
                "the pick the layout needs, by name and its written file's hash")
        compare(page.layoutFilePart.layoutNote, "Saved this layout to a file. It's a desktop layout: it loads on a desktop, "
                + "not on a phone. A layout isn't scaled between forms. It needs the plugin Boosty.")
        compare(page.layoutFilePart.layoutFileName(), "dashboard-test-car-desktop.json")
        page.destroy()

        // A layout of cells that read no pick names none
        page = storedPageOf({ vehicles: vehicles }, docOf([cellOf("A")]))
        const noneUrl = layoutUrl("layout-needs-none.json")
        verify(page.layoutFilePart.exportLayoutTo(noneUrl))
        compare(JSON.parse(Bridge.layoutFileText(noneUrl).text).files, [])
        page.destroy()

        // Loaded where the vehicle doesn't pick it: the load names it
        page = storedPageOf({}, docOf([cellOf("X")]))
        page.startEditing()
        verify(page.layoutFilePart.loadLayoutFrom(url), page.layoutFilePart.layoutNote)
        verify(page.layoutFilePart.layoutNote.indexOf("plugins this vehicle doesn't pick: Boosty") >= 0, page.layoutFilePart.layoutNote)
        compare(page.doc.pages[0].cells.length, 2, "the layout loads whole")
        page.destroy()

        // And where it picks it, nothing is said
        page = storedPageOf({ vehicles: vehicles }, docOf([cellOf("X")]))
        page.startEditing()
        verify(page.layoutFilePart.loadLayoutFrom(url), page.layoutFilePart.layoutNote)
        verify(page.layoutFilePart.layoutNote.indexOf("doesn't pick") < 0, page.layoutFilePart.layoutNote)
        page.destroy()

        // A file an earlier version saved names the signal file itself
        compare(picked[0].from.length, 1, "the plugin was made from one file")
        verify(picked[0].from[0] !== picked[0].sha256, "whose hash isn't the written file's")
        const text = DashboardBook.exportText(own, "desktop", [{ name: "Boosty", sha256: picked[0].from[0] }])
        verify(text !== "")
        const oldUrl = Harness.writeScratchFile("layout-earlier.json", text)
        page = storedPageOf({ vehicles: vehicles }, docOf([cellOf("X")]))
        page.startEditing()
        verify(page.layoutFilePart.loadLayoutFrom(oldUrl), page.layoutFilePart.layoutNote)
        verify(page.layoutFilePart.layoutNote.indexOf("doesn't pick") < 0, page.layoutFilePart.layoutNote)
        page.destroy()

        // The pick's variant replaced since the layout was saved: the pick
        // is the vehicle's still, and the load says its file is another
        const first = Bridge.plugins().find(p => p.profiles.indexOf(profile) >= 0)
        keptPlugin("Boosty", "boosty", "D101", first.sha256, profile.split(".").pop())
        verify(Bridge.pickFiles([profile])[0].sha256 !== picked[0].sha256, "another written file")
        page = storedPageOf({ vehicles: vehicles }, docOf([cellOf("X")]))
        page.startEditing()
        verify(page.layoutFilePart.loadLayoutFrom(url), page.layoutFilePart.layoutNote)
        compare(page.layoutFilePart.layoutNote, "Loaded the layout. it was saved with another version of Boosty: a gauge "
                + "reading a signal the picked version lacks reads not in profile")
    }

    // A plugin's name with its variant can pass a file name's bound: the
    // layout names the pick by a name cut to fit, its variant whole, and
    // a load finds the pick by its hash
    function test_aLongPluginNameWithAVariantStillExports() {
        // The longest name a signal file can carry: its profile's bound
        const name = "N".repeat(56)
        const variant = "v".repeat(24)
        const first = keptPlugin(name, "longy", "D100")
        const second = keptPlugin(name, "longer", "D101", first.sha256, variant)
        const profile = second.profiles.find(p => p.endsWith("." + variant))
        verify(profile !== undefined, JSON.stringify(second.profiles))
        const vehicles = VehicleBook.setPicks(VehicleBook.add(VehicleBook.empty(), "Test car"), [profile])
        const picked = Bridge.pickFiles([profile])
        compare(picked[0].name, name + ": " + variant, "past the bound as the store names it")

        const url = layoutUrl("layout-long-name.json")
        let page = storedPageOf({ vehicles: vehicles },
                                docOf([Object.assign(cellOf("Longer"), { refs: ["key:longer"] })]))
        verify(page.layoutFilePart.exportLayoutTo(url), page.layoutFilePart.layoutNote)
        compare(JSON.parse(Bridge.layoutFileText(url).text).files,
                [{ name: "N".repeat(51) + "...: " + variant, sha256: picked[0].sha256 }])
        page.destroy()

        page = storedPageOf({ vehicles: vehicles }, docOf([cellOf("X")]))
        page.startEditing()
        verify(page.layoutFilePart.loadLayoutFrom(url), page.layoutFilePart.layoutNote)
        compare(page.layoutFilePart.layoutNote, "Loaded the layout", "the pick is found by its hash")
    }

    // A pick no plugin here has gives no references, so a layout can't
    // say it needs it: the export's note names the pick instead
    function test_aPickTheStoreLacksIsNamedInTheExportsNote() {
        const vehicles = VehicleBook.setPicks(VehicleBook.add(VehicleBook.empty(), "Test car"),
                                              ["plugin.gone.base"])
        const own = docOf([cellOf("A"), Object.assign(cellOf("Gone"), { refs: ["key:gone"] })])
        const page = storedPageOf({ vehicles: vehicles }, own)
        verify(page.layoutFilePart.exportLayoutTo(layoutUrl("layout-unloaded.json")), page.layoutFilePart.layoutNote)
        verify(page.layoutFilePart.layoutNote.indexOf("plugin.gone.base") >= 0, page.layoutFilePart.layoutNote)
        verify(page.layoutFilePart.layoutNote.indexOf("aren't in the store") >= 0, page.layoutFilePart.layoutNote)
        verify(!page.layoutFilePart.layoutNoteBad, "a note, not a refusal")
        // A vehicle whose picks are all here says nothing of the kind
        const good = VehicleBook.setPicks(VehicleBook.add(VehicleBook.empty(), "Good car"),
                                          [keptPlugin("Loads", "loads", "D101").profiles[0]])
        const plain = storedPageOf({ vehicles: good },
                                   docOf([cellOf("A"), Object.assign(cellOf("Loads"), { refs: ["key:loads"] })]))
        verify(plain.layoutFilePart.exportLayoutTo(layoutUrl("layout-plain.json")), plain.layoutFilePart.layoutNote)
        verify(plain.layoutFilePart.layoutNote.indexOf("aren't in the store") < 0, plain.layoutFilePart.layoutNote)
    }

    // A gauge reading a plugin's signal the vehicle's picks don't give (no
    // vehicle, or the plugin only the session's profile) leaves `files`
    // empty: the export's note counts those gauges
    function test_aGaugeNoPickGivesAKeyForIsCountedInTheExportsNote() {
        const own = docOf([cellOf("A"), Object.assign(cellOf("Loose"), { refs: ["key:loose"] }),
                           Object.assign(cellOf("Loose 2"), { refs: ["key:loose2"] })])
        const none = storedPageOf({}, own)
        verify(none.layoutFilePart.exportLayoutTo(layoutUrl("layout-unbound.json")), none.layoutFilePart.layoutNote)
        verify(none.layoutFilePart.layoutNote.indexOf("2 gauges read signals none of the vehicle's picked plugins has") >= 0,
               none.layoutFilePart.layoutNote)
        verify(!none.layoutFilePart.layoutNoteBad, "a note, not a refusal")
        const one = storedPageOf({}, docOf([cellOf("A"), Object.assign(cellOf("Loose"), { refs: ["key:loose"] })]))
        verify(one.layoutFilePart.exportLayoutTo(layoutUrl("layout-unbound-one.json")), one.layoutFilePart.layoutNote)
        verify(one.layoutFilePart.layoutNote.indexOf("1 gauge reads a signal none of") >= 0, one.layoutFilePart.layoutNote)
        // A pick that gives the key says nothing of the kind
        const good = VehicleBook.setPicks(VehicleBook.add(VehicleBook.empty(), "Good car"),
                                          [keptPlugin("Loads", "loads", "D101").profiles[0]])
        const bound = storedPageOf({ vehicles: good },
                                   docOf([cellOf("A"), Object.assign(cellOf("Loads"), { refs: ["key:loads"] })]))
        verify(bound.layoutFilePart.exportLayoutTo(layoutUrl("layout-bound.json")), bound.layoutFilePart.layoutNote)
        verify(bound.layoutFilePart.layoutNote.indexOf("none of the vehicle's picked plugins") < 0, bound.layoutFilePart.layoutNote)
    }

    function test_theLayoutButtonsAreInTheMoreRow() {
        const page = storedPageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        click(page, "dashMore")
        verify(find(page, "moreExport").visible && find(page, "moreLoad").visible)
        page.layoutFilePart.noteLayout("Saved", false)
        verify(find(page, "dashLayoutNote").visible)
        page.stopEditing()
        compare(page.layoutFilePart.layoutNote, "", "the note goes with edit mode")
    }

    // ---- Presets: the "+" tab and Add page's list, Replace this page with...

    function openPresetList(page) {
        if (page.form === "phone") {
            click(page, "dashMore")
            click(page, "moreAddPage")
        } else {
            click(page, "dashPageAdd")
        }
        verify(find(page, "dashPresetList").visible, "the list opens")
    }

    function test_aPresetIsAddedAfterTheShownPageAsOneUndoStep_data() {
        return [{ tag: "desktop", props: {} },
                { tag: "phone", props: { form: "phone", width: 384, height: 832 } }]
    }
    function test_aPresetIsAddedAfterTheShownPageAsOneUndoStep(data) {
        const first = docOf([cellOf("A")])
        first.pages.push({ name: "Last", cells: [cellOf("B")] })
        const page = storedPageOf(data.props, first)
        page.startEditing()
        openPresetList(page)
        verify(find(page, "preset_blank").visible)
        verify(find(page, "preset_file").visible)
        click(page, "preset_temperatures")
        verify(!find(page, "dashPresetList").visible, "the list shuts")
        compare(page.doc.pages.map(p => p.name), ["Test", "Temperatures", "Last"], "after the shown page")
        compare(page.pageIndex, 1, "the new page shows")
        compare(DashboardBook.canonical(page.doc.pages[1].cells),
                DashboardBook.canonical(DashboardBook.presetPage("temperatures", page.form).cells))
        compare(storedText(page), DashboardBook.serialize(page.doc))
        compare(page.historyPart.undoWords, "added the page Temperatures")
        page.historyPart.undo()
        compare(page.doc.pages.map(p => p.name), ["Test", "Last"], "one Undo takes the page back")
        verify(!page.historyPart.canUndo, "and it was one step")
    }
    function test_aBlankPageStillComesFromTheList() {
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        openPresetList(page)
        click(page, "preset_blank")
        compare(page.doc.pages.length, 2)
        compare(page.doc.pages[1].cells.length, 0)
    }
    function test_theListCancelsAndGoesWithThePageChange() {
        const first = docOf([cellOf("A")])
        first.pages.push({ name: "Two", cells: [] })
        const page = pageOf({}, first)
        page.startEditing()
        openPresetList(page)
        click(page, "preset_cancel")
        verify(!find(page, "dashPresetList").visible)
        compare(page.doc.pages.length, 2, "nothing added")
        openPresetList(page)
        page.goTo(1)
        verify(!find(page, "dashPresetList").visible, "a page change shuts it")
    }
    function test_theListCantAddPastTheFormsPages() {
        let d = docOf([cellOf("A")])
        while (d.pages.length < DashboardBook.maxPages)
            d = DashboardEdit.addPage(d, "P" + d.pages.length)
        const page = pageOf({}, d)
        page.startEditing()
        verify(!find(page, "dashPageAdd").enabled, "the + tab is off")
    }
    function test_thePresetButtonsSayWhatTheyDo() {
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        openPresetList(page)
        compare(find(page, "preset_standard").Accessible.name, "Add the Standard preset as a new page")
        compare(find(page, "preset_blank").Accessible.name, "Add a blank page")
        click(page, "preset_cancel")
        click(page, "dashMore")
        click(page, "moreReplace")
        compare(find(page, "preset_showcase").Accessible.name, "Replace this page with the Showcase preset")
        verify(!find(page, "preset_blank").visible, "Replace has no blank")
        verify(!find(page, "preset_file").visible, "and no file")
    }

    function test_replacingAPageThatMatchesNoPresetAsksFirst() {
        const page = storedPageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        click(page, "dashMore")
        click(page, "moreReplace")
        click(page, "preset_standard")
        verify(find(page, "dashReplaceConfirm").visible, "asks")
        verify(find(page, "replaceQuestion").text.indexOf("Test") >= 0, "names the page")
        verify(find(page, "replaceQuestion").text.indexOf("Standard") >= 0, "and the preset")
        compare(labels(page), ["A"], "nothing yet")
        click(page, "replaceKeep")
        verify(!find(page, "dashReplaceConfirm").visible)
        compare(labels(page), ["A"], "kept")
        click(page, "moreReplace")
        click(page, "preset_standard")
        click(page, "replaceConfirm")
        compare(page.doc.pages.length, 1)
        compare(page.doc.pages[0].name, "Test", "the page keeps its name")
        compare(DashboardBook.canonical(page.doc.pages[0].cells),
                DashboardBook.canonical(DashboardBook.presetPage("standard", "desktop").cells))
        compare(storedText(page), DashboardBook.serialize(page.doc))
        compare(page.historyPart.undoWords, "replaced the page with Standard")
        page.historyPart.undo()
        compare(labels(page), ["A"], "one Undo takes it back")
        verify(!page.historyPart.canUndo)
    }
    function test_replacingAPresetPageAsksNothing() {
        const page = storedPageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        page.editsPart.replaceWithPreset("standard")
        click(page, "dashMore")
        click(page, "moreReplace")
        click(page, "preset_showcase")
        verify(!find(page, "dashReplaceConfirm").visible, "no question over a preset")
        compare(DashboardBook.canonical(page.doc.pages[0].cells),
                DashboardBook.canonical(DashboardBook.presetPage("showcase", "desktop").cells))
    }
    function test_theUndoLineShowsAReplaceOutsideEditMode() {
        const page = storedPageOf({ undoLineMs: 60000 }, docOf([cellOf("A")]))
        page.editsPart.replaceWithPreset("standard")
        compare(page.historyPart.undoLine, "Replaced the page with Standard")
        verify(find(page, "dashUndoLine").visible, "the line shows with edit mode off")
    }
    function test_replaceOnlyChangesTheShownPage() {
        const d = docOf([cellOf("A")])
        d.pages.push({ name: "Two", cells: [cellOf("B")] })
        const page = pageOf({}, d)
        page.goTo(1)
        page.editsPart.replaceWithPreset("temperatures")
        compare(page.doc.pages[0].cells.map(c => c.label), ["A"])
        compare(page.doc.pages[1].cells[0].label, "Coolant")
        compare(page.pageIndex, 1)
    }
    function test_replaceIsNotOfferedOverAnOpenQuestion() {
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        click(page, "dashMore")
        click(page, "moreReplace")
        click(page, "preset_standard")
        verify(!find(page, "dashMoreRow").visible, "the row waits for the answer")
        verify(!page.layoutFilePart.loadLayoutFrom(layoutUrl("never.json")), "a load waits too")
        compare(page.layoutFilePart.layoutNote, "Answer the question first")
    }

    function test_resetReadsAllPages() {
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        click(page, "dashMore")
        compare(find(page, "moreReset").text, "Reset all pages")
        click(page, "moreReset")
        verify(find(page, "dashResetConfirm").visible)
        compare(find(page, "resetConfirm").text, "Reset all")
    }

    // A file's pages are added after the shown page, one undo step
    function test_aFilesPagesAreAddedAfterTheShownPage() {
        const src = storedPageOf({}, docOf([cellOf("X")]))
        const url = layoutUrl("layout-add-pages.json")
        verify(src.layoutFilePart.exportLayoutTo(url), src.layoutFilePart.layoutNote)
        src.destroy()
        const page = storedPageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        verify(page.layoutFilePart.loadLayoutFrom(url, true), page.layoutFilePart.layoutNote)
        compare(page.doc.pages.length, 2)
        compare(page.doc.pages[0].cells.map(c => c.label), ["A"], "the page stays")
        compare(page.doc.pages[1].cells.map(c => c.label), ["X"])
        compare(page.pageIndex, 1)
        compare(page.historyPart.undoWords, "added pages from a file")
        page.historyPart.undo()
        compare(page.doc.pages.length, 1)
        verify(!page.historyPart.canUndo)
    }
    function test_aFileIsNotAddedPastTheFormsPages() {
        const src = storedPageOf({}, docOf([cellOf("X")]))
        const url = layoutUrl("layout-add-full.json")
        verify(src.layoutFilePart.exportLayoutTo(url), src.layoutFilePart.layoutNote)
        src.destroy()
        let d = docOf([cellOf("A")])
        while (d.pages.length < DashboardBook.maxPages)
            d = DashboardEdit.addPage(d, "P" + d.pages.length)
        const page = storedPageOf({}, d)
        page.startEditing()
        page.selected = 0
        verify(!page.layoutFilePart.loadLayoutFrom(url, true))
        verify(page.layoutFilePart.layoutNote.indexOf("Not added") === 0, page.layoutFilePart.layoutNote)
        compare(page.selected, 0, "a refused add keeps the user's place")
        compare(page.doc.pages.length, DashboardBook.maxPages)
    }

    // The list's buttons wrap inside the narrowest phone
    function test_thePresetListFitsTheNarrowestPhone_data() {
        // 280 stands in for a wider font on 320 (CI's Linux and macOS)
        return [{ tag: "320", width: 320 }, { tag: "280", width: 280 }, { tag: "240", width: 240 }]
    }
    function test_thePresetListFitsTheNarrowestPhone(data) {
        const page = pageOf({ form: "phone", width: data.width, height: 640 }, docOf([cellOf("A")]))
        page.startEditing()
        openPresetList(page)
        const flow = find(page, "dashPresetList")
        verify(waitForPolish(page.Window.window), "laid out")
        verify(flow.width <= flow.parent.width + 0.5, "the list is no wider than its bar: " + flow.width + " in " + flow.parent.width)
        for (const n of ["preset_blank", "preset_standard", "preset_temperatures", "preset_showcase",
                         "preset_file", "preset_cancel"]) {
            const b = find(page, n)
            const at = b.mapToItem(flow, 0, 0)
            verify(at.x >= 0 && at.x + b.width <= flow.width + 0.5, n + " is inside its flow: " + at.x + " + " + b.width + " of " + flow.width)
            verify(b.height >= Theme.touchTarget - 0.5, n + " is a touch's size")
        }
    }

    // ---- Presets: the review's cases

    function test_replacingAnEmptyPageAsksNothing() {
        const page = storedPageOf({}, docOf([]))
        page.startEditing()
        click(page, "dashMore")
        click(page, "moreReplace")
        click(page, "preset_standard")
        verify(!find(page, "dashReplaceConfirm").visible, "nothing to lose, nothing to ask")
        compare(page.doc.pages[0].cells.length, DashboardBook.presetPage("standard", "desktop").cells.length)
    }

    function test_aPresetThatCantBeReadIsSaidAndChangesNothing() {
        const page = storedPageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        openPresetList(page)
        verify(!page.editsPart.addPresetPage("nope"))
        verify(!find(page, "dashPresetList").visible, "the list shuts")
        compare(page.layoutFilePart.layoutNote, "The preset couldn't be read")
        verify(page.layoutFilePart.layoutNoteBad)
        verify(find(page, "dashLayoutNote").visible, "said in the page, with the More row shut")
        compare(page.doc.pages.length, 1)
        page.layoutFilePart.noteLayout("")
        verify(!page.editsPart.replaceWithPreset("nope"))
        compare(page.layoutFilePart.layoutNote, "The preset couldn't be read")
        page.layoutFilePart.noteLayout("")
        page.editsPart.replaceChosen("nope")
        compare(page.layoutFilePart.layoutNote, "The preset couldn't be read")
        verify(page.confirmReplaceKey === "", "no question over a preset that isn't there")
        compare(labels(page), ["A"])
    }

    function test_aFileThatDoesntFitSaysSoAlone() {
        const src = storedPageOf({}, docOf([cellOf("X")]))
        const url = layoutUrl("layout-add-note.json")
        verify(src.layoutFilePart.exportLayoutTo(url), src.layoutFilePart.layoutNote)
        src.destroy()
        let d = docOf([cellOf("A")])
        while (d.pages.length < DashboardBook.maxPages)
            d = DashboardEdit.addPage(d, "P" + d.pages.length)
        const page = storedPageOf({}, d)
        verify(!page.layoutFilePart.loadLayoutFrom(url, true))
        compare(page.layoutFilePart.layoutNote, "Not added: a form holds 10 pages at most")
        verify(page.layoutFilePart.layoutNoteBad)
    }

    function test_theFilePickIsForgottenWhenItIsCancelled() {
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        openPresetList(page)
        // Through the picker seam: a real FileDialog would stay open and
        // take the window's focus from every later case
        const dialog = find(page, "layoutOpenDialog")
        dialog.usePicker = true
        DocumentPicker.setPickerForTesting(true)
        click(page, "preset_file")
        verify(page.fileAdds, "the file's pages are what the pick adds")
        DocumentPicker.deliver(dialog.request, 0, "", "")
        verify(!page.fileAdds, "a cancel forgets it")
    }

    function test_theListClosesTheOtherQuestions() {
        const first = docOf([cellOf("A")])
        first.pages.push({ name: "Two", cells: [] })
        const page = pageOf({}, first)
        page.startEditing()
        click(page, "dashMore")
        click(page, "moreReset")
        verify(find(page, "dashResetConfirm").visible)
        page.editsPart.openPresets("add")
        verify(!find(page, "dashResetConfirm").visible, "Reset's question goes")
        verify(find(page, "dashPresetList").visible)
        page.confirmRemovePage = true
        page.editsPart.openPresets("replace")
        verify(!page.confirmRemovePage, "so does Remove's")
    }

    function test_thePresetButtonsAreOffAtThePageLimit() {
        let d = docOf([cellOf("A")])
        while (d.pages.length < DashboardBook.maxPages)
            d = DashboardEdit.addPage(d, "P" + d.pages.length)
        const page = pageOf({}, d)
        page.startEditing()
        page.editsPart.openPresets("add")
        for (const n of ["preset_blank", "preset_standard", "preset_temperatures", "preset_showcase", "preset_file"])
            verify(!find(page, n).enabled, n + " is off with " + DashboardBook.maxPages + " pages")
        page.editsPart.openPresets("replace")
        verify(find(page, "preset_standard").enabled, "a replace adds no page")
    }

    // ---- Presets: the phone pass's findings

    // A ring draws a square face: its box is at least one (the phone's row is
    // a quarter of a column), so its value and caption have the room a ring
    // of 2 by 6 lacked; the desktop's ring is the Standard page's own
    function test_aShowcaseRingIsAtLeastASquare() {
        for (const c of DashboardBook.presetPage("showcase", "phone").cells.filter(x => x.kind === "ring"))
            verify(Number(DashboardEdit.heightInColumns(c.h)) >= c.w, c.label + " is " + c.w + " by " + c.h)
        const std = DashboardBook.presetPage("standard", "desktop").cells.filter(x => x.kind === "ring")[0]
        for (const c of DashboardBook.presetPage("showcase", "desktop").cells.filter(x => x.kind === "ring"))
            verify(c.w === std.w && c.h === std.h, c.label + " is " + c.w + " by " + c.h)
    }

    // A Temperatures cell for a signal the car lacks says why in a whole
    // note, not "not in ...": at the phone's width and at 320, which stands
    // in for a wider font
    function test_theTemperaturesUnavailableNotesFitTheirCells_data() {
        return [{ tag: "384", width: 384 }, { tag: "320", width: 320 }]
    }
    function test_theTemperaturesUnavailableNotesFitTheirCells(data) {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const d = DashboardBook.builtIn("phone")
        d.pages = [DashboardBook.presetPage("temperatures", "phone")]
        const page = pageOf({ form: "phone", width: data.width, height: 832 }, d)
        const cells = d.pages[0].cells
        let seen = 0
        for (let i = 0; i < cells.length; ++i) {
            if (cells[i].label !== "Outside air" && cells[i].label !== "Catalyst")
                continue
            const cell = find(page, "dashCell_0_" + i)
            const note = find(cell, "gaugeNote")
            tryVerify(() => cell.width > 0 && note.visible, 5000, cells[i].label + " says why")
            verify(!note.truncated, cells[i].label + " is whole: " + note.text + " in " + note.width)
            const right = note.mapToItem(cell, note.width, 0).x
            verify(right <= cell.width + 0.5, "inside its cell: " + right + " of " + cell.width)
            ++seen
        }
        compare(seen, 2)
    }

    // The list takes the Page row's place on the phone, so the gauges keep
    // the height the row would have held
    function test_theListTakesThePageRowsPlaceOnAPhone() {
        const first = docOf([cellOf("A")])
        first.pages.push({ name: "Two", cells: [cellOf("B")] }, { name: "Three", cells: [] })
        const page = pageOf({ form: "phone", width: 384, height: 832 }, first)
        page.startEditing()
        click(page, "dashMore")
        verify(find(page, "dashMoreRow").visible)
        click(page, "moreAddPage")
        verify(find(page, "dashPresetList").visible)
        verify(!find(page, "dashMoreRow").visible, "the row gives way to the list")
        click(page, "preset_cancel")
        verify(find(page, "dashMoreRow").visible, "and comes back")
    }
}
