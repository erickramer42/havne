// File/layer: tests/qml/tst_phone.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.phone (test_qml)

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
        Relay.stop()
        Theme.developerMode = false
        Theme.zoom = 1.0
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Bridge.recording = false
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
                { tag: "recordingsPage" }, { tag: "recordingsOpened" }, { tag: "recordingsViewer" },
                { tag: "recordingsCompare" }, { tag: "recordingsImportCsv" },
                { tag: "recordingsImportCandump" }, { tag: "settingsPage" }]
    }

    function test_everyPageFitsAPhonesWidth(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        // After the load, which restores the mode kept between runs: its
        // controls (the viewer's Frame test, the badge) are measured too
        Theme.developerMode = true
        main.width = 384
        main.height = 800
        // Android's 14 sp text at the test phone's font scale, 1.15
        main.font.pixelSize = 16
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        // recordingsOpened: the viewer as it opens, its toolbar's hint
        // still showing; recordingsViewer: a cursor placed
        const viewer = ["recordingsOpened", "recordingsViewer", "recordingsCompare"].includes(data.tag)
        const importing = data.tag.startsWith("recordingsImport")
        const name = data.tag === "logFrames" ? "logPage"
                   : viewer || importing ? "recordingsPage" : data.tag
        const page = find(main.contentItem, name)
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        verify(page.visible, name + " is on screen")
        if (data.tag === "logFrames")
            find(page, "view_frames").clicked()
        // A long title and source, and the findings fixture in the viewer
        const row = { title: "2026-10-28 14:30:12 · A vehicle label forty characters long",
                      uri: "replay://C:/rec/x?profile=raw",
                      folder: Harness.recordingFixtures + "/findings",
                      source: "elm327://192.168.4.1:23?profile=raw&poll=0x0C", profile: "raw" }
        if (name === "recordingsPage")
            page.recordings = [row]
        // Import's panel with every row it can show, long names and words
        if (importing) {
            const candump = data.tag === "recordingsImportCandump"
            page.chooseImportFile(Harness.writeScratchFile(
                "a-logger-export-with-a-long-file-name-2026-11-07.csv",
                candump ? "(1.0) can0 7E8#01
" : "time,rpm
"))
            page.importMap = Harness.writeScratchFile(
                "a-column-map-with-a-long-name-for-the-phone.json", "{}")
            page.importAsksInterface = true
            page.importFailed = true
            page.importNote = candump
                ? "the log has several interfaces: name one of can0, can1, vcan0 and slcan0"
                : "the column map: line 1, column 2: the key 'signal' isn't one a column map takes"
        }
        if (viewer) {
            page.open(row)
            tryVerify(() => find(page, "signalRows").visible, 10000, "the recording opens")
            if (data.tag !== "recordingsOpened")
                page.setCursor(5000)
        }
        // Compared with itself, aligned on a rise: every field shown
        if (data.tag === "recordingsCompare") {
            page.compareWith(row)
            tryVerify(() => page.compareAligned, 10000, "the compared one opens")
            page.setAlignOn("rise")
            page.setCursor(5000)
        }
        if (data.tag === "diagnosticsPage") {
            tryVerify(() => find(page, "clearCodes").enabled, 2000, "the report is read")
            find(page, "clearCodes").clicked()
        }
        wait(300)
        waitForRendering(main.contentItem)
        const out = pastTheEdge(page, main.contentItem, [])
        compare(out.length, 0, "past " + main.contentItem.width + " px: " + out.join(", "))
        // Live's window buttons keep their text on one line: a wrapped
        // "120 s" fits the width but reads as two
        if (data.tag === "livePage") {
            for (const s of page.chartWindows) {
                const label = find(page, "chartWindow_" + s).contentItem
                compare(label.lineCount, 1, s + " s on " + label.lineCount + " lines")
            }
        }
        if (data.tag === "diagnosticsPage")
            find(page, "clearCancel").clicked()
    }

    // On a phone upright and on its side, the viewer's page scrolls to all
    // of it: the chart a readable height, the frames check's line inside
    // the page once scrolled to its end, and the filter's options inside
    // the window
    // The viewer's Frames view on a phone's width: the switch, the table
    // narrowed to the page (no DLC column), its rows inside the window, and
    // the filter's fields inside it too
    function test_theViewersFramesViewFitsAPhone() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        page.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        tryVerify(() => find(page, "chartFrame").visible, 10000, "the recording opens")
        verify(waitForPolish(main), "laid out")
        const frames = find(page, "detailView_frames")
        verify(frames.visible, "the switch is there")
        const right = frames.mapToItem(main.contentItem, frames.width, 0).x
        verify(right <= main.contentItem.width + 0.5,
               "the switch ends at " + right + " of " + main.contentItem.width)
        tap(frames, frames.width / 2, frames.height / 2)
        tryVerify(() => page.framesShown, 2000, "the Frames view")
        const table = find(page, "viewerFrameTable")
        tryVerify(() => table.visible && find(table, "frameList").count > 0, 10000, "the rows")
        verify(waitForPolish(main), "laid out")
        verify(table.narrowTable, "a phone's table is the narrow one")
        const scroll = find(page, "recordingsScroll")
        const edge = table.mapToItem(scroll, table.width, 0).x
        verify(edge <= scroll.width + 0.5, "the table ends at " + edge + " of " + scroll.width)
        const toggle = find(page, "framesFilterToggle")
        tap(toggle, toggle.width / 2, toggle.height / 2)
        tryVerify(() => find(page, "filterPanel").visible, 2000, "the filter opens")
        verify(waitForPolish(main), "laid out")
        const panel = find(page, "filterPanel")
        const panelRight = panel.mapToItem(scroll, panel.width, 0).x
        verify(panelRight <= scroll.width + 0.5,
               "the filter ends at " + panelRight + " of " + scroll.width)
    }

    function test_theViewerReachesAllOfItselfOnAPhone_data() {
        return [{ tag: "upright", w: 384, h: 800 }, { tag: "on its side", w: 800, h: 384 }]
    }

    function test_theViewerReachesAllOfItselfOnAPhone(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = data.h
        main.font.pixelSize = 16
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        page.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        tryVerify(() => find(page, "chartFrame").visible, 10000, "the recording opens")
        verify(waitForPolish(main), "laid out")
        // On its side the chart starts with the page and the cursor's
        // values beside it; the page's own layout is a press away
        compare(page.chartMaximized, data.h < 480, "maximized on its side only")
        if (page.chartMaximized) {
            verify(!find(page, "signalRows").visible, "the rows give way")
            const strip = find(page, "cursorStrip")
            verify(!strip.visible, "no cursor yet: the chart has the width")
            page.setCursor(5000)
            tryVerify(() => strip.visible, 2000, "a cursor: its values beside the chart")
            page.setCursor(NaN)
            find(page, "maximizeChart").clicked()
        }
        tryVerify(() => find(page, "signalRows").visible, 2000, "the rows are back")
        verify(waitForPolish(main), "laid out")
        const chart = find(page, "chartFrame")
        verify(chart.height >= 200, "the chart is " + chart.height + " px")

        const scroll = find(page, "recordingsScroll")
        scroll.contentY = Math.max(0, scroll.contentHeight - scroll.height)
        verify(waitForPolish(main), "scrolled")
        const check = find(page, "checkSummary")
        const bottom = check.mapToItem(scroll, 0, check.height).y
        verify(check.visible && bottom <= scroll.height + 0.5,
               "the frames check's line ends at " + bottom + " of " + scroll.height)

        const bar = find(page, "recordingFilter")
        find(bar, "filterButton").clicked()
        tryVerify(() => bar.options.opened, 2000, "the filter's options open")
        verify(waitForPolish(main), "the options laid out")
        const box = bar.options.contentItem
        const right = box.mapToItem(main.contentItem, box.width, 0).x
        verify(right <= main.contentItem.width + 0.5,
               "the filter's options end at " + right + " of " + main.contentItem.width)
        bar.options.close()
    }

    // A phone's density: upright (narrower than 720) and on its side
    // (shorter than 480), and not in a desktop's window
    function test_densityFollowsThePhonesShape_data() {
        return [{ tag: "upright", w: 384, h: 832, dense: true },
                { tag: "on its side", w: 832, h: 384, dense: true },
                { tag: "a laptop", w: 1280, h: 650, dense: false }]
    }
    function test_densityFollowsThePhonesShape(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = data.h
        tryCompare(Theme, "dense", data.dense)
        compare(Theme.pageMargin, data.dense ? 10 : 16)
        const live = find(main.contentItem, "livePage")
        compare(live.rowHeight, data.dense ? 36 : 44, "Live's rows follow it")
    }

    // An item's rectangle in the window's own units (the glass's, the
    // zoom applied)
    function onGlass(item, main) {
        return item.mapToItem(main.contentItem, 0, 0, item.width, item.height)
    }

    // The phone's tab bar upright: every tab a touch's size on the glass
    // at any zoom that shows the bar (67 % is the least at 384; at 50 %
    // the layout passes 720 and the sidebar shows), the targets ending a
    // gap above the window's bottom, and with the system's bars over the
    // window (Android edge to edge) no tab inside the bottom inset, the
    // bar's color reaching the window's edge and the header's row below
    // the top inset
    function test_theTabBarsTargetsAreATouchsSize_data() {
        const none = { left: 0, top: 0, right: 0, bottom: 0 }
        return [{ tag: "100 %", zoom: 1.0, insets: none },
                { tag: "67 %", zoom: 0.67, insets: none },
                { tag: "150 %", zoom: 1.5, insets: none },
                { tag: "the system's bars", zoom: 1.0,
                  insets: { left: 0, top: 24, right: 0, bottom: 48 } },
                { tag: "the system's bars at 67 %", zoom: 0.67,
                  insets: { left: 0, top: 24, right: 0, bottom: 48 } }]
    }

    function test_theTabBarsTargetsAreATouchsSize(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        Theme.zoom = data.zoom
        main.testInsets = data.insets
        verify(waitForPolish(main), "laid out")
        const bar = find(main.contentItem, "tabBar")
        tryVerify(() => bar.visible, 2000, "the tab bar shows upright")
        const glass = main.contentItem.height
        const floor = glass - data.insets.bottom - 8
        compare(bar.count, 7, "today's tabs")
        for (let i = 0; i < bar.count; ++i) {
            const tab = bar.itemAt(i)
            const r = onGlass(tab, main)
            verify(r.height >= 48 - 0.5, tab.text + " is " + r.height + " tall on the glass")
            verify(r.width >= 48 - 0.5, tab.text + " is " + r.width + " wide on the glass")
            verify(r.y + r.height <= floor + 0.5,
                   tab.text + " ends at " + (r.y + r.height) + ", past " + floor)
        }
        const back = onGlass(find(bar, "tabBarBackground"), main)
        verify(back.y + back.height >= glass - 0.5,
               "the bar's color ends at " + (back.y + back.height) + " of " + glass)
        const head = onGlass(find(main.contentItem, "headerRow"), main)
        verify(head.y >= data.insets.top - 0.5,
               "the header's row starts at " + head.y + ", inside the top inset")
        const rowHeight = Theme.toolBarHeight * Theme.zoom
        verify(head.height >= rowHeight - 0.5,
               "the header's row is " + head.height + " tall, under " + rowHeight)
        const headBar = onGlass(find(main.contentItem, "headerBar"), main)
        verify(headBar.y <= 0.5, "the header's color starts at the window's top")
    }

    // At the sidebar's width (a phone on its side) there's no tab bar, and
    // the pages still keep off the bottom inset
    function test_thePagesKeepOffTheBottomInsetBesideTheSidebar() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 832
        main.height = 384
        main.testInsets = { left: 0, top: 24, right: 0, bottom: 30 }
        verify(waitForPolish(main), "laid out")
        verify(!find(main.contentItem, "tabBar").visible, "the sidebar instead")
        const home = find(main.contentItem, "homePage")
        main.gotoPage(Array.from(home.parent.children).indexOf(home))
        verify(waitForPolish(main), "laid out")
        const r = onGlass(home.parent, main)
        verify(r.y + r.height <= main.contentItem.height - 30 + 0.5,
               "the pages end at " + (r.y + r.height) + " of " + main.contentItem.height)
    }

    // The chart maximized: on its side by default, upright on request,
    // each shape keeping its own choice; Live keeps its window row and
    // its link banner, and its list gives way
    function test_liveMaximizesItsChartByShape() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const live = find(main.contentItem, "livePage")
        main.gotoPage(Array.from(live.parent.children).indexOf(live))
        const list = find(live, "signalList")
        main.width = 832
        main.height = 384
        tryVerify(() => Theme.shortLayout, 2000, "on its side")
        verify(live.chartMaximized, "on its side the chart has the page")
        verify(!list.visible, "the list gives way")
        verify(find(live, "chartWindowRow").visible, "the window row stays")
        find(live, "maximizeChart").clicked()
        verify(!live.chartMaximized && list.visible, "the list is back")

        main.width = 384
        main.height = 832
        tryVerify(() => !Theme.shortLayout, 2000, "upright")
        verify(!live.chartMaximized, "upright it starts with the list")
        find(live, "maximizeChart").clicked()
        verify(live.chartMaximized, "maximized upright on request")
        main.width = 832
        main.height = 384
        tryVerify(() => Theme.shortLayout, 2000, "on its side again")
        verify(!live.chartMaximized, "on its side, the choice made on its side holds")
        main.width = 384
        main.height = 832
        tryVerify(() => !Theme.shortLayout, 2000, "upright again")
        verify(live.chartMaximized, "and upright, the one made upright")
        find(live, "maximizeChart").clicked()
    }

    // Harness.clickStamped's stamps start here (ms), past any of Qt Quick
    // Test's own: a test's clicks are each its own window's first
    readonly property int kStampBase: 100000000

    // One finger's tap, pressed and lifted in place
    function tap(item, x, y) {
        const seq = touchEvent(item)
        seq.press(0, item, x, y).commit()
        seq.release(0, item, x, y).commit()
    }

    // A double tap on Live's chart allows a thumb's drift
    // (DoubleTapHandler): two taps a little apart within the double-click
    // interval toggle it, two far apart or past the interval don't; a
    // mouse's two clicks the same way
    function test_aDoubleTapOnLivesChartAllowsAThumbsDrift_data() {
        return [{ tag: "taps 30 px apart", gap: 30, late: false, mouse: false, toggles: true },
                { tag: "taps 100 px apart", gap: 100, late: false, mouse: false, toggles: false },
                { tag: "taps past the interval", gap: 0, late: true, mouse: false, toggles: false },
                { tag: "clicks 30 px apart", gap: 30, late: false, mouse: true, toggles: true }]
    }
    function test_aDoubleTapOnLivesChartAllowsAThumbsDrift(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const live = find(main.contentItem, "livePage")
        main.gotoPage(Array.from(live.parent.children).indexOf(live))
        const frame = find(live, "chartFrame")
        // The mock's rows fill the list's 55 % of the page; the chart has
        // the rest, about 196 px here
        tryVerify(() => frame.width > 300 && frame.height > 150, 3000, "the chart at its size")
        verify(waitForPolish(main), "laid out")
        const before = live.chartMaximized
        const x = frame.width / 2 - data.gap / 2
        const y = frame.height / 2
        // Clicks stamped 100 ms apart: Qt Quick Test's own add the
        // double-click interval to their stamps after each release
        const press = data.mouse ? ((px, i) => Harness.clickStamped(frame, px, y, kStampBase + 100 * i))
                                 : ((px, i) => tap(frame, px, y))
        press(x, 0)
        if (data.late)
            wait(Qt.styleHints.mouseDoubleClickInterval + 150)
        press(x + data.gap, 1)
        compare(live.chartMaximized, data.toggles ? !before : before)
    }

    // The viewer's chart and splitter, and Live's splitter, take the same
    // double tap
    function test_theViewersChartAndBothSplittersAllowAThumbsDrift() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        page.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        const chart = find(page, "chartFrame")
        tryVerify(() => chart.visible && chart.width > 300, 10000, "the recording opens")
        verify(waitForPolish(main), "laid out")
        const keptHeight = page.chartHeight
        page.chartHeight = 300
        const chartSplitter = find(page, "chartSplitter")
        tryVerify(() => chartSplitter.visible, 2000, "the viewer's splitter shows")
        verify(waitForPolish(main), "laid out")
        tap(chartSplitter, chartSplitter.width / 2 - 15, chartSplitter.height / 2)
        tap(chartSplitter, chartSplitter.width / 2 + 15, chartSplitter.height / 2)
        compare(page.chartHeight, -1, "the viewer's splitter: taps 30 px apart give the page its own height")
        page.chartHeight = keptHeight
        verify(waitForPolish(main), "laid out")

        const before = page.chartMaximized
        tap(chart, chart.width / 2 - 15, chart.height / 2)
        tap(chart, chart.width / 2 + 15, chart.height / 2)
        compare(page.chartMaximized, !before, "the viewer's chart: taps 30 px apart")

        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const live = find(main.contentItem, "livePage")
        main.gotoPage(Array.from(live.parent.children).indexOf(live))
        const list = find(live, "signalList")
        tryVerify(() => list.count > 3, 5000, "rows")
        const splitter = find(live, "listSplitter")
        tryVerify(() => splitter.visible, 2000, "the splitter shows")
        const kept = live.listShare
        live.listShare = 0.1
        verify(waitForPolish(main), "laid out")
        tap(splitter, splitter.width / 2 - 15, splitter.height / 2)
        tap(splitter, splitter.width / 2 + 15, splitter.height / 2)
        compare(live.listShare, -1, "Live's splitter: taps 30 px apart size the list to its rows")
        live.listShare = kept
    }

    // The maximized viewer's chart is rasterized on the GUI thread (about
    // 370 ms a paint on the test phone), so the second of two taps lifted
    // 30 ms apart is handled a frame later. The gap is the events' own
    // stamps: a real wait between two clicks stamped 30 ms apart stands in
    // for that frame (the touch case past the interval, stamped as it
    // happens, checks the other way)
    function test_theViewersWayBackCountsTheTapsOwnTimes() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        page.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        const chart = find(page, "chartFrame")
        tryVerify(() => chart.visible && chart.width > 300, 10000, "the recording opens")
        if (!page.chartMaximized)
            page.toggleMaximized()
        verify(page.chartMaximized, "the chart has the page")
        verify(waitForPolish(main), "laid out")
        const y = chart.height / 2
        Harness.clickStamped(chart, 110, y, kStampBase)
        wait(Qt.styleHints.mouseDoubleClickInterval + 150)
        Harness.clickStamped(chart, 126, y + 8, kStampBase + 30)
        verify(!page.chartMaximized, "two clicks stamped close give the signals back")
    }

    // Live's splitter: a share of the page for the list, within the rows
    // it has; -1 sizes it to them again
    function test_liveSplitterSetsTheListsHeight() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const live = find(main.contentItem, "livePage")
        main.gotoPage(Array.from(live.parent.children).indexOf(live))
        const list = find(live, "signalList")
        tryVerify(() => list.count > 3, 5000, "rows")
        verify(find(live, "listSplitter").visible, "the splitter shows")
        const kept = live.listShare
        live.listShare = 0.1
        verify(waitForPolish(main), "laid out")
        fuzzyCompare(list.height, Math.max(live.rowHeight, 0.1 * list.parent.height), 1,
                     "a tenth of the page")
        live.listShare = -1
        verify(waitForPolish(main), "laid out")
        verify(list.height > 0.1 * list.parent.height + 1, "sized to its rows again")
        live.listShare = kept
    }

    // The Dashboard's two dials share a row on a phone upright, and stack
    // only on a narrower page
    function test_theDashboardsDialsShareARowUpright_data() {
        return [{ tag: "a phone upright", w: 384, columns: 2 },
                { tag: "narrower", w: 300, columns: 1 }]
    }
    function test_theDashboardsDialsShareARowUpright(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = 832
        const dash = find(main.contentItem, "dashboardPage")
        main.gotoPage(Array.from(dash.parent.children).indexOf(dash))
        tryCompare(find(dash, "dialGrid"), "columns", data.columns, 3000)
    }

    // Live's window by a pinch or the wheel: a step at a time through
    // its windows, stopping at either end; fingers spreading show less
    // time, as the viewer's pinch zooms in
    function test_livesPinchAndWheelStepItsWindow() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const live = find(main.contentItem, "livePage")
        main.gotoPage(Array.from(live.parent.children).indexOf(live))
        const kept = live.chartWindowS
        const frame = find(live, "chartFrame")
        // Laid out at the phone's size before a finger touches it
        tryVerify(() => frame.width > 300 && frame.height > 150, 3000, "the chart at its size")
        waitForRendering(main.contentItem)

        live.stepWindow(-1); live.stepWindow(-1); live.stepWindow(-1)
        compare(live.chartWindowS, 10, "the shortest, and no further")
        live.stepWindow(1)
        compare(live.chartWindowS, 30)
        live.stepWindow(1); live.stepWindow(1); live.stepWindow(1)
        compare(live.chartWindowS, 120, "the longest, and no further")

        mouseWheel(frame, frame.width / 2, frame.height / 2, 0, 120)
        compare(live.chartWindowS, 60, "the wheel up: less time")
        mouseWheel(frame, frame.width / 2, frame.height / 2, 0, -120)
        compare(live.chartWindowS, 120, "the wheel down: more")
        // A high-resolution wheel's or a touchpad's notch in parts: one
        // step once they add up to 120, not one per part
        for (let i = 0; i < 3; ++i)
            mouseWheel(frame, frame.width / 2, frame.height / 2, 0, 30)
        compare(live.chartWindowS, 120, "three quarters of a notch: no step yet")
        mouseWheel(frame, frame.width / 2, frame.height / 2, 0, 30)
        compare(live.chartWindowS, 60, "four quarters: one step")
        mouseWheel(frame, frame.width / 2, frame.height / 2, 120, 0)
        compare(live.chartWindowS, 60, "a sideways scroll leaves the window")
        mouseWheel(frame, frame.width / 2, frame.height / 2, 0, -120)
        compare(live.chartWindowS, 120)

        // Two fingers spreading from 40 px apart to 160, then closing
        // again. The handler takes its scale from where it starts, past
        // its drag threshold, so how many steps a pinch makes is the
        // gesture's; which way it steps is the rule
        const cx = frame.width / 2
        const cy = frame.height / 2
        const pinch = (from, to) => {
            const seq = touchEvent(frame)
            seq.press(0, frame, cx - from, cy).press(1, frame, cx + from, cy).commit()
            const step = from < to ? 4 : -4
            for (let d = from + step; from < to ? d <= to : d >= to; d += step)
                seq.move(0, frame, cx - d, cy).move(1, frame, cx + d, cy).commit()
            seq.release(0, frame, cx - to, cy).release(1, frame, cx + to, cy).commit()
        }
        pinch(20, 80)
        verify(live.chartWindowS < 120, "spreading shows less time: " + live.chartWindowS + " s")
        const spread = live.chartWindowS
        pinch(80, 20)
        verify(live.chartWindowS > spread, "closing shows more: " + live.chartWindowS + " s")

        live.stepWindow(1)
        while (live.chartWindowS !== kept && live.chartWindowS !== 120)
            live.stepWindow(1)
    }

    // Developer mode's corner badge: hidden otherwise, and counting the
    // frames the window draws while a session moves the page
    function test_theFrameRateBadgeCountsInDeveloperMode() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 800
        const badge = find(main.contentItem, "frameRate")
        verify(!badge.visible, "hidden out of developer mode")
        compare(main.framesPerSecond, 0)
        Theme.developerMode = true
        verify(badge.visible, "shown in developer mode")
        // Above a phone's tab bar, covering none of its tabs
        const tabs = find(main.contentItem, "tabBar")
        tryVerify(() => tabs.visible && tabs.height > 0, 2000, "the tab bar shows")
        const badgeBottom = badge.mapToItem(main.contentItem, 0, badge.height).y
        const tabsTop = tabs.mapToItem(main.contentItem, 0, 0).y
        verify(badgeBottom <= tabsTop, "the badge ends at " + badgeBottom + ", the tabs start at " + tabsTop)
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const live = find(main.contentItem, "livePage")
        main.gotoPage(Array.from(live.parent.children).indexOf(live))
        tryVerify(() => main.framesPerSecond > 0, 5000, "the frames drawn are counted")
        Theme.developerMode = false
        verify(!badge.visible, "hidden again")
        compare(main.framesPerSecond, 0, "the count starts over next time")
    }

    // While a session records, the header's Mark marks it from any page,
    // counting its marks, and the header still fits a phone
    function test_theHeadersMarkMarksTheRecording() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 800
        main.font.pixelSize = 16
        const button = find(main.contentItem, "markButton")
        const title = find(main.contentItem, "headerTitle")
        verify(!button.visible, "nothing to mark yet")
        verify(title.visible)
        Bridge.recording = true
        compare(Bridge.connectSession("mock://marked"), "", "mock://marked connects")
        verify(Bridge.recordingActive)
        verify(button.visible)
        compare(button.text, "Mark")
        button.clicked()
        compare(Bridge.markCount, 1)
        compare(button.text, "Mark (1)")
        waitForRendering(main.contentItem)
        verify(!title.visible, "the name gives way on a phone")
        const out = pastTheEdge(button.parent, main.contentItem, [])
        compare(out.length, 0, "past " + main.contentItem.width + " px: " + out.join(", "))
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        verify(!button.visible, "the recording ended")
        verify(title.visible)
    }

    // Diagnostics and Recordings take Settings' vehicles, so each report
    // names the picked vehicle's baseline
    function test_theReportsPagesTakeSettingsVehicles() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        const folder = Harness.recordingFixtures + "/findings"
        find(main.contentItem, "settingsPage").vehicles = { salt: "", picked: "v1", list: [{
            id: "v1", label: "Daily driver", vinKey: "", uri: "", plotted: [], unplotted: [],
            signals: [], own: "", plugin: "", baseline: folder }] }
        compare(find(main.contentItem, "diagnosticsPage").baselineFolder, folder)
        compare(find(main.contentItem, "recordingsPage").baselineFolder, folder)
    }

    // The relay's line beside its button (ADR-0047): the traffic wraps in
    // its own width rather than running under Stop relaying
    function test_relayTrafficFitsBesideItsButton() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        Theme.developerMode = true   // after Main, which loads the stored one
        main.width = 384
        main.height = 800
        main.font.pixelSize = 16
        const page = find(main.contentItem, "settingsPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        find(page, "adapterSection").open = true   // the relay's box is in it
        compare(Relay.start("elm327://bt:00:11:22:33:44:55", 0), "", "the relay starts")
        const traffic = find(page, "relayTraffic")
        const button = find(page, "relayButton")
        tryVerify(() => traffic.visible && button.text === "Stop relaying", 2000,
                  "the relay's line and Stop relaying are shown")
        waitForRendering(main.contentItem)
        verify(traffic.contentWidth <= traffic.width + 0.5,
               "the traffic, " + Math.round(traffic.contentWidth) + " px, fits its "
               + Math.round(traffic.width) + " px")
        verify(traffic.mapToItem(page, traffic.contentWidth, 0).x
               <= button.mapToItem(page, 0, 0).x + 0.5, "it ends before the button")
    }
}
