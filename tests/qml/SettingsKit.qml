// File/layer: tests/qml/SettingsKit.qml | Qt Quick Test, QML tier
// Testing: QML: the base of the tst_settings* files (test_qml); not a test itself

import QtCore
import QtQuick
import QtTest
import App
import AppTest

// The Settings page tests' shared helpers: the page in the window, the stores a case starts clean, and finding, tapping and pressing.
// A file derives from it and sets its `name`; `init` and `cleanup` are the kit's.
TestCase {
    id: testCase
    when: windowShown

    property alias dashComponent: dashComponent
    property alias hubComponent: hubComponent
    property alias stored: stored
    property alias storedVehicles: storedVehicles
    property alias uiStore: uiStore

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
}
