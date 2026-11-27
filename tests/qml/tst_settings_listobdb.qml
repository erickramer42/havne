// File/layer: tests/qml/tst_settings.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.settings (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

SettingsKit {
    id: testCase
    name: "SettingsListObdb"

    // An adapter's URI on every platform: havne-cli's ELM327 emulator on
    // loopback (Harness), since the mock and a replay are never kept. A
    // tree with havne-cli fails when it won't start, never skips
    function emulatorUri() {
        if (!Harness.hasHavneCli())
            skip("needs havne-cli, which this tree doesn't build")
        const uri = Harness.startElm327Emulator()
        verify(uri !== "", Harness.emulatorWhy())
        return uri
    }

    function sameList(a, b, what) {
        compare(JSON.stringify(a), JSON.stringify(b), what)
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

    // ---- A connect from a list reads the vehicle's plugin (ADR-0064 Q5) ----

    // The rule: an address that names no profile takes the picked
    // vehicle's set where it is asked: a J2534 or ELM327 adapter (obd2
    // alone is Default there), or a CAN interface that sends. Any other
    // address passes as it is, and each row has its word
    function test_aListsAddressTakesTheVehiclesSetOnlyWhereItIsAsked() {
        const page = createPage()
        page.j2534Devices = []
        page.serialPorts = [{ name: "COM5", device: "" }]
        page.bluetoothAdapters = []
        page.canInterfaces = [{ name: "vcan0", kind: "vcan", up: true },
                              { name: "can0", kind: "can", up: true, modes: ["one-shot"] },
                              { name: "can1", kind: "can", up: true, modes: ["listen-only"] },
                              { name: "can2", kind: "can", up: false, modes: ["one-shot"] }]
        press(page, "vehicleNew")
        compare(page.connectFromPart.listConnectUri("elm327://COM5"), "elm327://COM5", "obd2 alone is Default")
        compare(page.connectFromPart.listConnectUri("socketcan://vcan0"), "socketcan://vcan0?profile=obd2")
        verify(importFile(page, "plain.json", "Plain"))
        verify(importFile(page, "plus.json", "Plus", "D101", true))
        const plus = "plugin.plus.base"
        compare(page.picksPart.picks, ["obd2", "plugin.plain.base", plus])
        const set = page.picksPart.together.profile

        compare(page.connectFromPart.listConnectUri("elm327://COM5"), "elm327://COM5?profile=" + set)
        compare(page.connectFromPart.listConnectUri("j2534://Vendor X"), "j2534://Vendor X?profile=" + set)
        compare(page.connectFromPart.listConnectUri("elm327://192.168.0.10:35000?poll=0C"),
                "elm327://192.168.0.10:35000?poll=0C&profile=" + set,
                "appended last, the other keys where they were")
        compare(page.connectFromPart.listConnectUri("elm327://COM5?profile=obd2"), "elm327://COM5?profile=obd2",
                "an address that names a profile wins")
        compare(page.connectFromPart.listConnectUri("elm327://COM5?profile=raw&poll=0C"),
                "elm327://COM5?profile=raw&poll=0C")
        compare(page.connectFromPart.listConnectUri("mock://demo"), "mock://demo", "the demo's Default isn't obd2")
        compare(page.connectFromPart.listConnectUri("replay://C:/recordings/one"), "replay://C:/recordings/one")

        compare(page.connectFromPart.listConnectUri("socketcan://vcan0"), "socketcan://vcan0?profile=" + set,
                "a virtual bus sends")
        compare(page.connectFromPart.listConnectUri("socketcan://can0"), "socketcan://can0?profile=" + set,
                "a controller up in one-shot mode sends")
        compare(page.connectFromPart.listConnectUri("socketcan://can1"), "socketcan://can1", "a listener keeps listening")
        compare(page.connectFromPart.listConnectUri("socketcan://can2"), "socketcan://can2", "down")
        compare(page.connectFromPart.listConnectUri("socketcan://can9"), "socketcan://can9", "not listed")

        compare(page.connectFromPart.connectProfileWords["elm327://COM5"], "Vehicle 1")
        compare(page.connectFromPart.connectProfileWords["socketcan://vcan0"], "Vehicle 1")
        compare(page.connectFromPart.connectProfileWords["socketcan://can1"], undefined,
                "no word on a row that isn't composed")
        compare(page.connectFromPart.listProfileWord("elm327://COM5?profile=" + plus), "Plus",
                "a plugin's profile by its name")
        compare(page.connectFromPart.listConnectUri("elm327://COM5?profile=" + plus),
                "elm327://COM5?profile=" + plus, "a pick's profile, named, is read")
        compare(page.connectFromPart.listProfileWord("elm327://COM5?profile=raw"), "")
        compare(page.connectFromPart.listProfileWord("mock://demo"), "")

        // A pick the store lacks refuses the set: nothing is composed
        page.saveVehicles(VehicleBook.setPicks(page.vehicles, ["obd2", "plugin.gone.base", plus]))
        compare(page.picksPart.together.reads, "refused")
        compare(page.connectFromPart.listConnectUri("elm327://COM5"), "elm327://COM5")
        verify(page.connectFromPart.setRefusal("elm327://COM5") !== "")
        compare(page.connectFromPart.listProfileWord("elm327://COM5?profile=plugin.gone.base"), "a plugin not in the store")

        // A plugin's or a signal file's profile the vehicle doesn't pick is
        // dropped, as the field drops it; a built-in profile stays
        page.saveVehicles(VehicleBook.setPicks(page.vehicles, ["obd2", plus]))
        const now = page.picksPart.together.profile
        compare(page.connectFromPart.listConnectUri("elm327://COM5?profile=plugin.plain.base&poll=0C"),
                "elm327://COM5?poll=0C&profile=" + now, "the vehicle's set is read in its place")
        compare(page.connectFromPart.listProfileWord("elm327://COM5?profile=plugin.plain.base"), "Vehicle 1")
        compare(page.connectFromPart.listConnectUri("mock://demo?profile=plugin.plain.base"), "mock://demo")
        compare(page.connectFromPart.listConnectUri("elm327://COM5?profile=user.old"), "elm327://COM5?profile=" + now)
        press(page, "vehicle_none")
        compare(page.connectFromPart.listConnectUri("elm327://COM5"), "elm327://COM5", "no vehicle, no set")
        compare(page.connectFromPart.listConnectUri("elm327://COM5?profile=" + plus), "elm327://COM5",
                "nor a plugin's profile")
        compare(page.connectFromPart.listConnectUri("elm327://COM5?profile=raw"), "elm327://COM5?profile=raw")
        compare(page.connectFromPart.connectProfileWords["elm327://COM5"], undefined)
    }

    // A list's connect puts the composed address in the field, as typed
    // text, before the connect; one that fails is kept nowhere. The
    // address here is a closed TCP port, an adapter's on every platform
    function test_aListConnectFillsTheFieldWithTheProfileItReads() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "D100", true))
        const field = find(page, "uriField")
        field.text = "mock://other"
        const closed = "elm327://127.0.0.1:1"
        const set = page.picksPart.together.profile
        page.connectFromPart.connectFromList(closed)
        compare(field.text, closed + "?profile=" + set, "before the connect answers")
        compare(page.uriPart.profileChoice, set, "the row follows the field")
        verify(find(page, "profile_" + set).checked)
        tryVerify(() => !Bridge.connecting, 10000, "the connect ends")
        verify(!Bridge.connected, "nothing listens there")
        verify(find(page, "errorLabel").text !== "")
        compare(page.connectFromPart.recentUris, [], "a failed connect is no recent address")
        compare(page.pickedVehicle.uri, "")

        // The demo is left as it is
        page.connectFromPart.connectFromList("mock://demo")
        compare(field.text, "mock://demo")
        tryVerify(() => Bridge.connected, 5000, "connects")
        compare(Bridge.currentUri, "mock://demo")
    }

    // A session on the emulator, ended. It serves one host at a time,
    // so this waits until it has seen the host go
    function endSession() {
        const closed = Harness.emulatorClients("closed")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        tryVerify(() => Harness.emulatorClients("closed") > closed, 5000,
                  "the emulator sees the host go")
    }

    // On an adapter (havne-cli's ELM327 emulator, on every platform) the
    // connect reads the vehicle's set, and what is kept is the row's
    // address without it, so each connect adds the set of the vehicle
    // picked then, and another vehicle reads its own
    function test_aListConnectOnAnAdapterReadsTheVehiclesSetAndKeepsTheRowsAddress() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "1E1C", true))
        const uri = emulatorUri()
        const composed = uri + "?profile=" + page.picksPart.together.profile
        const field = find(page, "uriField")
        field.text = ""
        page.connectFromPart.connectFromList(uri)
        compare(field.text, composed)
        tryVerify(() => Bridge.connected, 10000, "connects")
        compare(Bridge.currentUri, composed)
        compare(find(page, "errorLabel").text, "")
        tryVerify(() => Bridge.signalModel.rowCount() > 0, 5000)
        verify(Bridge.profileSignals(Bridge.currentUri).some(r => r.ref === "key:a"),
               "the session reads the plugin's signal with obd2's")
        tryVerify(() => page.pickedVehicle.uri === uri, 10000, "the vehicle keeps the row's address")
        endSession()
        sameList(page.connectFromPart.connectUris, [uri])
        compare(page.connectFromPart.connectProfileWords[uri], "Vehicle 1", "the row says what it reads")

        // Another vehicle, which picks obd2 alone, reads the row as it is
        press(page, "vehicleNew")
        compare(page.picksPart.picks, ["obd2"])
        compare(page.connectFromPart.connectProfileWords[uri], undefined)
        page.connectFromPart.connectFromList(uri)
        compare(field.text, uri)
        tryVerify(() => Bridge.connected, 10000, "connects")
        compare(Bridge.currentUri, uri, "not the first vehicle's plugin")
        tryVerify(() => page.pickedVehicle.uri === uri, 10000)
        endSession()

        // The first again: the field reads its address as its row connects
        press(page, "vehicle_" + page.vehicles.list[0].id)
        compare(field.text, composed)
    }

    // The recent list keeps the row's address too; an address typed
    // after it is kept as typed
    function test_aListConnectKeepsTheRowsAddressInTheRecentList() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "1E1C", true))
        const uri = emulatorUri()
        find(page, "uriField").text = ""
        page.connectFromPart.connectFromList(uri)
        tryVerify(() => Bridge.connected, 10000, "connects")
        compare(Bridge.currentUri, uri + "?profile=" + page.picksPart.together.profile)
        sameList(page.connectFromPart.recentUris, [uri])
        endSession()
        const raw = uri + "?profile=raw"
        find(page, "uriField").text = raw
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "connects")
        sameList(page.connectFromPart.recentUris, [raw, uri])
        endSession()
    }

    // An address typed with a plugin's profile is kept as typed. A list
    // reads it for a vehicle that picks the plugin; for another it drops
    // the profile, so the plugin is never remembered as that vehicle's
    function test_aRowNamingAPluginReadsItOnlyForAVehicleThatPicksIt() {
        const page = createPage()
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "1E1C", true))
        const uri = emulatorUri()
        const composed = uri + "?profile=plugin.plus.base"
        const field = find(page, "uriField")
        field.text = composed
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "connects")
        tryVerify(() => page.pickedVehicle.uri === composed, 10000, "kept as typed")
        sameList(page.connectFromPart.recentUris, [composed])
        endSession()
        compare(page.connectFromPart.listConnectUri(composed), composed, "its vehicle picks the plugin")
        compare(page.connectFromPart.connectProfileWords[composed], "Plus")

        press(page, "vehicleNew")
        compare(page.connectFromPart.listConnectUri(composed), uri)
        compare(page.connectFromPart.connectProfileWords[composed], undefined, "no word: it reads Default")
        page.connectFromPart.connectFromList(composed)
        compare(field.text, uri)
        tryVerify(() => Bridge.connected, 10000, "connects")
        compare(Bridge.currentUri, uri, "not the first vehicle's plugin")
        tryVerify(() => page.pickedVehicle.uri === uri, 10000, "nor is it remembered as this vehicle's")
        sameList(page.connectFromPart.recentUris, [uri, composed])
        compare(page.vehicles.list[0].uri, composed, "the first vehicle's stays")
        endSession()
    }

    // A kept row follows the vehicle's picks: with one undone the set of
    // now is read, at a start as at a connect, and with the last plugin
    // removed the row connects as it is
    function test_aKeptRowFollowsTheVehiclesPicks() {
        let page = createPage()
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "1E1C", true))
        verify(importFile(page, "more.json", "More", "D102", true))
        const uri = emulatorUri()
        const both = uri + "?profile=" + page.picksPart.together.profile
        find(page, "uriField").text = ""
        page.connectFromPart.connectFromList(uri)
        tryVerify(() => Bridge.connected, 10000, "connects")
        compare(Bridge.currentUri, both)
        tryVerify(() => page.pickedVehicle.uri === uri, 10000)
        endSession()

        page.open("signals")
        pressRow(page, "plugin.plus.base")
        compare(page.picksPart.picks, ["obd2", "plugin.more.base"])
        const more = uri + "?profile=" + page.picksPart.together.profile
        verify(more !== both)
        sameList(page.connectFromPart.connectUris, [uri])
        compare(page.connectFromPart.connectProfileWords[uri], "Vehicle 1")
        compare(page.connectFromPart.listConnectUri(uri + "?profile=plugin.plus.base"), more,
                "a row that names the pick undone reads the set of now")
        page.connectFromPart.connectFromList(uri)
        tryVerify(() => Bridge.connected, 10000, "connects")
        compare(Bridge.currentUri, more)
        endSession()

        // As at a quit and a start
        page.destroy()
        wait(10)
        stored.sync()
        compare(stored.value("uri"), uri, "the last address is the row's")
        page = createPage()
        compare(find(page, "uriField").text, more, "and the field reads the set of now")

        verify(page.rowActionsPart.removePlugin(page.picksPart.pluginList.find(p => p.id === "more").sha256))
        compare(page.picksPart.picks, ["obd2"])
        compare(page.connectFromPart.listConnectUri(uri), uri)
        compare(page.connectFromPart.connectProfileWords[uri], undefined)
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

    // One Import, an OBDb set: the report shows what converted, what
    // differs and what's left out, converted again when the year is
    // changed, and nothing is kept until Keep; kept, it is a plugin of the
    // user's own, picked for the vehicle
    function test_anObdbSetShowsItsReportBeforeItIsKept() {
        const page = createPage()
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        page.open("signals")
        verify(!find(page, "obdbReport").visible)
        verify(obdbRead(page, ""))
        verify(find(page, "obdbReport").visible)
        compare(find(page, "obdbSummary").text,
                "Daily driver OBDb from OBDb: 6 signals converted, 5 fields differ, 6 left out, "
                + "no model year, so the commands OBDb gives for some years only are left out")
        find(page, "obdbYearField").text = "2021"
        compare(find(page, "obdbSummary").text,
                "Daily driver OBDb from OBDb: 7 signals converted, 6 fields differ, "
                + "5 left out, model year 2021")
        compare(find(page, "obdbChanged").count, 6)
        compare(find(page, "obdbSkipped").count, 5)
        compare(find(page, "obdbFileNotes").count, 2)
        compare(find(page, "obdbKeep").text, "Keep")
        compare(page.picksPart.pluginList.length, 0, "nothing kept yet")
        compare(Harness.storedPlugins(), [])

        press(page, "obdbKeep")
        verify(!find(page, "importPanel").visible)
        compare(page.picksPart.picks, ["obd2", "plugin.daily-driver-obdb.my2021"], "a typed year names the variant")
        compare(find(page, "signalNote").text, "Daily driver OBDb 1.0.0 added: picked for Daily driver")
        const row = rowOf(page, "plugin.daily-driver-obdb.my2021")
        compare(find(row, "rowCaption").text, "For this car:")
        compare(find(row, "rowTitle").text, "Daily driver OBDb 1.0.0")
        compare(find(row, "rowReads").text, "obd2 + 7 signals · yours")
        compare(Harness.storedPlugins().length, 1)
        verify(page.picksPart.pluginList[0].obdb)
    }

    function test_anObdbSetDiscardedIsNotKept() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(obdbRead(page, ""))
        verify(find(page, "obdbReport").visible)
        press(page, "importDiscard")
        verify(!find(page, "importPanel").visible)
        compare(page.picksPart.pluginList.length, 0)
        verify(!page.importPart.keepObdb(), "nothing to keep")
        compare(page.picksPart.pluginList.length, 0)

        // A report is the vehicle's it was read for
        verify(obdbRead(page, "2021"))
        press(page, "vehicleNew")
        compare(page.importPart.pendingImport, null, "another vehicle picked: the file no longer waits")
        verify(!page.importPart.keepObdb())
        compare(page.picksPart.pluginList.length, 0)
    }

    // The set read again takes the place of the vehicle's earlier import,
    // and says so before Keep; the pick follows it. A plugin the user
    // named so is never replaced
    function test_anObdbSetReadAgainReplacesTheEarlierImport() {
        const page = createPage()
        find(page, "vehicleLabelField").text = "Daily driver"
        press(page, "vehicleNew")
        page.open("signals")
        verify(obdbRead(page, "2019"))
        press(page, "obdbKeep")
        const earlier = page.picksPart.pluginList[0].sha256
        compare(page.picksPart.picks, ["obd2", "plugin.daily-driver-obdb.my2019"])

        verify(obdbRead(page, "2021"))
        verify(find(page, "obdbSummary").text.endsWith(
                   "; replaces the vehicle's earlier OBDb import"),
               find(page, "obdbSummary").text)
        compare(find(page, "obdbKeep").text, "Keep, replacing the earlier")
        // The report at a phone's width, as test_everyItemFitsAPhonesWidth
        const width = page.width
        page.width = 320
        const column = find(page, "settingsSignalsPageColumn")
        tryCompare(column, "width", 320 - 2 * Theme.pageMargin)
        waitForPolish(column)
        compare(pastTheEdge(column, column, []).join("; "), "",
                "nothing past the column's right edge, " + column.width)
        page.width = width
        press(page, "obdbKeep")
        compare(page.picksPart.pluginList.length, 1)
        verify(page.picksPart.pluginList[0].sha256 !== earlier)
        compare(page.picksPart.picks, ["obd2", "plugin.daily-driver-obdb.my2021"], "the pick follows the import")
        compare(find(page, "signalNote").text,
                "Daily driver OBDb replaced the vehicle's earlier OBDb import")
        compare(Harness.storedPlugins().length, 1, "the earlier copy went")

        // The same year again is the same plugin
        verify(obdbRead(page, "2021"))
        press(page, "obdbKeep")
        compare(page.picksPart.pluginList.length, 1)
        compare(find(page, "signalNote").text, "Daily driver OBDb is here already")

        // A plugin the user named so is the user's: the import takes an
        // id of its own beside it
        find(page, "vehicleLabelField").text = "Weekend car"
        press(page, "vehicleNew")
        verify(importFile(page, "mine.json", "Weekend car OBDb"))
        verify(obdbRead(page, "2021"))
        verify(!find(page, "obdbSummary").text.includes("replaces"))
        press(page, "obdbKeep")
        compare(page.picksPart.pluginList.filter(p => p.name === "Weekend car OBDb").map(p => p.id),
                ["weekend-car-obdb", "weekend-car-obdb-2"])
    }

    // The year: typed, else a typed VIN's, else the connected car's once
    // its VIN names the picked vehicle, kept once the session ends. A
    // VIN's year fills the field and names no variant; one that isn't a
    // year is refused, and the set waits for another
    function test_theObdbYearComesFromAVin() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        compare(page.importPart.obdbVinYear(), 0)
        find(page, "vinLookupField").text = "1FAFAKE10M0000001"
        compare(page.importPart.obdbVinYear(), 2021)
        find(page, "vinLookupField").text = "not one"
        compare(page.importPart.obdbVinYear(), 0)
        find(page, "vinLookupField").text = ""

        find(page, "uriField").text = "mock://demo"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connected")
        verify(!find(page, "signalImport").enabled, "nothing changes the store while it runs")
        Bridge.identifyForTesting("1FAFAKE13Y0000001")
        compare(page.importPart.obdbVinYear(), 0, "not the picked vehicle's VIN")
        const salt = page.vehicles.salt !== "" ? page.vehicles.salt : Bridge.newSalt()
        const doc = VehicleBook.copy(page.vehicles)
        doc.salt = salt
        page.saveVehicles(VehicleBook.change(doc, v => v.vinKey = Bridge.vinKey(salt)))
        Bridge.identifyForTesting("1FAFAKE13Y0000001")
        compare(page.importPart.obdbVinYear(), 2000)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
        compare(Bridge.vinModelYear(""), 0, "the VIN went with the session")
        compare(page.importPart.obdbVinYear(), 2000, "its year stays")
        verify(find(page, "signalImport").enabled)

        verify(obdbRead(page, "19x") === false)
        compare(find(page, "signalProblem").text, "A model year is 1980 to 2039, or empty for none")
        verify(!find(page, "obdbReport").visible)
        verify(find(page, "importPanel").visible, "the set waits for a year")
        find(page, "obdbYearField").text = "1979"
        compare(find(page, "signalProblem").text, "A model year is 1980 to 2039, or empty for none")
        find(page, "obdbYearField").text = "2021"
        compare(find(page, "signalProblem").text, "")
        verify(find(page, "obdbReport").visible)
        compare(page.importPart.obdbReport.year, 2021)

        verify(obdbRead(page, ""))
        compare(find(page, "obdbYearField").text, "2000", "the field takes the car's year")
        verify(page.importPart.obdbYearFromVin)
        compare(page.importPart.obdbReport.year, 2000)
        press(page, "obdbKeep")
        compare(page.picksPart.picks, ["obd2", "plugin.vehicle-1-obdb.base"], "a VIN's year names no variant")
    }

    // A model year typed digit by digit refuses nothing until its fourth
    // digit or the field's end, and meanwhile no report waits to be kept
    function test_aModelYearTypedIsJudgedWhole() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(obdbRead(page, "2019"))
        const field = find(page, "obdbYearField")
        field.text = ""
        for (const t of ["2", "20", "202"]) {
            field.text = t
            compare(find(page, "signalProblem").text, "", t + " is a year on its way")
            verify(!find(page, "obdbReport").visible, "no report waits beside " + t)
            verify(!find(page, "obdbKeep").visible, "nor Keep")
        }
        field.editingFinished()
        compare(find(page, "signalProblem").text, "A model year is 1980 to 2039, or empty for none")
        field.text = "2021"
        compare(find(page, "signalProblem").text, "")
        compare(page.importPart.obdbReport.year, 2021)
    }

    // The year in the field is the waiting set's: it goes with a discard
    // and with the vehicle, so one vehicle's year never converts another's
    function test_theObdbYearGoesWithItsSet() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        find(page, "vinLookupField").text = "1FAFAKE10M0000001"
        verify(obdbRead(page, ""))
        compare(find(page, "obdbYearField").text, "2021")
        verify(page.importPart.obdbYearFromVin)
        page.importPart.discardImport()
        compare(find(page, "obdbYearField").text, "")
        verify(!page.importPart.obdbYearFromVin)
        find(page, "vinLookupField").text = ""

        verify(obdbRead(page, "2019"))
        compare(page.importPart.obdbReport.year, 2019)
        press(page, "vehicleNew")
        compare(find(page, "obdbYearField").text, "", "the last vehicle's year isn't this one's")
        verify(obdbRead(page, ""))
        compare(page.importPart.obdbReport.year, 0)
    }

    // A long report shows a list's first rows and counts the rest
    function test_aLongObdbReportCountsWhatItDoesNotShow() {
        const page = createPage()
        press(page, "vehicleNew")
        const commands = [{ hdr: "7E0", rax: "7E8", cmd: { "22": "D100" }, freq: 1,
                            signals: [{ id: "KEPT", name: "Kept", fmt: { len: 8, unit: "scalar" } }] }]
        for (let i = 0; i < 120; ++i)
            commands.push({ hdr: "7E0", rax: "7E8", cmd: { "22": (0x2000 + i).toString(16) },
                            freq: 1, signals: [{ id: "HIDDEN_" + i, hidden: true, name: "H",
                                                 fmt: { len: 8, unit: "scalar" } }] })
        const url = Harness.writeScratchFile("long.json", JSON.stringify({ commands: commands }))
        find(page, "obdbYearField").text = ""
        verify(page.importPart.importFile(url))
        compare(page.importPart.obdbReport.skipped.length, 120)
        compare(find(page, "obdbSkipped").count, page.importPart.obdbRowsShown)
        compare(find(page, "obdbSkippedMore").text, "and " + (120 - page.importPart.obdbRowsShown) + " more")
        page.open("signals")
        verify(find(page, "obdbSkippedMore").visible)
        verify(!find(page, "obdbChangedMore").visible)
    }

    // With no vehicle picked an OBDb set is imported all the same: named
    // "OBDb set", imported for none, and not in use until a vehicle picks it
    function test_anObdbSetWithNoVehicleIsImportedForNone() {
        const page = createPage()
        page.open("signals")
        verify(obdbRead(page, ""))
        verify(find(page, "obdbSummary").text.startsWith("OBDb set from OBDb: "),
               find(page, "obdbSummary").text)
        press(page, "obdbKeep")
        compare(find(page, "signalNote").text, "OBDb set 1.0.0 added")
        compare(page.picksPart.pluginList[0].importedFor, "")
        verify(find(rowOf(page, "plugin.obdb-set.base"), "rowUnused").visible)
    }
}
