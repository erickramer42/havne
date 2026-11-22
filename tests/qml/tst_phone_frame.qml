// File/layer: tests/qml/tst_phone_frame.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.phone_frame (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

PhoneKit {
    id: testCase
    name: "Phone"

    // The bars' heights are fixed dp numbers, the same upright and on its
    // side: a header of the toolbar's height under the status bar, and a tab
    // row of a touch's size and a gap over the navigation bar. A phone keeps
    // its tab bar on its side (ADR-0057: the form follows the platform, not
    // the width a turn crosses). The window's real safe-area offset can't
    // be faked in a test (the content item's place), so the phone run
    // checks that the bars reach the window's edge
    function test_theBarsKeepTheirHeightsUprightAndOnItsSide_data() {
        return [{ tag: "upright", w: 384, h: 832,
                  insets: { left: 0, top: 34, right: 0, bottom: 48 } },
                { tag: "on its side", w: 832, h: 384,
                  insets: { left: 34, top: 30, right: 48, bottom: 0 } }]
    }

    function test_theBarsKeepTheirHeightsUprightAndOnItsSide(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        Theme.zoom = 1.0
        main.testMobile = true
        main.testInsets = data.insets
        main.width = data.w
        main.height = data.h
        verify(waitForPolish(main), "laid out")
        const bar = find(main.contentItem, "tabBar")
        tryVerify(() => bar.visible, 2000, "a phone's tab bar shows")
        const head = find(main.contentItem, "headerBar")
        compare(head.height - data.insets.top, Theme.touchTarget, "the header's own height")
        compare(bar.height - data.insets.bottom, Theme.touchTarget + Theme.barGap,
                "the tab row and its gap")
        compare(Theme.touchTarget + Theme.barGap, 56)
    }

    // zoomRoot reaches back out over the system's top and bottom bars
    // (the window's content item lies inside them), so its place is minus
    // the top margin and its drawn height the content item's and both
    // margins, at any zoom. testWindowSafe stands in for the window's
    // margins, which a test window has none of
    function test_zoomRootReachesOverTheBars_data() {
        return [{ tag: "67 %", zoom: 0.67 }, { tag: "100 %", zoom: 1.0 },
                { tag: "150 %", zoom: 1.5 }]
    }

    function test_zoomRootReachesOverTheBars(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        Theme.zoom = data.zoom
        main.testMobile = true
        main.testWindowSafe = { left: 20, top: 24, right: 10, bottom: 48 }
        main.width = 384
        main.height = 832
        verify(waitForPolish(main), "laid out")
        const root = find(main.contentItem, "zoomRoot")
        compare(root.y, -24, "starts over the top bar")
        fuzzyCompare(root.height * Theme.zoom, main.contentItem.height + 24 + 48, 0.01,
                     "ends under the bottom bar")
        compare(root.x, 0, "no side inset of its own")
        fuzzyCompare(root.width * Theme.zoom, main.contentItem.width, 0.01, "nor a side margin's width")
    }

    // A phone is a window with a short side under 600 dp, whatever the
    // zoom (a phone zoomed out is still one); at 600 a tablet's width rules
    function test_thePhoneFormsEdgeIs600Dp_data() {
        return [{ tag: "599", h: 599, zoom: 1.0, tabs: true },
                { tag: "600", h: 600, zoom: 1.0, tabs: false },
                { tag: "a phone at 50 %", h: 384, zoom: 0.5, tabs: true },
                { tag: "a phone at 67 %", h: 384, zoom: 0.67, tabs: true }]
    }

    function test_thePhoneFormsEdgeIs600Dp(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        Theme.zoom = data.zoom
        main.testMobile = true
        main.width = 832
        main.height = data.h
        verify(waitForPolish(main), "laid out")
        tryCompare(find(main.contentItem, "tabBar"), "visible", data.tabs)
    }

    // A tablet's short side (600 dp) and a computer go by the width
    function test_onlyAPhoneKeepsTheTabBarWhenWide() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        Theme.zoom = 1.0
        main.testMobile = true
        main.width = 1280
        main.height = 800
        verify(waitForPolish(main), "laid out")
        verify(!find(main.contentItem, "tabBar").visible, "a tablet's width gets the sidebar")
        main.width = 832
        main.height = 384
        tryVerify(() => find(main.contentItem, "tabBar").visible, 2000, "a phone on its side")
        main.testMobile = false
        tryVerify(() => !find(main.contentItem, "tabBar").visible, 2000, "a computer's window")
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
        main.gotoPage(stackIndex(page))
        page.openPart.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
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
        main.gotoPage(stackIndex(page))
        page.openPart.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        const chart = find(page, "chartFrame")
        tryVerify(() => chart.visible && chart.width > 300, 10000, "the recording opens")
        if (!page.chartMaximized)
            page.refreshPart.toggleMaximized()
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

    // The Dashboard's title row fits a phone's width whatever the font:
    // a title longer than any font makes "Dashboard" stands in for a wide
    // one, and gives way, eliding, while the buttons keep their words
    function test_theDashboardsHeaderFitsWhateverTheFont() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        const dash = find(main.contentItem, "dashboardPage")
        main.gotoPage(stackIndex(dash))
        dash.form = "desktop"
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        tryVerify(() => dash.compact && Theme.dense, 3000, "the phone's density")
        verify(waitForPolish(main), "laid out at 384 px")
        const header = find(dash, "resetPeaks").parent
        const title = header.children[0]
        compare(title.text, qsTr("Dashboard"), "the title")
        const whole = title.implicitWidth
        // At a phone's width the buttons are no wider than their words,
        // so the title has the row's width less theirs in any font. A
        // layout rounds a fractional width up to a whole pixel
        for (const n of ["resetPeaks", "dashEdit"]) {
            const b = find(dash, n)
            verify(b.width <= Math.ceil(Math.max(40, b.implicitContentWidth + 12)) + 0.5,
                   n + " is " + b.width + " px for " + b.implicitContentWidth + " px of words")
        }
        header.title = qsTr("Dashboard").repeat(4)
        verify(waitForPolish(main), "laid out")
        waitForRendering(main.contentItem)
        const out = pastTheEdge(header, main.contentItem, [])
        compare(out.length, 0, "past " + main.contentItem.width + " px: " + out.join(", "))
        verify(title.truncated, "the long title elides")
        for (const n of ["resetPeaks", "dashEdit"]) {
            const words = find(dash, n).contentItem
            compare(words.lineCount, 1, n + " on " + words.lineCount + " lines")
        }
        // Given room, the title is whole again and no wider than its words
        header.title = qsTr("Dashboard")
        verify(waitForPolish(main), "laid out again")
        compare(title.truncated, false, "whole at 384 px")
        verify(title.width <= Math.ceil(whole) + 0.5, "no wider than its words: " + title.width)
    }

    // A font whose advances aren't whole pixels (CoreText, DirectWrite at
    // a scaled display) gives the title a fractional width. Letter spacing
    // makes one on any platform; the title stays whole wherever it has room
    function test_aFractionalTitleIsNotElided_data() {
        return [
            { tag: "desktop at 384", form: "desktop", w: 384 },
            { tag: "desktop at 1280", form: "desktop", w: 1280 }
        ]
    }
    function test_aFractionalTitleIsNotElided(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = 832
        const dash = find(main.contentItem, "dashboardPage")
        main.gotoPage(stackIndex(dash))
        dash.form = data.form
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        verify(waitForPolish(main), "laid out")
        const title = find(dash, "resetPeaks").parent.children[0]
        title.font.letterSpacing = 0.13
        verify(title.implicitWidth !== Math.round(title.implicitWidth),
               "a fractional width: " + title.implicitWidth)
        verify(waitForPolish(main), "laid out with the spacing")
        waitForRendering(main.contentItem)
        compare(title.truncated, false,
                "whole: " + title.width + " px for " + title.implicitWidth + " px of words")
        Bridge.disconnectSession()
    }

    // The phone's one tool row: inside the page at any width and font,
    // one line of words on each button, every tool a touch target
    function test_thePhonesToolRowFits_data() {
        return [
            { tag: "at 320", w: 320, h: 800, session: true },
            { tag: "at 384", w: 384, h: 832, session: true },
            { tag: "on its side", w: 832, h: 384, session: true },
            { tag: "at 320 with no session", w: 320, h: 800, session: false },
            { tag: "at 384 with no session", w: 384, h: 832, session: false }
        ]
    }
    // With no session Drive reads "Connect first", longer than "Drive":
    // Reset peaks gives way before anything passes the edge
    function test_thePhonesToolRowFits(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = data.h
        const dash = find(main.contentItem, "dashboardPage")
        main.gotoPage(stackIndex(dash))
        dash.form = "phone"
        if (data.session)
            compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        tryVerify(() => find(dash, "toolRowNight").item, 3000, "the row is there")
        verify(waitForPolish(main), "laid out")
        waitForRendering(main.contentItem)
        const row = find(dash, "resetPeaks").parent
        compare(pastTheEdge(row, main.contentItem, []).length, 0, "inside the page")
        const drive = find(dash, "dashDrive")
        compare(drive.text, data.session ? "Drive" : "Connect first")
        compare(drive.enabled, data.session)
        for (const n of ["dashDrive", "resetPeaks", "dashEdit"]) {
            const b = find(dash, n)
            if (!b.visible)
                continue
            compare(b.contentItem.lineCount, 1, n + " on one line")
            verify(b.contentItem.contentWidth <= b.width + 0.5, n + " keeps its words")
            verify(b.height >= Theme.touchTarget - 0.5, n + " is " + b.height + " px tall")
        }
        const night = find(dash, "nightSwitch")
        verify(night.height >= Theme.touchTarget - 0.5, "Night is " + night.height + " px tall")
        verify(night.width >= Theme.touchTarget - 0.5, "Night is " + night.width + " px wide")
        fuzzyCompare(row.height, Theme.touchTarget, 0.5, "one touch target tall")
        if (data.session)
            Bridge.disconnectSession()
    }

    // Edit mode's controls on the phone: each a touch target, the bar
    // five buttons
    function test_thePhonesEditControlsAreTouchTargets() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        const dash = find(main.contentItem, "dashboardPage")
        main.gotoPage(stackIndex(dash))
        dash.form = "phone"
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        // Measured once the phone's density holds: before it, a button is
        // the desktop's height and the touch size the desktop's too
        tryVerify(() => dash.compact && Theme.dense, 3000, "the phone's density")
        verify(waitForPolish(main), "laid out")
        for (const n of ["resetPeaks", "dashEdit"])
            verify(find(dash, n).height >= Theme.touchTarget - 0.5,
                   n + " is " + find(dash, n).height + " px tall")
        dash.startEditing()
        const bar = find(dash, "dashEditBar")
        compare(Array.from(bar.children).filter(c => c.visible && c.objectName !== "").length, 5,
                "five on the bar")
        const tall = names => {
            waitForRendering(main.contentItem)
            for (const n of names) {
                const item = find(dash, n)
                verify(item.visible, n + " shows")
                verify(item.height >= Theme.touchTarget - 0.5,
                       n + " is " + item.height + " px tall")
            }
        }
        tall(["dashUndo", "dashRedo", "dashAdd", "dashMore", "dashDone", "dashAddCell_0"])
        find(dash, "dashMore").clicked()
        // Move page left and right show only with pages to move among
        tall(["moreRename", "moreAddPage", "moreRemovePage", "moreExport", "moreLoad", "moreReset"])
        dash.editsPart.addPage()
        tall(["moreLeft", "moreRight"])
        dash.editsPart.removePage()
        dash.confirmReset = true
        tall(["resetConfirm", "resetKeep"])
        dash.confirmReset = false
        dash.confirmRemovePage = true
        tall(["removePageConfirm", "removePageKeep"])
        dash.confirmRemovePage = false
        dash.moreShown = false
        dash.select(1)
        tall(["cellRemove", "panelClose", "panelLabel", "panelSignal", "panelFrom", "panelTo",
              "panelDecimals", "panelW", "panelH", "panelPeak", "panelEarlier", "panelLater",
              "panelStart", "panelEnd", "panelCopy", "panelPaste", "panelRemove"])
        dash.copySelected()
        tall(["dashAddCell_0", "dashPasteCell_0"])
        dash.addFlowPart.startAdd()
        tall(["addClose", "addKind_analog", "addKind_digital"])
        dash.addFlowPart.pickKind("analog")
        tall(["signalBack", "pickSignal_" + Bridge.gaugeSignals["rpm"][0]])
        dash.stopEditing()
    }

    // The edit bar holds its five controls at a phone's widths, at the test
    // font and at 1.3 of it, each on one line; the "+" cell's halves are a
    // touch's size
    function test_theEditBarFitsAPhone_data() {
        const rows = []
        for (const w of [320, 364, 384])
            for (const scale of [1, 1.3])
                rows.push({ tag: w + " at " + scale, w: w, scale: scale })
        return rows
    }
    function test_theEditBarFitsAPhone(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = 800
        const dash = find(main.contentItem, "dashboardPage")
        main.gotoPage(stackIndex(dash))
        dash.form = "phone"
        tryVerify(() => dash.compact && Theme.dense, 3000, "the phone's density")
        dash.startEditing()
        dash.copied = { form: "phone", cell: { kind: "blank", w: 3, h: 6, refs: [] } }
        const names = ["dashUndo", "dashRedo", "dashAdd", "dashMore", "dashDone"]
        for (const n of names) {
            const words = find(dash, n).contentItem
            words.font.pixelSize = Math.round(Theme.fontBody * data.scale)
        }
        verify(waitForPolish(main), "laid out")
        waitForRendering(main.contentItem)
        const bar = find(dash, "dashEditBar")
        const out = pastTheEdge(bar, main.contentItem, [])
        compare(out.length, 0, "past " + main.contentItem.width + " px: " + out.join(", "))
        for (const n of names) {
            const b = find(dash, n)
            compare(b.contentItem.lineCount, 1, n + " on one line")
            verify(b.contentItem.contentWidth <= b.width + 0.5, n + " keeps its words")
        }
        compare(find(dash, "dashAdd").text, "Add")
        compare(find(dash, "dashAdd").Accessible.name, "Add a gauge")
        for (const n of ["dashAddCell_0", "dashPasteCell_0"]) {
            const half = find(dash, n)
            verify(half.visible, n + " shows")
            verify(half.width >= Theme.touchTarget - 0.5 && half.height >= Theme.touchTarget - 0.5,
                   n + " is " + half.width + " x " + half.height)
        }
        dash.stopEditing()
    }

    // Settings' profile reaches the Dashboard's picker with no session, and
    // a change there reaches a list already open
    function test_theDashboardsPickerFollowsSettingsOffline() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        const dash = find(main.contentItem, "dashboardPage")
        const settings = find(main.contentItem, "settingsPage")
        settings.connectPart.uriField.text = ""
        settings.uriPart.chooseProfile("obd2")
        dash.startEditing()
        dash.addFlowPart.pickKind("ring")
        verify(dash.addFlowPart.profileRows.some(r => r.ref === Bridge.gaugeSignals["speed"][1]),
               "obd2's speed, with no URI typed")
        settings.connectPart.uriField.text = "mock://demo"
        tryVerify(() => dash.addFlowPart.profileRows.some(r => r.ref === Bridge.gaugeSignals["speed"][0]),
                  3000, "the list follows the field")
        dash.stopEditing()
    }

    // The Dashboard's phone form: six columns upright, the tach and its
    // first readout side by side under the shift lights, and twelve on
    // its side
    function test_theDashboardsDialsShareARowUpright_data() {
        return [{ tag: "a phone upright", w: 384, h: 832, columns: 6 },
                { tag: "a phone on its side", w: 832, h: 384, columns: 12 }]
    }
    function test_theDashboardsDialsShareARowUpright(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = data.h
        const dash = find(main.contentItem, "dashboardPage")
        main.gotoPage(stackIndex(dash))
        dash.form = "phone"   // the platform's on a phone, so set here
        tryCompare(find(dash, "dashPage_0"), "columns", data.columns, 3000)
        const tach = find(dash, "dashCell_0_1")
        const speedo = find(dash, "dashCell_0_2")
        tryVerify(() => tach.width > 0, 3000, "the grid lays the dials out")
        compare(tach.y, speedo.y, "side by side")
        verify(tach.x + tach.width <= speedo.x, "the tach first")
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

    // Live's window row on a phone, following and paused: Live takes the
    // row's end while paused, the window buttons compact to make room,
    // nothing passes the edge or overlaps, each label keeps one line, and
    // every button is a touch's height (Live a touch's width too)
    function test_livesWindowRowFitsWhilePaused_data() {
        return [{ tag: "384 following", w: 384, paused: false },
                { tag: "384 paused", w: 384, paused: true },
                { tag: "320 paused", w: 320, paused: true }]
    }
    function test_livesWindowRowFitsWhilePaused(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = 800
        main.font.pixelSize = 16
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const live = find(main.contentItem, "livePage")
        main.gotoPage(Array.from(live.parent.children).indexOf(live))
        tryCompare(Theme, "compact", true)
        if (data.paused) {
            tryVerify(() => {
                const s = Bridge.signalModel.chartData(0, 0)
                return s.length > 0 && s[0].samples.length >= 2
            }, 5000, "a history to pause on")
            live.pause()
            verify(live.paused, "paused")
        }
        verify(waitForPolish(main), "laid out")
        waitForRendering(main.contentItem)
        const row = find(live, "chartWindowRow")
        const button = find(live, "liveButton")
        compare(button.visible, data.paused, "Live shows while paused only")
        const out = pastTheEdge(row, main.contentItem, [])
        compare(out.length, 0, "past " + main.contentItem.width + " px: " + out.join(", "))

        const buttons = live.chartWindows.map(s => find(live, "chartWindow_" + s))
            .concat([find(live, "maximizeChart")]).concat(data.paused ? [button] : [])
        const glass = item => {
            const a = item.mapToItem(main.contentItem, 0, 0)
            const b = item.mapToItem(main.contentItem, item.width, item.height)
            return { x0: a.x, x1: b.x, h: b.y - a.y, w: b.x - a.x }
        }
        let right = -Infinity
        for (const b of buttons) {
            const g = glass(b)
            verify(g.h >= Theme.touchTarget - 0.5, b.objectName + " is " + g.h + " tall")
            verify(g.x0 >= right - 0.5, b.objectName + " overlaps the one before")
            right = g.x1
            compare(b.contentItem.lineCount, 1, b.objectName + " on " + b.contentItem.lineCount + " lines")
        }
        if (data.paused)
            verify(glass(button).w >= Theme.touchTarget - 0.5, "Live is " + glass(button).w + " wide")
    }

    // Developer mode's frame-rate readout: in the header, so it covers no
    // page; hidden otherwise, and counting the frames the window draws
    // while a session moves the page
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
        compare(badge.parent, find(main.contentItem, "headerRow"), "in the header's row")
        compare(badge.text, "0 frames/s")
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
        const recording = find(main.contentItem, "linkPillRecording")
        verify(!button.visible, "nothing to mark yet")
        verify(!recording.visible, "nothing recorded yet")
        verify(title.visible)
        Bridge.recording = true
        compare(Bridge.connectSession("mock://marked"), "", "mock://marked connects")
        verify(Bridge.recordingActive)
        verify(button.visible)
        compare(button.text, "Mark")
        verify(recording.visible, "the pill says why Mark is there")
        compare(recording.text, "· recording")
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
        verify(!recording.visible)
        verify(title.visible)
    }

    // A log from the header (ADR-0056 section 1): Start log while
    // connected, then Stop log and the readout in Mark's and Disconnect's
    // place, fitting a phone; Stop log opens the folder in the viewer
    // A log records the ticked rows, text rows among them: a session whose
    // one ticked row is a text offers Start log
    function test_startLogTakesATickedTextRowAlone() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        const start = find(main.contentItem, "startLogButton")
        compare(Bridge.connectSession("mock://demo?profile=uds-generic&did=F190:text"), "")
        tryVerify(() => start.visible, 5000, "Start log once connected")
        verify(!start.enabled, "nothing ticked: a text row's default is unticked")
        Bridge.signalModel.toggleSubscribed(0)
        tryVerify(() => start.enabled, 2000, "the ticked text row is a log's")
    }

    // The armed recorder's button (ADR-0063 section 7): off with its reason
    // while no condition records, "Armed" while it waits, "Recording
    // (window N)" in a window, Mark only in a window; it fits at 384 and 320 px
    function armedConditions(bound) {
        return '{"havne_conditions":1,"conditions":[{"key":"rec","name":"Engine stopped",'
               + '"when":{"signal":["id:3001"],"unit":"km/h",' + bound + '},'
               + '"record":{"before":0.2,"after":0.2}}]}'
    }
    function test_theHeadersArmedHasItsStatesAndFitsAPhone() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 800
        main.font.pixelSize = 16
        const armed = find(main.contentItem, "armedButton")
        const mark = find(main.contentItem, "markButton")
        const start = find(main.contentItem, "startLogButton")
        const row = find(main.contentItem, "headerRow")
        verify(!armed.visible, "nothing to arm before a connect")
        verify(Bridge.saveConditions(armedConditions('"above":90000')).ok, "a set that records")
        // With no record condition: shown, off, and says why
        Bridge.saveConditions('{"havne_conditions":1,"conditions":[{"key":"c","name":"C",'
                              + '"when":{"signal":["id:1001"],"unit":"RPM","above":90000},"mark":true}]}')
        compare(Bridge.connectSession("mock://armed"), "", "mock://armed connects")
        tryVerify(() => armed.visible, 5000, "Armed once connected")
        verify(!armed.enabled, "no condition records")
        compare(armed.text, "Arm", "off, it says what a press does")
        compare(Bridge.armedWhy, "no condition records")
        compare(armed.Accessible.description, "no condition records")
        // A tap on the disabled button says why, as a refused press does
        const host = find(main.contentItem, "armedHost")
        Bridge.clearLogNotice()
        waitForRendering(main.contentItem)
        mouseClick(host)
        tryCompare(Bridge, "logNotice", "Armed not started: no condition records", 3000)
        verify(!Bridge.armedActive, "a tap on the explanation arms nothing")
        const widths = [384, 320]
        for (const width of widths) {
            main.width = width
            waitForRendering(main.contentItem)
            const out = pastTheEdge(row, main.contentItem, [])
            compare(out.length, 0, "idle at " + width + " px: " + out.join(", "))
        }
        main.width = 384
        // A set that records, never firing: armed and waiting
        verify(Bridge.saveConditions(armedConditions('"above":90000')).ok)
        tryVerify(() => armed.enabled, 3000, "a condition records")
        Bridge.clearLogNotice()
        mouseClick(host)
        tryVerify(() => Bridge.armedActive, 3000, "a press arms; the explanation stays out of it")
        compare(Bridge.logNotice, "", "no refusal while it can start")
        verify(armed.checked, "shown as on")
        compare(armed.text, "Armed")
        verify(!start.visible, "Start log steps aside: one recorder")
        verify(!mark.visible, "Mark only inside a window")
        for (const width of widths) {
            main.width = width
            waitForRendering(main.contentItem)
            const out = pastTheEdge(row, main.contentItem, [])
            compare(out.length, 0, "waiting at " + width + " px: " + out.join(", "))
        }
        main.width = 384
        armed.clicked()
        verify(!Bridge.armedActive, "a second press disarms")
        tryVerify(() => start.visible, 2000, "Start log is back")
        // A set that fires at once: a window, with Mark
        verify(Bridge.saveConditions(armedConditions('"below":1')).ok)
        tryVerify(() => armed.enabled, 3000)
        armed.clicked()
        tryVerify(() => Bridge.armedRecording, 10000, "a window opens")
        compare(armed.text, "Window 1", "a phone's header has no room for more")
        verify(mark.visible, "Mark inside a window")
        for (const width of widths) {
            main.width = width
            waitForRendering(main.contentItem)
            compare(armed.text, "Window 1")
            const out = pastTheEdge(row, main.contentItem, [])
            compare(out.length, 0, "in a window at " + width + " px: " + out.join(", "))
        }
        // With room (420 px of header row and up) it says the whole of it
        main.width = 600
        waitForRendering(main.contentItem)
        compare(armed.text, "Recording (window 1)")
        compare(pastTheEdge(row, main.contentItem, []).length, 0, "in a window at 600 px")
        main.width = 384
        armed.clicked()
        verify(!Bridge.armedActive)
        verify(!mark.visible, "Mark goes with the recorder")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
    }
}
