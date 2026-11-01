// File/layer: tests/qml/tst_phone.qml | Qt Quick Test, QML tier
// Testing: every page in the app's own window (Main) at a phone's size,
//          connected to mock://demo with developer mode on: each page's
//          items lie inside the window's width, the Log page in its
//          Frames view too. The pages' own tests run them at 320 px
//          alone; this one runs them as the window holds them. How they
//          look on a phone is the phone's run.

import QtQuick
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "Phone"
    when: windowShown

    Component { id: mainComponent; Main {} }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        Theme.developerMode = false
    }

    function cleanup() {
        Theme.developerMode = false
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
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
            const painted = child.elide !== undefined && !child.clip
                            ? Math.max(child.width, child.contentWidth) : child.width
            const right = child.mapToItem(edge, painted, 0).x
            if (right > edge.width + 0.5)
                out.push(label(child) + " at " + Math.round(right))
            pastTheEdge(child, edge, out)
        }
        return out
    }

    // The test phone upright, in device-independent pixels (1440 px
    // at 600 dpi)
    function test_everyPageFitsAPhonesWidth_data() {
        return [{ tag: "homePage" }, { tag: "dashboardPage" }, { tag: "livePage" },
                { tag: "diagnosticsPage" }, { tag: "logPage" }, { tag: "logFrames" },
                { tag: "settingsPage" }]
    }

    function test_everyPageFitsAPhonesWidth(data) {
        Theme.developerMode = true
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 800
        // Android's 14 sp text at the test phone's font scale, 1.15
        main.font.pixelSize = 16
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const name = data.tag === "logFrames" ? "logPage" : data.tag
        const page = find(main.contentItem, name)
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        verify(page.visible, name + " is on screen")
        if (data.tag === "logFrames")
            find(page, "view_frames").clicked()
        if (data.tag === "diagnosticsPage") {
            tryVerify(() => find(page, "clearCodes").enabled, 2000, "the report is read")
            find(page, "clearCodes").clicked()
        }
        wait(300)
        waitForRendering(main.contentItem)
        const out = pastTheEdge(page, main.contentItem, [])
        compare(out.length, 0, "past " + main.contentItem.width + " px: " + out.join(", "))
        if (data.tag === "diagnosticsPage")
            find(page, "clearCancel").clicked()
    }
}
