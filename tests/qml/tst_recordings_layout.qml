// File/layer: tests/qml/tst_recordings_layout.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.recordings_layout (test_qml)

import QtQuick
import QtTest
import App
import AppTest

RecordingsKit {
    id: testCase
    name: "Recordings"

    // The viewer's Frames view (ADR-0049's appendix "frames read back"):
    // the switch is there for a recording with frames, the table holds the
    // rows of the part in view, times on the viewer's axis, and a tap puts
    // the cursor at the row's time
    function test_theFramesViewShowsThePartInView() {
        const page = openFindings()
        const values = find(page, "detailView_values")
        const frames = find(page, "detailView_frames")
        verify(values.visible && frames.visible, "a recording with frames has the switch")
        compare(page.detailView, "values", "the values each time it opens")
        verify(find(page, "signalRows").visible)
        verify(!find(page, "viewerFrames").visible)

        mouseClick(frames)
        verify(page.framesShown)
        verify(find(page, "viewerFrames").visible)
        verify(!find(page, "signalRows").visible, "the values give way")
        verify(!find(page, "recordingFilter").visible)
        const words = find(page, "framesWords")
        tryVerify(() => /^\d+ frames? in view$/.test(words.text), 10000,
                  "the part in view is read: " + words.text)
        const total = parseInt(words.text)
        verify(total > 0, words.text)
        const list = find(find(page, "viewerFrameTable"), "frameList")
        compare(list.count, total)

        // The part in view alone: zoomed to the first second there are
        // fewer rows, once the view has rested
        page.cursorsPart.setView(0, 1000)
        tryVerify(() => parseInt(words.text) < total, 10000,
                  "the rows follow the view: " + words.text)

        // The rows run oldest first in the viewer, each at its host_us:
        // the first at 0.100 s, the frame's own 0.101 s not shown
        tryVerify(() => list.itemAtIndex(0) !== null)
        const first = list.itemAtIndex(0)
        compare(first.model.timestampUs, 100000)

        // A tap puts the cursor at the row's time
        verify(!page.hasCursor)
        mouseClick(find(first, "frameRowArea"))
        verify(page.hasCursor)
        compare(page.cursorMs, 100)

        // A filter on the frames carries to nothing else: the next
        // recording opened has none
        mouseClick(find(page, "framesFilterToggle"))
        const ids = find(page, "filterIds")
        ids.text = "7E8"
        find(page, "filterPanel").apply()
        compare(find(page, "framesFilterToggle").text, "Filter (1)")

        // Back to the values; the frames are let go
        mouseClick(values)
        verify(!page.framesShown)
        verify(find(page, "signalRows").visible)
        compare(page.detailView, "values")

        page.openPart.backToList()
        page.openPart.open(findingsRow)
        tryVerify(() => find(page, "signalRows").visible, 10000, "it opens again")
        compare(find(page, "filterIds").text, "")
        compare(find(page, "framesFilterToggle").text, "Filter")
    }

    // The viewer's By ID list: one row per ID in view, and a tap on one
    // puts its ID in the filter and returns to the table
    function test_theViewersByIdListTakesATapAsAFilter() {
        const page = openFindings()
        mouseClick(find(page, "detailView_frames"))
        const words = find(page, "framesWords")
        tryVerify(() => /^\d+ frames? in view$/.test(words.text), 10000, "the frames in view are counted")
        verify(!find(page, "viewerFrameIds").visible, "the table first")

        mouseClick(find(page, "framesTable_byId"))
        verify(page.framesById)
        const idsView = find(page, "viewerFrameIds")
        verify(idsView.visible)
        verify(!find(find(page, "viewerFrameTable"), "frameList").visible)
        const ids = find(idsView, "idList")
        tryVerify(() => ids.count >= 1 && ids.itemAtIndex(0) !== null && ids.height > 0
                  && ids.itemAtIndex(0).width > 0, 10000, "the IDs show, laid out")
        verify(idsView.refreshing, "read again while it shows")

        const row = ids.itemAtIndex(0)
        const id = row.model.arbId
        mouseClick(find(row, "idRowArea"))
        verify(!page.framesById, "back to the table")
        verify(page.framesFilterOpen, "the filter opens")
        compare(find(page, "filterIds").text, id.toString(16).toUpperCase())
        compare(find(page, "framesFilterToggle").text, "Filter (2)", "the ID and its width")
        verify(!idsView.refreshing, "not read while hidden")
    }

    function test_aRecordingWithNoFramesHasNoFramesSwitch() {
        const page = createPage()
        const folder = Harness.importCsvFixture("logger-a", "viewer-frames-import")
        verify(folder !== "", "the import is made")
        page.openPart.open({ title: "Imported", folder: folder })
        tryVerify(() => find(page, "signalRows").visible, 10000, "it opens")
        verify(!find(page, "detailView_frames").visible)
        verify(!find(page, "detailView_values").visible)
        compare(find(page, "viewerFrames").visible, false)
    }

    // A part with more rows than the table holds says where the table
    // begins; a zoom into fewer drops the note
    function test_aLongPartSaysWhereTheTableBegins() {
        const folder = Harness.writeFramesRecording("viewer-frames-long", 10200, 1000)
        verify(folder !== "")
        const page = createPage()
        page.openPart.open({ title: "Long", folder: folder })
        // Each wait covers a read of 10,200 rows and a 10,000-row table
        // on whatever else the machine is doing (a build beside the
        // suite took the open past 10 s); the case measures the cap and
        // the words, nothing about time, so the waits are long
        const longWait = 60000
        tryVerify(() => page.viewing && find(page, "detailView_frames").visible, longWait)
        // The switch reaches its place at the layout's next polish: a press
        // before that lands where it isn't
        verify(waitForPolish(page.Window.window), "the viewer laid out")
        mouseClick(find(page, "detailView_frames"))
        verify(page.framesShown, "the press reached the switch")
        const words = find(page, "framesWords")
        tryVerify(() => words.text === "The first 10,000 frames from 0.0 s: zoom in for the rest",
                  longWait, "the table's cap is said")
        compare(find(find(page, "viewerFrameTable"), "frameList").count, 10000)
        page.cursorsPart.setView(0, 2000)
        tryVerify(() => words.text === "2000 frames in view", longWait, "the rows of 2 s")

        // A view that does not start at the recording's start: the rows
        // are those from its left edge
        page.cursorsPart.setView(5000, 6000)
        tryVerify(() => words.text === "1001 frames in view", longWait, "the rows from 5 s")
        const list = find(find(page, "viewerFrameTable"), "frameList")
        tryVerify(() => list.itemAtIndex(0) !== null, longWait)
        compare(list.itemAtIndex(0).model.timestampUs, 5000000)
    }

    // On a phone the strings that were cut at 384 dp are short forms, the
    // way back is a quiet arrow, and the name is the viewer's title
    function test_aPhoneGetsTheShortStrings() {
        const page = openFindings()
        const title = find(page, "openedTitle")
        const back = find(page, "backToList")
        const header = find(page, "recordingsHeader")
        compare(title.text, findingsRow.title)
        compare(title.elide, Text.ElideMiddle, "the folder's date at the end stays")
        compare(back.text, "All recordings")
        verify(header.visible, "the desktop keeps the page's heading")
        Theme.compact = true
        try {
            compare(find(page, "cursorTime").text, "Tap for values")
            verify(title.visible, "the name in the title row")
            compare(back.text, "←")
            verify(back.flat, "quiet")
            compare(back.Accessible.name, "All recordings")
            verify(!header.visible, "the tab names the page")
            page.cursorsPart.setCursor(5000)
            compare(find(page, "cursorTime").text, "At 5.0 s", "a cursor reads the same")
        } finally {
            Theme.compact = false
        }
        verify(header.visible && back.text === "All recordings", "and back")
    }

    // The desktop's one order: the chart, its zoom row, the switch and the
    // filter, the rows, then About this recording, Replay and Save report in
    // the title row. A phone upright pins the chart and gives About a view
    // of its own: the zoom row, the switch and the filter head the rows,
    // and About's view holds the recording's lines with Replay and Save
    // report among them
    function test_theViewersOrderIsChartRowsThenAbout_data() {
        return [{ tag: "desktop", w: 1280, h: 720, phone: false },
                { tag: "phone", w: 384, h: 832, phone: true }]
    }

    function test_theViewersOrderIsChartRowsThenAbout(data) {
        const page = openFindings()
        Theme.compact = data.phone
        try {
            page.width = data.w
            page.height = data.h
            verify(waitForPolish(page.Window.window), "laid out")
            const yOf = name => {
                const item = find(page, name)
                verify(item.visible, name + " shows")
                return item.mapToItem(page, 0, 0).y
            }
            const middleOf = name => {
                const item = find(page, name)
                return item.mapToItem(page, 0, item.height / 2).y
            }
            const inOrder = order => {
                const ys = order.map(yOf)
                for (let i = 1; i < order.length; ++i)
                    verify(ys[i] > ys[i - 1],
                           order[i] + " (" + ys[i] + ") under " + order[i - 1] + " (" + ys[i - 1] + ")")
            }
            if (data.phone) {
                verify(page.pinnedViewer, "the phone's viewer is pinned")
                inOrder(["openedTitle", "chartFrame", "chartSplitter", "zoomAll", "detailView_values",
                         "recordingFilter"])
                const rows = find(page, "signalRows")
                verify(rows.itemAtIndex(0).mapToItem(page, 0, 0).y > yOf("recordingFilter"),
                       "the first row under the filter")
                verify(!find(page, "aboutHeading").visible, "the switch names About, no heading does")
                verify(!find(page, "recordingInfo").visible, "About waits for its view")
                find(page, "detailView_about").clicked()
                verify(waitForPolish(page.Window.window), "About laid out")
                verify(!rows.visible, "the rows give way to About")
                const findings = find(page, "findingList")
                // the head is the list's first item, whatever height the font gives it
                const headAt = () => findings.headerItem.mapToItem(findings, 0, 0).y
                // a condition wait without tryVerify, which would end the case
                // before the numbers below are read
                const until = Date.now() + 2000
                while (headAt() < -0.5 && Date.now() < until)
                    wait(20)
                verify(headAt() >= -0.5, "About's list at its start: head at " + headAt()
                       + ", contentY " + findings.contentY + ", originY " + findings.originY
                       + ", head height " + findings.headerItem.height)
                inOrder(["chartSplitter", "zoomAll", "detailView_about", "recordingInfo", "decodeChoice",
                         "checkSummary"])
                const save = yOf("saveReport")
                verify(save > yOf("recordingInfo") && save < yOf("decodeChoice"), "Save report in About")
                compare(yOf("replayView"), save, "Replay beside it")
                compare(yOf("compareButton"), save, "and Compare")
            } else {
                inOrder(["openedTitle", "chartFrame", "chartSplitter", "zoomAll", "recordingFilter",
                         "signalRows", "aboutHeading", "recordingInfo", "decodeChoice", "checkSummary"])
                const rows = find(page, "signalRows")
                compare(rows.itemAtIndex(0).mapToItem(rows, 0, 0).y, 0, "no head over the desktop's rows")
                verify(rows.contentHeight <= rows.height + 0.5, "the rows hold their list: "
                       + rows.contentHeight + " in " + rows.height)
                const save = yOf("saveReport")
                const replay = yOf("replayView")
                verify(save < yOf("chartFrame") && replay < yOf("chartFrame"), "both in the title row")
                verify(yOf("compareButton") < yOf("chartFrame"), "and Compare, beside Export")
                const sw = find(page, "detailView_values")
                verify(sw.visible && Math.abs(middleOf("detailView_values") - middleOf("recordingFilter")) < 3
                       && sw.mapToItem(page, 0, 0).x < find(page, "recordingFilter").mapToItem(page, 0, 0).x,
                       "the switch at the filter's left")
            }
        } finally {
            Theme.compact = false
            page.width = 900
            page.height = 900
        }
    }

    // About's list opens at its start when its head grows after the switch,
    // even with a contentY reported between the head's height and its origin
    Component {
        id: wiggleOrigin
        Connections {
            property var list: null
            target: list
            function onOriginYChanged() {
                list.contentY += 0.5
                list.contentY -= 0.5
            }
        }
    }

    Component {
        id: tallItem
        Item { implicitHeight: 120; implicitWidth: 10 }
    }

    function test_aboutOpensAtItsStartWhenTheHeadGrowsLate() {
        const page = openFindings()
        Theme.compact = true
        let wiggle = null
        let tall = null
        try {
            page.width = 384
            page.height = 832
            verify(waitForPolish(page.Window.window), "laid out")
            find(page, "detailView_about").clicked()
            verify(waitForPolish(page.Window.window), "About laid out")
            const findings = find(page, "findingList")
            const headAt = () => findings.headerItem.mapToItem(findings, 0, 0).y
            tryVerify(() => headAt() >= -0.5, 2000, "About starts at its head")
            const before = findings.headerItem.height
            wiggle = wiggleOrigin.createObject(page, { list: findings })
            tall = tallItem.createObject(findings.headerItem.zoomSlot)
            tryVerify(() => findings.headerItem.height > before + 100, 2000, "the head grew")
            // a tryVerify's message is read at the call, so the numbers come after
            const until = Date.now() + 2000
            while (headAt() < -0.5 && Date.now() < until)
                wait(20)
            verify(headAt() >= -0.5, "back at its start: head at " + headAt()
                   + ", contentY " + findings.contentY + ", originY " + findings.originY)
        } finally {
            if (tall)
                tall.destroy()
            if (wiggle)
                wiggle.destroy()
            Theme.compact = false
            page.width = 900
            page.height = 900
        }
    }

    // On a page taller than the viewer the room left over goes under it:
    // the zoom row stays right under the splitter, maximized too
    function test_theZoomRowStaysUnderTheChart() {
        const page = openFindings()
        try {
            page.height = 2000
            verify(waitForPolish(page.Window.window), "laid out")
            const splitter = find(page, "chartSplitter")
            const zoom = find(page, "zoomAll")
            const gap = zoom.mapToItem(page, 0, 0).y - splitter.mapToItem(page, 0, splitter.height).y
            verify(gap >= 0 && gap <= 16, "the zoom row " + gap + " px under the splitter")
            page.refreshPart.toggleMaximized()
            verify(waitForPolish(page.Window.window), "maximized")
            const chart = find(page, "chartFrame")
            const under = zoom.mapToItem(page, 0, 0).y - chart.mapToItem(page, 0, chart.height).y
            verify(under >= 0 && under <= 16, "maximized, the zoom row " + under + " px under the chart")
            page.refreshPart.toggleMaximized()
        } finally {
            page.height = 900
        }
    }

    // The chart's height until the splitter sets one: 45 % of the page on
    // the desktop, 40 % less the 8 px of the Jump row's top margin on a
    // phone, never under what three panes, their seams and the time axis
    // need there
    function test_theChartTakesAShareOfThePage_data() {
        return [{ tag: "desktop", w: 1280, h: 720, phone: false, want: 0.45 * 720 },
                { tag: "phone", w: 384, h: 832, phone: true, want: 0.4 * 832 - 8 },
                { tag: "short phone", w: 384, h: 500, phone: true, want: -1 }]
    }

    function test_theChartTakesAShareOfThePage(data) {
        const page = openFindings()
        const kept = page.chartHeight
        page.chartHeight = -1
        Theme.compact = data.phone
        try {
            page.width = data.w
            page.height = data.h
            const chart = find(page, "chartFrame")
            const want = data.want > 0 ? data.want : page.phoneChartFloor
            if (data.want < 0) {
                // The Jump row stands above the chart, with its top margin of
                // 8: a page short enough to pin the viewer still has the floor
                // for the chart
                verify(waitForPolish(page.Window.window), "laid out")
                page.height = data.h + find(page, "jumpRow").height + Theme.spacing + 8
            }
            if (data.want < 0) {
                const room = want - 8 - ChartLayout.top - ChartLayout.axisHCursor - ChartLayout.bottomPad
                compare(ChartLayout.panesThatFit(room, ChartLayout.paneMinH), 3, "three panes fit the floor")
                compare(ChartLayout.panesThatFit(room - 1, ChartLayout.paneMinH), 2, "and not a pixel less")
            }
            tryVerify(() => Math.abs(chart.height - want) < 1, 2000, "the chart is " + want)
            verify(Math.abs(chart.height - want) < 1, "the chart is " + chart.height)
        } finally {
            Theme.compact = false
            page.chartHeight = kept
            page.width = 900
            page.height = 900
        }
    }

    // The desktop's F with Frames chosen gives the table the page, the
    // switch leading the zoom row; with Values, the chart
    function test_maximizedWithFramesTheTableHasThePage() {
        const page = openFindings()
        const chart = find(page, "chartFrame")
        const table = find(page, "viewerFrameTable")
        const frames = find(page, "detailView_frames")
        mouseClick(frames)
        verify(page.framesShown, "the Frames view")
        tryVerify(() => table.visible, 2000, "the table")
        page.refreshPart.toggleMaximized()
        try {
            verify(waitForPolish(page.Window.window), "laid out")
            verify(!chart.visible, "no chart")
            verify(table.visible && table.height > page.height * 0.6,
                   "the table has the page: " + table.height + " of " + page.height)
            verify(frames.visible && frames.mapToItem(page, 0, 0).y < table.mapToItem(page, 0, 0).y,
                   "the switch above it")
            verify(!find(page, "signalRows").visible && !find(page, "aboutHeading").visible,
                   "the rest gives way")
            mouseClick(find(page, "detailView_values"))
            tryVerify(() => chart.visible && !table.visible, 2000, "Values: the chart")
            verify(waitForPolish(page.Window.window), "laid out")
            verify(chart.height > page.height * 0.6, "the chart has the page: " + chart.height)
        } finally {
            if (page.chartMaximized)
                page.refreshPart.toggleMaximized()
            page.detailView = "values"
        }
    }

    // The Export panel opens under the title row, so the page goes to its
    // top, wherever it was scrolled; on a phone upright the pinned viewer
    // gives way to the panel and comes back
    function test_theExportPanelOpensAtThePagesTop() {
        const page = openFindings()
        try {
            page.width = 384
            page.height = 600
            verify(waitForPolish(page.Window.window), "laid out")
            const scroll = find(page, "recordingsScroll")
            verify(!page.pinnedViewer, "a desktop's page scrolls as a whole")
            verify(scroll.contentHeight > scroll.height, "the page scrolls")
            scroll.contentY = scroll.contentHeight - scroll.height
            page.reportPart.openExport()
            compare(scroll.contentY, 0, "at the top")
            const panel = find(page, "exportPanel")
            verify(panel.visible, "the panel")
            verify(panel.mapToItem(scroll, 0, 0).y < scroll.height / 2, "on the first screen")
            page.reportPart.closeExport()

            Theme.compact = true
            verify(waitForPolish(page.Window.window), "a phone upright")
            verify(page.pinnedViewer, "upright the viewer is pinned")
            verify(!scroll.interactive, "the page itself stays put")
            page.reportPart.openExport()
            verify(!page.pinnedViewer, "the panel takes the page")
            compare(scroll.contentY, 0, "at the top")
            verify(panel.visible && panel.mapToItem(scroll, 0, 0).y < scroll.height / 2,
                   "the panel on the first screen")
            verify(waitForPolish(page.Window.window), "the panel laid out")
            verify(scroll.contentHeight > scroll.height, "the panel scrolls the page")
            scroll.contentY = scroll.contentHeight - scroll.height
            page.reportPart.closeExport()
            verify(page.pinnedViewer, "pinned again")
            compare(scroll.contentY, 0, "the pinned page from its top")
        } finally {
            Theme.compact = false
            page.width = 900
            page.height = 900
        }
    }

    // On a phone every button, box, choice and field the page shows, its
    // list rows and the viewer's signal rows are a touch's size (48), and
    // nothing runs past the page's right edge; the desktop keeps its own sizes
    // The frame table's rows and the By ID list's are data rows, as dense
    // as Live's signal rows; the UI review judges them with those
    readonly property var dataRows: ["frameRowArea", "idRowArea"]

    function walkTargets(page) {
        const seen = []
        const small = []
        const wide = []
        const walk = item => {
            for (const child of item.children) {
                if (child.visible && child.width > 0
                        && (child.pressed !== undefined || child.placeholderText !== undefined)) {
                    const name = child.objectName || String(child)
                    const h = child.mapToItem(page, 0, child.height).y
                              - child.mapToItem(page, 0, 0).y
                    seen.push(name)
                    if (h < Theme.touchTarget - 0.5 && dataRows.indexOf(name) < 0)
                        small.push(name + " " + Math.round(h))
                    if (child.mapToItem(page, child.width, 0).x > page.width + 0.5)
                        wide.push(name + "@" + Math.round(child.mapToItem(page, child.width, 0).x) + " w" + Math.round(child.width))
                }
                // A label too: its box ends inside the page
                if (child.visible && child.width > 0 && child.contentWidth !== undefined
                        && child.pressed === undefined && child.placeholderText === undefined
                        && child.mapToItem(page, child.width, 0).x > page.width + 0.5)
                    wide.push((child.objectName || String(child)) + "@"
                              + Math.round(child.mapToItem(page, child.width, 0).x))
                walk(child)
            }
        }
        walk(page)
        return { seen: seen, small: small, wide: wide }
    }

    function test_aPhonesRecordingListRowsAreATouchsSize() {
        const page = createPage()
        page.recordings = [findingsRow]
        const list = find(page, "recordingList")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the row has a delegate")
        const desk = list.itemAtIndex(0).height
        Theme.compact = true
        try {
            page.width = 384
            verify(waitForPolish(page.Window.window), "laid out")
            verify(list.itemAtIndex(0).height >= Theme.touchTarget - 0.5,
                   "a list row is " + list.itemAtIndex(0).height)
            const b = walkTargets(page)
            verify(b.seen.length >= 1, "buttons found")
            compare(b.small.join(", "), "", "under " + Theme.touchTarget)
        } finally {
            Theme.compact = false
        }
        tryCompare(list.itemAtIndex(0), "height", desk, 2000, "the desktop's row is as it was")
        verify(desk >= 56)
    }

    // Each panel the page opens: the import's guessed map, the Export panel,
    // the Frames table with its filter and By ID list, the compare and align
    // rows, at the narrowest phone widths
    function test_aPhonesImportPanelIsATouchsSize_data() {
        return [{ tag: "320", w: 320 }, { tag: "384", w: 384 }]
    }

    function test_aPhonesImportPanelIsATouchsSize(data) {
        const page = createPage()
        page.openPart.chooseImportFile(Harness.writeScratchFile("phone-guess-plain.csv",
            "time,rpm\n0,800\n1,900\n2,950\n"))
        const preview = guessed(page)
        Theme.compact = true
        try {
            page.width = data.w
            verify(waitForPolish(page.Window.window), "laid out")
            verify(find(preview, "guessTimeUnit_ms").selected, "the unit shows, changeable")
            const b = walkTargets(page)
            for (const name of ["importKind_csv", "guessTimeUnit_ms"])
                verify(b.seen.indexOf(name) >= 0, name + " is seen: " + b.seen.join(", "))
            compare(b.small.join(", "), "", "under " + Theme.touchTarget)
            compare(b.wide.join(", "), "", "past the right edge at " + data.w)
        } finally {
            Theme.compact = false
        }
    }

    // The walk of an open viewer: every control a touch's size, every label
    // and control inside the page, and the names it must have reached
    function walkPanel(page, data, seen, what) {
        verify(waitForPolish(page.Window.window), "laid out")
        const b = walkTargets(page)
        for (const n of b.seen)
            seen.push(n)
        const out = []
        if (b.small.length > 0)
            out.push(what + ": under " + Theme.touchTarget + ": " + b.small.join(", "))
        if (b.wide.length > 0)
            out.push(what + ": past the right edge at " + data.w + ": " + b.wide.join(", "))
        return out
    }

    function wasWalked(seen, names) {
        for (const n of names)
            verify(seen.indexOf(n) >= 0, n + " was walked: " + seen.join(", "))
    }

    function test_aPhonesViewerPanelsAreATouchsSize_data() {
        return [{ tag: "320", w: 320 }, { tag: "384", w: 384 }]
    }

    function test_aPhonesViewerPanelsAreATouchsSize(data) {
        const page = openFindings()
        Theme.compact = true
        try {
            page.width = data.w
            verify(!find(page, "reportNote").visible, "no note, no row for it")
            page.reportNote = "Saved to a-recording-with-a-rather-long-file-name-for-a-phone.zip"
            verify(waitForPolish(page.Window.window), "laid out")
            const note = find(page, "reportNote")
            tryVerify(() => note.width > 0.6 * data.w, 2000,
                      "the note has the row: " + note.width + " of " + note.parent.width)
            const seen = []
            const problems = []
            problems.push(...walkPanel(page, data, seen, "the viewer"))
            openExport(page)
            problems.push(...walkPanel(page, data, seen, "the Export panel"))
            find(page, "exportButton").clicked()

            mouseClick(find(page, "detailView_frames"))
            tryVerify(() => /^\d+ frames? in view$/.test(find(page, "framesWords").text), 10000)
            find(page, "framesFilterToggle").clicked()
            problems.push(...walkPanel(page, data, seen, "the Frames table and its filter"))
            mouseClick(find(page, "framesTable_byId"))
            problems.push(...walkPanel(page, data, seen, "the By ID list"))
            mouseClick(find(page, "detailView_values"))
            problems.push(...walkPanel(page, data, seen, "the values again"))
            compare(problems.join("; "), "")
            wasWalked(seen, ["exportFormat_csv", "exportRows_sweep", "direction_0",
                             "framesTable_byId", "detailView_values", "decodeChoice"])
        } finally {
            Theme.compact = false
        }
    }

    function test_aPhonesCompareAndAlignRowsAreATouchsSize_data() {
        return [{ tag: "320", w: 320 }, { tag: "384", w: 384 }]
    }

    function test_aPhonesCompareAndAlignRowsAreATouchsSize(data) {
        const page = openEarly()
        Theme.compact = true
        try {
            page.width = data.w
            verify(waitForPolish(page.Window.window), "laid out")
            find(page, "detailView_about").clicked()   // the pinned form's About holds the rows
            find(page, "compareChoice").activated(1)   // which opens Compare's controls
            tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
            const seen = []
            const problems = []
            problems.push(...walkPanel(page, data, seen, "the compare and align rows"))
            compare(problems.join("; "), "")
            wasWalked(seen, ["compareChoice", "alignChoice"])
        } finally {
            Theme.compact = false
        }
    }

    function test_aPhonesViewerTargetsAreATouchsSize_data() {
        return [{ tag: "320", w: 320 }, { tag: "384", w: 384 }]
    }

    function test_aPhonesViewerTargetsAreATouchsSize(data) {
        const page = openFindings()
        const rows = find(page, "signalRows")
        compare(rows.itemAtIndex(0).height, page.rowHeight)
        const desk = rows.itemAtIndex(0).height
        verify(desk < 48, "a desktop's signal row is " + desk)
        Theme.compact = true
        try {
            page.width = data.w
            verify(waitForPolish(page.Window.window), "laid out")
            verify(rows.itemAtIndex(0).height >= Theme.touchTarget - 0.5,
                   "a signal row is " + rows.itemAtIndex(0).height)
            const b = walkTargets(page)
            compare(b.wide.join(", "), "", "past the right edge at " + data.w)
            verify(b.seen.indexOf("detailView_values") >= 0, "the view switch is seen: " + b.seen.join(", "))
            compare(b.small.join(", "), "", "under " + Theme.touchTarget)
        } finally {
            Theme.compact = false
        }
    }
}
