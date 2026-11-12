// File/layer: tests/qml/tst_log.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.log (test_qml)

import QtCore
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
        Bridge.model.setFilter("", "", 0, 0)
        Bridge.logPaused = false
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Bridge.recording = false   // only between sessions
        Harness.clearFakeKnobs()
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
        // Prose: the text face, wrapped at words, never inside one
        compare(log.wrapMode, TextEdit.WordWrap)
        verify(log.font.family !== Theme.monoFamily, "not mono: " + log.font.family)
        compare(Bridge.connectSession("mock://demo"), "")
        verify(/\d\d:\d\d:\d\d\.\d\d\d  Connected: mock:\/\/demo/.test(log.text),
               "stamped like the surrogate's log: " + log.text)
        verify(log.text.includes("Session: mock transport; profile mock"))
    }

    // Show log lands on the newest activity line, whichever view was up and
    // wherever the log was scrolled to
    function test_showActivityEndLandsOnTheNewestLine() {
        Theme.developerMode = true
        const page = create(logComponent)
        for (let i = 0; i < 150; ++i)
            page.appendLog("line " + i)
        find(page, "view_frames").clicked()
        verify(page.showingFrames, "the Frames view is up")
        find(page, "view_activity").clicked()
        const view = find(page, "activityView")
        const flick = view.contentItem
        tryVerify(() => flick.contentHeight > flick.height * 2, 2000, "a log to scroll")
        flick.contentY = 0
        find(page, "view_frames").clicked()
        page.showActivityEnd()
        verify(!page.showingFrames, "the activity view is shown")
        verify(view.visible, "visible")
        tryVerify(() => flick.atYEnd, 2000, "scrolled to its end")
        verify(flick.contentY > 0, "not left at the top")
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

    // The Activity | Frames switch is every user's; developer mode adds
    // nothing to the Log page's views
    function test_theFramesViewIsForEveryone() {
        const page = create(logComponent)
        const activity = find(page, "activityView")
        const frames = find(page, "frameView")
        const toFrames = find(page, "view_frames")
        verify(!Theme.developerMode)
        verify(toFrames.visible, "the switch, with developer mode off")
        verify(activity.visible, "the activity log until switched")
        verify(!frames.visible)
        toFrames.clicked()
        verify(frames.visible, "the raw frames")
        verify(!activity.visible)
        find(page, "view_activity").clicked()
        verify(activity.visible, "back to the activity log")
        verify(!frames.visible)

        Theme.developerMode = true
        toFrames.clicked()
        verify(frames.visible, "developer mode changes none of it")
        Theme.developerMode = false
        verify(frames.visible, "and turning it off leaves the view")
    }

    // A hidden table takes no rows, so a frame makes no delegate on any other
    // page; shown, it holds the ring's rows at once, not only the next ones
    function test_aHiddenTableHoldsNoRowsAndShowsTheRingWhenShown() {
        const page = create(logComponent)
        const list = find(page, "frameList")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => Bridge.model.count >= 5, 5000, "frames arrive")
        compare(list.count, 0, "hidden: no rows")

        Theme.developerMode = true
        compare(list.count, 0, "the activity log still shows")
        const held = Bridge.model.count
        find(page, "view_frames").clicked()
        verify(list.count >= held, list.count + " rows of the " + held + " held")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "a row is drawn")

        find(page, "view_activity").clicked()
        compare(list.count, 0, "hidden again: no rows")
    }

    // A hidden table keeps its place: paused and scrolled, it shows the same
    // first row when shown again, and doesn't follow
    function test_aHiddenTableKeepsItsPlace() {
        Theme.developerMode = true
        const page = create(logComponent)
        find(page, "view_frames").clicked()
        const list = find(page, "frameList")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => list.count >= 60, 10000, "frames to scroll through")
        Bridge.logPaused = true
        list.follow = false
        list.positionViewAtIndex(20, ListView.Beginning)
        tryCompare(list, "keptSeq", Bridge.model.seqAt(20), 2000, "noted while shown")
        find(page, "view_activity").clicked()
        compare(list.count, 0, "hidden: no rows")
        find(page, "view_frames").clicked()
        tryVerify(() => list.count > 0 && list.indexAt(1, list.contentY + 1) === 20, 2000,
                  "the same first row")
        verify(!list.follow, "and it doesn't follow")
    }

    // The place is the first row's arrival number, not its index: frames
    // that arrive while the table is hidden land above it (newest first), so
    // the same index is another frame. Shown again, the same frame leads the
    // view
    function test_aHiddenTableKeepsItsPlaceByArrivalWhileFramesArrive() {
        Theme.developerMode = true
        const page = create(logComponent)
        find(page, "view_frames").clicked()
        const list = find(page, "frameList")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => list.count >= 60, 10000, "frames to scroll through")
        Bridge.logPaused = true
        list.follow = false
        list.positionViewAtIndex(20, ListView.Beginning)
        const kept = Bridge.model.seqAt(20)
        tryCompare(list, "keptSeq", kept, 2000, "noted while shown")
        find(page, "view_activity").clicked()
        compare(list.count, 0, "hidden: no rows")
        const held = Bridge.model.count
        Bridge.logPaused = false
        tryVerify(() => Bridge.model.count >= held + 30, 10000, "frames arrive while hidden")
        Bridge.logPaused = true
        find(page, "view_frames").clicked()
        tryVerify(() => list.count > 0, 2000, "shown again")
        tryVerify(() => Bridge.model.seqAt(list.indexAt(1, list.contentY + 1)) === kept, 2000,
                  "the same frame leads the view")
        verify(list.indexAt(1, list.contentY + 1) >= 50, "its index moved down past the new rows")
        verify(!list.follow, "and it doesn't follow")
    }

    // A place whose row the filter leaves out comes back at the next row in
    // the view's order, not at its old index
    function test_aHiddenTablesPlaceWithItsRowFilteredOutFallsToTheNext() {
        Theme.developerMode = true
        const page = create(logComponent)
        find(page, "view_frames").clicked()
        const list = find(page, "frameList")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => list.count >= 60, 10000, "frames to scroll through")
        Bridge.logPaused = true
        Bridge.model.newestFirst = false
        list.follow = false
        list.positionViewAtIndex(20, ListView.Beginning)
        const kept = Bridge.model.seqAt(20)
        tryCompare(list, "keptSeq", kept, 2000, "noted while shown")
        tryVerify(() => list.itemAtIndex(20) !== null, 2000, "the kept row is drawn")
        const keptUs = list.itemAtIndex(20).model.timestampUs
        find(page, "view_activity").clicked()
        // Only the frames after the kept one: from a microsecond on
        const from = ((keptUs + 1) / 1e6).toFixed(6)
        compare(Bridge.model.setFilter("", "", 0, 0, "", from + "-").times, "")
        find(page, "view_frames").clicked()
        tryVerify(() => list.count > 0, 2000, "shown again")
        tryVerify(() => Bridge.model.seqAt(list.indexAt(1, list.contentY + 1)) > kept, 2000,
                  "the first row to arrive after the kept one")
        compare(list.indexAt(1, list.contentY + 1), 0, "the oldest shown")
    }

    // Clear forgets the place a hidden table keeps: scrolled up, cleared,
    // with rows again, the table comes back following, not at the old row
    function test_clearForgetsAHiddenTablesPlace() {
        Theme.developerMode = true
        const page = create(logComponent)
        find(page, "view_frames").clicked()
        const list = find(page, "frameList")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => list.count >= 60, 10000, "frames to scroll through")
        Bridge.logPaused = true
        list.follow = false
        list.positionViewAtIndex(20, ListView.Beginning)
        tryVerify(() => list.keptSeq > 0, 2000, "noted while shown")
        find(page, "clearFrames").clicked()
        tryCompare(list, "count", 0, 2000, "cleared")
        compare(list.follow, true, "following again")
        Bridge.logPaused = false
        tryVerify(() => list.count >= 5, 10000, "rows again")
        Bridge.logPaused = true
        find(page, "view_activity").clicked()
        compare(list.count, 0, "hidden: no rows")
        find(page, "view_frames").clicked()
        tryVerify(() => list.count > 0, 2000, "shown again")
        verify(list.follow, "it follows, not at the old index")
        verify(!find(page, "latestButton").visible, "and Latest is hidden")
    }

    // The list is one named node; its rows' labels are kept out of the tree
    function test_theFrameRowsLabelsAreIgnored() {
        Theme.developerMode = true
        const page = create(logComponent)
        find(page, "view_frames").clicked()
        const list = find(page, "frameList")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => list.count > 0 && list.itemAtIndex(0) !== null, 2000, "a row shows")
        compare(list.Accessible.name, "Frames")
        let labels = 0
        const walk = item => {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i]
                if (child.textFormat !== undefined) {
                    ++labels
                    verify(child.Accessible.ignored, "ignored: '" + child.text + "'")
                }
                walk(child)
            }
        }
        walk(list.itemAtIndex(0))
        compare(labels, 7, "every column's label")
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

    // The Time header turns the order and goes to the newest row: the top
    // in newest-first order, the end in oldest-first, from wherever the
    // view was scrolled; Latest goes there too
    function test_theTimeHeaderTurnsTheOrder() {
        Theme.developerMode = true
        Bridge.model.newestFirst = true
        const page = create(logComponent)
        find(page, "view_frames").clicked()
        const list = find(page, "frameList")
        const latest = find(page, "latestButton")
        tryVerify(() => list.height > 200, 2000, "the table is laid out")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => list.contentHeight > 3 * list.height, 15000, "enough rows to scroll")

        const order = find(page, "orderToggle")
        compare(order.contentItem.text, "Time ↓ (s)")
        list.positionViewAtEnd()
        verify(!list.atYBeginning, "scrolled away from the newest")
        verify(latest.visible, "Latest offered in newest-first order too")
        latest.clicked()
        tryVerify(() => list.atYBeginning, 2000, "Latest: back to the top")
        verify(!latest.visible)

        order.clicked()
        verify(!Bridge.model.newestFirst)
        compare(order.contentItem.text, "Time ↑ (s)")
        tryVerify(() => list.atYEnd, 2000, "oldest-first: the end")

        order.clicked()
        verify(Bridge.model.newestFirst)
        tryVerify(() => list.atYBeginning, 2000, "newest-first from oldest-first: the top")

        list.positionViewAtEnd()
        latest.clicked()
        tryVerify(() => list.atYBeginning, 2000, "Latest: the top")
    }

    function test_settingsDeveloperBoxSetsTheModeAndTheMockPreset() {
        const page = create(settingsComponent)
        const box = find(page, "developerBox")
        const mock = find(page, "preset_mock")
        verify(!box.checked)
        find(page, "presetsToggle").clicked()   // the presets start closed
        verify(find(page, "preset_j2534").visible === (Qt.platform.os === "windows"),
               "j2534:// where there is a J2534 driver")
        verify(find(page, "preset_elm327").visible)

        box.click()   // on the Advanced page, which is not shown
        verify(Theme.developerMode, "the box sets the mode")
        verify(mock.visible, "the mock:// preset, with the mode on or off")

        Theme.developerMode = false   // as a restore at launch would
        verify(!box.checked, "the box follows the mode")
        verify(mock.visible)
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
        const tools = find(page, "frameTools")
        const bar = find(page, "selectionBar")
        verify(!bar.visible && tools.visible, "the selection bar waits for a selection")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => list.count >= 4 && list.itemAtIndex(3) !== null, 5000, "rows show")
        Bridge.logPaused = true   // holds the rows still under the taps

        const row0 = list.itemAtIndex(0)
        mouseClick(row0, row0.width / 2, row0.height / 2)
        compare(Bridge.model.selectedCount, 1, "a tap selects the row")
        verify(Qt.colorEqual(row0.color, Qt.alpha(Theme.text, 0.12)), "and marks it in the text's tone")
        verify(bar.visible && !tools.visible, "the selection bar in the tools' place")
        verify(copy.visible && copy.enabled)
        compare(find(page, "selectedLabel").text, "1 selected")
        verify(Qt.colorEqual(find(page, "selectedLabel").color, Theme.textDim), "a count, in the dim tone")

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
        verify(!bar.visible && tools.visible, "the tools come back")
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

    // ---- The filter and By ID (framefilter.h) ----

    // The frames a mock session puts in the table, held still. The mock
    // sends its extended ID every 16th frame and 0x0C0 otherwise, so any
    // 17 in a row hold both
    function heldFrames(page) {
        find(page, "view_frames").clicked()
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => Bridge.model.heldCount >= 17, 10000, "frames arrive")
        Bridge.logPaused = true
    }

    // The filter's fields narrow the table, the toggle counts them, the
    // status says how many of how many show; Clear the filter shows all
    function test_theFilterNarrowsTheTable() {
        Theme.developerMode = true
        const page = create(logComponent)
        heldFrames(page)
        const toggle = find(page, "filterToggle")
        const panel = find(page, "filterPanel")
        verify(!panel.visible, "closed until asked for")
        compare(toggle.text, "Filter")
        toggle.clicked()
        verify(panel.visible)

        const held = Bridge.model.heldCount
        const ids = find(page, "filterIds")
        ids.text = "C0"
        page.applyFilter()
        compare(toggle.text, "Filter (1)")
        verify(Bridge.model.count > 0 && Bridge.model.count < held,
               Bridge.model.count + " of " + held)
        const list = find(page, "frameList")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "a row shows")
        for (let i = 0; i < Math.min(list.count, 5); ++i)
            compare(Bridge.model.data(Bridge.model.index(i, 0), 261), 0xC0)   // ArbitrationIdRole
        const status = find(page, "filterStatus")
        verify(status.visible)
        compare(status.text, "Showing " + Bridge.model.count + " of " + held)

        // A field that doesn't parse says why and keeps the filter
        const count = Bridge.model.count
        find(page, "filterData").text = "1G"
        page.applyFilter()
        compare(find(page, "filterDataProblem").text, "\"1G\" isn't a hex byte or ??")
        compare(Bridge.model.count, count)
        find(page, "filterData").text = ""
        page.applyFilter()
        verify(!find(page, "filterDataProblem").visible)

        // A flag no mock frame carries shows none, and the table says so
        const flag = find(page, "filterFlag")
        flag.currentIndex = 7   // Error
        page.applyFilter()
        compare(Bridge.model.count, 0)
        compare(toggle.text, "Filter (2)")

        // Sent: no transport reports its own frames yet, and it says so
        verify(!find(page, "sentNote").visible)
        find(page, "direction_2").clicked()
        verify(find(page, "sentNote").visible)

        find(page, "filterClear").clicked()
        compare(Bridge.model.filterTerms, 0)
        compare(Bridge.model.count, held)
        compare(ids.text, "")
        verify(!status.visible)
    }

    // By ID: one row per ID over what the filter shows; a tap on one reads
    // its frames in the table
    function test_byIdListsEachIdAndATapReadsIts() {
        Theme.developerMode = true
        const page = create(logComponent)
        heldFrames(page)
        find(page, "table_byId").clicked()
        const ids = find(page, "idList")
        verify(ids.visible)
        verify(!find(page, "frameList").visible)
        // Shown only now, the list is laid out a frame later: a tap before
        // that lands on a row with no size
        tryVerify(() => ids.count >= 2 && ids.itemAtIndex(0) !== null && ids.height > 0
                  && ids.itemAtIndex(0).width > 0, 3000, "the IDs show, laid out")
        let total = 0
        for (let i = 0; i < ids.count; ++i)
            total += Bridge.frameIds.data(Bridge.frameIds.index(i, 0), 259)   // CountRole
        compare(total, Bridge.model.heldCount, "every frame held, counted once")

        const first = Bridge.frameIds.data(Bridge.frameIds.index(0, 0), 257)   // ArbIdRole
        const row = ids.itemAtIndex(0)
        mouseClick(row, row.width / 2, row.height / 2)
        verify(!page.byId, "back to the frames")
        verify(find(page, "filterPanel").visible)
        compare(find(page, "filterIds").text, first.toString(16).toUpperCase())
        verify(Bridge.model.count > 0)
        for (let i = 0; i < Math.min(Bridge.model.count, 5); ++i)
            compare(Bridge.model.data(Bridge.model.index(i, 0), 261), first)
        compare(page.rateText(0), "-")
        compare(page.rateText(2.25), "2.3/s")
        compare(page.rateText(20.4), "20/s")
    }

    // One number as a standard and as an extended ID: each By ID row's
    // tap shows that row's own frames alone, whatever its width
    function test_aTapShowsItsOwnWidthsFramesAlone() {
        Theme.developerMode = true
        const page = create(logComponent)
        find(page, "view_frames").clicked()
        let rows = "host_us,timestamp_us,id,flags,data\n"
        for (let i = 0; i < 6; ++i) {
            const t = 1000 + i * 100000
            rows += t + "," + t + ",0x7E8,0x0,04 41 0C 1A F8\n"
            if (i < 4)
                rows += (t + 50000) + "," + (t + 50000) + ",0x7E8,0x1,04 62 F1 90 01\n"
        }
        const url = Harness.writeScratchFile("two-widths.csv", rows).toString()
        const path = decodeURIComponent(url.replace(/^file:\/\//, "")).replace(/^\/([A-Za-z]:)/, "$1")
        compare(Bridge.connectSession("replay://" + path + "?speed=max"), "")
        tryVerify(() => Bridge.model.heldCount === 10, 5000, "the ten frames arrive")

        for (const extended of [false, true]) {
            find(page, "table_byId").clicked()
            const ids = find(page, "idList")
            tryVerify(() => ids.count === 2 && ids.itemAtIndex(1) !== null && ids.height > 0
                      && ids.itemAtIndex(1).width > 0, 3000, "both rows, laid out")
            let at = -1
            for (let i = 0; i < ids.count; ++i) {
                if (Bridge.frameIds.data(Bridge.frameIds.index(i, 0), 258) === extended)   // ExtendedRole
                    at = i
            }
            verify(at >= 0, "the row of each width")
            compare(Bridge.frameIds.data(Bridge.frameIds.index(at, 0), 257), 0x7E8)   // ArbIdRole
            const row = ids.itemAtIndex(at)
            mouseClick(row, row.width / 2, row.height / 2)
            verify(!page.byId, "back to the frames")
            compare(Bridge.model.count, extended ? 4 : 6,
                    (extended ? "the extended" : "the standard") + " row's frames alone")
            find(page, "filterClear").clicked()
            compare(Bridge.model.count, 10)
        }
    }

    // Length and Time: lengths in bytes and a span of the table's seconds.
    // By ID marks the bytes that changed from an ID's frame before; a field
    // that doesn't parse says why and keeps its part; a By ID tap leaves
    // both fields as they are, and Clear the filter empties them
    function test_theLengthAndTimeFieldsNarrowTheTable() {
        Theme.developerMode = true
        const page = create(logComponent)
        find(page, "view_frames").clicked()
        // A frame a second from 0 s, five bytes long, then three
        let rows = "host_us,timestamp_us,id,flags,data\n"
        for (let i = 0; i < 6; ++i) {
            const t = 1000 + i * 1000000
            rows += t + "," + t + ",0x7E8,0x0," + (i % 2 === 0 ? "04 41 0C 1A F8" : "02 41 0C") + "\n"
        }
        const url = Harness.writeScratchFile("lengths-and-times.csv", rows).toString()
        const path = decodeURIComponent(url.replace(/^file:\/\//, "")).replace(/^\/([A-Za-z]:)/, "$1")
        compare(Bridge.connectSession("replay://" + path + "?speed=max"), "")
        tryVerify(() => Bridge.model.heldCount === 6, 5000, "the six frames arrive")

        // The newest, 02 41 0C, after 04 41 0C 1A F8: its first byte marked
        find(page, "table_byId").clicked()
        const ids = find(page, "idList")
        tryVerify(() => ids.count === 1 && ids.itemAtIndex(0) !== null && ids.height > 0
                  && ids.itemAtIndex(0).width > 0, 3000, "the row, laid out")
        compare(Bridge.frameIds.data(Bridge.frameIds.index(0, 0), 262), [0])   // ChangedRole
        compare(findChild(ids.itemAtIndex(0), "idPayload").text,
                "<font color=\"" + Theme.warning + "\"><b>02</b></font> 41 0C")
        find(page, "table_frames").clicked()

        const toggle = find(page, "filterToggle")
        toggle.clicked()
        const lengths = find(page, "filterLengths")
        const times = find(page, "filterTimes")
        lengths.text = "5"
        page.applyFilter()
        compare(Bridge.model.count, 3)
        compare(toggle.text, "Filter (1)")
        times.text = "2-"
        page.applyFilter()
        compare(Bridge.model.count, 2, "2 and 4 s")
        compare(toggle.text, "Filter (2)")
        verify(find(page, "filterClear").enabled)

        const lengthsProblem = find(page, "filterLengthsProblem")
        const timesProblem = find(page, "filterTimesProblem")
        verify(!lengthsProblem.visible && !timesProblem.visible)
        lengths.text = "5-"
        times.text = "2"
        page.applyFilter()
        compare(lengthsProblem.text,
                "\"5-\" isn't a length in bytes (up to 4096) or a range like 1-7")
        compare(timesProblem.text,
                "\"2\" isn't a range of seconds like 12.5-30, 12.5- (from then on) or -30 (up to then)")
        verify(lengthsProblem.visible && timesProblem.visible)
        compare(Bridge.model.count, 2, "both keep their part")
        lengths.text = "5"
        times.text = "2-"
        page.applyFilter()
        verify(!lengthsProblem.visible && !timesProblem.visible)

        // A By ID tap narrows the ID and leaves the length and the time
        find(page, "table_byId").clicked()
        tryVerify(() => ids.count === 1 && ids.itemAtIndex(0) !== null && ids.height > 0
                  && ids.itemAtIndex(0).width > 0, 3000, "the row, laid out")
        compare(Bridge.frameIds.data(Bridge.frameIds.index(0, 0), 262), [],
                "the frames shown are alike")
        const row = ids.itemAtIndex(0)
        mouseClick(row, row.width / 2, row.height / 2)
        verify(!page.byId, "back to the frames")
        compare(lengths.text, "5")
        compare(times.text, "2-")
        compare(Bridge.model.count, 2)

        find(page, "filterClear").clicked()
        compare(lengths.text, "")
        compare(times.text, "")
        compare(Bridge.model.filterTerms, 0)
        compare(Bridge.model.count, 6)
    }

    // Export CSV says what it leaves out and that Copy doesn't, then what
    // it did; a target it can't write says so (the file itself:
    // BridgeTest.ExportFramesWritesTheFileAndLogsOneLine)
    function test_exportCsvSaysItLeavesIdentificationOut() {
        Theme.developerMode = true
        const page = create(logComponent)
        heldFrames(page)
        const button = find(page, "exportFrames")
        verify(button.visible && button.enabled)
        const words = find(page, "exportWords")
        verify(words.visible)
        compare(words.text, "Export CSV leaves identification replies out; "
                            + "Copy copies the rows as they are.")
        verify(Qt.colorEqual(words.color, Theme.textDim))
        const folder = StandardPaths.writableLocation(StandardPaths.TempLocation)
        verify(!page.exportFramesTo(folder), "a folder isn't a file")
        compare(words.text, "Couldn't write the file")
        verify(Qt.colorEqual(words.color, Theme.danger))
    }

    // The narrowest phone, 320 px, with the filter open, By ID shown and
    // rows selected: nothing lies past the page's edge
    function test_theFilterAndByIdFitAPhone() {
        Theme.developerMode = true
        const page = createTemporaryObject(logComponent, testCase.parent,
                                           { width: 320, height: 832 })
        heldFrames(page)
        find(page, "filterToggle").clicked()
        find(page, "direction_2").clicked()
        waitForRendering(page)
        compare(pastTheEdge(page, page, []).join("; "), "", "the frames and the filter")
        find(page, "table_byId").clicked()
        tryVerify(() => find(page, "idList").itemAtIndex(0) !== null, 3000, "the IDs show")
        waitForRendering(page)
        compare(pastTheEdge(page, page, []).join("; "), "", "By ID")
        Bridge.model.toggleSelected(0)
        find(page, "table_frames").clicked()
        waitForRendering(page)
        compare(pastTheEdge(page, page, []).join("; "), "", "the selection bar")
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

    // ADR-0060: each module's session reaches the activity log, a refusal
    // with its code (the fake's module has no extended session)
    function test_aModulesSessionIsInTheActivityLog() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        Harness.armFakeKnob("ECUIO_FAKE_MODULE_DIDS", "7E9:D801=00 12")
        const page = create(logComponent)
        const log = find(page, "activityLog")
        compare(Bridge.connectSession("j2534://" + Harness.fakeDllPath
                                      + "?profile=uds-generic&did=1E1C,7E9:D801"
                                      + "&session=03&identify=0&diagnose=0"), "")
        tryVerify(() => log.text.includes(
                      "ECU 0x7E8: reading in the extended session (50 03)"),
                  5000, "the engine ECU's session line")
        tryVerify(() => log.text.includes(
                      "ECU 0x7E9: refused the extended session (subFunctionNotSupported, "
                      + "7F 10 12): reading in the default session"),
                  5000, "the module's refusal")
    }
}
