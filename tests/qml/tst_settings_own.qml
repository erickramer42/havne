// File/layer: tests/qml/tst_settings.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.settings (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

SettingsKit {
    id: testCase
    name: "SettingsOwn"

    // The visible buttons under item, by their size. Only what has down
    // and checkable is measured (a Button, CheckBox or Switch): a MouseArea
    // row or a TapHandler is not
    function buttonsUnder(item, out) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (!child.visible)
                continue
            // A button, or a ComboBox, which has no checkable
            if (child.down !== undefined
                    && (child.checkable !== undefined || child.popup !== undefined))
                out.push(child)
            buttonsUnder(child, out)
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

    // The Signals page's states on a phone (qml.md's density): at 384 x
    // 832 every button is a touch's size, and at 320 px, the narrowest
    // phone, nothing lies past the column
    function test_theSignalsPagesStatesFitAPhone_data() {
        return [{ tag: "list" }, { tag: "open row" }, { tag: "copy" }, { tag: "import panel" },
                { tag: "obdb report" }, { tag: "export note" }, { tag: "confirm" },
                { tag: "missing pick" }, { tag: "not moved" }]
    }

    function test_theSignalsPagesStatesFitAPhone(data) {
        Theme.compact = true
        Theme.dense = true
        if (data.tag === "not moved") {
            const book = VehicleBook.change(VehicleBook.add(VehicleBook.empty(), "W".repeat(40)), v => {
                v.unmigrated = [{ sha256: "a".repeat(64), name: "A file's name as long as a name may be",
                                  words: "line 12, column 40: the words a loader gives, at their length" }]
            })
            book.unmigrated = [{ sha256: "b".repeat(64), name: "Loose", words: "w", kept: false }]
            storedVehicles.setValue("book", VehicleBook.serialize(book))
            storedVehicles.sync()
        }
        const page = createPage(true)
        page.width = 384
        page.height = 832
        if (page.pickedVehicle === null) {
            find(page, "vehicleLabelField").text = "W".repeat(40)
            press(page, "vehicleNew")
        }
        verify(importExample(page))
        verify(importFile(page, "long.json", "A plugin of the user's own with a name that wraps"))
        const own = page.picksPart.ownPlugins[0]
        const theirs = "plugin.example-sim.my2019"
        if (data.tag === "open row") {
            page.rowActionsPart.openRow = theirs
        } else if (data.tag === "copy") {
            page.rowActionsPart.openRow = theirs
            page.rowActionsPart.copyShown = true
        } else if (data.tag === "import panel") {
            verify(page.importPart.importFile(Harness.writeScratchFile("track.json", signalFileText("Track day", "D101"))))
            page.importPart.importInto = own.sha256
        } else if (data.tag === "obdb report") {
            verify(obdbRead(page, "2021"))
        } else if (data.tag === "export note") {
            verify(obdbRead(page, "2021"))
            verify(page.importPart.keepObdb())
            const obdb = page.picksPart.pluginList.find(p => p.obdb)
            page.rowActionsPart.openRow = obdb.profiles[0]
            verify(page.rowActionsPart.exportWaits(obdb.sha256))
        } else if (data.tag === "confirm") {
            page.rowActionsPart.askRemovePlugin(own.sha256)
            verify(page.rowActionsPart.confirmingPlugin() !== null)
        } else if (data.tag === "missing pick") {
            page.saveVehicles(VehicleBook.setPicks(page.vehicles,
                                                   page.picksPart.picks.concat(["plugin.gone-from-the-store.base"])))
        }
        page.open("signals")
        const shown = find(page, "settingsSignalsPage")
        verify(shown.visible, "Signals shows")
        const column = find(page, "settingsSignalsPageColumn")
        waitForPolish(column)
        const buttons = buttonsUnder(shown, [])
        verify(buttons.length > 2, "buttons to measure")
        for (const b of buttons) {
            const o = b.mapToItem(null, 0, 0)
            const e = b.mapToItem(null, b.width, b.height)
            const name = b.objectName !== "" ? b.objectName : b.text
            verify(e.y - o.y >= 48 - 0.5, name + " is " + (e.y - o.y) + " tall")
            verify(e.x - o.x >= 48 - 0.5, name + " is " + (e.x - o.x) + " wide")
        }
        page.width = 320
        tryCompare(column, "width", 320 - 2 * Theme.pageMargin)
        waitForPolish(column)
        tryVerify(() => pastTheEdge(column, column, []).length === 0, 2000)
        compare(pastTheEdge(column, column, []).join("; "), "", "nothing past the column at 320")
    }

    // What the store holds for a vehicle goes with it: after Forget a
    // plugin imported for it is no longer first for a vehicle that takes
    // its id, and the plugins themselves stay
    function test_forgetReleasesWhatWasImportedForTheVehicle() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "extras.json", "Extras"))
        compare(page.picksPart.pluginList[0].importedFor, "v1")
        press(page, "vehicleDuplicate")
        compare(page.picksPart.picks, ["obd2", "plugin.extras.base"], "Duplicate copies the picks")
        verify(rowData(page, "plugin.extras.base").forCar, "the copy picks it")
        pressRow(page, "plugin.extras.base")
        verify(!rowData(page, "plugin.extras.base").forCar, "imported for the first, not the copy")
        press(page, "vehicleForget")
        press(page, "vehicleConfirm")
        page.ownPart.pickVehicle("v1")
        pressRow(page, "plugin.extras.base")
        compare(page.picksPart.picks, ["obd2"])
        verify(rowData(page, "plugin.extras.base").forCar, "imported for this one, picked or not")
        press(page, "vehicleForget")
        press(page, "vehicleConfirm")
        compare(page.vehicles.list.length, 0)
        compare(page.picksPart.pluginList.length, 1, "the plugin stays")
        compare(page.picksPart.pluginList[0].importedFor, "", "but is no vehicle's import")
        verify(find(rowOf(page, "plugin.extras.base"), "rowUnused").visible)

        press(page, "vehicleNew")
        verify(obdbRead(page, ""))
        press(page, "obdbKeep")
        compare(page.picksPart.pluginList.filter(p => p.importedFor !== "").length, 1)
        press(page, "vehicleForgetAll")
        press(page, "vehicleConfirm")
        compare(page.picksPart.pluginList.length, 2, "Forget all keeps the plugins")
        compare(page.picksPart.pluginList.filter(p => p.importedFor !== "").length, 0)
        verify(findChild(page, "profile_plugin.extras.base") === null, "no vehicle, no pick offered")
    }

    // A pick's profile is its vehicle's: a pick of another vehicle, or
    // New, puts the field back to Default (its connection kept), so it
    // never connects, and is remembered, with the last one's signals; a
    // built-in profile stays
    function test_aPickLeavesAnotherVehiclesProfileBehind() {
        const page = createPage()
        press(page, "vehicleNew")
        const first = page.pickedVehicle.id
        verify(importFile(page, "extras.json", "Extras"))
        find(page, "uriField").text = "mock://demo"
        page.uriPart.chooseProfile("plugin.extras.base")
        compare(find(page, "uriField").text, "mock://demo?profile=plugin.extras.base")
        press(page, "vehicleNew")
        compare(find(page, "uriField").text, "mock://demo", "New: Default")
        compare(page.uriPart.profileChoice, "default")

        const second = page.pickedVehicle.id
        press(page, "vehicle_" + first)
        page.uriPart.chooseProfile("plugin.extras.base")
        press(page, "vehicleDuplicate")
        verify(page.pickedVehicle.id !== first && page.pickedVehicle.id !== second)
        compare(find(page, "uriField").text, "mock://demo?profile=plugin.extras.base",
                "the copy has the pick: its profile stays")
        press(page, "vehicle_" + second)
        compare(find(page, "uriField").text, "mock://demo", "another's goes")

        page.uriPart.chooseProfile("raw")
        press(page, "vehicle_" + first)
        compare(find(page, "uriField").text, "mock://demo?profile=raw", "a built-in stays")
    }

    // A copy's own signals are its own: its own file is copied under a
    // name of its own, so each vehicle's Save goes ahead (one file to a
    // profile); and a label used again gets an own file named apart
    function test_eachVehiclesOwnSignalsSaveApart() {
        const page = createPage()
        press(page, "vehicleNew")
        const first = page.pickedVehicle.id
        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        press(page, "vehicleDuplicate")
        verify(page.pickedVehicle.id !== first)
        verify(page.pickedVehicle.own !== "" && page.pickedVehicle.own !== VehicleBook.find(page.vehicles, first).own,
               "the copy's own file is a copy")
        compare(page.pickedVehicle.own, "plugin.vehicle-1-copy-signals.base")
        compare(page.ownFile.name, "Vehicle 1 (copy) signals")
        compare(page.ownSignals.map(s => s.key), ["trans_temp"])
        typeSignal(page, "Gear", "D101")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.length, 2)

        press(page, "vehicle_" + first)
        compare(page.ownSignals.map(s => s.key), ["trans_temp"], "the first is as it was")
        typeSignal(page, "Fluid Pressure", "D102")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.length, 2)

        // Renamed, its label is free for New, whose own file is named apart
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleRename")
        press(page, "vehicleNew")
        compare(page.pickedVehicle.label, "Vehicle 1")
        typeSignal(page, "Oil Temp", "D103")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownFile.name, "Vehicle 1 signals")
        compare(page.pickedVehicle.own, "plugin.vehicle-1-signals-2.base", "an id of its own")
    }

    // The form and the section's words are the picked vehicle's: a pick or
    // New starts them afresh, an Edit's signal never lands in another
    // vehicle, and a question asked about one goes with it
    function test_aPickStartsTheFormAndNotesAfresh() {
        const page = createPage()
        press(page, "vehicleNew")
        const first = page.pickedVehicle.id
        const bad = Harness.writeScratchFile("bad.json", signalFileText("Bad").replace("bits", "bitz"))
        verify(!(page.importPart.importFile(bad) && page.importPart.keepPending("", "")))
        verify(find(page, "signalProblem").text !== "")
        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        press(page, "vehicleNew")
        compare(find(page, "signalProblem").text, "", "the other vehicle's refusal goes")
        const second = page.pickedVehicle.id

        press(page, "vehicle_" + first)
        press(page, "ownEdit_0")
        compare(find(page, "formName").text, "Trans Temp")
        press(page, "vehicle_" + second)
        compare(find(page, "formName").text, "", "the Edit stays with its vehicle")
        compare(page.ownPart.editingKey, "")

        press(page, "vehicle_" + first)
        press(page, "ownRemove_0")
        verify(page.confirming !== "", "asked")
        page.saveVehicles(VehicleBook.pick(page.vehicles, second))   // as a car recognized
        compare(page.confirming, "", "the question goes with the pick")
    }

    // Edit then a new name renames the signal under the key it has; a new
    // signal whose name makes a key a saved one keeps gets a key of its
    // own, never the other's place; the same name again replaces, as
    // designed; and an Edit can't take another saved signal's name
    function test_anEditKeepsTheKeyAndAKeyIsNeverTakenTwice() {
        const page = createPage()
        press(page, "vehicleNew")
        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        press(page, "ownEdit_0")
        find(page, "formName").text = "Trans Fluid Temp"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.map(s => s.name), ["Trans Fluid Temp"])
        compare(page.ownSignals.map(s => s.key), ["trans_temp"], "renamed, the key it was saved with")
        compare(page.ownPart.editingKey, "", "the Edit is over")

        // The name the first was saved under, typed for a new signal
        page.ownPart.clearForm()
        typeSignal(page, "Trans Temp", "D100")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.map(s => s.key), ["trans_temp", "trans_temp_2"])
        compare(page.ownSignals.map(s => s.read.number), ["1E1C", "D100"], "neither saved over")

        page.ownPart.clearForm()
        typeSignal(page, "Boost (psi)", "D101")
        press(page, "formSave")
        page.ownPart.clearForm()
        typeSignal(page, "Boost psi", "D102")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.map(s => s.key), ["trans_temp", "trans_temp_2", "boost_psi", "boost_psi_2"])
        page.ownPart.clearForm()
        typeSignal(page, "Boost (psi)", "D103")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.map(s => s.read.number), ["1E1C", "D100", "D103", "D102"],
                "the same name replaces")
        compare(page.ownSignals[2].key, "boost_psi")

        // The loaded variant has the keys the form kept
        compare(Bridge.profileSignals("mock://?profile=" + page.pickedVehicle.own).map(r => r.ref)
                    .filter(r => r.startsWith("key:")).sort(),
                ["key:boost_psi", "key:boost_psi_2", "key:trans_temp", "key:trans_temp_2"])

        // An Edit renamed to another saved signal's name is refused
        press(page, "ownEdit_0")
        find(page, "formName").text = "Boost psi"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "Boost psi is saved already: give this one another name")
        compare(page.ownSignals.map(s => s.name),
                ["Trans Fluid Temp", "Trans Temp", "Boost (psi)", "Boost psi"])
    }

    // A Dashboard gauge names a form signal by its key (ADR-0057), so it
    // reads on after the signal is renamed
    function test_aGaugeBoundToAFormSignalReadsOnAfterItsRename() {
        const page = createPage()
        press(page, "vehicleNew")
        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        const own = page.pickedVehicle.own
        const uri = "mock://demo?profile=" + own

        const dash = createTemporaryObject(dashComponent, testCase.Window.window.contentItem)
        const doc = DashboardBook.builtIn("desktop")
        doc.pages = [{ name: "Own", cells: [
            { kind: "digital", w: 2, h: 4, label: "Fluid temp", refs: ["key:trans_temp"], unit: "",
              options: {} }] }]
        dash.doc = doc
        const hub = createTemporaryObject(hubComponent, testCase)
        const gauge = gaugeLabeled(dash, "Fluid temp")
        verify(gauge, "the cell draws a gauge")

        compare(Bridge.connectSession(uri), "")
        tryVerify(() => gauge.hasValue, 5000, "the gauge reads the signal")
        compare(hub.byRef["key:trans_temp"].name, "Trans Temp")
        Bridge.disconnectSession()
        tryVerify(() => page.ownPart.idle, 5000, "the session ends")

        press(page, "ownEdit_0")
        find(page, "formName").text = "Transmission fluid temperature"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.pickedVehicle.own, own, "the same variant")

        compare(Bridge.connectSession(uri), "")
        tryVerify(() => hub.byRef["key:trans_temp"] !== undefined
                        && hub.byRef["key:trans_temp"].name === "Transmission fluid temperature", 5000,
                  "the renamed signal under its key")
        tryVerify(() => gauge.hasValue, 5000, "the gauge reads on")
        verify(gauge.note !== "not in profile", gauge.note)
    }

    // The page's gauge (a GaugeBase item) of that label, null for none
    function gaugeLabeled(within, label) {
        for (let i = 0; i < within.children.length; ++i) {
            const child = within.children[i]
            if (child.label === label && child.note !== undefined && child.valueText !== undefined)
                return child
            const found = gaugeLabeled(child, label)
            if (found !== null)
                return found
        }
        return null
    }

    // A Read once's reply shows only on the vehicle it was asked for
    function test_aReadOncesReplyShowsOnlyOnItsVehicle() {
        const page = createPage()
        press(page, "vehicleNew")
        const reply = { ok: true, request: "22 1E 1C", reply: "62 1E 1C 12 34",
                        value: "4660", nrc: 0, words: "" }
        page.ownPart.readOnceFor = "another vehicle"
        Bridge.readOnceResult(reply)
        compare(page.ownPart.readResult, null, "asked for another vehicle")
        page.ownPart.readOnceFor = page.ownPart.pickedId
        Bridge.readOnceResult(reply)
        compare(page.ownPart.readResult.reply, "62 1E 1C 12 34")
    }

    // One request ID to a module: a Save whose typed one differs moves the
    // module's other signals, and its note names them; a blank one keeps
    // the module's
    function test_aModulesRequestIdMovesItsSignalsAndSaysSo() {
        const page = createPage()
        press(page, "vehicleNew")
        typeSignal(page, "Gear", "D101")
        find(page, "formRx").text = "7E9"
        find(page, "formTx").text = ""
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(find(page, "formNote").text, "Gear saved in Vehicle 1 signals")
        typeSignal(page, "Oil", "D102")
        find(page, "formRx").text = "7E9"
        find(page, "formTx").text = "7E2"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(find(page, "formNote").text, "Oil saved in Vehicle 1 signals; 7E9 asks on 7E2 for Gear too")
        compare(page.ownFile.ecus.map(e => e.tx), ["7E2"])
        typeSignal(page, "Pressure", "D103")
        find(page, "formRx").text = "7E9"
        find(page, "formTx").text = ""
        press(page, "formSave")
        compare(find(page, "formNote").text, "Pressure saved in Vehicle 1 signals")
        compare(page.ownFile.ecus.map(e => e.tx), ["7E2"], "blank keeps the module's")
    }

    // ---- The vehicle's own signals (ADR-0051's form) ----

    function typeSignal(page, name, number, bits) {
        find(page, "formName").text = name
        find(page, "formNumber").text = number
        if (bits !== undefined)
            find(page, "formBits").text = bits
    }
}
