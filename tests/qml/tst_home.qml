// File/layer: tests/qml/tst_home.qml | Qt Quick Test, QML tier
// Testing: Home at a phone's sizes (docs/bench-sessions/2026-10-28-android.md):
//          upright, every item lies inside the page's width, developer
//          mode's hint line wrapped; on its side, the column starts at
//          the margin rather than above the page, and the page scrolls
//          to its last item.

import QtQuick
import QtTest
import QtTune

TestCase {
    id: testCase
    name: "Home"
    when: windowShown

    Component {
        id: pageComponent
        HomePage {}
    }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/QtTune\//)
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
}
