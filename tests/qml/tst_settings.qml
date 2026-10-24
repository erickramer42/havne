// File/layer: tests/qml/tst_settings.qml | Qt Quick Test, QML tier
// Testing: Settings composes the URI from a preset (the base), the
//          profile row and uds-generic's DID field, keeps a hand-written
//          URI's base, shows core's refusal of a DID list in the error
//          line, connects a composed uds-generic URI through the mock, and
//          locks every control, the device rows among them, while a
//          connect is in flight (Windows: the fake's open stalled by a
//          knob). The live registry is manual smoke.

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

    // A binding error in the app's QML fails the test
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/QtTune\//)
    }

    // Every connect here is asynchronous, so a case that fails between
    // pressing Connect and the completion leaves one in flight: let it
    // land before the disconnect, or the next case meets a connected
    // bridge and fails for the wrong reason
    function cleanup() {
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
        compare(uri.text, "mock://demo?profile=raw", "mock takes no filter key")
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
        compare(uri.text, "j2534://?profile=raw&filter=0x7E8",
                "raw on an adapter listens to the engine ECU")
        verify(!didRow.visible)
        press(page, "profile_default")
        compare(uri.text, "j2534://", "the default profile needs no query")

        // A hand-written URI connects as written; the next choice composes
        // again from its base
        uri.text = "j2534://Vendor?poll=0x0C"
        press(page, "profile_uds-generic")
        compare(uri.text, "j2534://Vendor?profile=uds-generic&did=1E1C:u16,F190:text")
        compare(did.text, " 1E1C:u16, F190:text ", "the field keeps what was typed")
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
