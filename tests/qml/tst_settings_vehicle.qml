// File/layer: tests/qml/tst_settings.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.settings (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

SettingsKit {
    id: testCase
    name: "SettingsVehicle"

    Component { id: logSpyComponent; SignalSpy {} }

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

    // ---- The vehicle's own signals (ADR-0051's form) ----

    function typeSignal(page, name, number, bits) {
        find(page, "formName").text = name
        find(page, "formNumber").text = number
        if (bits !== undefined)
            find(page, "formBits").text = bits
    }

    function ownText(page, i) {
        return find(find(page, "ownSignal_" + i), "ownSignalText").text
    }

    // Each box is named above it, so a box keeps saying what it holds once
    // its placeholder has gone
    function test_theFormsBoxesKeepTheirNamesOnceTyped() {
        const page = createPage()
        press(page, "vehicleNew")
        compare(page.ownPart.formKind, "number")
        const caption = name => find(page, name).parent.children[0]
        const named = { formName: "Name", formRx: "Reply ID", formTx: "Request ID",
                        formNumber: "DID", formByte: "Byte", formBits: "Bits",
                        formBit: "Start bit", formScale: "Scale", formOffset: "Offset",
                        formUnit: "Unit", formDecimals: "Decimals" }
        press(page, "formService_22")
        for (const box in named) {
            find(page, box).text = "1"
            compare(caption(box).text, named[box], box)
            page.open("ownform")
            verify(caption(box).visible, box)
        }
        press(page, "formService_01")
        compare(caption("formNumber").text, "PID")
        press(page, "formKind_hex")
        verify(!caption("formBits").visible, "no bits for a hex field")
        verify(!caption("formBit").visible)
        verify(caption("formBytes").visible)
        compare(caption("formBytes").text, "Bytes")
    }

    function test_theFormSavesTheVehiclesOwnSignals() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("ownform")
        verify(find(page, "signalForm").visible)
        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(find(page, "formNote").text, "Trans Temp saved in Vehicle 1 signals")
        compare(ownText(page, 0), "Trans Temp: 22 1E1C on 7E8")
        const own = "plugin.vehicle-1-signals.base"
        compare(page.pickedVehicle.own, own, "the vehicle's own variant")
        compare(page.picksPart.picks, [own], "made with the first save, and picked")
        compare(find(rowOf(page, own), "rowTitle").text, "Vehicle 1 signals 1.0.0")
        compare(find(rowOf(page, own), "rowReads").text, "1 signal · yours")
        compare(find(rowOf(page, own), "rowCaption").text, "For this car:")
        verify(findChild(page, "profile_" + own), "a profile under Profile")
        compare(Harness.storedPlugins().length, 1)

        // A second signal goes into the same variant; the last copy goes
        page.ownPart.fillForm(SignalForm.emptyFields())
        typeSignal(page, "Gear", "D101")
        press(page, "formKind_enum")
        find(page, "formValues").text = "0=Park, 3=Drive"
        press(page, "formSave")
        compare(page.ownSignals.map(s => s.key), ["trans_temp", "gear"])
        compare(page.ownSignals[1].values, { "0": "Park", "3": "Drive" })
        compare(Harness.storedPlugins().length, 1, "one copy: the plugin's newest")
        compare(page.picksPart.pluginList.length, 1, "no second plugin")
        compare(page.pickedVehicle.own, own)
        compare(page.picksPart.picks, [own])
        compare(find(rowOf(page, own), "rowReads").text, "2 signals · yours")

        // Edit fills the form; Save replaces the signal of that name
        press(page, "ownEdit_0")
        compare(find(page, "formName").text, "Trans Temp")
        compare(find(page, "formBits").text, "16")
        compare(page.ownPart.formKind, "number")
        find(page, "formNumber").text = "1E1D"
        press(page, "formSave")
        compare(page.ownSignals.map(s => s.read.number), ["1E1D", "D101"])

        // The variant as a signal file, named by its plugin
        const what = page.rowActionsPart.variantSave(rowData(page, own))
        compare(what.file, "vehicle-1-signals.json")
        const url = Harness.writeScratchFile("own.json", "")
        page.rowActionsPart.saving = what
        verify(page.rowActionsPart.saveTo(url))
        compare(JSON.parse(Harness.readScratchFile(url)).signals.map(s => s.name),
                ["Trans Temp", "Gear"])

        // The last signal's Remove takes the plugin, once asked: it was
        // written here and has no copy elsewhere
        press(page, "ownRemove_1")
        compare(page.ownSignals.length, 1)
        compare(find(page, "formNote").text, "Removed from Vehicle 1 signals")
        press(page, "ownRemove_0")
        compare(page.ownSignals.length, 1, "asked first")
        const question = find(page, "ownConfirm")
        verify(question.visible, "asked under the own signals, where it was pressed")
        compare(find(question, "signalsConfirmText").text,
                "Remove Vehicle 1 signals? It is yours, and no other copy exists")
        find(question, "signalsConfirmRemove").clicked()
        compare(page.picksPart.picks, [])
        compare(page.pickedVehicle.own, "")
        compare(Harness.storedPlugins(), [])

        // The next save makes the variant again
        typeSignal(page, "Oil Temp", "D103")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.pickedVehicle.own, own)
        compare(page.picksPart.picks, [own])
    }

    // The last signal's Remove of a plugin with several variants asks as
    // well, and drops that variant alone once answered
    function test_theLastSignalsRemoveAsksWhereThePluginHasSeveralVariants() {
        const page = createPage()
        press(page, "vehicleNew")
        typeSignal(page, "Trans Temp", "1E1C", "16")
        press(page, "formSave")
        const own = page.pickedVehicle.own
        const url = Harness.writeScratchFile("second.json", signalFileText("Second", "D105"))
        verify(page.importPart.importFile(url))
        verify(page.importPart.keepPending(page.ownPlace.plugin.sha256, "second"))
        compare(page.ownPlace.plugin.variants.length, 2, "a second variant")
        press(page, "ownRemove_0")
        compare(page.pickedVehicle.own, own, "asked first, nothing removed")
        const question = find(page, "ownConfirm")
        verify(question.words !== "", "asked under the own signals")
        verify(find(question, "signalsConfirmText").text.startsWith("Remove the variant "),
               find(question, "signalsConfirmText").text)
        find(question, "signalsConfirmCancel").clicked()
        compare(page.pickedVehicle.own, own, "Cancel keeps it")
        press(page, "ownRemove_0")
        find(find(page, "ownConfirm"), "signalsConfirmRemove").clicked()
        compare(page.pickedVehicle.own, "", "the variant went once asked")
        compare(Harness.storedPlugins().length, 1, "the plugin stayed with its other variant")
    }

    function test_theFormShowsTheLoadersWords() {
        const page = createPage()
        press(page, "vehicleNew")
        typeSignal(page, "Bad", "1E1C", "12")
        press(page, "formSave")
        const words = find(page, "formProblem").text
        verify(words.includes("big-endian"), words)
        verify(!/^line \d/.test(words), "no line and column: " + words)
        compare(page.ownSignals, [])
        compare(Harness.storedPlugins(), [])

        // Read once checks the draft first, then needs an adapter
        find(page, "uriField").text = "mock://demo"
        press(page, "formReadOnce")
        verify(find(page, "formProblem").text.includes("big-endian"))
        find(page, "formBits").text = "16"
        press(page, "formReadOnce")
        compare(find(page, "formProblem").text,
                "Read once needs an adapter: j2534://, elm327:// or socketcan://")
        find(page, "formRx").text = "7B8"
        press(page, "formReadOnce")
        compare(find(page, "formProblem").text,
                "The module needs its request ID: only 7E8 to 7EF and 18DAF1xx have one by rule")

        // No Read once while a session holds the adapter
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000)
        verify(!find(page, "formReadOnce").enabled)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
    }

    // The fake DLL's ECU answers 1E1C with 12 34
    function test_readOnceShowsTheRepliesBytesAndValue() {
        if (Harness.fakeDllPath === "")
            skip("the fake J2534 DLL is Windows only")
        const page = createPage()
        press(page, "vehicleNew")
        find(page, "uriField").text = "j2534://" + Harness.fakeDllPath + "?profile=obd2"
        typeSignal(page, "Test", "1E1C", "16")
        find(page, "formScale").text = "0.1"
        find(page, "formUnit").text = "kPa"
        find(page, "formDecimals").text = "1"
        press(page, "formReadOnce")
        compare(find(page, "formProblem").text, "")
        tryVerify(() => page.ownPart.readResult !== null, 15000, "the read ends")
        compare(find(page, "formReadResult").text, "Asked 22 1E 1C, got 62 1E 1C 12 34: 466.0 kPa")
        verify(!Bridge.connected, "no session left behind")
    }

    function test_pickingAVehicleFillsItsConnection() {
        const page = createPage()
        press(page, "vehicleNew")
        page.saveVehicles(VehicleBook.rememberUri(page.vehicles,
                                                  "elm327://COM6?profile=uds-generic&did=F190"))
        press(page, "vehicle_none")
        find(page, "uriField").text = "mock://demo"
        tap(find(page, "vehicle_v1"))
        compare(find(page, "uriField").text, "elm327://COM6?profile=uds-generic&did=F190")
        verify(find(page, "profile_uds-generic").checked, "read back into the row")
        compare(find(page, "didField").text, "F190")
    }

    // The vehicle a session runs under can't change beneath it, and a
    // mock's session isn't that car's
    function test_aMockSessionLeavesTheVehicleAloneAndLocksTheSection() {
        const page = createPage()
        press(page, "vehicleNew")
        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "the mock connects")
        verify(!find(page, "vehicleNew").enabled)
        verify(!find(page, "vehicle_none").enabled)
        verify(!find(page, "vehicleForgetAll").enabled)
        Bridge.signalModel.toggleSubscribed(0)
        compare(page.pickedVehicle.uri, "", "a mock's URI isn't kept")
        compare(page.pickedVehicle.unplotted, [], "nor its plotting")
        Bridge.signalModel.toggleSubscribed(0)
    }

    // A text row's tick is the vehicle's too, though it changes no plotted
    // count: the ticked count carries it
    function test_aTextRowsTickIsKeptForTheVehicle() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const page = createPage()
        find(page, "vehicleLabelField").text = "Weekend car"
        press(page, "vehicleNew")
        find(page, "uriField").text = "j2534://" + Harness.fakeDllPath
            + "?profile=uds-generic&did=F190:text,1E1C:u16&identify=0&diagnose=0"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "the fake connects")
        tryVerify(() => page.plottedApplied, 5000, "the vehicle's choices applied")
        const plotted = Bridge.signalModel.subscribedCount
        const writes = logSpyComponent.createObject(testCase,
            { target: page, signalName: "vehiclesChanged" })
        Bridge.signalModel.toggleSubscribed(0)   // DID F190, a text row
        compare(Bridge.signalModel.subscribedCount, plotted, "it plots nothing")
        const keys = Bridge.signalModel.plottedKeys()
        compare(keys.length, 1)
        compare(page.pickedVehicle.plotted, keys, "the tick is kept")
        compare(writes.count, 1, "one write for the text row's tick")
        // A number's untick changes both counts, and is still one write
        Bridge.signalModel.toggleSubscribed(1)   // DID 1E1C
        compare(writes.count, 2, "one write for the number's untick")
        writes.destroy()
    }

    // ADR-0027 sections 2-4 through the fake: the connect keeps the URI, the VIN
    // is learned and logged, Live's plotting is kept and comes back
    function test_anAdaptersConnectKeepsItsVehiclesSetup() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        let log = []
        const collect = line => log.push(line)
        Bridge.logMessage.connect(collect)
        const page = createPage()
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        const uri = "j2534://" + Harness.fakeDllPath
        find(page, "uriField").text = uri
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "the fake connects")
        compare(page.pickedVehicle.uri, uri)
        tryVerify(() => page.pickedVehicle.vinKey !== "", 5000, "the VIN is learned")
        verify(page.vehicles.salt.length === 32, "a salt made for it")
        verify(log.includes("Vehicle: Daily driver (recognized from now on)"), log.join("\n"))
        verify(!JSON.stringify(page.vehicles).includes("FAKEJ2534VIN"), "no VIN in the store")

        Bridge.signalModel.toggleSubscribed(1)   // Coolant Temp: plotted by rank
        const unticked = Bridge.signalModel.unplottedKeys()
        compare(unticked, ["7E8:4105:2.0:8"], "by its key")
        compare(page.pickedVehicle.unplotted, unticked, "an untick is kept")
        Bridge.signalModel.toggleSubscribed(5)   // Throttle Position: unranked
        compare(Bridge.signalModel.plottedKeys(), ["7E8:4111:2.0:8"])
        compare(page.pickedVehicle.plotted, ["7E8:4111:2.0:8"], "a tick past the default too")

        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        log = []
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "connects again")
        compare(Bridge.signalModel.unplottedKeys(), unticked, "the untick comes back")
        compare(Bridge.signalModel.plottedKeys(), ["7E8:4111:2.0:8"], "and the tick")
        tryVerify(() => log.includes("Vehicle: Daily driver"), 5000, "recognized: " + log.join("\n"))
        Bridge.logMessage.disconnect(collect)
    }

    // ADR-0065 section 9: a vehicle's ticks are kept by key. A book written
    // with names ticks the same rows and is written by key at the next
    // tick, and a tick whose signal this set lacks stays in the book
    function test_ticksKeptByNameAreReadAndRewrittenByKey() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const page = createPage()
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        const elsewhere = "7E9:62F410:3.0:16"   // another plugin's signal
        page.saveVehicles(VehicleBook.rememberPlotting(page.vehicles,
            ["Throttle Position", elsewhere], ["Coolant Temp"]))
        find(page, "uriField").text = "j2534://" + Harness.fakeDllPath
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "the fake connects")
        tryVerify(() => page.plottedApplied, 5000, "the vehicle's choices applied")
        compare(Bridge.signalModel.unplottedKeys(), ["7E8:4105:2.0:8"], "the name's row is unticked")
        compare(Bridge.signalModel.plottedKeys(), ["7E8:4111:2.0:8", elsewhere],
                "the name's row ticked, the other carried")
        compare(page.pickedVehicle.plotted, ["Throttle Position", elsewhere],
                "the book is as it was until a tick")

        Bridge.signalModel.toggleSubscribed(2)   // RPM: plotted by rank
        compare(page.pickedVehicle.unplotted, ["7E8:4105:2.0:8", "7E8:410C:2.0:16"],
                "written by key")
        compare(page.pickedVehicle.plotted, ["7E8:4111:2.0:8", elsewhere],
                "and the tick no row answers is still the vehicle's")

        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "connects again")
        tryVerify(() => page.plottedApplied, 5000, "the vehicle's choices applied")
        compare(Bridge.signalModel.unplottedKeys(), ["7E8:4105:2.0:8", "7E8:410C:2.0:16"])
        compare(Bridge.signalModel.plottedKeys(), ["7E8:4111:2.0:8", elsewhere])
    }

    // A car whose VIN makes a new vehicle: the new vehicle is added
    // unpicked, so the session's ticks and the picked vehicle's stay
    function test_aNewVehicleTheVinAddsIsNotPickedAndTakesNoTicks() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const page = createPage()
        find(page, "vehicleLabelField").text = "Another car"
        press(page, "vehicleNew")
        const elsewhere = "7E9:62F410:3.0:16"
        const doc = VehicleBook.rememberPlotting(page.vehicles, ["Throttle Position", elsewhere], [])
        page.saveVehicles(VehicleBook.change(doc, v => v.vinKey = "another car's"))
        const other = page.pickedVehicle.id
        find(page, "uriField").text = "j2534://" + Harness.fakeDllPath
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "the fake connects")
        tryVerify(() => page.vehicles.list.length === 2, 5000, "the VIN made a vehicle")
        compare(page.pickedVehicle.id, other, "the session's pick stays")
        compare(page.pickedVehicle.plotted, ["Throttle Position", elsewhere], "its ticks are its own")
        const added = page.vehicles.list.find(v => v.id !== other)
        compare(added.plotted, [], "the new vehicle has none")
    }

    // A vehicle with no VIN picked on a car another vehicle is saved with:
    // the pick and the session's profile stay, and the log and the section
    // name the other vehicle as an offer for the next connect, never as
    // the vehicle read
    function test_theCarsVinNamingAnotherVehicleOffersItAndPicksNothing() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        let log = []
        const collect = line => log.push(line)
        Bridge.logMessage.connect(collect)
        const page = createPage()
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        const daily = page.pickedVehicle.id
        const uri = "j2534://" + Harness.fakeDllPath
        find(page, "uriField").text = uri
        press(page, "connectButton")
        tryVerify(() => page.pickedVehicle.vinKey !== "", 10000, "the VIN is learned")
        const note = find(page, "vehicleVinPickNote")
        verify(!note.visible, "learning the VIN is no offer")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)

        find(page, "vehicleLabelField").text = "Weekend car"
        press(page, "vehicleNew")
        const weekend = page.pickedVehicle.id
        find(page, "uriField").text = uri
        log = []
        press(page, "connectButton")
        const offer = "Vehicle: Weekend car (the car's VIN is Daily driver's; pick Daily driver for the next connect)"
        tryVerify(() => log.includes(offer), 10000, "the offer is logged")
        verify(!log.some(l => l.startsWith("Vehicle: Daily driver")), "no line names it as the one read")
        compare(page.pickedVehicle.id, weekend, "the session's pick stays")
        page.open("vehicle")
        verify(note.visible)
        const said = "This car's VIN is Daily driver's and Weekend car is picked. "
                     + "Pick Daily driver to read its signals at the next connect"
        compare(note.text, said)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        verify(note.visible, "it stays true after the session")
        compare(note.text, said)
        compare(page.pickedVehicle.id, weekend)

        // A change of the pick ends it, New included, and a session's start;
        // the part's text is read, since the note shows on the Vehicle page only
        const own = page.ownPart
        find(page, "vehicleLabelField").text = "Third car"
        press(page, "vehicleNew")
        compare(own.vinPickNote, "", "New ends the offer")
        press(page, "vehicle_" + weekend)
        press(page, "connectButton")
        tryVerify(() => own.vinPickNote !== "", 10000, "offered again by the next session")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        press(page, "connectButton")
        compare(own.vinPickNote, "", "a session's start ends it")
        tryVerify(() => own.vinPickNote !== "", 10000, "and the VIN offers it again")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        press(page, "vehicle_" + daily)
        compare(own.vinPickNote, "", "the user's pick ends it")
        Bridge.logMessage.disconnect(collect)
    }

    // A VIN naming another vehicle during a session leaves the pick, the
    // field and the vehicle's connection as they were: the session reads
    // the picked vehicle's plugin and the vehicle keeps it
    function test_theVinsNamingAnotherVehicleLeavesTheFieldAndTheConnection() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const log = []
        const collect = line => log.push(line)
        Bridge.logMessage.connect(collect)
        const page = createPage()
        const uri = "j2534://" + Harness.fakeDllPath
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        find(page, "uriField").text = uri
        press(page, "connectButton")
        tryVerify(() => page.pickedVehicle.vinKey !== "", 10000, "the VIN is learned")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)

        find(page, "vehicleLabelField").text = "Weekend car"
        press(page, "vehicleNew")
        const weekend = page.pickedVehicle.id
        verify(importFile(page, "extras.json", "Extras"))
        find(page, "uriField").text = uri
        page.uriPart.chooseProfile("plugin.extras.base")
        const named = uri + "?profile=plugin.extras.base"
        compare(find(page, "uriField").text, named)
        press(page, "connectButton")
        tryVerify(() => Bridge.connected && page.pickedVehicle.uri === named, 30000,
                  "the connect keeps the URI")
        tryVerify(() => log.some(l => l.includes("the car's VIN is Daily driver's")), 10000,
                  "the VIN has been read")
        compare(page.pickedVehicle.id, weekend, "the pick stays")
        compare(find(page, "uriField").text, named, "the field stays")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        compare(page.pickedVehicle.id, weekend)
        compare(find(page, "uriField").text, named, "the field keeps the plugin after the session")
        compare(page.pickedVehicle.uri, named, "the vehicle keeps its own plugin's connection")
        Bridge.logMessage.disconnect(collect)
    }

    // A vehicle the VIN adds during a session holds no signal file, so its
    // connection doesn't name the last vehicle's; the picked vehicle and
    // the field keep the plugin the session read
    function test_aVehicleTheVinAddsKeepsNoOtherVehiclesFile() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const page = createPage()
        const uri = "j2534://" + Harness.fakeDllPath
        find(page, "vehicleLabelField").text = "Weekend car"
        press(page, "vehicleNew")
        const weekend = page.pickedVehicle.id
        verify(importFile(page, "extras.json", "Extras"))
        // Another car's VIN: the fake's is no vehicle's yet
        page.saveVehicles(VehicleBook.change(page.vehicles, v => v.vinKey = "another-cars-key"))
        find(page, "uriField").text = uri
        page.uriPart.chooseProfile("plugin.extras.base")
        press(page, "connectButton")
        tryVerify(() => page.vehicles.list.length === 2, 30000, "the VIN adds a vehicle")
        const added = page.vehicles.list.find(v => v.id !== weekend)
        compare(added.picks, [])
        compare(added.uri, uri, "no profile of Weekend car's pick")
        compare(page.pickedVehicle.id, weekend, "the session's pick stays")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 30000)
        const named = uri + "?profile=plugin.extras.base"
        compare(find(page, "uriField").text, named, "the field keeps the plugin")
        compare(page.pickedVehicle.uri, named, "and so does the picked vehicle's address")
    }

    // ADR-0056 section 4: the row lists Off and the bridge's choices,
    // shows the bridge's and sets it, and says what it is for
    function test_theMinimumTimeBetweenSweepsIsARowOfChoices() {
        const page = createPage(true)
        page.open("advanced")
        const choice = find(page, "minSweepChoice")
        verify(choice.visible)
        verify(find(page, "minSweepNote").text.includes("file size"))
        // Every requesting session's, not a log's alone
        verify(find(page, "minSweepNote").text.includes("on Live, the Dashboard and in a log"))
        // Core paces a pick alone, never the whole set
        verify(find(page, "minSweepNote").text.includes("asking every signal (its profile ranks none) isn't held to it"))
        compare(choice.Accessible.name, "Minimum time between sweeps")
        compare(choice.count, 5)
        compare(choice.textAt(0), "Off")
        compare(choice.textAt(2), "250 ms")
        compare(Bridge.minSweepMs, 0)
        compare(choice.currentIndex, 0)
        Bridge.minSweepMs = 500
        compare(choice.currentIndex, 3, "it shows the bridge's before any pick")
        Bridge.minSweepMs = 0
        choice.currentIndex = 2
        choice.activated(2)
        compare(Bridge.minSweepMs, 250)
        Bridge.minSweepMs = 1000
        compare(choice.currentIndex, 4, "it follows the bridge")
        choice.activated(0)
        compare(Bridge.minSweepMs, 0)
        compare(choice.currentIndex, 0)
    }

    // While a vendor's open blocks, nothing may rewrite the URI field or
    // start another connect: the device rows lock like every other control
    function test_controlsLockWhileAConnectIsInFlight() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        Harness.armFakeKnob("ECUIO_FAKE_OPEN_DELAY_MS", "1500")
        const page = createPage()
        page.j2534Devices = [{ vendorName: "Fake", dllPath: Harness.fakeDllPath,
                               architecture: "x64" }]
        const list = find(page, "deviceList")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the row has a delegate")
        const rowArea = find(list.itemAtIndex(0), "deviceRowArea")
        const uri = find(page, "uriField")
        verify(rowArea.enabled, "idle: the row can fill the field")

        uri.text = "j2534://" + Harness.fakeDllPath
        press(page, "connectButton")
        tryVerify(() => Bridge.connecting, 2000, "the connect is in flight")
        verify(!rowArea.enabled, "a row click can't rewrite the field under the connect")
        verify(!uri.enabled)
        verify(!find(page, "preset_mock").enabled)
        verify(!find(page, "profile_raw").enabled)
        verify(!find(page, "connectButton").enabled)

        tryVerify(() => Bridge.connected, 10000, "the stalled open completes")
        compare(uri.text, "j2534://" + Harness.fakeDllPath, "the field is what was connected")
        verify(!rowArea.enabled, "and locked for the session")
    }
}
