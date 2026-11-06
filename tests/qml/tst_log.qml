// File/layer: tests/qml/tst_log.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.log (test_qml)

import QtQuick
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "Log"
    when: windowShown

    Component {
        id: logComponent
        LogPage { width: 1000; height: 700 }
    }
    Component {
        id: settingsComponent
        SettingsPage { width: 1000; height: 800 }
    }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        Theme.developerMode = false
    }

    function cleanup() {
        Theme.developerMode = false
        Bridge.model.newestFirst = true
        Bridge.model.clearSelection()
        Bridge.logPaused = false
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Bridge.recording = false   // only between sessions
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    function create(component) {
        const page = createTemporaryObject(component, testCase.parent)
        verify(page, "the page loads")
        return page
    }

    function test_theActivityLogTakesTheBridgesLines() {
        const page = create(logComponent)
        const log = find(page, "activityLog")
        compare(log.text, "Ready.")
        compare(Bridge.connectSession("mock://demo"), "")
        verify(/\d\d:\d\d:\d\d\.\d\d\d  Connected: mock:\/\/demo/.test(log.text),
               "stamped like the surrogate's log: " + log.text)
        verify(log.text.includes("Session: mock transport; profile mock"))
    }

    function test_theActivityLogKeepsItsNewestLines() {
        const page = create(logComponent)
        const log = find(page, "activityLog")
        for (let i = 0; i < page.maxLogLines + 50; ++i)
            page.appendLog("line " + i)
        verify(log.lineCount <= page.maxLogLines, "bounded: " + log.lineCount)
        verify(log.text.includes("line " + (page.maxLogLines + 49)), "the newest is kept")
        verify(!log.text.includes("Ready."), "the oldest went first")
    }

    function test_framesOnlyInDeveloperMode() {
        const page = create(logComponent)
        const activity = find(page, "activityView")
        const frames = find(page, "frameView")
        const toFrames = find(page, "view_frames")
        verify(activity.visible, "the activity log, alone")
        verify(!frames.visible)
        verify(!toFrames.visible, "no switch without developer mode")

        Theme.developerMode = true
        verify(toFrames.visible, "the switch comes with developer mode")
        verify(activity.visible, "still the activity log until switched")
        toFrames.clicked()
        verify(frames.visible, "the raw frames")
        verify(!activity.visible)

        Theme.developerMode = false
        verify(activity.visible, "back to the activity log")
        verify(!frames.visible)
        verify(!toFrames.visible)
    }

    // The items painted past the page's right edge (a Text by its
    // painted width): the page scrolls in neither direction, so on a
    // phone they can't be seen
    function pastTheEdge(item, page, out) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (!child.visible || child.width <= 0)
                continue
            const painted = child.elide !== undefined && !child.clip
                            ? Math.max(child.width, child.contentWidth) : child.width
            const right = child.mapToItem(page, painted, 0).x
            if (right > page.width + 0.5)
                out.push((child.objectName || child.text || String(child))
                         + " at " + Math.round(right))
            pastTheEdge(child, page, out)
        }
        return out
    }

    // The narrowest phone, 320 px: the Frames view's header, status
    // line and table, a row's payload among them, lie inside the page
    function test_theFramesFitAPhonesWidth() {
        Theme.developerMode = true
        const page = createTemporaryObject(logComponent, testCase.parent,
                                           { width: 320, height: 700 })
        find(page, "view_frames").clicked()
        const list = find(page, "frameList")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => list.count > 0 && list.itemAtIndex(0) !== null, 2000, "a row shows")
        waitForRendering(page)
        const out = pastTheEdge(page, page, [])
        compare(out.length, 0, "past 320 px: " + out.join(", "))
    }

    // The switch stays where it is when the view changes, though the
    // buttons beside it do: a second click lands on the other option
    function test_theViewSwitchStaysPut() {
        Theme.developerMode = true
        const page = create(logComponent)
        const toFrames = find(page, "view_frames")
        const where = () => toFrames.mapToItem(page, 0, 0).x
        waitForRendering(page)
        const before = where()
        toFrames.clicked()
        verify(find(page, "frameView").visible)
        waitForRendering(page)
        compare(where(), before, "the switch in the Frames view")
        find(page, "view_activity").clicked()
        waitForRendering(page)
        compare(where(), before, "and back in the Activity view")
    }

    // Either order button, and Latest, goes to the newest row: the top in
    // newest-first order, the end in oldest-first, from wherever the view
    // was scrolled, and whether or not the order changes
    function test_theOrderButtonsGoToTheNewestRow() {
        Theme.developerMode = true
        Bridge.model.newestFirst = true
        const page = create(logComponent)
        find(page, "view_frames").clicked()
        const list = find(page, "frameList")
        const latest = find(page, "latestButton")
        tryVerify(() => list.height > 200, 2000, "the table is laid out")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => list.contentHeight > 3 * list.height, 15000, "enough rows to scroll")

        list.positionViewAtEnd()
        verify(!list.atYBeginning, "scrolled away from the newest")
        verify(latest.visible, "Latest offered in newest-first order too")
        find(page, "order_newest").clicked()
        tryVerify(() => list.atYBeginning, 2000, "the same order: back to the top")
        verify(!latest.visible)

        find(page, "order_oldest").clicked()
        tryVerify(() => list.atYEnd, 2000, "oldest-first: the end")

        find(page, "order_newest").clicked()
        tryVerify(() => list.atYBeginning, 2000, "newest-first from oldest-first: the top")

        list.positionViewAtEnd()
        latest.clicked()
        tryVerify(() => list.atYBeginning, 2000, "Latest: the top")
    }

    function test_settingsDeveloperBoxSetsTheModeAndTheMockPreset() {
        const page = create(settingsComponent)
        find(page, "adapterSection").open = true
        find(page, "displaySection").open = true
        const box = find(page, "developerBox")
        const mock = find(page, "preset_mock")
        verify(!box.checked)
        verify(!mock.visible, "only real devices are offered")
        verify(find(page, "preset_j2534").visible)
        verify(find(page, "preset_elm327").visible)

        box.click()   // below the test page's fold once the sections open
        verify(Theme.developerMode, "the box sets the mode")
        verify(mock.visible, "the mock:// preset in developer mode")

        Theme.developerMode = false   // as a restore at launch would
        verify(!box.checked, "the box follows the mode")
        verify(!mock.visible)
    }

    // Paused, the status says what still takes new frames: the recording
    // when one is under way, nothing else on this page
    function test_pausedSaysWhereNewFramesGo() {
        Theme.developerMode = true
        const page = create(logComponent)
        find(page, "view_frames").clicked()
        const status = find(page, "frameStatus")
        for (const recording of [false, true]) {
            Bridge.recording = recording
            compare(Bridge.connectSession("mock://pause-" + recording), "")
            tryVerify(() => Bridge.recordingActive === recording, 5000, "recording " + recording)
            Bridge.logPaused = true
            verify(status.text.indexOf(recording ? "the recording keeps every frame"
                                                 : "leaves new frames out") > 0, status.text)
            Bridge.logPaused = false
            verify(status.text.indexOf("Logging:") === 0, status.text)
            Bridge.disconnectSession()
            tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        }
        Bridge.recording = false
    }

    // The Frames view's rows: a tap selects one, a shift-click the rows
    // from it to another, and Copy puts them on the clipboard, a header
    // line first; Select none lets them go
    function test_selectedFramesAreCopied() {
        Theme.developerMode = true
        const page = create(logComponent)
        find(page, "view_frames").clicked()
        const list = find(page, "frameList")
        const copy = find(page, "copyFrames")
        verify(copy.visible && !copy.enabled, "Copy waits for a selection")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => list.count >= 4 && list.itemAtIndex(3) !== null, 5000, "rows show")
        Bridge.logPaused = true   // holds the rows still under the taps

        const row0 = list.itemAtIndex(0)
        mouseClick(row0, row0.width / 2, row0.height / 2)
        compare(Bridge.model.selectedCount, 1, "a tap selects the row")
        verify(Qt.colorEqual(row0.color, Qt.alpha(Theme.info, 0.22)), "and marks it")
        verify(copy.enabled)
        verify(find(page, "selectedLabel").visible)

        const row2 = list.itemAtIndex(2)
        mouseClick(row2, row2.width / 2, row2.height / 2, Qt.LeftButton, Qt.ShiftModifier)
        compare(Bridge.model.selectedCount, 3, "a shift-click selects through it")

        const row1 = list.itemAtIndex(1)
        mouseClick(row1, row1.width / 2, row1.height / 2)
        compare(Bridge.model.selectedCount, 2, "a tap on a selected row lets it go")

        copy.clicked()
        const pasted = createTemporaryQmlObject("import QtQuick; TextEdit {}", testCase)
        pasted.paste()
        const lines = pasted.text.split("\n").filter(l => l.length > 0)
        compare(lines.length, 3, "a header and two rows: " + pasted.text)
        compare(lines[0], "time_s\tdlc\text\tid\tdata")
        verify(/^\d+\.\d{6}\t\d+\t[-E]\t0x[0-9A-F]+\t[0-9A-F ]*$/.test(lines[1]),
               "a row's columns: " + lines[1])

        const none = find(page, "selectNone")
        none.clicked()
        compare(Bridge.model.selectedCount, 0)
        verify(!copy.enabled && !none.enabled)
    }

    // Ctrl+C (Cmd+C on a Mac) copies the selected rows as Copy does
    function test_ctrlCCopiesTheSelectedFrames() {
        Theme.developerMode = true
        const page = create(logComponent)
        find(page, "view_frames").clicked()
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => Bridge.model.count >= 3, 5000, "rows arrive")
        Bridge.logPaused = true
        Bridge.model.toggleSelected(0)
        compare(Bridge.model.selectedCount, 1)
        // Something else on the clipboard first, so a key that copies
        // nothing can't pass on an earlier test's rows
        const pasted = createTemporaryQmlObject("import QtQuick; TextEdit {}", testCase)
        pasted.text = "not the rows"
        pasted.selectAll()
        pasted.copy()
        pasted.text = ""
        page.forceActiveFocus()
        keySequence(StandardKey.Copy)
        pasted.paste()
        const lines = pasted.text.split("\n").filter(l => l.length > 0)
        compare(lines.length, 2, "a header and the row: " + pasted.text)
        compare(lines[0], "time_s\tdlc\text\tid\tdata")
        verify(/^\d+\.\d{6}\t\d+\t[-E]\t0x[0-9A-F]+\t[0-9A-F ]*$/.test(lines[1]),
               "a row's columns: " + lines[1])
    }

    // A phone can't select the log's text: Copy puts all of it on the
    // clipboard (docs/bench-sessions/2026-10-28-android-2.md)
    function test_copyPutsTheWholeLogOnTheClipboard() {
        const page = create(logComponent)
        Bridge.logEvent("a line to copy")
        const log = find(page, "activityLog")
        tryVerify(() => log.text.indexOf("a line to copy") >= 0, 2000, "the line is logged")
        find(page, "copyLog").clicked()
        compare(log.selectedText, "", "nothing is left selected")
        const pasted = createTemporaryQmlObject("import QtQuick; TextEdit {}", testCase)
        pasted.paste()
        verify(pasted.text.indexOf("Ready.") >= 0 && pasted.text.indexOf("a line to copy") >= 0,
               "the whole log was copied: " + pasted.text)
    }
}
