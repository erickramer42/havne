// File/layer: tests/qml/tst_settings.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.settings (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "SettingsStore"
    when: windowShown

    Component {
        id: pageComponent
        // Tall enough that the device list sits inside the page's
        // scroll area, which clips what's below it from a click
        SettingsPage { width: 1000; height: 1400 }
    }
    Component { id: dashComponent; DashboardPage { width: 1280; height: 800 } }
    Component { id: hubComponent; SignalHub {} }

    // The page's stored URI, Record switch and versions (test_qml keeps
    // them in a scratch ini), written straight to the store
    Settings {
        id: stored
        category: "session"
    }
    Settings {
        id: storedVehicles
        category: "vehicles"
    }
    Settings {
        id: uiStore
        category: "ui"
    }

    // A binding error in the app's QML fails the test. Each case starts
    // with nothing stored, or the last case's URI would fill the field
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        stored.setValue("uri", "")
        stored.setValue("recentUris", "[]")
        stored.setValue("recording", false)
        stored.setValue("adapterVersions", "{}")
        stored.sync()
        storedVehicles.setValue("book", "")
        storedVehicles.setValue("document", "")
        storedVehicles.sync()
        // After a turn: the last case's page writes its Settings as it goes
        wait(0)
        uiStore.setValue("driveHideMs", undefined)
        uiStore.setValue("driveOrientation", undefined)
        uiStore.sync()
    }

    // Every connect here is asynchronous, so a case that fails between
    // pressing Connect and the completion leaves one in flight: let it
    // land before the disconnect, or the next case meets a connected
    // bridge and fails for the wrong reason
    function cleanup() {
        DocumentPicker.setPickerForTesting(false)
        Theme.compact = false
        Theme.dense = false
        Theme.look = "dark"
        Theme.night = "off"
        Theme.nightStyle = "amber"
        Theme.nightFrom = "19:00"
        Theme.nightUntil = "07:00"
        Bridge.unitSystem = 0
        Bridge.recording = false
        tryVerify(() => !Bridge.connecting, 10000, "a connect in flight ends")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Harness.clearFakeKnobs()
        Harness.stopElm327Emulator()
        Bridge.vehicleLabel = ""
        Bridge.dropImport()                   // no file waits for the next case
        for (const p of Bridge.plugins())     // and the store starts empty
            Bridge.removePlugin(p.sha256)
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    // The page goes under the window's root, not the TestCase: TestCase is
    // an invisible item, and visible reads the effective visibility, so a
    // child of it would read invisible whatever the page did. It is
    // 1000 wide, so Connection shows beside the list; a case reading what
    // another page shows opens that page first
    function createPage(asItOpens) {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        verify(page, "Settings loads")
        if (!asItOpens)
            page.open("connection")
        return page
    }

    // A tap on an item that takes it with a MouseArea or a Button: the
    // handler runs, as a click would run it. The page is 1000 wide in a
    // 200 px window and a mouse event outside the window's bounds goes
    // nowhere, which the list beside the pages now puts every row past
    function tap(item) {
        verify(item.enabled, "the item takes taps")
        item.clicked(item instanceof MouseArea ? null : undefined)
    }

    // A button's click, through its own handler
    function press(page, name) {
        find(page, name).clicked()
    }

    // The visible items under item whose right edge lies past column's,
    // named by objectName, text or type. A Text's (it has elide) is its
    // painted text's when wider: an unwrapped label paints past its width
    function pastTheEdge(item, column, out) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (!child.visible || child.width <= 0)
                continue
            const painted = child.elide !== undefined && !child.clip
                            ? Math.max(child.width, child.contentWidth) : child.width
            const right = child.mapToItem(column, painted, 0).x
            if (right > column.width + 0.5)
                out.push((child.objectName || child.text || String(child))
                         + " at " + Math.round(right))
            pastTheEdge(child, column, out)
        }
        return out
    }

    // ---- The one store's list and Import (ADR-0064) ----

    // A signal file of one DID on the engine ECU; with obd2, one that
    // reads all of obd2 too
    function signalFileText(name, did, obd2) {
        const doc = {
            havne_signals: 1, name: name, source: { name: "t" },
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [{ key: "a", name: "A", read: { service: "22", number: did || "D100" },
                        field: { byte: 0, bits: 8 } }]
        }
        if (obd2)
            doc.extends = "obd2"
        return JSON.stringify(doc)
    }

    // A signal file read through Import and kept as a new plugin
    function importFile(page, fileName, name, did, obd2) {
        const url = Harness.writeScratchFile(fileName, signalFileText(name, did, obd2))
        verify(url.toString() !== "", "the scratch file is written")
        return page.importPart.importFile(url) && page.importPart.keepPending("", "")
    }

    // The example plugin's package read through Import, under version
    // when given
    function importExample(page, version) {
        const url = version === undefined
            ? Harness.writeExamplePlugin("example-sim.havne-plugin.json")
            : Harness.writeExamplePlugin("example-sim-" + version + ".havne-plugin.json", version)
        verify(url.toString() !== "", "the example plugin is packed")
        return page.importPart.importFile(url)
    }

    // The list's row for a profile: what the page holds, and its item
    function rowData(page, profile) {
        const r = page.picksPart.variantRows.find(e => e.profile === profile)
        verify(r !== undefined, profile + " is listed")
        return r
    }

    function rowOf(page, profile) {
        const i = page.picksPart.variantRows.findIndex(e => e.profile === profile)
        verify(i >= 0, profile + " is listed")
        return find(page, "signalRow_" + i)
    }

    // A press on a row: picked for the vehicle, or its pick undone
    function pressRow(page, profile) {
        const b = find(rowOf(page, profile), "rowPick")
        verify(b.enabled, "the row takes a press")
        b.clicked()
    }

    function signalsRow(page) {
        return page.pageList.find(r => r.key === "signals")
    }

    // A plugin is everyone's: it is imported and listed with no vehicle,
    // and says so until a vehicle picks one of its variants
    function test_aPluginNoVehiclePicksSaysItIsNotInUse() {
        const page = createPage()
        page.open("signals")
        verify(find(page, "signalsNone").visible)
        compare(signalsRow(page).summary, "none yet")
        verify(find(page, "signalImport").visible, "Import needs no vehicle")
        verify(!find(page, "settingsOwnSignalsPart").visible, "no own signals with no vehicle")
        verify(importFile(page, "extras.json", "Extras"))
        compare(find(page, "signalNote").text, "Extras 1.0.0 added")
        verify(!find(page, "signalsNone").visible)
        compare(signalsRow(page).summary, "1 plugin")
        let row = rowOf(page, "plugin.extras.base")
        verify(!find(row, "rowPick").enabled, "no vehicle to pick it for")
        verify(find(row, "rowUnused").visible)
        compare(find(row, "rowUnused").text, "Not in use: add or pick a vehicle, then pick it here.")
        verify(!find(row, "rowCaption").visible, "no car, no groups")

        press(page, "vehicleNew")
        compare(signalsRow(page).summary, "1 plugin, none picked")
        row = rowOf(page, "plugin.extras.base")
        verify(find(row, "rowUnused").visible, "still no vehicle's")
        compare(find(row, "rowUnused").text, "Not in use: press it to pick it for Vehicle 1.")
        verify(!find(row, "rowCaption").visible, "imported for no vehicle, and not picked")
        verify(find(page, "settingsOwnSignalsPart").visible, "own signals with a vehicle")
        pressRow(page, "plugin.extras.base")
        compare(page.picksPart.picks, ["plugin.extras.base"])
        row = rowOf(page, "plugin.extras.base")
        compare(find(row, "rowCaption").text, "For this car:", "a pick is this car's")
        verify(find(row, "rowPick").checked)
        verify(find(row, "rowPickMark").visible, "a mark, not the fill alone")
        verify(!find(row, "rowUnused").visible)
        compare(find(row, "rowPick").Accessible.name, "Extras 1.0.0, 1 signal · yours, picked")
        compare(signalsRow(page).summary, "1 plugin: Extras")
        verify(signalsRow(page).warn !== true)

        // Another vehicle's pick keeps it in use; a press again undoes a pick
        press(page, "vehicleNew")
        compare(page.picksPart.picks, [])
        verify(!find(rowOf(page, "plugin.extras.base"), "rowUnused").visible, "Vehicle 1 picks it")
        verify(!rowData(page, "plugin.extras.base").forCar, "Vehicle 1's pick, not this one's")
        compare(signalsRow(page).summary, "1 plugin, none picked")
        pressRow(page, "plugin.extras.base")
        verify(rowData(page, "plugin.extras.base").forCar)
        pressRow(page, "plugin.extras.base")
        compare(page.picksPart.picks, [], "pressed again, undone")
        verify(!rowData(page, "plugin.extras.base").forCar, "and no longer this car's")
    }

    // One Import, a signal file: it waits for where it goes, and As a new
    // plugin makes it a plugin of the user's own, imported for the picked
    // vehicle and picked for it at once. The book is stored under its own
    // key, the earlier version's left as it was
    function test_aSignalFileWaitsThenIsKeptAsAPluginAndPicked() {
        storedVehicles.setValue("document", "an earlier version's text")
        storedVehicles.sync()
        let page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(page.importPart.importFile(Harness.writeScratchFile("extras.json", signalFileText("Extras"))))
        verify(find(page, "importPanel").visible, "it waits for where it goes")
        compare(find(page, "importText").text, "Extras: a signal file")
        verify(findChild(page, "importInto_0") === null, "no plugin of the user's own to take it")
        compare(Harness.storedPlugins(), [], "nothing kept yet")
        press(page, "importAsNew")
        verify(!find(page, "importPanel").visible)
        compare(Harness.storedPlugins().length, 1)
        compare(page.picksPart.picks, ["plugin.extras.base"], "picked at once for the picked vehicle")
        compare(find(page, "signalNote").text, "Extras 1.0.0 added: picked for Vehicle 1")
        const row = rowOf(page, "plugin.extras.base")
        compare(find(row, "rowCaption").text, "For this car:")
        compare(find(row, "rowTitle").text, "Extras 1.0.0")
        compare(find(row, "rowReads").text, "1 signal · yours")
        verify(find(row, "rowPick").checked)

        // Its profile is a choice under Profile, and a session decodes
        // with it; nothing changes the store or the picks meanwhile
        find(page, "uriField").text = "mock://demo"
        const choice = find(page, "profile_plugin.extras.base")
        compare(choice.text, "Extras")
        choice.clicked()
        compare(find(page, "uriField").text, "mock://demo?profile=plugin.extras.base")
        compare(find(page, "profileHint").text, "The signals of Extras 1.0.0, picked for this vehicle")
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "a session decodes with it")
        compare(find(page, "errorLabel").text, "")
        page.open("signals")
        verify(find(page, "signalLockedNote").visible)
        verify(!find(page, "signalImport").enabled)
        verify(!find(rowOf(page, "plugin.extras.base"), "rowPick").enabled)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)

        page.destroy()
        wait(10)
        tryVerify(() => { storedVehicles.sync(); return String(storedVehicles.value("book", "")) !== "" },
                  2000, "the book is written")
        const book = JSON.parse(storedVehicles.value("book"))
        compare(book.format, 2)
        compare(book.list[0].picks, ["plugin.extras.base"])
        compare(storedVehicles.value("document"), "an earlier version's text",
                "the earlier version's key is never written")
        page = createPage()
        compare(page.picksPart.picks, ["plugin.extras.base"], "a restart keeps the pick")
        verify(find(rowOf(page, "plugin.extras.base"), "rowPick").checked)

        // The same file again is the same plugin; two signals are counted
        verify(importFile(page, "again.json", "Extras"))
        compare(find(page, "signalNote").text, "Extras 1.0.0 is here already")
        compare(Harness.storedPlugins().length, 1)
        const pair = JSON.parse(signalFileText("Pair"))
        pair.signals.push({ key: "b", name: "B", read: { service: "22", number: "D101" },
                            field: { byte: 0, bits: 8 } })
        verify(page.importPart.importFile(Harness.writeScratchFile("pair.json", JSON.stringify(pair))))
        press(page, "importAsNew")
        compare(find(rowOf(page, "plugin.pair.base"), "rowReads").text, "2 signals · yours")
        compare(signalsRow(page).summary, "2 plugins: Extras and 1 more")
    }

    // Through the phone's picker: the Signals page's one open hands its
    // pick to the import, and a picker with no document says so, with no
    // earlier note left beside the words and Replace's target dropped
    function test_theSignalsPagesPickReachesItsImport() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        DocumentPicker.setPickerForTesting(true)
        const dialog = find(page, "signalImportDialog")
        dialog.usePicker = true
        dialog.open()
        DocumentPicker.deliver(dialog.request, -1,
            Harness.writeScratchFile("picked (1).json", signalFileText("Picked")).toString(),
            "picked (1).json")
        verify(find(page, "importPanel").visible, "the pick reached the import: the file waits")
        compare(find(page, "importText").text, "Picked: a signal file")
        verify(page.importPart.keepPending("", ""))
        compare(find(page, "signalNote").text, "Picked 1.0.0 added: picked for Vehicle 1")

        dialog.open()
        DocumentPicker.deliver(dialog.request, -1, "", "")
        compare(find(page, "signalProblem").text, "The picker returned no document")
        compare(find(page, "signalNote").text, "", "no earlier note beside the words")

        page.picksPart.signalProblem = ""
        page.importPart.startReplace(rowData(page, "plugin.picked.base"))
        dialog.open()
        DocumentPicker.deliver(dialog.request, -1, "", "")
        compare(find(page, "signalProblem").text, "The picker returned no document")
        compare(page.importPart.replaceTarget, null, "and the next Import is an import")
    }

    // Through the phone's save picker: the Signals page's one save offers
    // the page's own name each time and hands its pick to the write; a
    // picker with no document says so, with no earlier note left beside
    // the words and no save left waiting, and a cancel leaves none either
    function test_theSignalsPagesSavePickReachesItsWrite() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(page.importPart.importFile(Harness.writeScratchFile("picked.json", signalFileText("Picked")))
               && page.importPart.keepPending("", ""))
        page.open("signals")
        DocumentPicker.setPickerForTesting(true)
        const row = rowData(page, "plugin.picked.base")
        const save = find(page, "signalSaveDialog")
        save.usePicker = true
        page.saveVariantAs(row)
        compare(DocumentPicker.titleForTesting(), "picked.json")
        const out = Harness.writeScratchFile("picked out (1).json", "")
        DocumentPicker.deliver(save.request, -1, out.toString(), "89", -1)
        compare(find(page, "signalProblem").text, "")
        compare(find(page, "signalNote").text, "Picked saved to the file you chose")
        verify(Harness.readScratchFile(out).includes("Picked"), "the picked document holds the file")

        page.saveVariantAs(row)
        compare(DocumentPicker.titleForTesting(), "picked.json",
                "the page's name again, never the last answer's")
        DocumentPicker.deliver(save.request, -1, "", "")
        compare(find(page, "signalProblem").text, "The picker returned no document")
        compare(find(page, "signalNote").text, "", "no earlier note beside the words")
        compare(page.saving, null, "and no save waits")

        page.exportPluginAs(row.sha256)
        compare(DocumentPicker.titleForTesting(), "picked.havne-plugin.json",
                "an export's own name")
        DocumentPicker.deliver(save.request, 0, "", "")
        compare(page.saving, null, "a cancel leaves no save waiting")
        compare(find(page, "signalProblem").text, "", "and says nothing")

        // A pick answered with no save waiting is written nowhere, and
        // its document's verdict is taken as any refused save's is
        const uri = "content://com.android.externalstorage.documents/document/"
                    + "primary%3ADownload%2Fpicked.json"
        page.saveVariantAs(row)
        page.saving = null
        DocumentPicker.deliver(save.request, -1, uri, "picked.json",
                               DocumentPicker.launchedMsForTesting() + 50)
        compare(DocumentPicker.verdictOfForTesting(uri), 0, "the refusal took the verdict")
    }

    // A file core refuses, or one that is none of the three kinds, keeps
    // nothing and says why, never naming the path
    function test_aFileCoreRefusesOrOfNoKindIsNotKept() {
        const page = createPage()
        press(page, "vehicleNew")
        const bad = Harness.writeScratchFile("bad.json", signalFileText("Bad").replace("bits", "bitz"))
        verify(!(page.importPart.importFile(bad) && page.importPart.keepPending("", "")))
        let words = find(page, "signalProblem").text
        verify(words.includes("bitz") && words.includes("line"), words)
        verify(!words.includes("bad.json"), "never the path")
        compare(page.picksPart.picks, [])
        compare(Harness.storedPlugins(), [])
        page.importPart.discardImport()

        verify(!page.importPart.importFile(Harness.writeScratchFile("other.json", "{\"ecu\": 1}")))
        compare(find(page, "signalProblem").text,
                "The file is neither a plugin, a signal file nor an OBDb signal set")
        verify(!find(page, "importPanel").visible, "nothing waits")

        verify(!page.importPart.importFile(Harness.writeScratchFile("broken.havne-plugin.json",
                                                         "{\"havne_plugin\": 2}")))
        words = find(page, "signalProblem").text
        verify(words !== "" && !words.includes("broken.havne"), words)
        compare(Harness.storedPlugins(), [])

        verify(importFile(page, "good.json", "Extras"))
        compare(find(page, "signalProblem").text, "", "a good import clears the last refusal")
    }

    // Into: the waiting file as a variant of a plugin of the user's own,
    // under the id typed, new or in a variant's place. Replace... on a row
    // puts the next signal file read in that row's place, and nothing else
    function test_aSignalFileGoesIntoAPluginOfTheUsersOwnOrInAVariantsPlace() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "extras.json", "Extras"))
        verify(page.importPart.importFile(Harness.writeScratchFile("track.json", signalFileText("Track day", "D101"))))
        const into = find(page, "importInto_0")
        compare(into.text, "Into Extras")
        verify(!find(page, "importVariantField").visible, "no variant asked before Into")
        into.clicked()
        verify(find(page, "importVariantField").visible)
        compare(page.importVariantField.text, "track-day", "the id the file's name gives")
        verify(!find(page, "importReplaces").visible)
        page.importVariantField.text = "base"
        compare(find(page, "importReplaces").text, "replaces base")
        page.importVariantField.text = "Bad Id"
        press(page, "importKeep")
        verify(find(page, "signalProblem").text !== "", "core refuses the id")
        verify(find(page, "importPanel").visible, "and the file still waits")
        compare(page.picksPart.pluginList[0].version, "1.0.0", "nothing changed")
        page.importVariantField.text = "track"
        press(page, "importKeep")
        compare(find(page, "signalProblem").text, "")
        verify(!find(page, "importPanel").visible)
        compare(find(page, "signalNote").text,
                "Extras 1.1.0 now has the variant track: picked for Vehicle 1")
        compare(page.picksPart.variantRows.map(r => page.picksPart.rowTitle(r)),
                ["Extras 1.1.0: base", "Extras 1.1.0: track"])
        compare(page.picksPart.picks, ["plugin.extras.base", "plugin.extras.track"])
        compare(Harness.storedPlugins().length, 1, "one copy: the plugin's newest")
        compare(signalsRow(page).summary, "1 plugin: Extras: base and 1 more")

        // The same id again takes that variant's place; its pick stays
        verify(page.importPart.importFile(Harness.writeScratchFile("other.json", signalFileText("Other", "D102"))))
        find(page, "importInto_0").clicked()
        page.importVariantField.text = "track"
        press(page, "importKeep")
        compare(find(page, "signalNote").text, "Extras 1.2.0: variant track replaced")
        compare(page.picksPart.picks.length, 2)

        // The field emptied: the id is the one the file's name gives, so
        // the variant of that id is the one said, replaced and picked
        pressRow(page, "plugin.extras.base")
        compare(page.picksPart.picks, ["plugin.extras.track"])
        verify(page.importPart.importFile(Harness.writeScratchFile("base.json", signalFileText("Base", "D104"))))
        find(page, "importInto_0").clicked()
        compare(page.importVariantField.text, "base")
        page.importVariantField.text = ""
        compare(find(page, "importReplaces").text, "replaces base")
        press(page, "importKeep")
        compare(find(page, "signalNote").text, "Extras 1.3.0: variant base replaced: picked for Vehicle 1")
        compare(page.picksPart.picks, ["plugin.extras.track", "plugin.extras.base"], "not the plugin's last variant")
        verify(Bridge.pluginVariantText(page.picksPart.pluginList[0].sha256, "base").text.includes("D104"))

        // Replace...: only a signal file takes a variant's place
        page.importPart.startReplace(rowData(page, "plugin.extras.base"))
        compare(find(page, "signalImportDialog").title, "Replace Extras: base with a signal file")
        verify(!page.importPart.importFile(Harness.writeExamplePlugin("example-sim.havne-plugin.json")))
        compare(find(page, "signalProblem").text, "Not a signal file: only one takes a variant's place")
        compare(page.picksPart.pluginList.length, 1, "the package was not imported")
        compare(page.importPart.replaceTarget, null, "and the next Import is an import")
        // It picks nothing: a variant the vehicle doesn't pick stays so
        pressRow(page, "plugin.extras.base")
        compare(page.profileChoice, "default")
        page.importPart.startReplace(rowData(page, "plugin.extras.base"))
        verify(page.importPart.importFile(Harness.writeScratchFile("third.json", signalFileText("Third", "D103"))))
        verify(!find(page, "importPanel").visible, "kept at once")
        compare(find(page, "signalNote").text, "Extras 1.4.0: variant base replaced")
        verify(Bridge.pluginVariantText(page.picksPart.pluginList[0].sha256, "base").text.includes("D103"),
               "the row reads the new file")
        compare(page.picksPart.picks, ["plugin.extras.track"], "Replace picks nothing")
        compare(page.profileChoice, "default", "and the field reads what it read")
        pressRow(page, "plugin.extras.base")

        // One variant of several is removed alone, its pick with it
        page.openRow = "plugin.extras.track"
        const remove = find(rowOf(page, "plugin.extras.track"), "rowRemoveVariant")
        verify(remove.visible)
        remove.clicked()
        find(rowOf(page, "plugin.extras.track"), "signalsConfirmRemove").clicked()
        compare(page.picksPart.picks, ["plugin.extras.base"])
        compare(page.picksPart.variantRows.map(r => page.picksPart.rowTitle(r)), ["Extras 1.5.0"])
        page.openRow = "plugin.extras.base"
        const row = rowOf(page, "plugin.extras.base")
        verify(find(row, "rowReplace").visible)
        verify(!find(row, "rowRemoveVariant").visible, "never a plugin's last variant")
        verify(!find(row, "rowCopy").visible, "the user's own needs no copy")
    }

    // One Import, a package: it goes in as it is, each variant a row the
    // picked vehicle picks; the car's VIN (typed in the lookup here: the
    // mock has none) puts the variants it covers first. A pick under
    // Default is read at once and connects; a newer version takes the
    // earlier one's place, and a removal takes the picks
    function test_aPackageGoesInAsItIsAndItsVariantsArePickedHere() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importExample(page))
        verify(!find(page, "importPanel").visible, "a package asks nothing")
        compare(find(page, "signalNote").text,
                "Example: the simulated car 1.0.0 added: pick a variant below")
        compare(page.picksPart.variantRows.map(r => page.picksPart.rowTitle(r)),
                ["Example: the simulated car 1.0.0: base", "Example: the simulated car 1.0.0: my2019"])
        // Its Boost takes the place of obd2's: the row says so, then what is added
        compare(page.picksPart.variantRows.map(r => page.picksPart.rowReads(r)),
                ["obd2, Boost replaced, 4 more", "obd2, Boost replaced, 5 more"])
        compare(page.picksPart.picks, [], "several variants: the pick is the user's")
        verify(!find(rowOf(page, "plugin.example-sim.base"), "rowCaption").visible, "no VIN, no groups")
        compare(find(rowOf(page, "plugin.example-sim.my2019"), "rowUnused").text,
                "Not in use: press a variant to pick it for Vehicle 1.")
        compare(Harness.storedPlugins().length, 1)

        find(page, "vinLookupField").text = "ZZZFAKE11G0000001"   // 2016: before my2019
        compare(page.picksPart.variantRows.map(r => r.variant), ["base", "my2019"])
        compare(page.picksPart.variantRows.map(r => r.caption), ["For this car:", ""], "a match lifts the plugin whole")
        verify(!find(rowOf(page, "plugin.example-sim.my2019"), "rowCaption").visible)
        find(page, "vinLookupField").text = "ZZZFAKE11M0000001"   // 2021
        compare(page.picksPart.variantRows.map(r => r.caption), ["For this car:", ""])
        find(page, "vinLookupField").text = ""
        verify(!rowData(page, "plugin.example-sim.base").forCar, "no VIN, no match")

        const field = find(page, "uriField")
        field.text = "mock://demo"
        pressRow(page, "plugin.example-sim.my2019")
        compare(page.picksPart.picks, ["plugin.example-sim.my2019"])
        compare(field.text, "mock://demo?profile=plugin.example-sim.my2019", "read at once under Default")
        compare(page.picksPart.variantRows.map(r => r.variant), ["base", "my2019"])
        compare(page.picksPart.variantRows.map(r => r.caption), ["For this car:", ""], "a pick lifts its plugin")
        verify(find(rowOf(page, "plugin.example-sim.my2019"), "rowPickMark").visible, "with its mark")
        verify(!rowData(page, "plugin.example-sim.base").picked, "the other variant beside it")
        const choice = find(page, "profile_plugin.example-sim.my2019")
        compare(choice.text, "Example: the simulated car: my2019")
        verify(choice.checked)
        compare(find(page, "profileHint").text,
                "obd2's signals and those of Example: the simulated car 1.0.0: my2019, "
                + "picked for this vehicle")
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "a session decodes with the variant")
        compare(find(page, "errorLabel").text, "")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)

        verify(importExample(page, "1.1.0"))
        compare(find(page, "signalNote").text, "Example: the simulated car 1.1.0 replaces 1.0.0")
        compare(Harness.storedPlugins().length, 1)
        compare(page.picksPart.picks, ["plugin.example-sim.my2019"], "the pick is the new version's")
        verify(importExample(page, "1.1.0"))
        compare(find(page, "signalNote").text, "Example: the simulated car 1.1.0 is here already")

        // Someone else's plugin can be imported again: its removal asks nothing
        page.openRow = "plugin.example-sim.base"
        const row = rowOf(page, "plugin.example-sim.base")
        compare(find(row, "rowRemove").text, "Remove the plugin")
        find(row, "rowRemove").clicked()
        compare(page.confirming, "")
        verify(find(page, "signalsNone").visible)
        compare(Harness.storedPlugins(), [])
        compare(page.picksPart.picks, [], "a removed plugin's variant is no pick")
        compare(field.text, "mock://demo", "the field reads Default again")
        tryVerify(() => findChild(page, "profile_plugin.example-sim.my2019") === null, 1000,
                  "a variant no plugin registers isn't offered")
    }

    // A plugin is this car's when it was imported for the vehicle, a VIN
    // matches one of its variants or the vehicle picks one. It then lists
    // whole under "For this car:", its other variants beside that one, so
    // a first pick lifts the plugin's rows once and a second moves nothing
    function test_aPluginWithAVariantOfThisCarsListsWhole() {
        const page = createPage()
        page.open("signals")
        verify(importExample(page))   // with no vehicle: nobody's import
        press(page, "vehicleNew")
        verify(importFile(page, "extras.json", "Extras"))
        const extras = "plugin.extras.base"
        const base = "plugin.example-sim.base"
        const my2019 = "plugin.example-sim.my2019"
        pressRow(page, extras)   // the import's pick, undone: still this vehicle's import
        compare(page.picksPart.picks, [])
        compare(page.picksPart.variantRows.map(r => r.profile), [extras, base, my2019])
        compare(page.picksPart.variantRows.map(r => r.caption), ["For this car:", "Other plugins:", ""])
        compare(find(rowOf(page, base), "rowCaption").text, "Other plugins:")

        pressRow(page, my2019)
        compare(page.picksPart.picks, [my2019])
        compare(page.picksPart.variantRows.map(r => r.caption), ["For this car:", "", ""], "the pick lifts its plugin")
        verify(rowData(page, base).forCar, "the variant beside the picked one")
        verify(!rowData(page, base).picked)
        verify(find(rowOf(page, my2019), "rowPickMark").visible)
        const order = page.picksPart.variantRows.map(r => r.profile)
        compare(order.indexOf(my2019), order.indexOf(base) + 1, "the plugin's variants in their order")
        pressRow(page, my2019)
        pressRow(page, base)
        compare(page.picksPart.variantRows.map(r => r.profile), order, "another variant picked moves nothing")
        pressRow(page, base)
        compare(page.picksPart.picks, [])
        compare(page.picksPart.variantRows.map(r => r.caption), ["For this car:", "Other plugins:", ""],
                "the picks undone, the plugin is back with the others")

        find(page, "vinLookupField").text = "ZZZFAKE11G0000001"   // 2016: base's, before my2019
        compare(page.picksPart.variantRows.map(r => r.caption), ["For this car:", "", ""], "a VIN's match lifts it too")
        verify(rowData(page, my2019).forCar, "the variant the VIN doesn't cover, beside the one it does")
    }

    // A press keeps the focus on its variant's row: the rows stay as they
    // are through a pick and its undoing, and a first pick, which lifts
    // the plugin past another, takes the focus with the row
    function test_aPickKeepsTheFocusOnItsRow() {
        const page = createPage()
        page.open("signals")
        verify(importExample(page))
        verify(importFile(page, "zed.json", "Zed"))   // with no vehicle: nobody's, after the example
        press(page, "vehicleNew")
        const my2019 = "plugin.example-sim.my2019"
        const zed = "plugin.zed.base"
        compare(page.picksPart.variantRows.map(r => r.profile), ["plugin.example-sim.base", my2019, zed])

        const button = find(rowOf(page, my2019), "rowPick")
        button.forceActiveFocus()
        verify(button.activeFocus)
        button.clicked()
        compare(page.picksPart.picks, [my2019])
        verify(find(rowOf(page, my2019), "rowPick") === button, "the same button after the pick")
        verify(button.checked)
        verify(button.activeFocus, "the pick keeps the focus")
        button.clicked()
        compare(page.picksPart.picks, [], "pressed again, undone")
        verify(find(rowOf(page, my2019), "rowPick") === button && button.activeFocus, "and once undone")

        // Zed's pick lifts it past the example: the focus goes with the row
        const last = find(rowOf(page, zed), "rowPick")
        last.forceActiveFocus()
        last.clicked()
        compare(page.picksPart.variantRows.map(r => r.profile), [zed, "plugin.example-sim.base", my2019])
        const moved = find(rowOf(page, zed), "rowPick")
        verify(moved !== last, "the row is in another place")
        verify(moved.activeFocus, "the focus is on the variant pressed")
        verify(moved.checked)
        moved.clicked()
        compare(page.picksPart.variantRows.map(r => r.profile), ["plugin.example-sim.base", my2019, zed])
        verify(find(rowOf(page, zed), "rowPick").activeFocus, "and back with it")
    }

    // A session's matches are kept on the vehicle its VIN identified, by
    // their profiles alone and never another car's, and stay once the VIN
    // goes with the session: through a page made again, a VIN typed in the
    // lookup, which is asked about in their place, and another vehicle's
    // turn, which has none of them. A replaced plugin takes its own, and a
    // session whose VIN is no vehicle's marks only while it runs
    function test_aSessionsVinMatchesStayWithItsVehicle() {
        let page = createPage()
        press(page, "vehicleNew")
        const car = page.pickedVehicle.id
        page.open("signals")
        verify(importExample(page))
        const base = "plugin.example-sim.base"
        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connected")
        Bridge.identifyForTesting("ZZZFAKE11G0000001")   // a 2016: base alone
        tryVerify(() => rowData(page, base).forCar, 1000, "marked while it runs")
        compare(page.pickedVehicle.vinMatches, [], "the VIN is no vehicle's: nothing kept")

        // The VIN names the picked vehicle (an adapter's recognize does
        // it; the mock's URI is no adapter's, so the key is set here)
        const salt = page.vehicles.salt !== "" ? page.vehicles.salt : Bridge.newSalt()
        const doc = VehicleBook.copy(page.vehicles)
        doc.salt = salt
        page.saveVehicles(VehicleBook.change(doc, v => v.vinKey = Bridge.vinKey(salt)))
        Bridge.identifyForTesting("ZZZFAKE11G0000001")
        compare(page.pickedVehicle.vinMatches, [base])
        verify(!VehicleBook.serialize(page.vehicles).includes("ZZZFAKE"), "never the VIN")

        // Another car's VIN with this vehicle picked (a replay of it, which
        // recognizes no vehicle) keeps nothing
        Bridge.identifyForTesting("ZZZFAKE11M0000001")   // 2021: both variants
        compare(page.pickedVehicle.vinMatches, [base], "not another car's")

        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
        compare(Bridge.pluginMatches(""), [], "the VIN went with the session")
        verify(rowData(page, base).forCar, "the vehicle's matches stay")

        // The page made again (the app's next start) reads them from the book
        page.destroy()
        wait(10)
        page = createPage()
        page.open("signals")
        compare(page.pickedVehicle.id, car)
        verify(rowData(page, base).forCar)

        // A VIN typed in the lookup is the one asked about, in their place
        find(page, "vinLookupField").text = "1FAFAKE13Y0000001"   // another manufacturer
        verify(!rowData(page, base).forCar, "the typed VIN's: none")
        find(page, "vinLookupField").text = ""
        verify(rowData(page, base).forCar)

        // Another vehicle has none of them; its own turn back has them
        press(page, "vehicleNew")
        verify(!rowData(page, base).forCar, "not this one's")
        page.pickVehicle(car)
        verify(rowData(page, base).forCar)

        // A replacement takes the replaced plugin's matches (its new
        // version's match may not cover the car) and keeps the pick
        pressRow(page, base)
        verify(importExample(page, "1.1.0"))
        compare(page.pickedVehicle.vinMatches, [])
        compare(page.picksPart.picks, [base], "the pick kept")
        pressRow(page, base)
        verify(!rowData(page, base).forCar, "no match left, and no pick")

        // A session whose VIN is no vehicle's: marked while it runs alone
        press(page, "vehicleNew")
        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connected again")
        Bridge.identifyForTesting("ZZZFAKE11M0000001")
        tryVerify(() => rowData(page, base).forCar, 1000, "marked")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
        tryVerify(() => !rowData(page, base).forCar, 1000, "gone with the VIN")
        compare(page.pickedVehicle.vinMatches, [], "nothing kept on this one")
    }

    // A session's VIN puts the variants it covers first (ADR-0052 section
    // 5); nothing is picked while it runs, and once it ends the matches
    // stay on the vehicle its VIN identified, though the VIN goes
    function test_aSessionsVinMarksItsVariantsPickedOnceItEnds() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importExample(page))
        verify(importFile(page, "extras.json", "Extras"))
        pressRow(page, "plugin.extras.base")   // the import's pick, undone
        // The car's VIN names the picked vehicle
        const doc = VehicleBook.copy(page.vehicles)
        doc.salt = Bridge.newSalt()
        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connected")
        Bridge.identifyForTesting("ZZZFAKE11M0000001")   // a 2021 the example covers
        page.saveVehicles(VehicleBook.change(doc, v => v.vinKey = Bridge.vinKey(doc.salt)))
        Bridge.identifyForTesting("ZZZFAKE11M0000001")
        tryVerify(() => rowData(page, "plugin.example-sim.my2019").forCar, 1000, "marked")
        compare(page.picksPart.variantRows.map(r => r.caption), ["For this car:", "", ""],
                "the plugin imported for this vehicle and the VIN's two")
        verify(!find(rowOf(page, "plugin.example-sim.base"), "rowPick").enabled)
        verify(!find(page, "signalImport").enabled)
        verify(!find(page, "formSave").enabled)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
        compare(page.pickedVehicle.vinMatches,
                ["plugin.example-sim.base", "plugin.example-sim.my2019"], "kept on the vehicle")
        verify(rowData(page, "plugin.example-sim.base").forCar, "the session's matches stay")
        pressRow(page, "plugin.example-sim.base")
        compare(page.picksPart.picks, ["plugin.example-sim.base"])
        compare(find(page, "uriField").text, "mock://demo?profile=plugin.example-sim.base")
        pressRow(page, "plugin.example-sim.base")
        compare(page.picksPart.picks, [], "pressed again, undone")
        compare(find(page, "uriField").text, "mock://demo")

        // Another vehicle has no matches of this car's
        press(page, "vehicleNew")
        verify(!rowData(page, "plugin.example-sim.base").forCar)
    }
}
