// File/layer: tests/qml/tst_settings.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.settings (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

SettingsKit {
    id: testCase
    name: "SettingsDevices"

    // A URI the page didn't compose: typed, pasted or set from outside
    // (UI Automation's SetValue is a plain set, as this is)
    function type(uri, text) {
        uri.text = text
    }

    // The vehicle, signals and recording pages say why they are dim, and
    // what a vehicle is for
    function test_thePagesSayWhyTheyAreLockedAndWhatAVehicleIsFor() {
        const page = createPage()
        page.open("vehicle")
        verify(find(page, "vehicleNote").visible, "what a vehicle is for")
        verify(!find(page, "vehicleLockedNote").visible, "idle: nothing locked")
        page.open("recording")
        verify(!find(page, "recordLockedNote").visible)
        verify(find(page, "recordNote").visible, "what Record sessions keeps, always")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => Bridge.connected, 5000)
        verify(find(page, "recordLockedNote").visible, "connected: Recording's line")
        compare(find(page, "recordLockedNote").text, "Disconnect to turn recording on or off.")
        page.open("vehicle")
        verify(find(page, "vehicleLockedNote").visible, "connected: Vehicle's line")
        Bridge.disconnectSession()
        tryVerify(() => !find(page, "vehicleLockedNote").visible, 5000, "the line goes with the session")
    }

    // A typed URI sets the row, the hint and the DID field, and is left as
    // typed; the next click composes from those choices, so typed DIDs
    // survive a trip through another profile
    function test_aTypedUriSetsTheRow() {
        const page = createPage()
        const uri = find(page, "uriField")
        const did = find(page, "didField")
        const didRow = find(page, "didRow")
        const hint = find(page, "profileHint")

        type(uri, "mock://demo?profile=uds-generic&did=1E1C:u16")
        compare(uri.text, "mock://demo?profile=uds-generic&did=1E1C:u16", "left as typed")
        compare(page.uriPart.profileChoice, "uds-generic")
        verify(find(page, "profile_uds-generic").checked)
        verify(!find(page, "profile_default").checked)
        verify(didRow.visible)
        compare(did.text, "1E1C:u16")
        verify(hint.text.indexOf("Service 0x22") >= 0, "the hint is uds-generic's: " + hint.text)

        press(page, "profile_default")
        compare(uri.text, "mock://demo")
        press(page, "profile_uds-generic")
        compare(uri.text, "mock://demo?profile=uds-generic&did=1E1C:u16", "the DIDs are back")

        type(uri, "j2534://Vendor?profile=raw&filter=0x7E9")
        compare(page.uriPart.profileChoice, "raw")
        verify(find(page, "profile_raw").checked)
        verify(!didRow.visible)
        compare(uri.text, "j2534://Vendor?profile=raw&filter=0x7E9", "left as typed")
        press(page, "profile_default")
        compare(uri.text, "j2534://Vendor?filter=0x7E9", "a typed filter stays")

        // A profile the row doesn't offer: no button lit, the hint says so
        type(uri, "j2534://Vendor?profile=obd2")
        compare(page.uriPart.profileChoice, "obd2")
        verify(!find(page, "profile_default").checked)
        verify(!find(page, "profile_raw").checked)
        verify(hint.text.indexOf("obd2") >= 0, "the hint names it: " + hint.text)
        type(uri, "j2534://Vendor")
        compare(page.uriPart.profileChoice, "default")
        verify(find(page, "profile_default").checked)
    }

    // A DID edit changes did= only; keys typed by hand stay where they were
    function test_didEditsKeepTheOtherKeys() {
        const page = createPage()
        const uri = find(page, "uriField")
        const did = find(page, "didField")
        type(uri, "j2534://V?identify=0&profile=uds-generic&did=1E1C")
        compare(did.text, "1E1C")
        did.text = "1E1C, F190:text"
        compare(uri.text, "j2534://V?identify=0&profile=uds-generic&did=1E1C,F190:text")
        did.text = ""
        compare(uri.text, "j2534://V?identify=0&profile=uds-generic", "no DIDs, no did= key")
        press(page, "preset_mock")
        compare(uri.text, "mock://demo?profile=uds-generic",
                "a new scheme keeps the keys it takes: the mock takes no identify=")
    }

    // diagnose= (ADR-0037) is a key the mock takes too: it survives the
    // move from an adapter to the simulated car
    function test_diagnoseOffCrossesToTheMock() {
        const page = createPage()
        const uri = find(page, "uriField")
        type(uri, "j2534://V?identify=0&diagnose=0")
        press(page, "preset_mock")
        compare(uri.text, "mock://demo?diagnose=0")
    }

    // The vehicle run sheet's URIs, typed, pass through untouched
    function test_runSheetUrisPassThroughUntouched_data() {
        return [
            { tag: "bare", uri: "j2534://OBDX Pro - VX", profile: "default" },
            { tag: "raw", uri: "j2534://OBDX Pro - VX?profile=raw&filter=0x7E8", profile: "raw" },
            { tag: "poll", uri: "j2534://OBDX Pro - VX?poll=0x0C", profile: "default" },
            { tag: "bitmap", uri: "j2534://OBDX Pro - VX?profile=raw&filter=0x7E8&poll=0x00",
              profile: "raw" },
        ]
    }

    function test_runSheetUrisPassThroughUntouched(data) {
        const page = createPage()
        const uri = find(page, "uriField")
        type(uri, data.uri)
        compare(uri.text, data.uri, "left as typed")
        compare(page.uriPart.profileChoice, data.profile)
        // The same row clicked again changes nothing
        press(page, "profile_" + data.profile)
        compare(uri.text, data.uri, "the row's own click keeps it")
    }

    // The page adds and takes no key of any scheme's: raw composes no
    // filter on any base (a replay takes none), and a typed filter stays
    // through every choice; core judges the keys a scheme takes
    function test_noSchemesKeysAddedOrTaken() {
        const page = createPage()
        const uri = find(page, "uriField")
        type(uri, "replay://C:/recordings/20261025-164746")
        press(page, "profile_raw")
        compare(uri.text, "replay://C:/recordings/20261025-164746?profile=raw",
                "a replay takes no filter")
        type(uri, "j2534://OBDX Pro - VX?profile=raw&filter=0x7E8&poll=0x00")
        press(page, "profile_default")
        compare(uri.text, "j2534://OBDX Pro - VX?filter=0x7E8&poll=0x00")
        press(page, "profile_raw")
        compare(uri.text, "j2534://OBDX Pro - VX?filter=0x7E8&poll=0x00&profile=raw")
    }

    // Query metacharacters can't be typed into the DID field, so a DID
    // list can never split the URI into other keys; core judges the rest
    function test_didFieldRefusesQueryCharacters() {
        const page = createPage()
        const did = find(page, "didField")
        press(page, "preset_mock")
        press(page, "profile_uds-generic")
        did.forceActiveFocus()
        tryVerify(() => did.activeFocus, 1000, "the DID field has focus")
        keyClick("1"); keyClick("E"); keyClick("1"); keyClick("C")
        compare(did.text, "1E1C")
        keyClick("&"); keyClick("="); keyClick("?"); keyClick("#")
        compare(did.text, "1E1C", "the query characters are refused")
        keyClick(":"); keyClick("u"); keyClick("1"); keyClick("6")
        keyClick(","); keyClick(" "); keyClick("F"); keyClick("1"); keyClick("9"); keyClick("0")
        keyClick("-"); keyClick("F"); keyClick("1"); keyClick("9"); keyClick("1")
        compare(did.text, "1E1C:u16, F190-F191", "the grammar's characters all pass")
        compare(find(page, "uriField").text, "mock://demo?profile=uds-generic&did=1E1C:u16,F190-F191")
    }

    // The error line shows core's words for a DID list it refuses, and
    // for the profile chosen with no DIDs at all
    function test_coresRefusalShowsInTheErrorLine() {
        const page = createPage()
        const error = find(page, "errorLabel")
        press(page, "preset_mock")
        press(page, "profile_uds-generic")
        verify(!error.visible)

        press(page, "connectButton")
        tryVerify(() => error.visible, 5000, "a refusal arrives")
        verify(error.text.indexOf("needs ?did= to say which DIDs to read") >= 0,
               "core's words: " + error.text)
        verify(!Bridge.connected)

        find(page, "didField").text = "1E1C:u12"
        press(page, "connectButton")
        tryVerify(() => error.visible && error.text.indexOf("u12") >= 0, 5000,
                  "the bad type is named: " + error.text)
        verify(error.text.indexOf("?did= needs") >= 0, "core's words: " + error.text)
        verify(!Bridge.connected)
    }

    function test_composedUdsGenericUriConnectsThroughTheMock() {
        const page = createPage()
        const error = find(page, "errorLabel")
        press(page, "preset_mock")
        press(page, "profile_uds-generic")
        find(page, "didField").text = "1E1C:u16"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "the composed URI connects")
        compare(Bridge.currentUri, "mock://demo?profile=uds-generic&did=1E1C:u16")
        verify(!error.visible, "no error line: " + error.text)
        compare(Bridge.signalModel.rowCount(), 1, "one def per DID")
        verify(!find(page, "didField").enabled, "the choices are fixed while connected")
        verify(!find(page, "profile_raw").enabled)
        verify(!find(page, "preset_mock").enabled)
        compare(find(page, "connectButton").text, "Disconnect")
    }

    // A restart gets back the Record switch. Recording is switched on only
    // after any connect, so no session here records
    function test_theRecordSwitchComesBack() {
        let page = createPage()
        Bridge.recording = true
        verify(find(page, "recordBox").checked)

        // The page's store writes when the page goes, as at a quit
        page.destroy()
        wait(10)
        Bridge.recording = false   // a fresh bridge starts with it off

        page = createPage()
        verify(Bridge.recording, "the Record switch comes back")
        verify(find(page, "recordBox").checked)
    }

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

    // Connects the field's URI and disconnects. The emulator serves one
    // host at a time, so this waits until it has seen the host go
    function connectAndLeave(page, uri) {
        const closed = Harness.emulatorClients("closed")
        find(page, "uriField").text = uri
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, uri + " connects")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        tryVerify(() => Harness.emulatorClients("closed") > closed, 5000,
                  "the emulator sees the host go")
    }

    // A restart gets back the last URI that connected, not a refused one
    function test_theLastConnectedUriComesBack() {
        const emu = emulatorUri()
        let page = createPage()
        const error = find(page, "errorLabel")
        connectAndLeave(page, emu + "?profile=raw")

        find(page, "uriField").text = emu + "?profile=uds-generic"
        press(page, "connectButton")
        tryVerify(() => error.visible, 5000, "core refuses it: no ?did=")

        // The page's store writes when the page goes, as at a quit
        page.destroy()
        wait(10)

        page = createPage()
        compare(find(page, "uriField").text, emu + "?profile=raw",
                "the URI that connected, not the refused one")
        verify(find(page, "profile_raw").checked, "read back into the row")
    }

    function sameList(a, b, what) {
        compare(JSON.stringify(a), JSON.stringify(b), what)
    }

    // The recent addresses: newest first, a duplicate moved to the top, at
    // most five
    function test_recentUrisAreNewestFirstAndCapped() {
        const page = createPage()
        sameList(page.connectFromPart.withRecentUri([], "a"), ["a"], "the first")
        sameList(page.connectFromPart.withRecentUri(["a", "b"], "c"), ["c", "a", "b"], "newest first")
        sameList(page.connectFromPart.withRecentUri(["a", "b", "c"], "b"), ["b", "a", "c"], "a duplicate moves up")
        sameList(page.connectFromPart.withRecentUri(["a", "b"], "a"), ["a", "b"], "the top stays")
        sameList(page.connectFromPart.withRecentUri(["a", "b", "c", "d", "e"], "f"), ["f", "a", "b", "c", "d"],
                 "the oldest goes past five")
        sameList(page.connectFromPart.withRecentUri(["a", "b", "c", "d", "e"], "e"), ["e", "a", "b", "c", "d"],
                 "a duplicate doesn't cost a row")
    }

    // The stored list as it reads back: strings only, damage reads as none,
    // and with none the last URI seeds it
    function test_recentUrisReadBackFromTheStoreOrTheLastUri() {
        const page = createPage()
        sameList(page.connectFromPart.parseRecentUris('["a","b"]', "z"), ["a", "b"], "the stored list")
        sameList(page.connectFromPart.parseRecentUris('[]', "z"), ["z"], "seeded from the last URI")
        sameList(page.connectFromPart.parseRecentUris('[]', ""), [], "nothing at all")
        sameList(page.connectFromPart.parseRecentUris('not json', "z"), ["z"], "damage reads as none")
        sameList(page.connectFromPart.parseRecentUris('{"a":1}', ""), [], "not a list")
        sameList(page.connectFromPart.parseRecentUris('["a",3,"",null]', ""), ["a"], "strings only")
        sameList(page.connectFromPart.parseRecentUris('["a","b","c","d","e","f"]', ""), ["a", "b", "c", "d", "e"],
                 "at most five")
    }

    function test_aFirstRunWithAnOlderUriSeedsTheList() {
        stored.setValue("uri", "elm327://COM1?profile=obd2")
        stored.sync()
        const page = createPage()
        sameList(page.connectFromPart.recentUris, ["elm327://COM1?profile=obd2"], "what session/uri holds")
    }

    // Only a connect that succeeded is written, the newest first, and the
    // list survives a restart
    function test_onlyASuccessfulConnectWritesTheRecentUris() {
        const emu = emulatorUri()
        const raw = emu + "?profile=raw"
        let page = createPage()
        sameList(page.connectFromPart.recentUris, [], "nothing yet")
        for (const uri of [emu, raw, emu])
            connectAndLeave(page, uri)
        sameList(page.connectFromPart.recentUris, [emu, raw], "newest first, once")

        find(page, "uriField").text = emu + "?profile=uds-generic"
        press(page, "connectButton")
        tryVerify(() => find(page, "errorLabel").visible, 5000, "core refuses it: no ?did=")
        sameList(page.connectFromPart.recentUris, [emu, raw], "a refusal writes nothing")

        // The page's store writes when the page goes, as at a quit
        page.destroy()
        wait(10)
        page = createPage()
        sameList(page.connectFromPart.recentUris, [emu, raw], "kept between runs")
    }

    // A replay or the mock is never kept (Recordings is a replay's way
    // back), and one an earlier version stored is dropped as it's read
    function test_aReplayOrTheMockIsNeverKept() {
        const kept = "elm327://COM1?profile=obd2"
        const replay = "replay://" + Harness.replayFixtures + "/2026-10-25-engine-start?speed=max"
        stored.setValue("uri", replay)
        stored.setValue("recentUris", JSON.stringify([replay, kept, " MOCK://demo"]))
        stored.sync()
        let page = createPage()
        compare(find(page, "uriField").text, "", "the stored replay isn't the field's")
        sameList(page.connectFromPart.recentUris, [kept], "nor in the list")
        sameList(page.connectFromPart.parseRecentUris("[]", "mock://demo"), [], "the mock seeds nothing")
        verify(page.connectFromPart.keptUri("socketcan://can0") && page.connectFromPart.keptUri("j2534://Fake"), "an adapter is kept")

        const field = find(page, "uriField")
        for (const uri of ["mock://demo", replay]) {
            field.text = uri
            press(page, "connectButton")
            tryVerify(() => Bridge.connected, 10000, uri + " connects")
            Bridge.disconnectSession()
            tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        }
        sameList(page.connectFromPart.recentUris, [kept], "a connect to either writes nothing")

        page.destroy()
        wait(10)
        stored.sync()
        compare(stored.value("uri"), "", "the stored replay is gone")
        compare(stored.value("recentUris"), JSON.stringify([kept]), "and the list without either")
    }

    // The bridge's URI can differ from the request: a connect from this
    // page leaves its field as it was typed
    function test_aConnectLeavesTheFieldAsTyped() {
        const page = createPage()
        const field = find(page, "uriField")
        field.text = "  mock://demo  "
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "connects")
        compare(field.text, "  mock://demo  ")
    }

    // Dash's list: the address connects through the field, like one typed
    function test_aConnectFromTheListFillsTheFieldFirst() {
        const page = createPage()
        const field = find(page, "uriField")
        field.text = "mock://other"
        page.connectFromPart.connectFromList("mock://demo")
        compare(field.text, "mock://demo", "before the connect answers")
        verify(Bridge.connecting)
        tryVerify(() => Bridge.connected, 5000, "connects")
        compare(Bridge.currentUri, "mock://demo")
    }

    function versionsOf(page, index) {
        const list = find(page, "deviceList")
        tryVerify(() => list.itemAtIndex(index) !== null, 2000, "the row has a delegate")
        const label = find(list.itemAtIndex(index), "deviceVersions")
        return label.visible ? label.text : ""
    }

    function listDevices(page, vendorName) {
        page.j2534Devices = [{ vendorName: vendorName, dllPath: "C:/drivers/fake.dll",
                               architecture: "x64" }]
        page.serialPorts = [{ name: "COM5", device: "" }]
    }

    // A connect keeps its adapter's versions for that row; a port shows
    // whatever answered there last, and nothing comes from a session with
    // no adapter
    function test_aDeviceShowsTheVersionsLastSeenThere() {
        let page = createPage()
        listDevices(page, "Fake")
        compare(versionsOf(page, 0), "", "nothing seen yet")
        compare(versionsOf(page, 1), "")

        page.rememberVersions("elm327://COM5?profile=raw", "ELM327 v1.4b, STN2255 v5.12.2")
        page.rememberVersions("mock://demo", "")
        compare(versionsOf(page, 1), "last seen: ELM327 v1.4b, STN2255 v5.12.2",
                "keyed by the URI's base, its query left out")
        page.rememberVersions("elm327://COM5", "ELM327 V2.1")
        compare(versionsOf(page, 1), "last seen: ELM327 V2.1", "another adapter on the port")
        compare(versionsOf(page, 0), "")

        page.destroy()
        wait(10)
        page = createPage()
        listDevices(page, "Fake")
        compare(versionsOf(page, 1), "last seen: ELM327 V2.1", "kept across a restart")
    }

    function test_aDamagedVersionStoreShowsNone() {
        stored.setValue("adapterVersions", "not json")
        stored.sync()
        const page = createPage()
        listDevices(page, "Fake")
        compare(versionsOf(page, 1), "")
    }

    function test_aConnectKeepsTheAdaptersVersions() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const page = createPage()
        listDevices(page, Harness.fakeDllPath)   // a row whose base is the URI's
        find(page, "uriField").text = "j2534://" + Harness.fakeDllPath + "?profile=raw"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 10000, "the fake connects")
        compare(versionsOf(page, 0),
                "last seen: firmware fake-fw 1.0, DLL fake_j2534 0.3, API 04.04")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        compare(versionsOf(page, 0),
                "last seen: firmware fake-fw 1.0, DLL fake_j2534 0.3, API 04.04",
                "still shown with no session")
    }

    function labels(page) {
        return page.vehicles.list.map(v => v.label)
    }

    // ADR-0043: a typed VIN is decoded on the device, by the bridge's one
    // summary, and kept nowhere (made up around FAKE, its check digit by
    // 49 CFR 565.15)
    function test_aTypedVinIsDecodedOnTheDevice() {
        const page = createPage()
        const field = find(page, "vinLookupField")
        const result = find(page, "vinLookupResult")
        verify(!result.visible, "nothing typed, nothing said")
        verify(find(page, "vinLookupHint").text.indexOf("(vPICList_lite_") > 0,
               "the hint names the vPIC release")

        field.text = "1fa fake10m0000001"
        page.open("vehicle")
        verify(result.visible)
        compare(result.text, "Model year 2021; Ford (manufacturer code 1FA); check digit matches")
        compare(JSON.stringify(page.vehicles).indexOf("FAKE10"), -1, "not in the vehicles")

        field.text = "FAKEJ2534VIN00001"
        compare(result.text, "Not a VIN: 49 CFR 565 takes 17 digits and capital letters, never I, O or Q")
        field.text = ""
        verify(!result.visible)
    }

    // ADR-0027 section 1: every change to the list is made in the section, a
    // label typed or left to its default
    function test_theVehicleSectionMakesNamesAndForgetsVehicles() {
        let page = createPage()
        page.open("vehicle")
        const field = find(page, "vehicleLabelField")
        verify(find(page, "vehicle_none").checked, "no vehicle to start")
        verify(!find(page, "vehicleDuplicate").enabled, "nothing to copy")
        compare(find(page, "vehicleLabelHint").text, "Left empty, New makes “Vehicle 1”")

        press(page, "vehicleNew")
        compare(labels(page), ["Vehicle 1"], "the default, with the field empty")
        compare(Bridge.vehicleLabel, "Vehicle 1", "the recording's label follows the pick")
        verify(find(page, "vehicle_v1").checked)

        field.text = "Daily driver"
        press(page, "vehicleRename")
        compare(labels(page), ["Daily driver"])
        compare(field.text, "", "the field empties for the next label")

        press(page, "vehicleDuplicate")
        compare(labels(page), ["Daily driver", "Daily driver (copy)"])
        compare(page.vehicles.picked, "v2")
        compare(find(page, "vehicleLabelHint").text,
                "Left empty, New makes “Vehicle 1” and Duplicate “Daily driver (copy 2)”")

        field.text = " daily DRIVER "
        verify(!find(page, "vehicleNew").enabled, "a label in use")
        verify(!find(page, "vehicleRename").enabled)
        page.open("vehicle")
        verify(find(page, "vehicleLabelProblem").visible)
        field.text = "x".repeat(41)
        verify(!find(page, "vehicleNew").enabled, "over 40 characters")
        field.text = ""

        press(page, "vehicleForget")
        compare(find(page, "vehicleConfirmText").text, "Forget Daily driver (copy)?")
        press(page, "vehicleCancel")
        compare(labels(page).length, 2, "Cancel keeps it")
        press(page, "vehicleForget")
        // A change rebuilds the buttons, and the Flow places them at its
        // next polish
        const v1 = find(page, "vehicle_v1")
        tryVerify(() => v1.x > 0, 2000, "the button is placed")
        tap(v1)
        compare(page.confirming, "", "picking another backs out")
        compare(labels(page).length, 2)
        press(page, "vehicleForget")
        press(page, "vehicleConfirm")
        compare(labels(page), ["Daily driver (copy)"])
        verify(find(page, "vehicle_none").checked)

        // The store writes as it changes, and a new page (a restart) reads it
        page.destroy()
        wait(10)
        page = createPage()
        compare(labels(page), ["Daily driver (copy)"])

        press(page, "vehicleForgetAll")
        compare(find(page, "vehicleConfirmText").text, "Forget all vehicles?")
        press(page, "vehicleConfirm")
        compare(page.vehicles, VehicleBook.empty())
    }

    // More opens a row of its own in the page, not a menu in the window's
    // overlay: the buttons are the page's, so they follow its zoom
    function test_moreOpensItsActionsInPlace() {
        const page = createPage()
        page.open("vehicle")
        press(page, "vehicleNew")
        const row = find(page, "vehicleMoreRow")
        verify(!row.visible, "closed to start")
        find(page, "vehicleMore").clicked()
        verify(row.visible, "More opens the row")
        for (const n of ["vehicleRename", "vehicleForgetVin", "vehicleForget", "vehicleForgetAll"]) {
            const b = find(page, n)
            verify(b.visible, n + " shows")
            verify(b.mapToItem(page, b.width, 0).x <= page.width + 0.5, n + " is inside the page")
        }
        find(page, "vehicleMore").clicked()
        verify(!row.visible, "More again closes it")
        find(page, "vehicleMore").clicked()
        press(page, "vehicleForget")
        verify(!row.visible, "a choice closes the row")
        compare(page.confirming, "forget", "and asks in place")
        press(page, "vehicleCancel")
        verify(!row.visible, "and Cancel leaves it closed")
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

    // The drive view's settings (ADR-0059 section 9), a phone's: stored at
    // once and read back, each value one segment
    function test_theDriveViewsSettingsRoundTrip_data() {
        return [{ tag: "2 s", ms: 2000 }, { tag: "4 s", ms: 4000 },
                { tag: "8 s", ms: 8000 }, { tag: "Never", ms: 0 }]
    }

    function test_theDriveViewsSettingsRoundTrip(data) {
        const page = createPage()
        page.open("display")
        const display = find(page, "driveHideRow").parent
        display.driveShown = true
        display.orientationShown = true
        waitForRendering(page)
        // From another value: a click on the stored one writes nothing
        mouseClick(find(page, "driveHide_" + (data.ms === 4000 ? 2000 : 4000)))
        const seg = find(page, "driveHide_" + data.ms)
        mouseClick(seg)
        verify(seg.selected, "the chosen segment shows it")
        for (const other of [2000, 4000, 8000, 0]) {
            if (other !== data.ms)
                verify(!find(page, "driveHide_" + other).selected, "only one")
        }
        compare(find(page, "driveSettings").driveHideMs, data.ms)
        for (const mode of ["side", "follow", "upright"]) {
            const button = find(page, "driveOrientation_" + mode)
            mouseClick(button)
            compare(find(page, "driveSettings").driveOrientation, mode)
            verify(button.selected, mode + " shows it")
        }
        // A Settings object writes a change on a short timer: until it has, a read from the store sees the old value
        tryVerify(() => { uiStore.sync(); return uiStore.value("driveHideMs", -1) === data.ms }, 3000, "written where it's read")
        tryVerify(() => { uiStore.sync(); return uiStore.value("driveOrientation", "") === "upright" }, 3000, "and the orientation")
    }

    function test_theDriveViewsSettingsStartAtFourSecondsAndFollow() {
        const page = createPage()
        page.open("display")
        const stored = find(page, "driveSettings")
        compare(stored.driveHideMs, 4000)
        compare(stored.driveOrientation, "follow")
    }

    function test_theDriveSettingsAreAPhonesAndTheLockWhereThereIsOne() {
        const page = createPage()
        page.open("display")
        const row = find(page, "driveHideRow")
        const display = row.parent
        const lock = find(page, "driveOrientationRow")
        compare(display.driveShown, false, "a desktop's")
        compare(display.orientationShown, false, "a desktop's")
        display.orientationShown = true
        verify(!lock.visible, "the lock is a phone's too")
        display.orientationShown = false
        display.driveShown = false
        verify(!row.visible, "none on the desktop")
        display.driveShown = true
        display.orientationShown = false
        verify(row.visible)
        verify(!lock.visible, "no lock where the phone can't hold one")
        display.orientationShown = true
        verify(lock.visible)
    }

    // With no session a page lists its signals from Settings' choice: the
    // field's URI, or the chosen profile alone when no URI is typed
    function test_theOfflineUriFollowsTheFieldAndTheChoice() {
        const page = createPage()
        page.connectPart.uriField.text = ""
        compare(page.uriPart.offlineUri, "", "nothing typed, nothing chosen")

        page.uriPart.chooseProfile("obd2")
        compare(page.connectPart.uriField.text, "", "no URI is made up in the field")
        compare(page.uriPart.offlineUri, "mock://?profile=obd2")
        verify(Bridge.profileSignals(page.uriPart.offlineUri).some(r => r.ref === Bridge.gaugeSignals["speed"][1]),
               "a built-in profile's rows, with no URI typed")

        page.connectPart.uriField.text = "mock://demo"
        compare(page.uriPart.offlineUri, "mock://demo", "a typed URI is read as it is")
        page.uriPart.chooseProfile("obd2")
        compare(page.uriPart.offlineUri, "mock://demo?profile=obd2", "with the choice it composes")

        page.connectPart.uriField.text = ""
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "offline.json", "Offline"))
        page.uriPart.chooseProfile("plugin.offline.base")
        compare(page.uriPart.offlineUri, "mock://?profile=plugin.offline.base")
        compare(Bridge.profileSignals(page.uriPart.offlineUri).map(r => r.name), ["A"],
                "the picked vehicle's own plugin, before any connect")
    }

    // With Default chosen and no URI typed (as after a restart) the list
    // falls back to the picked vehicle's first pick; the picks and the
    // profiles' names reach the Dashboard
    function test_theOfflineUriFallsBackToTheVehiclesFirstPick() {
        const page = createPage()
        page.connectPart.uriField.text = ""
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "fallback.json", "Fallback"))
        page.uriPart.chooseProfile("default")
        compare(page.uriPart.offlineUri, "mock://?profile=plugin.fallback.base", "the vehicle's pick")
        compare(Bridge.profileSignals(page.uriPart.offlineUri).map(r => r.name), ["A"])

        verify(importExample(page))
        pressRow(page, "plugin.example-sim.my2019")
        page.uriPart.chooseProfile("default")
        compare(page.uriPart.offlineUri, "mock://?profile=plugin.fallback.base", "the first pick, in the picks' order")
        compare(JSON.stringify(page.uriPart.pickedSets),
                JSON.stringify([{ profile: "plugin.fallback.base", name: "Fallback", variant: "base" },
                                { profile: "plugin.example-sim.my2019",
                                  name: "Example: the simulated car", variant: "my2019" }]))
        compare(page.uriPart.profileNames["plugin.example-sim.base"], "Example: the simulated car",
                "every variant of a loaded plugin by its name")
        compare(page.uriPart.profileNames["plugin.fallback.base"], "Fallback")

        page.connectPart.uriField.text = "mock://demo"
        compare(page.uriPart.offlineUri, "mock://demo", "a typed URI still wins")
        page.connectPart.uriField.text = ""
        press(page, "vehicle_none")
        compare(page.uriPart.offlineUri, "", "no vehicle, nothing to fall back to")
        compare(page.uriPart.pickedSets.length, 0)
    }

    // An adapter's address under Default (the vehicle's saved one, restored
    // at a restart) lists the vehicle's first pick that reads all of obd2,
    // which stands in for Default there; a typed profile= or the Profile
    // row's choice still wins, and a plugin removed after its pick leaves
    // the address as it is
    function test_anAdaptersAddressListsTheVehiclesObd2StandIn() {
        const page = createPage()
        press(page, "vehicleNew")
        page.open("signals")
        verify(importFile(page, "standin.json", "Standin"))
        page.connectPart.uriField.text = "elm327://COM3"
        compare(page.uriPart.profileChoice, "default")
        compare(page.uriPart.offlineUri, "elm327://COM3", "a pick that doesn't read obd2 stands in for nothing")

        verify(importExample(page))
        pressRow(page, "plugin.example-sim.my2019")
        page.connectPart.uriField.text = "elm327://COM3"
        compare(page.uriPart.offlineUri, "mock://?profile=plugin.example-sim.my2019", "the pick that extends obd2")
        verify(Bridge.profileSignals(page.uriPart.offlineUri).some(r => r.ref === "key:clutch_slip"),
               "the plugin's own signals are listed")
        page.connectPart.uriField.text = "j2534://Some Adapter"
        compare(page.uriPart.offlineUri, "mock://?profile=plugin.example-sim.my2019", "on any adapter")
        page.connectPart.uriField.text = "mock://demo"
        compare(page.uriPart.offlineUri, "mock://demo", "not on the demo, whose Default isn't obd2")

        page.connectPart.uriField.text = "elm327://COM3?profile=raw"
        compare(page.uriPart.offlineUri, "elm327://COM3?profile=raw", "a typed profile= wins")
        page.connectPart.uriField.text = "elm327://COM3"
        page.uriPart.chooseProfile("uds-generic")
        compare(page.uriPart.offlineUri, page.connectPart.uriField.text, "the Profile row's choice wins")
        verify(page.uriPart.offlineUri.indexOf("profile=uds-generic") >= 0, page.uriPart.offlineUri)

        page.uriPart.chooseProfile("default")
        compare(page.connectPart.uriField.text, "elm327://COM3")
        verify(page.rowActionsPart.removePlugin(page.picksPart.pluginList.find(p => p.id === "example-sim").sha256))
        compare(page.picksPart.picks, ["plugin.standin.base"], "the pick goes with the plugin")
        compare(page.uriPart.offlineUri, "elm327://COM3", "and the address is listed as it is")
        page.connectPart.uriField.text = ""
        compare(page.uriPart.offlineUri, "mock://?profile=plugin.standin.base", "the vehicle's pick, with no address")
    }
}
