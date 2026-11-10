// File/layer: tests/qml/tst_home.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.home (test_qml)

import QtQuick
import QtTest
import App
TestCase {
    id: testCase
    name: "Home"
    when: windowShown

    Component {
        id: pageComponent
        HomePage {}
    }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
    }

    // A singleton's state, shared by every case
    function cleanup() {
        Theme.developerMode = false
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    // Under the window's root, so visible reads the page's own
    function createPage(w, h) {
        const page = createTemporaryObject(pageComponent, testCase.parent,
                                           { width: w, height: h })
        verify(page, "Home loads")
        return page
    }

    // The visible items under item whose right edge lies past page's
    function pastTheEdge(item, page, out) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (!child.visible || child.width <= 0)
                continue
            const right = child.mapToItem(page, child.width, 0).x
            if (right > page.width + 0.5)
                out.push((child.objectName || child.text || String(child))
                         + " at " + Math.round(right))
            pastTheEdge(child, page, out)
        }
        return out
    }

    // Home's Connect... is secondary beside the header's primary, and
    // Disconnect is quiet
    function test_theConnectButtonRanksUnderTheHeaders() {
        const page = createPage(380, 800)
        const button = find(page, "homeConnect")
        compare(button.text, "Connect…")
        verify(!button.highlighted, "secondary")
        verify(!button.flat, "not quiet: it is the page's action")
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        tryCompare(button, "text", "Disconnect", 5000)
        verify(button.flat, "ending a session is quiet")
        verify(!button.highlighted)
        Bridge.disconnectSession()
        tryCompare(button, "text", "Connect…", 5000)
    }

    // Developer mode's hint line is the longest
    function test_uprightEverythingFitsTheWidth() {
        Theme.developerMode = true
        const page = createPage(380, 800)
        const column = find(page, "homeColumn")
        waitForPolish(column)
        compare(pastTheEdge(page, page, []).join("; "), "",
                "nothing past the page's right edge")
    }

    function test_onItsSideThePageScrollsToTheLastItem() {
        const page = createPage(800, 330)
        const flick = find(page, "homeFlick")
        const column = find(page, "homeColumn")
        waitForPolish(column)
        verify(column.height > page.height, "the column is taller than the page")
        compare(column.y, Theme.pageMargin, "the column starts at the margin")
        verify(flick.contentHeight >= column.y + column.height,
               "the page scrolls to the column's end: " + flick.contentHeight)
    }

    // The hint names every way in, not the first two the app had
    function test_theHintNamesEveryWayIn() {
        const page = createPage(1280, 720)
        const hint = find(page, "homeHint")
        verify(hint.visible, "shown with no session")
        compare(hint.text, "Pick what to connect to in Settings: a J2534 interface, "
                + "an ELM327 adapter over a serial port, Bluetooth or WiFi, or a SocketCAN bus")
        Theme.developerMode = true
        compare(hint.text, "Pick what to connect to in Settings: a J2534 interface, "
                + "an ELM327 adapter over a serial port, Bluetooth or WiFi, a SocketCAN bus, "
                + "or the mock://demo preset")
        Theme.developerMode = false
    }
}
