// File/layer: tests/qml/DashEditKit.qml | Qt Quick Test, QML tier
// Testing: QML: the base of the tst_dashedit* files (test_qml); not a test itself

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

// The Dashboard's edit-mode tests' shared helpers: a page in the window, its store put back, finding, pressing and typing.
// A file derives from it and sets its `name`; `init` and `cleanup` are the kit's.
TestCase {
    id: testCase
    when: windowShown
    // The window holds a desktop page whole, so a click lands on it
    width: 1280
    height: 800

    property alias storeComponent: storeComponent

    Component { id: pageComponent; DashboardPage { width: 1280; height: 800 } }
    // The page's own store, to put each case's back
    Component {
        id: storeComponent
        Settings {
            category: "dashboards"
            property string phone: ""
            property string desktop: ""
            property int phonePage: 0
            property int desktopPage: 0
            property string phoneUnread: ""
            property string desktopUnread: ""
        }
    }

    // A binding error in the app's QML fails the test. The page's
    // Settings stores a write when the page goes, so the store is put
    // back once the last case's pages are gone, through setValue
    function init() {
        // Each case starts from no session, whatever an earlier one left
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting && !Bridge.connected, 5000, "no session at the start")
        wait(0)
        const store = createTemporaryObject(storeComponent, testCase)
        for (const key of ["phone", "desktop", "phoneUnread", "desktopUnread"])
            store.setValue(key, "")
        store.setValue("phonePage", 0)
        store.setValue("desktopPage", 0)
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        failOnWarning(/^Model size of /)
    }

    function cleanup() {
        DocumentPicker.setPickerForTesting(false)
        Theme.night = "off"
        Theme.look = "dark"
        Bridge.unitSystem = 0
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        for (const p of Bridge.plugins())     // the store starts empty
            Bridge.removePlugin(p.sha256)
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    // A page in the window (an item's `visible` is its own there), showing
    // `doc` when one is given
    function pageOf(props, doc) {
        const page = createTemporaryObject(pageComponent, testCase.Window.window.contentItem, props)
        if (doc)
            page.doc = doc
        return page
    }

    // A page showing `doc` with its text stored, as the app has it after an
    // edit, so an undo stores the text it had
    function storedPageOf(props, doc) {
        const page = pageOf(props, doc)
        find(page, "dashStore")[page.form === "phone" ? "phone" : "desktop"] = DashboardBook.serialize(doc)
        return page
    }

    // A one-page document of these cells
    function docOf(cells) {
        const d = DashboardBook.builtIn("desktop")
        d.pages = [{ name: "Test", cells: cells }]
        return d
    }
    // A number by default: a dial is many times slower to build, so a case passes "analog" only where the kind matters
    function cellOf(label, kind) {
        return { kind: kind || "digital", w: 2, h: 4, label: label, refs: ["id:1001"],
                 unit: "RPM", options: { from: 0, to: 8000 } }
    }
    function labels(page) {
        return page.doc.pages[page.pageIndex].cells.map(c => c.label)
    }
    function storedText(page) {
        return find(page, "dashStore")[page.form === "phone" ? "phone" : "desktop"]
    }
    // A press where the item is, once the window is laid out and the item
    // has stopped moving (a page change slides the view). The layout comes
    // first: a row that opens keeps the place it last had, and until the
    // layout pass moves the page below it, the page covers it
    function click(page, name) {
        const item = find(page, name)
        verify(item.visible && item.enabled, name + " can be pressed")
        press(item)
    }
    function press(item) {
        verify(waitForPolish(item.Window.window), item.objectName + " is laid out")
        // A slide's first frame can come after both samples below, with the target still past the window's edge
        for (let up = item.parent; up; up = up.parent) {
            if (up.objectName === "dashPages") {
                const view = up.contentItem
                tryVerify(() => view.currentItem && Math.abs(view.contentX - view.currentItem.x) < 0.5,
                          3000, item.objectName + "'s page has slid in")
            }
        }
        // A cell below a tall panel is scrolled into the page's view first,
        // and the page put back after the press
        let scrolled = null
        let was = 0
        let inSide = false
        // The side column scrolls by itself: bring the item into its view
        for (let up = item.parent; up; up = up.parent) {
            if (up.objectName === "dashSidePanel" && up.visible) {
                inSide = true
                const at = item.mapToItem(up, item.width / 2, item.height / 2)
                if (at.y > up.height - 24 || at.y < 24)
                    up.contentY = Math.max(0, Math.min(up.contentHeight - up.height,
                                                       up.contentY + at.y - up.height / 2))
            }
        }
        for (let up = item.parent; up && !inSide; up = up.parent) {
            if (up.objectName.startsWith("dashPage_")) {
                const at = item.mapToItem(up, item.width / 2, item.height / 2)
                if (at.y > up.height - 24) {
                    was = up.contentY
                    up.contentY = Math.max(0, Math.min(up.contentHeight - up.height,
                                                       up.contentY + at.y - up.height / 2))
                    scrolled = up
                }
            }
        }
        let last = null
        tryVerify(() => {
            const at = item.mapToItem(null, 0, 0)
            const still = last !== null && at.x === last.x && at.y === last.y
            last = at
            return still
        }, 3000, item.objectName + " stops moving")
        mouseClick(item)
        if (scrolled)
            scrolled.contentY = was
    }
    // Text typed into a field and the field left; it emits textEdited, as real typing does
    function type(field, text) {
        field.text = text
        field.textEdited()
        field.editingFinished()
    }
    function spin(box, value) {
        box.value = value
        box.valueModified()
    }
}
