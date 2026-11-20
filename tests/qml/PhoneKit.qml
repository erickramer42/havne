// File/layer: tests/qml/PhoneKit.qml | Qt Quick Test, QML tier
// Testing: QML: the base of the tst_phone_* files (test_qml); not a test itself

import QtCore
import QtQuick
import QtTest
import App
import AppTest

// The phone tests' shared helpers: Main in a window, its `ui` store put back, finding,
// tapping and the layout checks. A file derives from it and sets its `name`; `init` and
// `cleanup` are the kit's.
TestCase {
    id: testCase
    when: windowShown
    // The parts create Main from the kit's component
    property alias mainComponent: mainComponent

    Component { id: mainComponent; Main {} }
    // Main's `ui` store, as a window made later reads it
    Component {
        id: uiStoreComponent
        Settings {
            category: "ui"
            property int minSweepMs: -1
        }
    }
    function storedUi(key) {
        const reader = uiStoreComponent.createObject(null)
        const value = reader[key]
        reader.destroy()
        return value
    }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        Theme.developerMode = false
    }

    function cleanup() {
        // The armed cases save a set that records: none outlives them
        Bridge.saveConditions('{"havne_conditions":1,"conditions":[{"key":"c","name":"C",'
                              + '"when":{"signal":["id:1001"],"unit":"RPM","above":90000},"mark":true}]}')
        Relay.stop()
        Theme.developerMode = false
        Theme.night = "off"
        Theme.nightStyle = "amber"
        Theme.look = "dark"
        Theme.zoom = 1.0
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Bridge.recording = false
        Bridge.minSweepMs = 0
    }

    // The readouts group digits with the system locale, as Live and the gauges do.
    function grouped(n) { return Number(n).toLocaleString(Qt.locale(), 'f', 0) }

    // A page's place in the StackLayout: the Dashboard sits in its dashFrame
    function stackIndex(page) {
        const item = page.parent.objectName === "dashFrame" ? page.parent : page
        return Array.from(item.parent.children).indexOf(item)
    }
    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    // An item's name, or its kind and its nearest named ancestor's
    function label(item) {
        if (item.objectName || item.text)
            return item.objectName || item.text
        let up = item.parent
        while (up && !up.objectName)
            up = up.parent
        return String(item).replace(/\(0x[0-9a-f]+\)/, "") + " in " + (up ? up.objectName : "?")
    }

    // The items painted past `edge`'s right side (a Text by its painted
    // width): no page scrolls sideways, so on a phone they can't be seen
    function pastTheEdge(item, edge, out) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (!child.visible || child.width <= 0)
                continue
            // A rotated plain Item paints nothing itself (a gauge's tick is
            // a dial-sized Item turned about the center): its children,
            // which do, are judged instead
            if (/^QQuickItem(_QML_\d+)?\(/.test(String(child)) && child.rotation !== 0) {
                pastTheEdge(child, edge, out)
                continue
            }
            // Any other rotated item (a gauge's needle, a Shape turned about
            // the dial's center) is judged by the circle its corners sweep:
            // one mapped corner would depend on the angle, which follows
            // the mock's drive cycle, so the same layout passes or fails
            // with how long the connect took
            if (child.rotation !== 0) {
                const mid = child.mapToItem(edge, child.width / 2, child.height / 2).x
                const right = mid + Math.hypot(child.width, child.height) / 2
                if (right > edge.width + 0.5)
                    out.push(label(child) + " at " + Math.round(right))
                continue
            }
            const painted = child.elide !== undefined && !child.clip
                            ? Math.max(child.width, child.contentWidth) : child.width
            const right = child.mapToItem(edge, painted, 0).x
            if (right > edge.width + 0.5)
                out.push(label(child) + " at " + Math.round(right))
            pastTheEdge(child, edge, out)
        }
        return out
    }

    // A CSV whose guess has a time with no unit, a long header, a column
    // the guess leaves out, and notes
    readonly property string guessFile:
        "Time since the logger started,Engine speed measured at the crankshaft position sensor [rpm],"
        + "Intake air temperature after the intercooler (deg C),Always empty\n"
        + "0,800.5,31.5,\n1,900.25,31.75,\n2,950,32,\n"

    // Main on the test phone upright, the Recordings page shown with a
    // recording open at the chart's default height
    function uprightViewer(row) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        main.font.pixelSize = 16
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(stackIndex(page))
        page.chartHeight = -1
        page.openPart.open(row)
        tryVerify(() => find(page, "signalRows").visible && find(page, "signalRows").itemAtIndex(0) !== null,
                  10000, "the recording opens")
        verify(waitForPolish(main), "laid out")
        return { main: main, page: page }
    }

    // An item's rectangle in the window's own units (the glass's, the
    // zoom applied)
    function onGlass(item, main) {
        return item.mapToItem(main.contentItem, 0, 0, item.width, item.height)
    }

    // One finger's tap, pressed and lifted in place
    function tap(item, x, y) {
        const seq = touchEvent(item)
        seq.press(0, item, x, y).commit()
        seq.release(0, item, x, y).commit()
    }

    // A Main on a phone's form at 384 x 832, laid out, with Android's text
    function phoneMain() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.testMobile = true
        main.width = 384
        main.height = 832
        main.font.pixelSize = 16
        verify(waitForPolish(main), "laid out")
        return main
    }
}
