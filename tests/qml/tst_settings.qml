// File/layer: tests/qml/tst_settings.qml | Qt Quick Test, QML tier
// Testing: Settings composes the URI from a preset (the base), the
//          profile row and uds-generic's DID field, reads a typed URI
//          back into the row and the field and keeps its other keys (the
//          vehicle run sheet's URIs pass through untouched), refuses
//          query characters in the DID field, shows core's refusal of a
//          DID list in the error line, connects a composed uds-generic
//          URI through the mock, lists serial ports after the interfaces
//          (a port's row and the elm327:// preset filling the field), and
//          locks every control, the device rows among them, while a
//          connect is in flight (Windows: the fake's open stalled by a
//          knob); the Units row sets the bridge's unit system, connected
//          or not; a new page (a restart) gets back the last URI that
//          connected, read into the row, and the Record switch, while a
//          refused URI isn't kept. The live registry, and the units kept
//          across a restart (Main.qml's Settings), are manual smoke.

import QtCore
import QtQuick
import QtTest
import QtTune
import QtTuneTest

TestCase {
    id: testCase
    name: "Settings"
    when: windowShown

    Component {
        id: pageComponent
        SettingsPage { width: 1000; height: 800 }
    }

    // The page's stored URI and Record switch (test_qml keeps them in a
    // scratch ini), written straight to the store
    Settings {
        id: stored
        category: "session"
    }

    // A binding error in the app's QML fails the test. Each case starts
    // with nothing stored, or the last case's URI would fill the field
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/QtTune\//)
        stored.setValue("uri", "")
        stored.setValue("recording", false)
        stored.sync()
    }

    // Every connect here is asynchronous, so a case that fails between
    // pressing Connect and the completion leaves one in flight: let it
    // land before the disconnect, or the next case meets a connected
    // bridge and fails for the wrong reason
    function cleanup() {
        Bridge.unitSystem = 0
        Bridge.recording = false
        tryVerify(() => !Bridge.connecting, 10000, "a connect in flight ends")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Harness.clearFakeKnobs()
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    // The page goes under the window's root, not the TestCase: TestCase is
    // an invisible item, and visible reads the effective visibility, so a
    // child of it would read invisible whatever the page did
    function createPage() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        verify(page, "Settings loads")
        return page
    }

    // A button's click, through its own handler
    function press(page, name) {
        find(page, name).clicked()
    }

    // The Units row sets the bridge's unit system, connected or not, and
    // shows the one in force
    function test_unitsRowSetsTheBridge() {
        const page = createPage()
        const metric = find(page, "units_metric")
        const imperial = find(page, "units_imperial")
        compare(Bridge.unitSystem, 0)
        verify(metric.highlighted && !imperial.highlighted)
        press(page, "units_imperial")
        compare(Bridge.unitSystem, 1)
        verify(imperial.highlighted && !metric.highlighted)

        compare(Bridge.connectSession("mock://demo"), "", "connects")
        verify(metric.enabled, "units change while connected")
        press(page, "units_metric")
        compare(Bridge.unitSystem, 0)
        verify(metric.highlighted)
    }

    function test_presetsProfilesAndDidsComposeTheUri() {
        const page = createPage()
        const uri = find(page, "uriField")
        const did = find(page, "didField")
        const didRow = find(page, "didRow")
        compare(uri.text, "", "nothing chosen, nothing composed")
        verify(!didRow.visible, "the DID field belongs to uds-generic")

        press(page, "preset_mock")
        compare(uri.text, "mock://demo")
        press(page, "profile_raw")
        compare(uri.text, "mock://demo?profile=raw")
        press(page, "profile_uds-generic")
        compare(uri.text, "mock://demo?profile=uds-generic", "no DIDs yet: core will say so")
        verify(didRow.visible)
        did.text = " 1E1C:u16, F190:text "
        compare(uri.text, "mock://demo?profile=uds-generic&did=1E1C:u16,F190:text",
                "the DIDs as typed, minus whitespace")

        press(page, "preset_j2534")
        compare(uri.text, "j2534://?profile=uds-generic&did=1E1C:u16,F190:text",
                "a new base keeps the profile and the DIDs")
        press(page, "profile_raw")
        compare(uri.text, "j2534://?profile=raw",
                "core gives raw on an adapter the engine ECU's channel")
        verify(!didRow.visible)
        press(page, "profile_default")
        compare(uri.text, "j2534://", "the default profile needs no query")

        // A hand-written URI connects as written; the next choice composes
        // again from it, keeping its other keys in place
        uri.text = "j2534://Vendor?poll=0x0C"
        press(page, "profile_uds-generic")
        compare(uri.text, "j2534://Vendor?poll=0x0C&profile=uds-generic&did=1E1C:u16,F190:text")
        compare(did.text, " 1E1C:u16, F190:text ", "the field keeps what was typed")
    }

    // Serial ports join the device list after the interfaces; the
    // elm327:// preset gives the scheme alone and a port's row the whole
    // base, keeping the query either way
    function test_serialPortRowsFillAnElm327Uri() {
        const page = createPage()
        const uri = find(page, "uriField")
        const list = find(page, "deviceList")
        const count = find(page, "deviceCount")

        page.j2534Devices = []
        page.serialPorts = []
        verify(count.text.indexOf("None found") >= 0, "the empty state: " + count.text)
        verify(!list.visible)

        page.j2534Devices = [{ vendorName: "Fake", dllPath: "C:/drivers/fake.dll",
                               architecture: "x64" }]
        page.serialPorts = [{ name: "COM5", device: "\\Device\\BthModem0" },
                            { name: "/dev/ttyUSB0", device: "" }]
        compare(list.count, 3)
        verify(count.text.indexOf("J2534 interfaces: 1, serial ports: 2") >= 0, count.text)
        tryVerify(() => list.itemAtIndex(2) !== null, 2000, "the rows have delegates")
        compare(find(list.itemAtIndex(0), "deviceTitle").text, "Fake  (x64)")
        compare(find(list.itemAtIndex(1), "deviceTitle").text, "COM5")
        compare(find(list.itemAtIndex(1), "deviceDetail").text,
                "Serial port, for elm327:// · \\Device\\BthModem0")
        compare(find(list.itemAtIndex(2), "deviceDetail").text, "Serial port, for elm327://")

        press(page, "preset_elm327")
        compare(uri.text, "elm327://", "the port is the user's pick")
        press(page, "profile_raw")
        compare(uri.text, "elm327://?profile=raw")
        mouseClick(find(list.itemAtIndex(1), "deviceRowArea"))
        compare(uri.text, "elm327://COM5?profile=raw", "the row's base, the query kept")
        mouseClick(find(list.itemAtIndex(2), "deviceRowArea"))
        compare(uri.text, "elm327:///dev/ttyUSB0?profile=raw")
        mouseClick(find(list.itemAtIndex(0), "deviceRowArea"))
        compare(uri.text, "j2534://Fake?profile=raw")
    }

    // A URI the page didn't compose: typed, pasted or set from outside
    // (UI Automation's SetValue is a plain set, as this is)
    function type(uri, text) {
        uri.text = text
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
        compare(page.profileChoice, "uds-generic")
        verify(find(page, "profile_uds-generic").highlighted)
        verify(!find(page, "profile_default").highlighted)
        verify(didRow.visible)
        compare(did.text, "1E1C:u16")
        verify(hint.text.indexOf("Service 0x22") >= 0, "the hint is uds-generic's: " + hint.text)

        press(page, "profile_default")
        compare(uri.text, "mock://demo")
        press(page, "profile_uds-generic")
        compare(uri.text, "mock://demo?profile=uds-generic&did=1E1C:u16", "the DIDs are back")

        type(uri, "j2534://Vendor?profile=raw&filter=0x7E9")
        compare(page.profileChoice, "raw")
        verify(find(page, "profile_raw").highlighted)
        verify(!didRow.visible)
        compare(uri.text, "j2534://Vendor?profile=raw&filter=0x7E9", "left as typed")
        press(page, "profile_default")
        compare(uri.text, "j2534://Vendor?filter=0x7E9", "a typed filter stays")

        // A profile the row doesn't offer: no button lit, the hint says so
        type(uri, "j2534://Vendor?profile=obd2")
        compare(page.profileChoice, "obd2")
        verify(!find(page, "profile_default").highlighted)
        verify(!find(page, "profile_raw").highlighted)
        verify(hint.text.indexOf("obd2") >= 0, "the hint names it: " + hint.text)
        type(uri, "j2534://Vendor")
        compare(page.profileChoice, "default")
        verify(find(page, "profile_default").highlighted)
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
        compare(uri.text, "mock://demo?identify=0&profile=uds-generic", "a new base keeps the query")
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
        compare(page.profileChoice, data.profile)
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

    // A restart gets back the last URI that connected and the Record
    // switch. Recording is switched on only after the connects, so no
    // session here records
    function test_theLastConnectedUriAndTheRecordSwitchComeBack() {
        let page = createPage()
        const error = find(page, "errorLabel")
        find(page, "uriField").text = "mock://demo?profile=raw"
        press(page, "connectButton")
        tryVerify(() => Bridge.connected, 5000, "the URI connects")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")

        find(page, "uriField").text = "mock://demo?profile=uds-generic"
        press(page, "connectButton")
        tryVerify(() => error.visible, 5000, "core refuses it: no ?did=")
        Bridge.recording = true
        verify(find(page, "recordBox").checked)

        // The page's store writes when the page goes, as at a quit
        page.destroy()
        wait(10)
        Bridge.recording = false   // a fresh bridge starts with it off

        page = createPage()
        compare(find(page, "uriField").text, "mock://demo?profile=raw",
                "the URI that connected, not the refused one")
        verify(find(page, "profile_raw").highlighted, "read back into the row")
        verify(Bridge.recording, "the Record switch comes back")
        verify(find(page, "recordBox").checked)
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
