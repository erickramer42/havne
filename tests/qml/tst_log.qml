// File/layer: tests/qml/tst_log.qml | Qt Quick Test, QML tier
// Testing: the Log page shows the activity log: "Ready.", then each line
//          the bridge logs, stamped HH:MM:SS.mmm, kept to its newest
//          lines. With developer mode off it shows nothing else; on, the
//          Activity | Frames switch shows the raw frame table, staying in
//          place in either view, and turning developer mode off goes
//          back to the activity log. Settings'
//          developer box sets Theme.developerMode and the mock:// preset
//          follows it. Copy puts the whole log on the clipboard and
//          leaves nothing selected. How the page looks is manual smoke.

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
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
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
