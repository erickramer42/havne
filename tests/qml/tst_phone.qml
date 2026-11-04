// File/layer: tests/qml/tst_phone.qml | Qt Quick Test, QML tier
// Testing: every page in the app's own window (Main) at a phone's size,
//          connected to mock://demo with developer mode on: each page's
//          items lie inside the window's width, the Log page in its
//          Frames view too. The pages' own tests run them at 320 px
//          alone; this one runs them as the window holds them, the
//          relay's line beside its button, the header's Mark while
//          recording, and developer mode's frame-rate badge. How they
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
        Relay.stop()
        Theme.developerMode = false
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
                { tag: "recordingsPage" }, { tag: "recordingsViewer" }, { tag: "recordingsCompare" },
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
        const name = data.tag === "logFrames" ? "logPage"
                   : data.tag === "recordingsViewer" || data.tag === "recordingsCompare"
                   ? "recordingsPage" : data.tag
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
        if (data.tag === "recordingsViewer" || data.tag === "recordingsCompare") {
            page.open(row)
            tryVerify(() => find(page, "signalRows").visible, 10000, "the recording opens")
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
        if (data.tag === "diagnosticsPage")
            find(page, "clearCancel").clicked()
    }

    // On a phone upright and on its side, the viewer's page scrolls to all
    // of it: the chart a readable height, the frames check's line inside
    // the page once scrolled to its end, and the filter's options inside
    // the window
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
            verify(find(page, "cursorStrip").visible, "the values beside the chart")
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
        tryVerify(() => frame.width > 300 && frame.height > 200, 3000, "the chart at its size")
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
