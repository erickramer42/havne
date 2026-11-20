// File/layer: tests/qml/DriveViewKit.qml | Qt Quick Test, QML tier
// Testing: QML: the base of the tst_driveview*.qml files (test_qml); not a test itself

import QtCore
import QtQuick
import QtTest
import App
import AppTest

// The drive view (ADR-0059): a state of the Dash page, driven in Main so the
// top bar and the tab bar are the real ones, at the phone's size
// The shared setup and helpers of tst_driveview*.qml: a file derives from it and sets its `name`;
// `init` and `cleanup` are the kit's.
TestCase {
    id: testCase
    property alias mainComponent: mainComponent
    property alias dashStore: dashStore
    property alias uiStore: uiStore

    when: windowShown

    Component { id: mainComponent; Main {} }

    // Stands in for the window, so a test never takes the desktop full screen
    Component { id: windowComponent; QtObject { property int visibility: Window.Maximized } }

    Settings {
        id: dashStore
        category: "dashboards"
        property bool phoneDrive: false
        property int driveHints: 0
    }

    Settings {
        id: uiStore
        category: "ui"
        property int driveHideMs: 4000
        property string driveOrientation: "follow"
    }

    function resetStores() {
        dashStore.setValue("phoneDrive", false)
        dashStore.setValue("driveHints", 3)    // the hint has its own cases
        dashStore.sync()
        uiStore.setValue("driveHideMs", 4000)
        uiStore.setValue("driveOrientation", "follow")
        uiStore.sync()
    }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        resetStores()
    }

    function cleanup() {
        // The armed case saves a set that records: none outlives it
        Bridge.saveConditions('{"havne_conditions":1,"conditions":[{"key":"c","name":"C",'
                              + '"when":{"signal":["id:1001"],"unit":"RPM","above":90000},"mark":true}]}')
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Bridge.recording = false
        Bridge.clearLogNotice()
        Harness.clearFakeKnobs()
        PhoneWindow.lockOrientation("follow")
        PhoneWindow.setKeepScreenOn(false)
        Immersive.leave()
        resetStores()
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    function phoneMain(props) {
        const main = createTemporaryObject(mainComponent, testCase, props)
        verify(main, "Main loads")
        main.testMobile = true
        main.width = 384
        main.height = 832
        main.font.pixelSize = 16
        const dash = find(main.contentItem, "dashboardPage")
        dash.form = "phone"
        dash.drivePart.driveWindow = createTemporaryObject(windowComponent, testCase)
        verify(waitForPolish(main), "laid out")
        return { main: main, dash: dash, stand: dash.drivePart.driveWindow }
    }

    function connect(uri) {
        compare(Bridge.connectSession(uri), "", uri + " connects")
        tryVerify(() => Bridge.connected, 5000, "connected to " + uri)
    }

    function replayUri() {
        return "replay://" + Harness.replayFixtures + "/2026-10-25-engine-start?profile=obd2&speed=max"
    }

    // A phone Main with a mock session, the Dash page showing
    function driven(uri) {
        const s = phoneMain()
        connect(uri === undefined ? "mock://demo" : uri)
        tryCompare(s.main, "currentPage", s.main.dashboardPage, 2000)
        return s
    }

    function enterOn(s) {
        s.dash.drivePart.enterDrive()
        verify(s.dash.drivePart.driving, "the view is on")
        verify(waitForPolish(s.main), "laid out")
    }

    // Main's header and tab bar, and the view's sheet
    function bars(s) {
        return { head: find(s.main.contentItem, "headerBar"), tab: find(s.main.contentItem, "tabBar"),
                 tools: find(s.dash, "driveSheet"), sheet: find(s.dash, "driveToolsBackdrop") }
    }

    function revealedOn(s) {
        s.dash.drivePart.driveTap()
        verify(s.dash.drivePart.barsUp, "revealed")
        tryCompare(s.dash.drivePart, "barsShown", 1, 2000, "slid in")
    }

    // The focus is somewhere the back key reaches zoomRoot's handler from
    function focusReachesTheHandler(s) {
        for (let i = s.main.activeFocusItem; i; i = i.parent) {
            if (i.objectName === "zoomRoot")
                return true
        }
        return false
    }

    // Answers whether QML took the key, as Android's system reads it
    function back(s) {
        const taken = Harness.pressBack(s.main)
        verify(focusReachesTheHandler(s), "the next Back arrives too")
        return taken
    }
}
