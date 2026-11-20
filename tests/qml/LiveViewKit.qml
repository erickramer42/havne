// File/layer: tests/qml/LiveViewKit.qml | Qt Quick Test, QML tier
// Testing: QML: the base of the tst_liveview*.qml files (test_qml); not a test itself

import QtCore
import QtQuick
import QtTest
import App
import AppTest

// The shared setup and helpers of tst_liveview*.qml: a file derives from it and sets its `name`;
// `init` and `cleanup` are the kit's.
TestCase {
    id: testCase
    property alias pageComponent: pageComponent

    // A palette by its name: Dark is Night off, the others Night on in a style
    function usePalette(name) {
        const night = Theme.nightPalettes.indexOf(name) >= 0
        Theme.night = night ? "on" : "off"
        if (night)
            Theme.nightStyle = name === "nightRed" ? "red" : "amber"
        else
            Theme.look = name
    }

    when: windowShown

    Component {
        id: pageComponent
        // Tall enough that every mock row has a delegate
        LiveViewPage { width: 900; height: 1000 }
    }

    // A binding error in the app's QML fails the test
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
    }

    // The test window's size before a case grew it, or null
    property var grownFrom: null

    function cleanup() {
        if (grownFrom) {
            testCase.parent.Window.window.width = grownFrom[0]
            testCase.parent.Window.window.height = grownFrom[1]
            grownFrom = null
        }
        Bridge.unitSystem = 0
        Bridge.liveSignalModel.filter.clear()
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Bridge.saveConditions('{"havne_conditions":1,"conditions":[]}')
        Harness.clearFakeKnobs()
        for (const p of Bridge.plugins())     // the store starts empty
            Bridge.removePlugin(p.sha256)
        Theme.night = "off"
        Theme.nightStyle = "amber"
        Theme.look = "dark"
    }

    function sameColor(actual, expected, what) {
        verify(Qt.colorEqual(actual, expected), what + ": " + actual + ", expected " + expected)
    }

    function connectMock(page) {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const list = findChild(page, "signalList")
        verify(list.count > 0, "the mock set decodes signals")
        tryVerify(() => list.itemAtIndex(list.count - 1) !== null, 2000, "every row has a delegate")
        return list
    }
}
