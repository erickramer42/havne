// File/layer: tests/qml/tst_settings.qml | Qt Quick Test, QML tier
// Testing: Settings composes the URI from a preset (the base), the
//          profile row and uds-generic's DID field, keeps a hand-written
//          URI's base, shows core's refusal of a DID list in the error
//          line, and connects a composed uds-generic URI through the mock.
//          The device list and the live registry are manual smoke.

import QtQuick
import QtTest
import QtTune

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

    function cleanup() {
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
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
}
