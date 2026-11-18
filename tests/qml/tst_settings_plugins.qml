// File/layer: tests/qml/tst_settings.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.settings (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "SettingsPlugins"
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

    // On a phone the removal's question is drawn where Remove was pressed:
    // the last plugin of a list longer than the page asks inside the
    // page's view, never at the top of a page whose controls then all read
    // disabled
    function test_aRemovalAsksInsideAPhonesView() {
        const page = createPage()
        page.width = 384
        page.height = 832
        press(page, "vehicleNew")
        page.open("signals")
        for (let i = 1; i <= 24; ++i)
            verify(importFile(page, "p" + i + ".json", "Plugin " + i, "D1" + String(i).padStart(2, "0")))
        const last = page.picksPart.variantRows[page.picksPart.variantRows.length - 1].profile
        page.rowActionsPart.openRow = last
        const flick = find(page, "settingsSignalsPage")
        const row = rowOf(page, last)
        const remove = find(row, "rowRemove")
        verify(waitForPolish(page.Window.window), "laid out")
        // Scrolled as a thumb would: Remove at the view's foot
        const bottomOf = item => item.mapToItem(flick.contentItem, 0, item.height).y
        flick.contentY = Math.max(0, bottomOf(remove) - flick.height)
        // The page's top, Import and all, is scrolled out of the view
        const importBottom = bottomOf(find(page, "signalImport"))
        verify(importBottom < flick.contentY + flick.headerHeight,
               "Import ends at " + importBottom + ", the view starts at " + (flick.contentY + flick.headerHeight))
        remove.clicked()
        const question = find(row, "signalsConfirm")
        tryVerify(() => question.visible && question.height > 0, 2000, "the row asks")
        verify(waitForPolish(page.Window.window), "laid out")
        for (const name of ["signalsConfirmText", "signalsConfirmRemove", "signalsConfirmCancel"]) {
            const item = find(question, name)
            const top = item.mapToItem(flick.contentItem, 0, 0).y
            verify(top >= flick.contentY + flick.headerHeight - 0.5
                   && bottomOf(item) <= flick.contentY + flick.height + 0.5,
                   name + " from " + top + " to " + bottomOf(item) + " in a view from "
                   + (flick.contentY + flick.headerHeight) + " to " + (flick.contentY + flick.height))
        }
        find(question, "signalsConfirmCancel").clicked()
        compare(page.confirming, "")
    }

    // Remove this variant asks in its row first, as the whole plugin's
    // Remove does: the list is as it was until the question is answered
    function test_removeThisVariantAsksFirst() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "trip.json", "Trip extras"))
        verify(page.importPart.importFile(Harness.writeScratchFile("two.json", signalFileText("Two", "D101"))))
        verify(page.importPart.keepPending(page.picksPart.pluginList[0].sha256, "two"))
        const two = "plugin.trip-extras.two"
        const listed = () => page.picksPart.variantRows.map(r => r.profile).sort()
        const before = listed()
        compare(before.length, 2)
        page.rowActionsPart.openRow = two
        const row = rowOf(page, two)
        find(row, "rowRemoveVariant").clicked()
        compare(listed(), before, "asked first")
        const question = find(row, "signalsConfirm")
        verify(question.visible, "in the variant's row")
        compare(find(question, "signalsConfirmText").text,
                "Remove the variant two of Trip extras? It is yours, and no other copy exists")
        verify(!find(row, "rowActions").visible, "the question in the actions' place")
        verify(!find(page, "signalImport").enabled, "nothing else past the question")
        find(question, "signalsConfirmCancel").clicked()
        compare(listed(), before, "Cancel keeps it")
        find(row, "rowRemoveVariant").clicked()
        find(question, "signalsConfirmRemove").clicked()
        compare(listed(), ["plugin.trip-extras.base"])
        compare(page.confirming, "")
    }

    // A signal file waiting on Into keeps no choice of a plugin that goes
    // or is rewritten (a variant dropped gives it a new sha256): Keep
    // hides until Into is chosen again, and the file still waits
    function test_aWaitingIntoChoiceGoesWithItsPlugin() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "trip.json", "Trip extras"))
        verify(page.importPart.importFile(Harness.writeScratchFile("two.json", signalFileText("Two", "D101"))))
        verify(page.importPart.keepPending(page.picksPart.pluginList[0].sha256, "two"))
        verify(page.importPart.importFile(Harness.writeScratchFile("three.json", signalFileText("Three", "D102"))))
        press(page, "importInto_0")
        verify(find(page, "importKeep").visible, "Into chosen: Keep shows")

        verify(page.rowActionsPart.removeVariant(rowData(page, "plugin.trip-extras.two")))
        compare(page.importPart.importInto, "", "the rewritten plugin's old sha256 is no choice")
        verify(!find(page, "importKeep").visible, "Keep waits for Into again")
        press(page, "importInto_0")
        verify(find(page, "importKeep").visible)

        verify(page.rowActionsPart.removePlugin(page.picksPart.pluginList[0].sha256))
        compare(page.importPart.importInto, "")
        verify(!find(page, "importKeep").visible)
        verify(page.importPart.pendingImport !== null, "the file still waits")
    }

    // Mine marks an imported plugin as the user's: its variants can then
    // be changed here, its removal asks, and a newer export of the same
    // plugin updates it in place, saying from which version to which
    function test_mineMarksAPluginAsTheUsersAndItsNewerExportUpdatesIt() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "trip.json", "Trip extras"))
        verify(page.importPart.importFile(Harness.writeScratchFile("two.json", signalFileText("Two", "D101"))))
        verify(page.importPart.keepPending(page.picksPart.pluginList[0].sha256, "two"))
        const first = Harness.writeScratchFile("trip-1.havne-plugin.json", "")
        page.rowActionsPart.saving = page.rowActionsPart.pluginSave(page.picksPart.pluginList[0].sha256)
        compare(page.rowActionsPart.saving.file, "trip-extras.havne-plugin.json", "the name the dialog starts from")
        verify(page.rowActionsPart.saveTo(first))
        compare(find(page, "signalNote").text, "Trip extras exported to the file you chose")
        verify(page.importPart.importFile(Harness.writeScratchFile("three.json", signalFileText("Three", "D102"))))
        verify(page.importPart.keepPending(page.picksPart.pluginList[0].sha256, "three"))
        const second = Harness.writeScratchFile("trip-2.havne-plugin.json", "")
        page.rowActionsPart.saving = page.rowActionsPart.pluginSave(page.picksPart.pluginList[0].sha256)
        verify(page.rowActionsPart.saveTo(second))

        // The user's own has no copy anywhere else: its removal asks, in
        // its row, in the actions' place
        page.rowActionsPart.openRow = "plugin.trip-extras.base"
        const asking = rowOf(page, "plugin.trip-extras.base")
        find(asking, "rowRemove").clicked()
        verify(find(asking, "signalsConfirm").visible)
        verify(!find(asking, "rowActions").visible, "the question in the actions' place")
        compare(find(asking, "signalsConfirmText").text,
                "Remove Trip extras? It is yours, and no other copy exists")
        verify(!find(page, "signalImport").enabled, "nothing else past the question")
        verify(!find(asking, "rowMore").enabled, "the asking row stays open")
        find(asking, "signalsConfirmCancel").clicked()
        compare(page.picksPart.pluginList.length, 1, "Cancel keeps it")
        verify(find(asking, "rowActions").visible, "the actions back")
        find(asking, "rowRemove").clicked()
        find(asking, "signalsConfirmRemove").clicked()
        compare(page.picksPart.pluginList.length, 0)
        compare(page.picksPart.picks, [])

        // As it comes to another device: someone else's until Mine
        verify(page.importPart.importFile(first))
        compare(find(page, "signalNote").text, "Trip extras 1.1.0 added: pick a variant below")
        page.rowActionsPart.openRow = "plugin.trip-extras.base"
        let row = rowOf(page, "plugin.trip-extras.base")
        compare(find(row, "rowReads").text, "1 signal")
        verify(find(row, "rowMine").visible)
        verify(find(row, "rowCopy").visible)
        verify(!find(row, "rowReplace").visible, "someone else's variants aren't changed here")
        verify(find(row, "rowMineLine").visible)
        compare(find(row, "rowMineLine").text,
                "Mine marks it as yours, so its variants can be changed here; a newer export "
                + "of the same plugin then updates it in place.")
        find(row, "rowCopy").clicked()
        verify(find(row, "rowCopyPanel").visible)
        find(row, "rowMine").clicked()
        compare(find(page, "signalNote").text, "Trip extras is yours now")
        row = rowOf(page, "plugin.trip-extras.base")
        compare(find(row, "rowReads").text, "1 signal · yours")
        verify(find(row, "rowReplace").visible)
        verify(!find(row, "rowMine").visible)
        verify(!find(row, "rowMineLine").visible)
        verify(!find(row, "rowCopy").visible)
        verify(!find(row, "rowCopyPanel").visible, "Copy's choices go with Copy")
        verify(!page.rowActionsPart.copyShown)
        pressRow(page, "plugin.trip-extras.two")

        verify(page.importPart.importFile(second))
        compare(find(page, "signalNote").text, "Trip extras updated from 1.1.0 to 1.2.0")
        compare(page.picksPart.variantRows.map(r => r.variant), ["base", "two", "three"])
        verify(page.picksPart.variantRows.every(r => r.mine), "still the user's")
        compare(page.picksPart.picks, ["plugin.trip-extras.two"], "the pick stays")
        compare(Harness.storedPlugins().length, 1)
    }

    // More opens a row's actions and leaves the row as wide as it was, so
    // a title beside it keeps its lines, on a phone too
    function test_anOpenRowIsAsWideAsAClosedOne_data() {
        return [{ tag: "desktop", width: 1000, compact: false }, { tag: "phone", width: 384, compact: true }]
    }

    function test_anOpenRowIsAsWideAsAClosedOne(data) {
        Theme.compact = data.compact
        const page = createPage(data.compact)
        page.width = data.width
        press(page, "vehicleNew")
        verify(importExample(page))
        page.open("signals")
        const profile = "plugin.example-sim.my2019"
        const pick = find(rowOf(page, profile), "rowPick")
        waitForPolish(find(page, "settingsSignalsPageColumn"))
        const closed = { width: pick.width, height: pick.height }
        verify(closed.width > 100, "laid out")
        find(rowOf(page, profile), "rowMore").clicked()
        compare(page.rowActionsPart.openRow, profile)
        waitForPolish(find(page, "settingsSignalsPageColumn"))
        const open = find(rowOf(page, profile), "rowPick")
        compare(open.width, closed.width)
        compare(open.height, closed.height, "the title wraps no further")
        verify(find(rowOf(page, profile), "rowMore").checked)
    }

    // Copy to my plugin puts a variant of someone else's plugin into a
    // plugin of the user's own, new or one of theirs; Save as a signal
    // file writes a variant as the store has it, session or not
    function test_aVariantIsCopiedIntoAPluginOfTheUsersOwnAndSavedAsASignalFile() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importExample(page))
        const theirs = page.picksPart.pluginList[0].sha256
        page.rowActionsPart.openRow = "plugin.example-sim.my2019"
        let row = rowOf(page, "plugin.example-sim.my2019")
        verify(!find(row, "rowCopyPanel").visible)
        find(row, "rowCopy").clicked()
        verify(find(row, "rowCopyPanel").visible)
        verify(findChild(row, "copyInto_0") === null, "no plugin of the user's own yet")
        find(row, "copyAsNew").clicked()
        compare(find(page, "signalProblem").text, "")
        compare(page.picksPart.ownPlugins.length, 1)
        const copy = page.picksPart.ownPlugins[0]
        compare(find(page, "signalNote").text,
                "Example: the simulated car: my2019 copied into " + copy.name + " " + copy.version
                + " (The copy won't follow Example: the simulated car's updates to my2019)")
        verify(!page.rowActionsPart.copyShown, "the choice closes")
        compare(page.picksPart.pluginList.length, 2, "the original stays")
        compare(page.picksPart.picks, [], "a copy is no pick")
        verify(rowData(page, copy.profiles[0]).mine)

        page.rowActionsPart.openRow = "plugin.example-sim.base"
        row = rowOf(page, "plugin.example-sim.base")
        find(row, "rowCopy").clicked()
        const intoOwn = find(row, "copyInto_0")
        compare(intoOwn.text, "Into " + copy.name)
        intoOwn.clicked()
        compare(find(page, "signalProblem").text, "")
        compare(page.picksPart.ownPlugins.length, 1)
        compare(page.picksPart.ownPlugins[0].variants.length, 2, "a variant of its own beside the first")

        const what = page.rowActionsPart.variantSave(rowData(page, "plugin.example-sim.my2019"))
        compare(what.file, "example-sim-my2019.json", "the variant, where the plugin has several")
        const url = Harness.writeScratchFile("saved.json", "x".repeat(2000))
        page.rowActionsPart.saving = what
        verify(page.rowActionsPart.saveTo(url))
        compare(page.rowActionsPart.saving, null)
        compare(Harness.readScratchFile(url), Bridge.pluginVariantText(theirs, "my2019").text)
        compare(find(page, "signalNote").text,
                "Example: the simulated car: my2019 saved to the file you chose")
        verify(page.importPart.importFile(url), "it reads back as a signal file")
        compare(page.importPart.pendingImport.kind, "signals")
        press(page, "importDiscard")
        compare(page.importPart.pendingImport, null)
        verify(!find(page, "importPanel").visible)

        verify(importFile(page, "extras.json", "Extras"))
        compare(page.rowActionsPart.variantSave(rowData(page, "plugin.extras.base")).file, "extras.json")
        page.rowActionsPart.saving = { kind: "variant", sha256: "a".repeat(64), variant: "base", name: "Gone",
                        file: "gone.json" }
        verify(!page.rowActionsPart.saveTo(url))
        compare(find(page, "signalProblem").text, "The store has no such plugin")
        compare(find(page, "signalNote").text, "")

        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connected")
        page.open("signals")
        page.rowActionsPart.openRow = "plugin.example-sim.base"
        row = rowOf(page, "plugin.example-sim.base")
        verify(find(row, "rowSave").enabled, "Save reads the copy alone")
        verify(find(row, "rowExport").enabled)
        verify(!find(row, "rowCopy").enabled)
        verify(!find(row, "rowRemove").enabled)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
    }

    // Export plugin... writes the store's copy, on every plugin; one with
    // a share-alike variant says what that asks before the dialog. The
    // file imports again as the same plugin
    function test_exportSaysWhatAShareAlikeVariantAsksAndWritesTheStoresCopy() {
        const page = createPage()
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        page.open("signals")
        verify(obdbRead(page, "2021"))
        press(page, "obdbKeep")
        const profile = "plugin.daily-driver-obdb.my2021"
        const sha = rowData(page, profile).sha256
        page.rowActionsPart.openRow = profile
        const row = rowOf(page, profile)
        verify(!find(row, "rowExportNote").visible)
        verify(page.rowActionsPart.exportWaits(sha), "the note comes before the dialog")
        verify(find(row, "rowExportNote").visible)
        compare(find(row, "exportNoteText").text,
                "Variant 'my2021' holds data under CC-BY-SA-4.0, a share-alike license: whoever "
                + "passes the plugin on keeps that license and the variant's source with it")
        find(row, "exportCancel").clicked()
        verify(!find(row, "rowExportNote").visible)

        const url = Harness.writeScratchFile("out.havne-plugin.json", "")
        page.rowActionsPart.saving = page.rowActionsPart.pluginSave(sha)
        compare(page.rowActionsPart.saving.file, "daily-driver-obdb.havne-plugin.json")
        verify(page.rowActionsPart.saveTo(url))
        compare(find(page, "signalNote").text, "Daily driver OBDb exported to the file you chose")
        verify(page.importPart.importFile(url))
        compare(find(page, "signalNote").text, "Daily driver OBDb 1.0.0 is here already")
        compare(page.picksPart.pluginList.length, 1)

        // Someone else's plugin under no such license goes straight to
        // the dialog, and its export is the package byte for byte
        const packed = Harness.writeExamplePlugin("example-sim.havne-plugin.json")
        verify(page.importPart.importFile(packed))
        const example = page.picksPart.pluginList.find(p => p.id === "example-sim").sha256
        verify(!page.rowActionsPart.exportWaits(example))
        compare(page.rowActionsPart.exportNote, "")
        const copy = Harness.writeScratchFile("example-out.havne-plugin.json", "")
        page.rowActionsPart.saving = page.rowActionsPart.pluginSave(example)
        verify(page.rowActionsPart.saveTo(copy))
        compare(Harness.readScratchFile(copy), Harness.readScratchFile(packed))
        verify(page.rowActionsPart.exportWaits("0123"), "a plugin the store lacks opens no dialog")
        compare(find(page, "signalProblem").text, "The store has no such plugin")
    }

    // The profile row with one and two picks that read all of obd2: on an
    // adapter, where Default is obd2, the first such pick takes Default's
    // place and says "obd2 + <name>"; a pick made while the field reads
    // Default is read at once, one made under a chosen profile leaves the
    // field alone; on mock:// Default isn't obd2, so nothing stands in
    function test_theFirstPickThatReadsObd2StandsInForDefaultOnAnAdapter() {
        const page = createPage()
        press(page, "vehicleNew")
        const field = find(page, "uriField")
        field.text = "elm327://COM9"
        compare(page.profileChoice, "default")
        verify(importFile(page, "plus.json", "Plus", "D100", true))
        compare(find(page, "signalNote").text,
                "Plus 1.0.0 added: picked for Vehicle 1, now reading obd2 + Plus")
        compare(field.text, "elm327://COM9?profile=plugin.plus.base")
        compare(page.picksPart.rowReads(rowData(page, "plugin.plus.base")), "obd2 + 1 signal · yours")
        const plus = find(page, "profile_plugin.plus.base")
        compare(plus.text, "obd2 + Plus")
        verify(plus.checked)
        compare(page.profiles.filter(p => p.name === "plugin.plus.base").length, 1,
                "one button for the pick: the stand-in")
        tryVerify(() => findChild(page, "profile_default") === null, 1000,
                  "the pick reads all of obd2: Default goes")
        compare(find(page, "profileHint").text,
                "obd2's signals and those of Plus 1.0.0, picked for this vehicle")

        // Typed back to Default, both show, so the row is the URI's
        field.text = "elm327://COM9"
        verify(find(page, "profile_default").checked)
        verify(!find(page, "profile_plugin.plus.base").checked)

        // A second such pick is read too, but the first still stands in
        verify(importFile(page, "more.json", "More", "D101", true))
        compare(page.picksPart.picks, ["plugin.plus.base", "plugin.more.base"])
        compare(field.text, "elm327://COM9?profile=plugin.more.base")
        field.text = "elm327://COM9"
        compare(page.standIn.profile, "plugin.plus.base", "the first pick, not the last")
        compare(page.profiles.map(p => p.name),
                ["default", "plugin.plus.base", "raw", "uds-generic", "plugin.more.base"])
        compare(find(page, "profile_plugin.more.base").text, "obd2 + More")

        // The first unpicked, the next stands in
        pressRow(page, "plugin.plus.base")
        compare(page.standIn.profile, "plugin.more.base")
        pressRow(page, "plugin.more.base")
        compare(page.standIn, null)
        compare(page.profiles.map(p => p.name), ["default", "raw", "uds-generic"])

        // A profile the user chose stays when a pick is made
        press(page, "vehicleNew")
        field.text = "elm327://COM9?profile=raw"
        verify(importFile(page, "third.json", "Third", "D102", true))
        compare(field.text, "elm327://COM9?profile=raw")
        compare(page.profileChoice, "raw")
        compare(find(page, "signalNote").text, "Third 1.0.0 added: picked for Vehicle 2")
        compare(find(page, "profile_plugin.third.base").text, "obd2 + Third")
        tryVerify(() => findChild(page, "profile_default") === null, 1000)

        // On mock://, Default isn't obd2: the pick is a choice of its own
        field.text = "mock://demo"
        compare(page.standIn, null)
        compare(find(page, "profile_plugin.third.base").text, "Third")
        verify(find(page, "profile_default"))
    }

    // A pick no plugin here has (a version that dropped the variant, a
    // plugin removed elsewhere) is listed so it can be seen and undone,
    // and the list's Signals row says so, in the danger color
    function test_aPickTheStoreLacksIsListedAndUnpicked() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "extras.json", "Extras"))
        page.saveVehicles(VehicleBook.setPicks(page.vehicles,
            ["plugin.gone.base", "plugin.extras.base", "plugin.extras.dropped"]))
        compare(page.picksPart.missingPicks.map(m => m.text),
                ["plugin.gone.base: not in the store", "dropped is no longer in Extras"])
        compare(signalsRow(page).summary, "1 plugin: Extras, 2 picks missing")
        verify(signalsRow(page).warn, "flagged for the list's danger color")
        const first = find(page, "pickMissing_0")
        verify(first.visible)
        compare(find(first, "pickMissingText").text, "plugin.gone.base: not in the store")
        find(first, "pickMissingUnpick").clicked()
        compare(page.picksPart.picks, ["plugin.extras.base", "plugin.extras.dropped"])
        compare(signalsRow(page).summary, "1 plugin: Extras, 1 pick missing")
        find(find(page, "pickMissing_0"), "pickMissingUnpick").clicked()
        compare(page.picksPart.picks, ["plugin.extras.base"])
        verify(signalsRow(page).warn !== true)
        tryVerify(() => findChild(page, "pickMissing_0") === null, 1000)

        // A missing first pick stands in for nothing: the next one does
        verify(importFile(page, "plus.json", "Plus", "D101", true))
        page.saveVehicles(VehicleBook.setPicks(page.vehicles, ["plugin.gone.base", "plugin.plus.base"]))
        find(page, "uriField").text = "elm327://COM9"
        compare(page.standIn.profile, "plugin.plus.base")
    }

    // The signal files the move into the plugin store left behind are
    // listed with why, the picked vehicle's then the book's own; Save
    // writes one out and Remove drops its record, asking first in the
    // file's row where the kept copy goes with it
    function test_theFilesThatDidntMoveAreListedAndRemoved() {
        const a = "a".repeat(64)
        const b = "b".repeat(64)
        const book = VehicleBook.change(VehicleBook.add(VehicleBook.empty(), "Daily driver"), v => {
            v.unmigrated = [{ sha256: a, name: "Old extras", words: "line 3: no such key 'bitz'" }]
        })
        book.unmigrated = [{ sha256: b, name: "Loose", words: "Its vehicle was not read", kept: false }]
        storedVehicles.setValue("book", VehicleBook.serialize(book))
        storedVehicles.sync()
        const page = createPage()
        page.open("signals")
        verify(find(page, "unmigratedTitle").visible)
        compare(find(page, "unmigratedTitle").text, "Not moved from the earlier version:")
        compare(find(find(page, "unmigrated_0"), "unmigratedText").text,
                "Old extras: line 3: no such key 'bitz'")
        compare(find(find(page, "unmigrated_1"), "unmigratedText").text,
                "Loose: Its vehicle was not read (its copy is still in the earlier version's folder)")
        compare(page.rowActionsPart.unmigratedSave(page.rowActionsPart.unmigratedRows[0]).file, "signals.json")

        // Neither copy is anywhere in this test's folders: Save says so
        page.rowActionsPart.saving = page.rowActionsPart.unmigratedSave(page.rowActionsPart.unmigratedRows[0])
        verify(!page.rowActionsPart.saveTo(Harness.writeScratchFile("left.json", "")))
        compare(find(page, "signalProblem").text, "Its copy is gone from the earlier version's folder")

        const kept = find(page, "unmigrated_0")
        find(kept, "unmigratedRemove").clicked()
        compare(page.rowActionsPart.unmigratedRows.map(r => r.name), ["Old extras", "Loose"], "asked first")
        const question = find(kept, "unmigratedConfirm")
        verify(question.visible, "in the file's row")
        compare(find(question, "signalsConfirmText").text, "Remove Old extras? Its kept copy goes with it")
        verify(!find(kept, "unmigratedSave").visible, "the question in the buttons' place")
        verify(!find(page, "signalImport").enabled, "nothing else past the question")
        find(question, "signalsConfirmCancel").clicked()
        compare(page.rowActionsPart.unmigratedRows.length, 2, "Cancel keeps it")
        find(kept, "unmigratedRemove").clicked()
        find(question, "signalsConfirmRemove").clicked()
        compare(page.rowActionsPart.unmigratedRows.map(r => r.name), ["Loose"])
        verify(page.pickedVehicle.unmigrated === undefined, "the vehicle's record is gone")
        // Its copy is still in the earlier version's folder: nothing to ask
        find(find(page, "unmigrated_0"), "unmigratedRemove").clicked()
        compare(page.rowActionsPart.unmigratedRows, [])
        verify(page.vehicles.unmigrated === undefined)
        tryVerify(() => !find(page, "unmigratedTitle").visible, 1000)

        // Forget all keeps the book's own records: they are no vehicle's
        page.saveVehicles(Object.assign(VehicleBook.copy(page.vehicles),
                                        { unmigrated: [{ sha256: b, name: "Loose", words: "w" }] }))
        page.ownPart.forgetVehicles(true)
        compare(page.rowActionsPart.unmigratedRows.map(r => r.name), ["Loose"])
    }

    // The bytes the move kept of a file go only once no vehicle lists the
    // file; until then Save as a signal file writes them out
    function test_aFileThatDidntMoveKeepsItsBytesWhileAVehicleListsIt() {
        const c = "c".repeat(64)
        const text = signalFileText("Kept", "D105")
        verify(Harness.keepUnmigrated(c, text))
        try {
            const listed = v => {
                v.unmigrated = [{ sha256: c, name: "Kept", words: "line 2: no such key" }]
            }
            let book = VehicleBook.change(VehicleBook.add(VehicleBook.empty(), "Daily driver"), listed)
            book = VehicleBook.change(VehicleBook.add(book, "Weekend car"), listed)
            storedVehicles.setValue("book", VehicleBook.serialize(book))
            storedVehicles.sync()
            const page = createPage()
            page.open("signals")
            compare(page.pickedVehicle.label, "Weekend car")
            const out = Harness.writeScratchFile("kept.json", "")
            page.rowActionsPart.saving = page.rowActionsPart.unmigratedSave(page.rowActionsPart.unmigratedRows[0])
            verify(page.rowActionsPart.saveTo(out))
            compare(Harness.readScratchFile(out), text, "the bytes as the move kept them")
            compare(find(page, "signalNote").text, "Kept saved to the file you chose")

            find(find(page, "unmigrated_0"), "unmigratedRemove").clicked()
            compare(page.rowActionsPart.unmigratedRows, [], "another vehicle lists it: no question")
            compare(Harness.keptUnmigrated(), [c], "the other vehicle lists it still")
            press(page, "vehicle_" + page.vehicles.list[0].id)
            compare(page.rowActionsPart.unmigratedRows.map(r => r.name), ["Kept"])
            find(find(page, "unmigrated_0"), "unmigratedRemove").clicked()
            compare(Harness.keptUnmigrated(), [c], "the last listing asks first")
            find(find(page, "unmigrated_0"), "signalsConfirmRemove").clicked()
            compare(page.rowActionsPart.unmigratedRows, [])
            compare(Harness.keptUnmigrated(), [], "no vehicle lists it: the bytes go")
        } finally {
            Bridge.removeUnmigratedFile(c)
        }
    }

    // A forgotten vehicle's file that didn't move is the book's own from
    // then on: listed, saved and removed as before, its bytes kept until
    // it is removed
    function test_aForgottenVehiclesFileThatDidntMoveStaysListed() {
        const d = "d".repeat(64)
        const text = signalFileText("Kept", "D106")
        verify(Harness.keepUnmigrated(d, text))
        try {
            const book = VehicleBook.change(VehicleBook.add(VehicleBook.empty(), "Daily driver"), v => {
                v.unmigrated = [{ sha256: d, name: "Kept", words: "line 2: no such key" }]
            })
            storedVehicles.setValue("book", VehicleBook.serialize(book))
            storedVehicles.sync()
            const page = createPage()
            page.open("signals")
            compare(page.rowActionsPart.unmigratedRows.map(r => r.loose), [false], "the vehicle's")
            press(page, "vehicleForget")
            press(page, "vehicleConfirm")
            compare(page.pickedVehicle, null)
            compare(page.rowActionsPart.unmigratedRows.map(r => r.name), ["Kept"], "listed still")
            compare(page.rowActionsPart.unmigratedRows.map(r => r.loose), [true], "the book's own now")
            compare(Harness.keptUnmigrated(), [d])
            const out = Harness.writeScratchFile("kept.json", "")
            page.rowActionsPart.saving = page.rowActionsPart.unmigratedSave(page.rowActionsPart.unmigratedRows[0])
            verify(page.rowActionsPart.saveTo(out))
            compare(Harness.readScratchFile(out), text, "and saved as before")
            find(find(page, "unmigrated_0"), "unmigratedRemove").clicked()
            find(find(page, "unmigrated_0"), "signalsConfirmRemove").clicked()
            compare(page.rowActionsPart.unmigratedRows, [])
            compare(Harness.keptUnmigrated(), [], "removed, its bytes go")
        } finally {
            Bridge.removeUnmigratedFile(d)
        }
    }

    // A book a later Havne saved is left as it is: no vehicle is shown,
    // the page says why, and nothing is written over it
    function test_aBookFromALaterHavneIsLeftAsItIs() {
        const later = JSON.stringify({ format: 3, salt: "s", picked: "v1",
                                       list: [{ id: "v1", label: "Daily driver", shape: "new" }] })
        storedVehicles.setValue("book", later)
        storedVehicles.sync()
        const page = createPage()
        verify(page.bookLater)
        compare(page.pickedVehicle, null)
        page.open("vehicle")
        verify(find(page, "vehicleBookLater").visible)
        compare(find(page, "vehicleBookLater").text,
                "This vehicle list was saved by a later Havne and is left as it is.")
        verify(!find(page, "vehicleNew").enabled)
        page.saveVehicles(VehicleBook.add(VehicleBook.empty(), "Mine"))
        compare(page.pickedVehicle, null, "no vehicle is made over it")
        verify(importFile(page, "extras.json", "Extras"), "the store works without the book")
        page.destroy()
        wait(10)
        storedVehicles.sync()
        compare(storedVehicles.value("book"), later, "the stored book is as it was")
    }

    // ---- An OBDb set imported (ADR-0051's appendix of 2026-11-06, ADR-0064 Q3) ----

    // The OBDb fixture read through Import under the year typed
    function obdbRead(page, year) {
        if (!page.importPart.importFile(Harness.obdbFixture))
            return false
        if (year !== "") {
            find(page, "obdbYearField").text = ""   // a year typed, even the one a VIN gave
            find(page, "obdbYearField").text = year
        }
        return page.importPart.obdbReport !== null
    }
}
