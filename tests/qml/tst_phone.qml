// File/layer: tests/qml/tst_phone.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.phone (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "Phone"
    when: windowShown

    Component { id: mainComponent; Main {} }
    // Main's `ui` store, as a window made later reads it
    Component {
        id: uiStoreComponent
        Settings {
            category: "ui"
            property int minSweepMs: -1
        }
    }
    function storedUi(key) {
        const reader = uiStoreComponent.createObject(null)
        const value = reader[key]
        reader.destroy()
        return value
    }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        Theme.developerMode = false
    }

    function cleanup() {
        Relay.stop()
        Theme.developerMode = false
        Theme.night = "off"
        Theme.zoom = 1.0
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Bridge.recording = false
        Bridge.minSweepMs = 0
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
            // Any other rotated item (a gauge's needle, a Shape turned about
            // the dial's center) is judged by the circle its corners sweep:
            // one mapped corner would depend on the angle, which follows
            // the mock's drive cycle, so the same layout passes or fails
            // with how long the connect took
            if (child.rotation !== 0) {
                const mid = child.mapToItem(edge, child.width / 2, child.height / 2).x
                const right = mid + Math.hypot(child.width, child.height) / 2
                if (right > edge.width + 0.5)
                    out.push(label(child) + " at " + Math.round(right))
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

    // A CSV whose guess has a time with no unit, a long header, a column
    // the guess leaves out, and notes
    readonly property string guessFile:
        "Time since the logger started,Engine speed measured at the crankshaft position sensor [rpm],"
        + "Intake air temperature after the intercooler (deg C),Always empty\n"
        + "0,800.5,31.5,\n1,900.25,31.75,\n2,950,32,\n"

    // The Log's buttons and segments reach a phone's touch size, in both
    // views, so a thumb never meets a 30 px switch
    function test_theLogsControlsReachATouchTarget() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        Theme.developerMode = true
        main.width = 384
        main.height = 800
        main.font.pixelSize = 16
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const page = find(main.contentItem, "logPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        verify(waitForPolish(main), "laid out")
        verify(Theme.touchTarget >= 48, "the phone's target")
        page.view = "activity"
        for (const n of ["copyLog", "view_activity", "view_frames"]) {
            const b = find(page, n)
            verify(b.visible, n + " shows")
            verify(b.height >= Theme.touchTarget - 0.5, n + " is " + b.height + " high")
        }
        page.view = "frames"
        verify(waitForPolish(main), "laid out")
        for (const n of ["pauseFrames", "clearFrames", "exportFrames", "table_frames", "table_byId",
                         "filterToggle"]) {
            const b = find(page, n)
            verify(b.visible, n + " shows")
            verify(b.height >= Theme.touchTarget - 0.5, n + " is " + b.height + " high")
        }
    }

    // The test phone upright, in device-independent pixels (1440 px
    // at 600 dpi)
    function test_everyPageFitsAPhonesWidth_data() {
        return [{ tag: "homePage" }, { tag: "dashboardPage" },
                { tag: "dashboardPhone" }, { tag: "dashboardPhoneCell" },
                { tag: "dashboardPhoneAdd" }, { tag: "dashboardPhoneSignal" },
                { tag: "dashboardPhoneReset" }, { tag: "dashboardPhoneUndo" },
                { tag: "livePage" },
                { tag: "diagnosticsPage" }, { tag: "logPage" }, { tag: "logFrames" },
                { tag: "recordingsPage" }, { tag: "recordingsOpened" }, { tag: "recordingsViewer" },
                { tag: "recordingsCompare" }, { tag: "recordingsImportCsv" },
                { tag: "recordingsImportCandump" }, { tag: "recordingsImportGuess" },
                { tag: "settingsPage" }]
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
        // dashboardPage is the desktop form at a phone's width;
        // dashboardPhone* the phone's, in edit mode with each of its
        // panels, and outside it with the undo line (nothing left stored)
        const dashPhone = data.tag.startsWith("dashboardPhone")
        const name = data.tag === "logFrames" ? "logPage"
                   : viewer || importing ? "recordingsPage"
                   : dashPhone ? "dashboardPage" : data.tag
        const page = find(main.contentItem, name)
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        verify(page.visible, name + " is on screen")
        if (dashPhone) {
            page.form = "phone"
            if (data.tag !== "dashboardPhone")
                page.startEditing()
            if (data.tag === "dashboardPhoneCell") {
                page.select(1)
                page.moreShown = true
            } else if (data.tag === "dashboardPhoneAdd") {
                page.startAdd()
            } else if (data.tag === "dashboardPhoneSignal") {
                page.pickKind("analog")
            } else if (data.tag === "dashboardPhoneReset") {
                page.moreShown = true
                page.confirmReset = true
            } else if (data.tag === "dashboardPhoneUndo") {
                page.stopEditing()
                page.removeCell(1)
                page.undo()
                verify(find(page, "dashUndoLineUndo").visible, "the line with its Undo")
            }
        }
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
        if (data.tag === "recordingsImportGuess") {
            // The guessed map's preview: long header names, a unit to
            // choose, a left-out column, and the notes open
            page.chooseImportFile(Harness.writeScratchFile(
                "a-logger-export-with-a-long-file-name-2026-11-07.csv", guessFile))
            tryVerify(() => !page.importGuessing && !Bridge.recordingImport.busy, 10000,
                      "the guess ends")
            const preview = find(page, "guessPreview")
            verify(preview.visible, "the preview shows")
            preview.notesOpen = true
        } else if (importing) {
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

    // The drive view's settings on the narrowest phone, also at the top
    // zoom: a word longer than any row can't widen the page or leave its
    // button. Only 100 % judges the whole page: other rows pass the edge at
    // the top zoom.
    function test_theDriveSettingsFitAPhone_data() {
        return [{ tag: "100 %", zoom: 1.0 }, { tag: "the top zoom", zoom: 2.0 }]
    }
    function test_theDriveSettingsFitAPhone(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 320
        main.height = 800
        main.font.pixelSize = 16
        Theme.zoom = data.zoom
        const page = find(main.contentItem, "settingsPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        page.open("display")
        const display = find(page, "driveHideRow").parent
        wait(300)
        const widthBefore = display.width
        display.driveShown = true
        display.orientationShown = true
        find(page, "driveHide_0").text = "W".repeat(60)
        find(page, "driveOrientation_follow").text = "W".repeat(60)
        for (const name of ["look_dark", "night_off", "nightStyle_amber", "units_metric"])
            find(page, name).text = "W".repeat(60)
        wait(300)
        // Geometry alone is read below; a render waits out 5 s at 100 % when no frame is due
        verify(waitForPolish(main), "laid out")
        compare(display.width, widthBefore, "the rows leave the page's width alone")
        for (const row of ["driveHideRow", "driveOrientationRow"]) {
            const past = pastTheEdge(find(page, row), display, [])
            compare(past.length, 0, row + " past the page: " + past.join(", "))
        }
        if (data.zoom === 1.0) {
            const out = pastTheEdge(page, main.contentItem, [])
            compare(out.length, 0, "past " + main.contentItem.width + " px: " + out.join(", "))
        }
        for (const name of ["driveHide_2000", "driveHide_4000", "driveHide_8000", "driveHide_0",
                            "driveOrientation_follow", "driveOrientation_upright",
                            "driveOrientation_side", "look_dark", "night_off", "nightStyle_amber",
                            "units_metric"]) {
            const seg = find(page, name)
            const label = seg.contentItem
            verify(label.contentWidth <= label.width + 0.5,
                   name + " is " + label.contentWidth + " wide in " + label.width)
            verify(label.contentHeight <= seg.height + 0.5,
                   name + " is " + label.contentHeight + " high in " + seg.height)
        }
    }

    // On a phone upright and on its side, the viewer's page scrolls to all
    // of it: the chart a readable height, the frames check's line inside
    // the page once scrolled to its end, and the filter's options inside
    // the window
    // The viewer's Frames view on a phone's width: the switch, the table
    // narrowed to the page (no DLC column), its rows inside the window, and
    // the filter's fields inside it too
    function test_theExportPanelFitsAPhone() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        Theme.developerMode = true
        main.width = 384
        main.height = 832
        main.font.pixelSize = 16
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        page.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        tryVerify(() => find(page, "chartFrame").visible, 10000, "the recording opens")
        verify(waitForPolish(main), "laid out")
        const button = find(page, "exportButton")
        verify(button.mapToItem(main.contentItem, button.width, 0).x <= main.contentItem.width + 0.5,
               "the button is on the screen")
        button.clicked()
        const panel = find(page, "exportPanel")
        tryVerify(() => panel.visible && panel.sizes.length >= 3, 10000, "the sizes")
        panel.rowChoice = "custom"
        panel.raw = true
        verify(waitForPolish(main), "laid out")
        const over = []
        pastTheEdge(panel, main.contentItem, over)
        compare(over, [], "past the edge: " + over.join(", "))
        // The panel's controls are a touch's size on the glass
        for (const name of ["exportFormat_csv", "exportFormat_folder", "exportLayout_wide",
                            "exportRows_sweep", "exportRows_custom", "exportCustomMs",
                            "exportGo", "exportClose", "exportRaw"]) {
            const item = find(panel, name)
            const top = item.mapToItem(main.contentItem, 0, 0).y
            const bottom = item.mapToItem(main.contentItem, 0, item.height).y
            verify(bottom - top >= 47.5, name + " is " + (bottom - top) + " tall")
        }
        panel.rowChoice = "sweep"
        panel.raw = false
        Theme.developerMode = false
    }

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

    // The test phone upright: a recording opens onto its chart, under the
    // title row, and the rows that drive it start on the first screen;
    // what's on it is a touch's size
    function test_theViewersDataIsOnTheFirstScreen() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        main.font.pixelSize = 16
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        const kept = page.chartHeight
        page.chartHeight = -1
        page.open({ title: "Findings", uri: "replay://x?profile=obd2", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "obd2" })
        const rows = find(page, "signalRows")
        tryVerify(() => rows.visible && rows.itemAtIndex(0) !== null, 10000, "the recording opens")
        verify(waitForPolish(main), "laid out")
        try {
            const scroll = find(page, "recordingsScroll")
            compare(scroll.contentY, 0, "the page's top")
            verify(!find(page, "recordingsHeader").visible, "no page heading while viewing")
            const chart = find(page, "chartFrame")
            const chartTop = chart.mapToItem(scroll, 0, 0).y
            verify(chartTop <= 120, "the chart starts at " + chartTop)
            verify(chart.height >= 248, "the chart is " + chart.height)
            const first = rows.itemAtIndex(0)
            const rowTop = first.mapToItem(scroll, 0, 0).y
            const rowBottom = first.mapToItem(scroll, 0, first.height).y
            verify(rowBottom <= scroll.height + 0.5,
                   "the first row ends at " + rowBottom + " of " + scroll.height)
            verify(rowTop > chartTop + chart.height, "under the chart")
            for (const name of ["backToList", "exportButton", "zoomAll", "zoomOut", "zoomIn",
                                "maximizeChart", "detailView_values", "detailView_frames"]) {
                const item = find(page, name)
                const top = item.mapToItem(main.contentItem, 0, 0).y
                const bottom = item.mapToItem(main.contentItem, 0, item.height).y
                verify(item.visible && bottom - top >= 47.5, name + " is " + (bottom - top) + " tall")
                const right = item.mapToItem(main.contentItem, item.width, 0).x
                verify(right <= main.contentItem.width + 0.5, name + " ends at " + right)
            }
            const back = find(page, "backToList")
            compare(back.Accessible.name, "All recordings")
            verify(back.mapToItem(main.contentItem, 0, 0).x < 24, "the back arrow at the left edge")
        } finally {
            page.chartHeight = kept
        }
    }

    // A recording folder of twelve number signals, three samples each
    function wideRecording(name) {
        const folder = Harness.writeFramesRecording(name, 1, 100000)
        verify(folder !== "", "the folder is made")
        let header = "ecuio session recording, format 2 (ADR-0049)\n"
                     + "started  2026-11-03 10:00:00.000 (local time)\n"
                     + "utc  2026-11-03T16:00:00.000Z\ncore  0.3.6\n"
                     + "uri  socketcan://vcan0?profile=raw\nprofile  raw\n"
        let rows = "host_us,signal,value,unit\n"
        for (let s = 1; s <= 12; ++s)
            header += "signal  Signal " + s + ",number,V,0,0\n"
        for (let t = 1; t <= 3; ++t) {
            for (let s = 1; s <= 12; ++s)
                rows += (t * 100000) + ",Signal " + s + "," + s + ",V\n"
        }
        header += "\n10:00:01.000  +1.000  end  havne-cli: seconds: 1 frames, 36 values, "
                  + "0 records dropped, 0 notes dropped\n"
        Harness.writeScratchFile(name + "/events.log", header)
        Harness.writeScratchFile(name + "/signals.csv", rows)
        return folder
    }

    // The test phone upright: twelve signals' rows stop at five, under
    // 40 % of the screen, so there is always more page than list to swipe
    function test_theViewersRowsStopAtFiveOnAPhone() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        main.font.pixelSize = 16
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        page.open({ title: "Twelve", uri: "replay://x?profile=raw", source: "",
                    folder: wideRecording("twelve-signals"), profile: "raw" })
        const rows = find(page, "signalRows")
        tryVerify(() => rows.visible && rows.count === 12, 10000, "the recording opens")
        verify(waitForPolish(main), "laid out")
        const scroll = find(page, "recordingsScroll")
        verify(rows.height <= 5 * page.rowHeight + 0.5,
               "the rows are " + rows.height + " for rows of " + page.rowHeight)
        verify(rows.height <= 0.4 * scroll.height + 0.5, "the rows are " + rows.height + " of " + scroll.height)
        verify(rows.height >= 4 * page.rowHeight, "four rows show at least: " + rows.height)
    }

    // On its side the viewer gives the page to the view the switch names:
    // Values the chart, Frames the table, and the switch leads the zoom
    // row in both, so either leads back
    function test_theSideViewFollowsTheSwitch() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 832
        main.height = 384
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        page.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        const chart = find(page, "chartFrame")
        tryVerify(() => chart.visible, 10000, "the recording opens")
        verify(waitForPolish(main), "laid out")
        verify(Theme.shortLayout && page.chartMaximized, "on its side the chart has the page")
        const table = find(page, "viewerFrameTable")
        const frames = find(page, "detailView_frames")
        const values = find(page, "detailView_values")
        const scroll = find(page, "recordingsScroll")
        verify(!table.visible, "Values: no table")
        verify(frames.visible, "the switch shows on its side")
        verify(frames.mapToItem(scroll, 0, 0).y > chart.mapToItem(scroll, 0, 0).y + chart.height - 1,
               "in the zoom row, under the chart")
        tap(frames, frames.width / 2, frames.height / 2)
        tryVerify(() => page.framesShown && table.visible, 2000, "Frames: the table")
        verify(waitForPolish(main), "laid out")
        verify(!chart.visible, "and no chart")
        verify(page.chartMaximized, "still the side view")
        tryVerify(() => find(table, "frameList").count > 0, 10000, "the rows")
        const foot = table.mapToItem(scroll, 0, table.height).y
        verify(foot >= scroll.height - Theme.pageMargin - 1 && foot <= scroll.height + 0.5,
               "the table runs to the page's foot: " + foot + " of " + scroll.height)
        verify(table.height >= 3 * Theme.touchTarget, "the table is " + table.height)
        verify(values.visible && frames.mapToItem(scroll, 0, 0).y < table.mapToItem(scroll, 0, 0).y,
               "the switch above the table")
        tap(values, values.width / 2, values.height / 2)
        tryVerify(() => !page.framesShown && chart.visible, 2000, "Values: the chart again")
        verify(!table.visible, "no table")
    }

    // Upright on the narrowest phone, the maximized viewer's switch has a
    // row of its own above the zoom row, which would otherwise push the
    // page past the glass
    function test_aNarrowMaximizedViewerKeepsTheSwitchInARow() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 320
        main.height = 800
        main.font.pixelSize = 16
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        page.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        const chart = find(page, "chartFrame")
        tryVerify(() => chart.visible, 10000, "the recording opens")
        if (!page.chartMaximized)
            page.toggleMaximized()
        try {
            verify(waitForPolish(main), "laid out")
            const frames = find(page, "detailView_frames")
            const zoom = find(page, "zoomAll")
            verify(frames.visible, "the switch shows maximized")
            verify(frames.mapToItem(main.contentItem, 0, frames.height).y
                   <= zoom.mapToItem(main.contentItem, 0, 0).y + 0.5, "in a row above the zoom row")
            const over = []
            pastTheEdge(page, main.contentItem, over)
            compare(over, [], "past the edge: " + over.join(", "))
            tap(frames, frames.width / 2, frames.height / 2)
            const table = find(page, "viewerFrameTable")
            tryVerify(() => page.framesShown && table.visible && !chart.visible, 2000,
                      "Frames: the table has the page")
            verify(waitForPolish(main), "laid out")
            pastTheEdge(page, main.contentItem, over)
            compare(over, [], "past the edge in Frames: " + over.join(", "))
        } finally {
            page.detailView = "values"
            if (page.chartMaximized)
                page.toggleMaximized()
        }
    }

    // The Import panel's preview on a phone: every control in it is a
    // touch's size (48 on the glass at any zoom, in the layout's units
    // Theme.touchTarget), and none is past the window's edge
    function test_theImportPreviewsTargetsAreATouchsSize() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        tryCompare(Theme, "compact", true)
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        page.chooseImportFile(Harness.writeScratchFile("targets.csv", guessFile))
        tryVerify(() => !page.importGuessing && !Bridge.recordingImport.busy, 10000,
                  "the guess ends")
        const preview = find(page, "guessPreview")
        verify(preview.visible)
        verify(waitForPolish(main), "laid out")
        const seen = []
        const small = []
        const walk = item => {
            for (const child of item.children) {
                if (child.visible && child.width > 0
                        && (child.pressed !== undefined || child.placeholderText !== undefined)) {
                    const top = child.mapToItem(main.contentItem, 0, 0).y
                    const bottom = child.mapToItem(main.contentItem, 0, child.height).y
                    seen.push(child.objectName || String(child))
                    if (bottom - top < Theme.touchTarget - 0.5)
                        small.push((child.objectName || String(child)) + " " + Math.round(bottom - top))
                }
                walk(child)
            }
        }
        walk(preview)
        verify(seen.length >= 10, "controls found: " + seen.join(", "))
        compare(small.length, 0, "under " + Theme.touchTarget + ": " + small.join(", "))
    }

    // The text fields are a touch's size on a phone without a page asking
    function test_theFieldsAreATouchsSizeOnAPhone() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        tryCompare(Theme, "compact", true)
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const live = find(main.contentItem, "livePage")
        main.gotoPage(Array.from(live.parent.children).indexOf(live))
        verify(waitForPolish(main), "laid out")
        const field = find(live, "filterText")
        verify(field && field.visible, "Find a signal shows")
        const h = field.mapToItem(main.contentItem, 0, field.height).y
                  - field.mapToItem(main.contentItem, 0, 0).y
        verify(h >= Theme.touchTarget - 0.5, "Find a signal is " + h + " tall")
        verify(field.width >= 80, "and not squeezed out: " + field.width)
        // A signal row's plot box has no text and is a touch's width, and
        // the row still fits the phone
        const box = find(live, "plotCheck")
        verify(box && box.visible, "a plot box shows")
        verify(box.width >= Theme.touchTarget - 0.5, "the plot box is " + box.width + " wide")
        const row = box.parent.parent
        const edge = row.mapToItem(main.contentItem, row.width, 0).x
        const names = []
        const walk = item => {
            for (const c of item.children) {
                if (c.visible && c.width > 0
                        && c.mapToItem(main.contentItem, c.width, 0).x > edge + 0.5)
                    names.push(c.objectName || String(c))
                walk(c)
            }
        }
        walk(row)
        compare(names.length, 0, "past the row's edge: " + names.join(", "))
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

    // The Bluetooth permission is not asked for at launch: the app opens on
    // another page, so the ask waits until Settings is seen, and at 1400
    // wide it is then one ask, with Connection beside the list
    function test_theBluetoothAskWaitsForSettingsToBeSeen() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 1400
        main.height = 800
        verify(waitForPolish(main), "laid out")
        const page = find(main.contentItem, "settingsPage")
        verify(main.currentPage !== main.settingsPage, "the app opens elsewhere")
        verify(!page.visible, "Settings is hidden")
        compare(page.bluetoothAsks, 0, "no ask at launch")
        main.gotoPage(main.settingsPage)
        tryCompare(page, "shownKey", "connection")   // wide: Connection beside the list
        compare(page.bluetoothAsks, 1, "seen: one ask")
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
        compare(bar.count, 5, "the five tabs")
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

    // The Dashboard's title row fits a phone's width whatever the font:
    // a title longer than any font makes "Dashboard" stands in for a wide
    // one, and gives way, eliding, while the buttons keep their words
    function test_theDashboardsHeaderFitsWhateverTheFont() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        const dash = find(main.contentItem, "dashboardPage")
        main.gotoPage(Array.from(dash.parent.children).indexOf(dash))
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
        main.gotoPage(Array.from(dash.parent.children).indexOf(dash))
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
        main.gotoPage(Array.from(dash.parent.children).indexOf(dash))
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
        main.gotoPage(Array.from(dash.parent.children).indexOf(dash))
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
        tall(["moreRename", "moreAddPage", "moreRemovePage", "moreReset"])
        dash.addPage()
        tall(["moreLeft", "moreRight"])
        dash.removePage()
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
        dash.startAdd()
        tall(["addClose", "addKind_analog", "addKind_digital"])
        dash.pickKind("analog")
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
        main.gotoPage(Array.from(dash.parent.children).indexOf(dash))
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
        settings.uriField.text = ""
        settings.chooseProfile("obd2")
        dash.startEditing()
        dash.pickKind("ring")
        verify(dash.profileRows.some(r => r.ref === Bridge.gaugeSignals["speed"][1]),
               "obd2's speed, with no URI typed")
        settings.uriField.text = "mock://demo"
        tryVerify(() => dash.profileRows.some(r => r.ref === Bridge.gaugeSignals["speed"][0]),
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
        main.gotoPage(Array.from(dash.parent.children).indexOf(dash))
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
    function test_theHeadersLogStartsStopsAndOpensItsFolder() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 800
        main.font.pixelSize = 16
        const start = find(main.contentItem, "startLogButton")
        const stop = find(main.contentItem, "stopLogButton")
        const readout = find(main.contentItem, "logReadout")
        const mark = find(main.contentItem, "markButton")
        const disconnect = find(main.contentItem, "disconnectButton")
        const title = find(main.contentItem, "headerTitle")
        const row = find(main.contentItem, "headerRow")
        verify(!start.visible, "nothing to log before a connect")
        compare(Bridge.connectSession("mock://logged"), "", "mock://logged connects")
        tryVerify(() => start.visible && start.enabled, 5000, "Start log once connected")
        verify(disconnect.visible)
        waitForRendering(main.contentItem)
        verify(!title.visible, "the name gives way to Start log on a phone")
        const pillText = find(main.contentItem, "linkPillText")
        tryCompare(pillText, "text", "Live", 5000, "the state is a word, not 'Ses...'")
        verify(!pillText.truncated, "and it fits")
        compare(find(main.contentItem, "linkPillAddress").text, "· mock://logged")
        let out = pastTheEdge(row, main.contentItem, [])
        compare(out.length, 0, "before the log, past " + main.contentItem.width + " px: "
                               + out.join(", "))
        start.clicked()
        verify(Bridge.logActive, "the log runs")
        verify(stop.visible && readout.visible, "Stop log and the readout")
        verify(!start.visible && !mark.visible && !disconnect.visible,
               "Start log, Mark and Disconnect step aside")
        tryVerify(() => Bridge.logBytes > 0 && Bridge.logElapsedMs >= 1000, 10000,
                  "the readout's second")
        // A second's log is well under the 0.1 MB the size shows from
        tryVerify(() => /^\d+:\d\d$/.test(readout.text), 2000, readout.text)
        verify(!find(main.contentItem, "linkPillRecording").visible, "a log is no recording")
        waitForRendering(main.contentItem)
        verify(!title.visible, "the name gives way on a phone")
        out = pastTheEdge(row, main.contentItem, [])
        compare(out.length, 0, "past " + main.contentItem.width + " px: " + out.join(", "))

        // A ten-hour log's readout: the name stays away at 420 px of
        // window and comes back at 420 px of header row, and both fit
        const gutter = main.width - row.width
        readout.text = "10:05:07\n123.4 MB"
        for (const [width, named] of [[420, false], [420 + gutter, true]]) {
            main.width = width
            waitForRendering(main.contentItem)
            compare(title.visible, named, "the name at " + width + " px")
            out = pastTheEdge(row, main.contentItem, [])
            compare(out.length, 0, "past " + main.contentItem.width + " px: " + out.join(", "))
        }
        main.width = 384

        const folder = Bridge.recordingFolder
        stop.clicked()
        verify(!Bridge.logActive, "the log stopped")
        verify(!stop.visible && !readout.visible)
        verify(start.visible && disconnect.visible, "Start log and Disconnect again")
        const recordings = find(main.contentItem, "recordingsPage")
        compare(main.currentPage, main.recordingsPage, "on Recordings")
        tryVerify(() => recordings.viewing, 5000, "the log's folder opens")
        compare(recordings.openedTitle, Bridge.recordingRow(folder).title)

        // A log the session's end closes leaves the driver where they are
        const livePage = 2
        main.gotoPage(livePage)
        start.clicked()
        verify(Bridge.logActive, "a second log runs")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting && !Bridge.logActive, 5000,
                  "the disconnect ends the log")
        compare(main.currentPage, livePage, "still on Live")
    }

    // The log readout's text: the time alone under 0.1 MB, then the size
    // beside it where the row has room, else under it
    function test_theLogReadoutShowsASizeOnceThereIsOne_data() {
        return [
            { tag: "aSecond", ms: 2000, bytes: 1000, oneLine: false, text: "0:02" },
            { tag: "aSecondWide", ms: 2000, bytes: 1000, oneLine: true, text: "0:02" },
            { tag: "justUnder", ms: 12000, bytes: 104857, oneLine: true, text: "0:12" },
            { tag: "twoLines", ms: 12000, bytes: 104858, oneLine: false, text: "0:12\n0.1 MB" },
            { tag: "oneLine", ms: 12000, bytes: 104858, oneLine: true, text: "0:12 · 0.1 MB" },
            { tag: "tenHours", ms: 36307000, bytes: 129395916, oneLine: false,
              text: "10:05:07\n123.4 MB" },
        ]
    }
    function test_theLogReadoutShowsASizeOnceThereIsOne(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        compare(main.logReadoutText(data.ms, data.bytes, data.oneLine), data.text)
    }

    // A running log's readout takes one line where the header row has
    // room (a tablet, a desktop window) and two on a phone
    function test_theLogReadoutTakesOneLineWhereTheRowHasRoom() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 800
        const readout = find(main.contentItem, "logReadout")
        const row = find(main.contentItem, "headerRow")
        compare(Bridge.connectSession("mock://readout"), "", "mock://readout connects")
        const start = find(main.contentItem, "startLogButton")
        tryVerify(() => start.visible && start.enabled, 5000, "Start log once connected")
        start.clicked()
        verify(Bridge.logActive, "the log runs")
        // The log's own size is a second's worth: what the row would show
        // of a 0.1 MB one is read from the function with the row's choice
        verify(row.width < 560, "a phone's row is under 560 px: " + row.width)
        verify(!main.logReadoutOneLine, "two lines on a phone")
        compare(main.logReadoutText(1000, 104858, main.logReadoutOneLine), "0:01\n0.1 MB")
        main.width = 700
        waitForRendering(main.contentItem)
        verify(row.width >= 560, "a wide row: " + row.width)
        verify(main.logReadoutOneLine, "one line where the row has room")
        compare(main.logReadoutText(1000, 104858, main.logReadoutOneLine), "0:01 · 0.1 MB")
        compare(readout.font.pixelSize, Theme.fontBody, "the readout is body text")
        main.width = 559 + (main.width - row.width)
        waitForRendering(main.contentItem)
        verify(!main.logReadoutOneLine, "two lines again at " + row.width)
    }

    // The header ends in the one divider the look has, a hairline
    function test_theHeaderEndsInAHairline() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        const rule = find(main.contentItem, "headerRule")
        compare(rule.height, 1)
        compare(rule.color, Theme.hairline)
        verify(rule.visible)
    }

    // The tab bar's labels at a narrow bar (a phone zoomed far in): each
    // stays inside its tab, smaller and elided before it runs into the
    // next
    function test_theTabLabelsStayInsideTheirTabs() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 320
        main.height = 700
        const tabs = find(main.contentItem, "tabBar")
        tryVerify(() => tabs.visible && tabs.height > 0, 2000, "the tab bar shows")
        const pages = [main.dashboardPage, 2, 3, main.recordingsPage, main.settingsPage]
        for (const page of pages)
            compare(find(main.contentItem, "tabLabel_" + page).font.pixelSize, Theme.fontBody,
                    "body text at 64 px a tab")
        Theme.zoom = 2
        waitForRendering(main.contentItem)
        for (const page of pages) {
            const tab = find(main.contentItem, "tab_" + page)
            const label = find(main.contentItem, "tabLabel_" + page)
            verify(tab.width < 64, "a tab at zoom 2: " + tab.width)
            compare(label.font.pixelSize, Theme.fontCaption, "caption text in a narrow tab")
            verify(label.width <= tab.width, label.text + " is " + label.width + " in " + tab.width)
            verify(label.contentWidth <= label.width + 0.5,
                   label.text + " paints " + label.contentWidth + " in " + label.width)
        }
        const settings = find(main.contentItem, "tabLabel_" + main.settingsPage)
        verify(settings.truncated, "Settings elides in a tab of " + settings.width)
    }

    // A recording's Replay connects it in one tap and lands on Live, the
    // address in Settings' field as a typed one would be
    function test_replayConnectsInOneTapAndLandsOnLive() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 800
        main.gotoPage(main.recordingsPage)
        const row = Bridge.recordingRow(Harness.recordingFixtures + "/findings")
        verify(row.uri.startsWith("replay://"), "a replay address: " + row.uri)
        main.replayRecording(row.uri)
        tryVerify(() => Bridge.connected, 10000, "the replay connects")
        compare(Bridge.currentUri, row.uri)
        tryCompare(main, "currentPage", main.livePage, 2000, "on Live once it connects")
        compare(find(main.contentItem, "settingsPage").uriField.text, row.uri, "Settings shows it")
    }

    // A Replay the core refuses (its folder gone) leaves the user on
    // Settings' Connection page with the field's error, never on Live
    function test_aFailedReplayShowsSettingsWithItsReason() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 800
        main.gotoPage(main.recordingsPage)
        const settings = find(main.contentItem, "settingsPage")
        const uri = "replay://" + Harness.recordingFixtures + "/no-such-recording?profile=obd2"
        main.replayRecording(uri)
        tryVerify(() => settings.errorLabel.text !== "", 10000, "the refusal arrives")
        verify(!Bridge.connected, "nothing connected")
        compare(main.currentPage, main.settingsPage, "on Settings, not Live")
        verify(settings.errorLabel.visible, "the refusal shows")
        compare(settings.shownKey, "connection", "the Connection page")
        compare(settings.uriField.text, uri, "the address in the field")
        verify(settings.errorLabel.text !== "", "with its reason")
        compare(main.replayPending, "", "nothing left waiting")
        // Back returns to Recordings, as Other address returns to Dash
        verify(main.goBack(), "Back is the app's")
        compare(main.currentPage, main.recordingsPage, "back to Recordings")
    }

    // One primary on Dash: while its connect list shows, the header's
    // Connect... is secondary and the list's Connect the primary; on
    // another page the header's is primary again
    function test_theHeadersConnectGivesWayToDashsList() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        const dash = find(main.contentItem, "dashboardPage")
        dash.form = "phone"
        main.gotoPage(main.dashboardPage)
        const header = find(main.contentItem, "disconnectButton")
        tryVerify(() => dash.connectShown, 3000, "Dash shows its connect list")
        verify(header.visible, "the header's Connect... shows")
        verify(!header.highlighted, "secondary on Dash")
        verify(!header.flat, "a secondary, not a quiet one")
        main.gotoPage(main.livePage)
        verify(header.highlighted, "primary elsewhere")
    }

    // Show log beside a refusal and the Log's back arrow return to Connection
    // with Back still going to the page that sent it
    function test_aLogRoundTripKeepsBackGoingToTheSender() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        main.gotoPage(main.dashboardPage)
        main.openConnectFromDash()
        compare(main.currentPage, main.settingsPage)
        verify(settings.backLeaves, "Back leaves for Dash")
        settings.requestConnect("mock://demo?profile=no-such-profile")
        tryVerify(() => find(settings, "errorShowLog").visible, 5000, "the refusal")
        settings.logRequested()
        compare(main.currentPage, main.logPage)
        main.logBack()
        compare(main.currentPage, main.settingsPage, "the Log's back arrow")
        verify(settings.backLeaves, "still leaving for Dash")
        verify(main.goBack(), "Back is taken")
        compare(main.currentPage, main.dashboardPage, "where it came from, not the Log")
        verify(!settings.backLeaves, "and the flag is spent")
    }

    // The zoom badge is a small rounded rectangle, not a capsule
    function test_theZoomBadgeIsNotACapsule() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        const badge = find(main.contentItem, "zoomBadge")
        verify(badge.radius <= Theme.radiusCard, "radius " + badge.radius)
    }

    // The desktop's Home is a connect list too, so the header's Connect...
    // is secondary there while no session runs: one primary a page
    function test_theHeadersConnectGivesWayToHomesList() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 1280
        main.height = 720
        const settings = find(main.contentItem, "settingsPage")
        settings.recentUris = ["mock://demo"]
        main.gotoPage(main.homePage)
        const header = find(main.contentItem, "disconnectButton")
        const list = find(main.contentItem, "homeConnectList")
        tryCompare(list, "chosen", "mock://demo", 2000, "a recent row is chosen")
        verify(header.visible, "the header's Connect... shows")
        verify(!header.highlighted, "secondary on Home's list")
        verify(find(list, "connectGo").highlighted, "the list's Connect is the primary")
        main.gotoPage(main.livePage)
        verify(header.highlighted, "primary elsewhere")
    }

    // The header at the narrowest phone and the top zoom: the pill's word
    // elides inside the pill, and the pill stays inside the page. The
    // header's Connect... passes the edge there (160 layout units hold no
    // button of its words beside a pill), which is the review's open item
    function test_thePillsWordElidesAtTheTopZoom() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 320
        main.height = 800
        main.font.pixelSize = 16
        const pill = find(main.contentItem, "linkPill")
        const word = find(main.contentItem, "linkPillText")
        const rate = find(main.contentItem, "frameRate")
        const was = Theme.developerMode
        Theme.developerMode = true
        try {
            verify(waitForPolish(main), "laid out at 100 %")
            verify(!word.truncated, "at 100 % the word is whole")
            verify(!rate.visible, "the frame rate gives way in a row this narrow")
            main.width = 384
            // The row follows the window a resize event later
            tryVerify(() => rate.visible, 2000, "and shows at 384")
            verify(waitForPolish(main), "laid out at 384")
            verify(rate.x >= word.mapToItem(rate.parent, word.width, 0).x + 8 - 0.5,
                   "apart from the word")
            main.width = 320
        } finally {
            Theme.developerMode = was
        }
        Theme.zoom = 2
        verify(waitForPolish(main), "laid out")
        waitForRendering(main.contentItem)
        verify(word.width <= pill.width + 0.5, "the word inside the pill")
        verify(word.contentWidth <= word.width + 0.5 || word.truncated, "painted inside its box or elided")
        verify(pill.mapToItem(main.contentItem, pill.width, 0).x <= main.contentItem.width + 0.5,
               "the pill inside the page")
        compare(find(main.contentItem, "disconnectButton").visible, true, "Connect... still on the row")
    }

    // A Main on a phone's form at 384 x 832, laid out, with Android's text
    function phoneMain() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.testMobile = true
        main.width = 384
        main.height = 832
        main.font.pixelSize = 16
        verify(waitForPolish(main), "laid out")
        return main
    }

    function checkedTabs(bar) {
        const on = []
        for (let i = 0; i < bar.count; ++i) {
            if (bar.itemAt(i).checked)
                on.push(bar.itemAt(i).text)
        }
        return on.join()
    }

    // The bar lists five pages and each tab marks its own; the phone opens
    // on Dash. The Log, reached from the status, marks none, and a tap on
    // the tab that was marked still goes there
    function test_theTabBarHasFivePagesAndThePhoneOpensOnDash() {
        const main = phoneMain()
        const bar = find(main.contentItem, "tabBar")
        tryVerify(() => bar.visible, 2000, "the tab bar shows")
        compare(main.currentPage, main.dashboardPage, "a phone opens on Dash")
        const pages = [main.dashboardPage, 2, 3, main.recordingsPage, main.settingsPage]
        const names = ["Dash", "Live", "Codes", "Recs", "Settings"]
        compare(bar.count, names.length)
        for (let i = names.length - 1; i >= 0; --i) {
            const tab = bar.itemAt(i)
            compare(tab.text, names[i])
            mouseClick(tab)
            tryCompare(main, "currentPage", pages[i], 2000, names[i] + "'s page")
            compare(checkedTabs(bar), names[i], "only its own tab is marked")
            verify(find(tab, "tabMark_" + pages[i]).visible, names[i] + "'s mark")
        }
        const pill = find(main.contentItem, "linkPill")
        mouseClick(pill)
        tryCompare(main, "currentPage", main.logPage, 2000, "the status opens the Log")
        compare(checkedTabs(bar), "", "no tab is the Log's")
        mouseClick(bar.itemAt(0))
        tryCompare(main, "currentPage", main.dashboardPage, 2000, "the last tab still answers")
        compare(checkedTabs(bar), "Dash")
    }

    // The mark is the primary fill in Dark and the text at night, where the
    // primary is a dim fill
    function test_theTabMarkIsTheTextAtNight() {
        const main = phoneMain()
        const mark = find(find(main.contentItem, "tabBar"), "tabMark_" + main.dashboardPage)
        verify(Qt.colorEqual(mark.color, Theme.primary), "Dark")
        Theme.night = "on"
        verify(Theme.nightActive)
        verify(Qt.colorEqual(mark.color, Theme.text), "night")
        verify(!Qt.colorEqual(mark.color, Theme.primary), "not the dim fill")
    }

    // Every header button is a touch's size on the glass and draws inside
    // the bar, in each state the header can be in, the address keeping its
    // width beside them at 100 %
    function checkHeaderButtons(main, names) {
        for (const name of names) {
            const button = find(main.contentItem, name)
            verify(button.visible, name + " shows")
            const r = onGlass(button, main)
            verify(r.height >= 48 - 0.5, name + " is " + r.height + " tall on the glass")
            verify(r.width >= 48 - 0.5, name + " is " + r.width + " wide on the glass")
            verify(button.background.height < button.height, name + " draws inside its target")
        }
    }

    function test_theHeadersButtonsAreATouchsSize_data() {
        return [{ tag: "100 %", zoom: 1.0 }, { tag: "67 %", zoom: 0.67 }]
    }

    function test_theHeadersButtonsAreATouchsSize(data) {
        const main = phoneMain()
        Theme.zoom = data.zoom
        verify(waitForPolish(main), "laid out")
        checkHeaderButtons(main, ["disconnectButton"])    // Connect...

        compare(Bridge.connectSession("mock://demo"), "", "connects")
        const start = find(main.contentItem, "startLogButton")
        tryVerify(() => start.visible && start.enabled, 5000, "Start log")
        waitForRendering(main.contentItem)
        checkHeaderButtons(main, ["startLogButton", "disconnectButton"])
        if (data.zoom === 1.0)
            verify(!find(main.contentItem, "linkPillAddress").truncated, "the address keeps its width")

        start.clicked()
        verify(Bridge.logActive, "the log runs")
        waitForRendering(main.contentItem)
        checkHeaderButtons(main, ["stopLogButton"])
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")

        Bridge.recording = true
        compare(Bridge.connectSession("mock://marked"), "", "a recorded session connects")
        const mark = find(main.contentItem, "markButton")
        tryVerify(() => mark.visible, 5000, "Mark")
        waitForRendering(main.contentItem)
        checkHeaderButtons(main, ["markButton", "disconnectButton"])
    }

    // On the phone Settings' list is where the core's version is shown
    function test_thePhonesSettingsShowTheCoresVersion() {
        const main = phoneMain()
        main.gotoPage(main.settingsPage)
        const label = find(find(main.contentItem, "settingsPage"), "settingsCoreVersion")
        tryVerify(() => label.visible, 2000, "on Settings' list")
        compare(label.text, "core " + Bridge.coreVersion)
        verify(Bridge.coreVersion !== "", "a version to show")
    }

    // A window narrowed past the sidebar's width leaves Home, which the bar
    // doesn't list; the sidebar's own pages stay
    function test_aNarrowedWindowLeavesHomeForDash() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 1400
        main.height = 800
        verify(waitForPolish(main), "laid out")
        compare(main.currentPage, 0, "the desktop opens on Home")
        main.width = 500
        tryCompare(main, "currentPage", main.dashboardPage, 2000)
        main.width = 1400
        tryVerify(() => !find(main.contentItem, "tabBar").visible, 2000, "the sidebar again")
        compare(main.currentPage, main.dashboardPage, "and the page stays")
    }

    // The status reads the state and the address, the address eliding from
    // its end and the state word never; with the sidebar it is no button
    function test_theStatusReadsTheStateAndTheAddress_data() {
        return [{ tag: "100 %", zoom: 1.0 }, { tag: "67 %", zoom: 0.67 }]
    }

    function test_theStatusReadsTheStateAndTheAddress(data) {
        const main = phoneMain()
        Theme.zoom = data.zoom
        verify(waitForPolish(main), "laid out")
        const state = find(main.contentItem, "linkPillText")
        const address = find(main.contentItem, "linkPillAddress")
        const pill = find(main.contentItem, "linkPill")
        const row = find(main.contentItem, "headerRow")
        compare(state.text, "No session")
        verify(!address.visible, "no address without a session")
        verify(onGlass(pill, main).height >= 48 - 0.5,
               "a touch's size: " + onGlass(pill, main).height)

        compare(Bridge.connectSession("mock://demo"), "", "connects")
        tryCompare(state, "text", "Live", 5000)
        compare(address.text, "· mock://demo")
        waitForRendering(main.contentItem)
        verify(address.visible && !address.truncated, "a short address fits")

        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        const long = "mock://a-long-name-for-an-adapter-that-cannot-fit-beside-the-buttons"
        compare(Bridge.connectSession(long), "", "the long one connects")
        tryCompare(state, "text", "Live", 5000)
        waitForRendering(main.contentItem)
        verify(address.truncated, "the address elides")
        verify(!state.truncated && state.width >= state.implicitWidth - 0.5, "the word doesn't")
        compare(pastTheEdge(row, main.contentItem, []).length, 0, "the row fits")
    }

    function test_theStatusIsNoButtonBesideTheSidebar() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 1400
        main.height = 800
        verify(waitForPolish(main), "laid out")
        const pill = find(main.contentItem, "linkPill")
        tryVerify(() => !find(main.contentItem, "tabBar").visible, 2000, "the sidebar shows")
        verify(!pill.enabled, "the sidebar lists the Log")
        const before = main.currentPage
        mouseClick(pill)
        wait(0)
        compare(main.currentPage, before, "a tap stays put")
    }

    // Dash with no session on the phone: the recent addresses, the newest
    // chosen, and one Connect; the list goes once a session runs
    // Dash's list with these recent addresses, and these Bluetooth adapters
    // in place of the machine's own interfaces and ports (none by default),
    // which a list on screen reads again
    function connectListOn(main, uris, bluetooth) {
        const dash = find(main.contentItem, "dashboardPage")
        const settings = find(main.contentItem, "settingsPage")
        dash.form = "phone"
        settings.recentUris = uris
        tryVerify(() => findChild(dash, "dashConnectList") !== null, 2000, "the list loads")
        const list = findChild(dash, "dashConnectList")
        settings.j2534Devices = []
        settings.serialPorts = []
        settings.canInterfaces = []
        settings.bluetoothAdapters = bluetooth || []
        verify(waitForPolish(main), "laid out")
        return list
    }

    // The desktop's Home connects the chosen row through Settings, as
    // Dash's list does: the address in its field, then the session
    function test_homeConnectsThroughSettings() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 1280
        main.height = 720
        const settings = find(main.contentItem, "settingsPage")
        const home = find(main.contentItem, "homePage")
        main.gotoPage(Array.from(home.parent.children).indexOf(home))
        settings.recentUris = ["mock://demo"]
        const list = find(home, "homeConnectList")
        tryCompare(list, "chosen", "mock://demo", 2000, "the recent address")
        find(list, "connectGo").clicked()
        tryVerify(() => Bridge.connected, 5000, "it connects")
        compare(settings.uriField.text, "mock://demo", "through Settings' field")
        tryVerify(() => find(home, "homeSession").visible, 2000, "the session's card")
    }

    // A fresh phone: no recent address, the paired adapter and a BLE one
    // on the list, neither chosen while there are two
    function test_aFreshPhoneListsItsPairedAdapters() {
        const main = phoneMain()
        const list = connectListOn(main, [], [
            { name: "OBD adapter", address: "00:11:22:33:44:55", kind: "bt" },
            { name: "", address: "AA:BB:CC:DD:EE:FF", kind: "ble" }])
        tryVerify(() => findChild(list, "connectRow_1") !== null, 2000, "two rows")
        const first = find(list, "connectRow_0")
        compare(find(first, "connectRowAddress").text, "OBD adapter")
        compare(find(first, "connectRowKind").text, "Bluetooth")
        const second = find(list, "connectRow_1")
        compare(find(second, "connectRowAddress").text, "AA:BB:CC:DD:EE:FF", "no name: the address")
        compare(find(second, "connectRowKind").text, "Bluetooth LE")
        verify(!find(list, "connectRecent").visible, "no Recent group")
        verify(find(list, "connectAdapters").visible, "the Adapters group")
        compare(list.chosen, "", "two adapters: none chosen")
        verify(find(list, "connectPickHint").visible, "says to pick one")
        const go = find(list, "connectGo")
        verify(!go.enabled, "Connect waits for a pick")
        mouseClick(first)
        tryCompare(list, "chosen", "elm327://bt:00:11:22:33:44:55", 2000, "the tap")
        verify(go.enabled && go.highlighted, "then Connect is the primary")
        verify(!find(list, "connectPickHint").visible)
    }

    // Without the permission the Adapters group says why and offers Allow,
    // which asks only when tapped; with it and nothing paired, where to pair
    // one and a scan
    function test_withoutThePermissionTheListAsks() {
        const main = phoneMain()
        const list = connectListOn(main, ["elm327://COM1"])
        compare(list.bluetoothAsks, 0, "nothing asked at the list's first show")
        list.bluetoothAvailable = true
        list.bluetoothPermission = "undetermined"
        verify(waitForPolish(main), "laid out")
        const allow = find(list, "connectAllow")
        verify(allow.visible && find(list, "connectBluetoothWhy").visible, "Allow, and why")
        compare(allow.text, "Allow")
        compare(list.bluetoothAsks, 0, "still nothing asked")
        mouseClick(allow)
        compare(list.bluetoothAsks, 1, "a tap asks")
        list.bluetoothNeedsSettings = true
        compare(allow.text, "Open settings", "asked no more: the settings")
        list.bluetoothNeedsSettings = false
        list.bluetoothPermission = "granted"
        verify(waitForPolish(main), "laid out")
        verify(!allow.visible, "allowed: no Allow")
        verify(find(list, "connectBluetoothNone").visible && find(list, "connectScan").visible,
               "nothing paired: where to pair, and Scan")
        find(main.contentItem, "settingsPage").bluetoothAdapters =
            [{ name: "OBD adapter", address: "00:11:22:33:44:55", kind: "bt" }]
        tryVerify(() => !find(list, "connectBluetoothNone").visible, 2000, "one paired: its row instead")
        list.bluetoothAvailable = false
        list.bluetoothPermission = "unavailable"
    }

    // Where to pair stays within the helper text's two lines at the
    // narrowest phone, beside Scan
    function test_theNoPairedAdapterWordsFitTwoLinesAt320() {
        const main = phoneMain()
        main.width = 320
        const list = connectListOn(main, ["elm327://COM1"])
        list.bluetoothAvailable = true
        list.bluetoothPermission = "granted"
        verify(waitForPolish(main), "laid out")
        const words = find(list, "connectBluetoothNone")
        verify(words.visible, "nothing paired")
        verify(words.lineCount <= 2, "the words take " + words.lineCount + " lines at 320")
    }

    // With no recent address and exactly one adapter, it is chosen; the
    // developer's demo row doesn't count
    function test_aSoleAdapterIsChosen() {
        const main = phoneMain()
        const list = connectListOn(main, [])
        Theme.developerMode = true
        try {
            find(main.contentItem, "settingsPage").serialPorts = [{ name: "COM9", device: "" }]
            tryVerify(() => findChild(list, "connectRow_1") !== null, 2000, "the port and the demo")
            compare(find(find(list, "connectRow_1"), "connectRowKind").text, "Demo data")
            compare(list.chosen, "elm327://COM9", "the one adapter")
            verify(find(list, "connectRow_0").checked, "its row marked")
            const go = find(list, "connectGo")
            verify(go.enabled && go.highlighted && go.text === "Connect", "one tap connects")
            Theme.developerMode = false
            tryVerify(() => findChild(list, "connectRow_1") === null, 2000, "no demo row off developer mode")
            find(main.contentItem, "settingsPage").serialPorts =
                [{ name: "COM9", device: "" }, { name: "COM8", device: "" }]
            tryCompare(list, "chosen", "", 2000, "two: none chosen")
        } finally {
            Theme.developerMode = false
        }
    }

    // An adapter that is also a recent address shows once, under Recent
    function test_anAdapterAlsoRecentShowsOnce() {
        const main = phoneMain()
        const list = connectListOn(main, ["elm327://COM9?profile=obd2"])
        find(main.contentItem, "settingsPage").serialPorts =
            [{ name: "COM9", device: "" }, { name: "COM8", device: "" }]
        tryVerify(() => findChild(list, "connectRow_1") !== null, 2000, "the recent and COM8")
        compare(find(find(list, "connectRow_1"), "connectRowAddress").text, "COM8")
        compare(findChild(list, "connectRow_2"), null, "COM9 once")
        compare(list.chosen, "elm327://COM9?profile=obd2", "the recent one")
    }

    // At most six adapter rows; the rest are Other address...'s
    function test_atMostSixAdaptersShow() {
        const main = phoneMain()
        const list = connectListOn(main, [])
        const ports = []
        for (let i = 1; i <= 8; ++i)
            ports.push({ name: "COM" + i, device: "" })
        find(main.contentItem, "settingsPage").serialPorts = ports
        tryVerify(() => findChild(list, "connectRow_5") !== null, 2000, "six rows")
        compare(findChild(list, "connectRow_6"), null, "no seventh")
        verify(find(list, "connectOther").visible, "the rest through Other address")
    }

    // Every row and button on the list is a touch's size, inside the glass
    function test_theListsTargetsAreATouchsSize() {
        const main = phoneMain()
        const list = connectListOn(main, ["elm327://COM1"],
                                   [{ name: "OBD adapter", address: "00:11:22:33:44:55", kind: "bt" }])
        list.bluetoothAvailable = true
        list.bluetoothPermission = "undetermined"
        verify(waitForPolish(main), "laid out")
        try {
            for (const name of ["connectRow_0", "connectRow_1", "connectAllow", "connectOther", "connectGo"]) {
                const item = find(list, name)
                const top = item.mapToItem(main.contentItem, 0, 0).y
                const bottom = item.mapToItem(main.contentItem, 0, item.height).y
                verify(item.visible && bottom - top >= 47.5, name + " is " + (bottom - top) + " tall")
            }
            const over = []
            pastTheEdge(list, main.contentItem, over)
            compare(over, [], "past the edge: " + over.join(", "))
        } finally {
            list.bluetoothAvailable = false
            list.bluetoothPermission = "unavailable"
        }
    }

    function test_dashsConnectListConnectsTheChosenAddress() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const list = connectListOn(main, ["mock://newest", "elm327://bt:00:11:22:33:44:55",
                                          "mock://older"])
        const first = find(list, "connectRow_0")
        const second = find(list, "connectRow_1")
        verify(first.checked, "the newest is chosen")
        verify(find(first, "connectRowMark").visible, "and marked")
        verify(!find(second, "connectRowMark").visible)
        compare(find(first, "connectRowAddress").text, "mock://newest")
        compare(find(first, "connectRowKind").text, "Demo data")
        compare(find(second, "connectRowAddress").text, "elm327://bt:…:44:55", "the short form")
        compare(find(list, "connectGo").text, "Connect")
        verify(find(list, "connectGo").highlighted, "a chosen row: Connect is the list's primary")

        const third = find(list, "connectRow_2")
        mouseClick(third)
        tryVerify(() => third.checked && !first.checked, 2000, "a tap chooses the row")
        mouseClick(third)
        wait(0)
        verify(third.checked, "a second tap keeps it chosen")

        mouseClick(find(list, "connectGo"))
        tryVerify(() => Bridge.connected, 5000, "the chosen address connects")
        compare(Bridge.currentUri, "mock://older")
        compare(JSON.stringify(settings.recentUris), JSON.stringify(
                    ["mock://newest", "elm327://bt:00:11:22:33:44:55", "mock://older"]),
                "the mock is never kept: the list as it was")
        compare(find(settings, "uriField").text, "mock://older", "Settings shows it")
        const dash = find(main.contentItem, "dashboardPage")
        tryVerify(() => findChild(dash, "dashConnectList") === null, 2000, "a session ends the list")

        // The list comes back as it was, the tapped row still chosen
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        tryVerify(() => findChild(dash, "dashConnectList") !== null, 2000, "the list returns")
        const again = findChild(dash, "dashConnectList")
        tryVerify(() => findChild(again, "connectRow_2") !== null, 2000, "its rows")
        verify(find(again, "connectRow_2").checked, "the tap still stands")
        compare(find(find(again, "connectRow_2"), "connectRowAddress").text, "mock://older")
    }

    // Nothing in the list takes a tap while a session starts or ends
    function test_theConnectListWaitsWhileASessionStartsOrEnds() {
        const main = phoneMain()
        const dash = find(main.contentItem, "dashboardPage")
        const list = connectListOn(main, ["mock://demo"])
        const go = find(list, "connectGo")
        verify(go.enabled, "ready")
        go.clicked()
        verify(Bridge.connecting, "the connect is under way")
        verify(!go.enabled, "Connect waits")
        verify(!find(list, "connectRow_0").enabled, "a row waits")
        verify(!find(list, "connectOther").enabled, "Other address waits")
        compare(go.text, "Connecting…")
        tryVerify(() => Bridge.connected, 5000, "connected")

        Bridge.disconnectSession()
        verify(Bridge.disconnecting, "the teardown is under way")
        const back = findChild(dash, "dashConnectList")
        verify(back !== null, "the list is back at once")
        verify(!find(back, "connectGo").enabled, "Connect waits")
        verify(!find(back, "connectRow_0").enabled, "a row waits")
        compare(find(back, "connectGo").text, "Disconnecting…")
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        tryVerify(() => find(back, "connectGo").enabled, 2000, "and it's ready again")
    }

    // A relay holds the adapter: the list can't connect, as Settings can't
    function test_aRelayStopsTheListsConnect() {
        const main = phoneMain()
        const list = connectListOn(main, ["mock://demo"])
        const go = find(list, "connectGo")
        verify(go.enabled)
        const note = find(list, "connectRelayNote")
        verify(!note.visible, "nothing to say before the relay")
        compare(Relay.start("elm327://bt:00:11:22:33:44:55", 0), "", "the relay starts")
        tryVerify(() => !go.enabled, 2000, "no connect while it runs")
        tryVerify(() => note.visible, 2000, "and the list says why")
        compare(note.text, "Stop the relay to connect")
        Relay.stop()
        tryVerify(() => go.enabled, 2000, "and again after")
        verify(!note.visible, "the words go with the relay")
    }

    function test_aRelayLeavesTheLoneConnectToSettings() {
        const main = phoneMain()
        const list = connectListOn(main, [])
        compare(Relay.start("elm327://bt:00:11:22:33:44:55", 0), "", "the relay starts")
        const go = find(list, "connectGo")
        wait(0)
        verify(go.enabled, "it only opens Settings")
    }

    function vehicleWith(label, uri) {
        const doc = VehicleBook.add(VehicleBook.empty(), label)
        return uri === "" ? doc : VehicleBook.rememberUri(doc, uri)
    }

    function rowIs(list, index, uri) {
        const row = findChild(list, "connectRow_" + index)
        return row !== null && row.checked && find(row, "connectRowAddress").text === uri
    }

    // Picking a vehicle fills Settings' field, so the row Dash has chosen
    // is that vehicle's address, or a connect would save another into it
    function test_theChosenRowIsThePickedVehiclesAddress() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const list = connectListOn(main, ["elm327://COM1", "elm327://COM2"])
        verify(rowIs(list, 0, "elm327://COM1"), "no vehicle: the newest")

        settings.vehicles = vehicleWith("Daily driver", "elm327://COM2")
        tryVerify(() => rowIs(list, 1, "elm327://COM2"), 2000, "its address, not the newest")
        compare(list.chosen, "elm327://COM2")

        settings.vehicles = vehicleWith("Weekend car", "elm327://COM9")
        tryVerify(() => rowIs(list, 0, "elm327://COM9"), 2000, "listed first when it isn't recent")
        tryCompare(list, "chosen", "elm327://COM9")
        compare(JSON.stringify(settings.recentUris),
                JSON.stringify(["elm327://COM1", "elm327://COM2"]),
                "listing it writes nothing to the recent addresses")

        settings.vehicles = vehicleWith("New car", "")
        tryVerify(() => rowIs(list, 0, "elm327://COM1"), 2000, "none saved: the newest")
    }

    // A tapped row is a choice, as a typed URI is in Settings, until
    // another vehicle is picked
    function test_aTappedRowOutranksTheVehiclesAddress() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const list = connectListOn(main, ["elm327://COM1", "elm327://COM2", "elm327://COM3"])
        settings.vehicles = vehicleWith("Daily driver", "elm327://COM2")
        tryVerify(() => rowIs(list, 1, "elm327://COM2"), 2000, "its address")
        mouseClick(find(list, "connectRow_0"))
        tryVerify(() => rowIs(list, 0, "elm327://COM1"), 2000, "the tap stands")
        compare(list.chosen, "elm327://COM1")
        settings.vehicles = vehicleWith("Weekend car", "elm327://COM2")
        wait(0)
        verify(rowIs(list, 0, "elm327://COM1"), "the same address changes nothing")
        settings.vehicles = vehicleWith("Other car", "elm327://COM3")
        tryVerify(() => rowIs(list, 2, "elm327://COM3"), 2000, "another vehicle's address replaces the tap")
    }

    // A list that changed under a tap, as a connect from Settings changes
    // it, goes back to the newest
    function test_aNewListDropsTheTappedRow() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const list = connectListOn(main, ["mock://first", "mock://second"])
        mouseClick(find(list, "connectRow_1"))
        tryVerify(() => rowIs(list, 1, "mock://second"), 2000, "the tap")
        settings.recentUris = ["mock://third", "mock://first", "mock://second"]
        tryVerify(() => rowIs(list, 0, "mock://third"), 2000, "the newest again")
        compare(list.chosen, "mock://third")
    }

    // A build without Bluetooth has no permission to ask for: a list
    // reading it offers no Allow (Android's answers are the phone's)
    function test_aComputerHasNoBluetoothPermissionToAsk() {
        verify(!Bluetooth.available, "no Bluetooth in this build")
        compare(Bluetooth.permission, "unavailable")
    }

    // Settings' Log row opens the Log page itself, and Back returns to
    // Settings' list
    function test_backFromTheLogReturnsToSettings() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        main.gotoPage(main.settingsPage)
        const row = find(find(settings, "settingsList"), "settingsRow_log")
        tryVerify(() => row.visible && row.height > 0, 2000, "the Log row")
        verify(waitForPolish(main), "laid out")
        const at = row.mapToItem(main.contentItem, row.width / 2, row.height / 2)
        verify(at.y > 0 && at.y < main.contentItem.height, "the row on the glass at " + at.y)
        mouseClick(row)
        tryCompare(main, "currentPage", main.logPage, 2000, "the Log")
        verify(main.goBack(), "Back is taken")
        compare(main.currentPage, main.settingsPage, "back on Settings")
        compare(settings.pageKey, "", "its list")
    }

    // A failed connect on Dash's list offers the Log beside its words; the
    // Log opens on its activity at the newest line, in developer mode too,
    // and Back returns to Dash
    function test_aFailedConnectOffersTheLog() {
        const main = phoneMain()
        Theme.developerMode = true
        const log = find(main.contentItem, "logPage")
        log.view = "frames"
        try {
            const list = connectListOn(main, ["mock://demo?profile=no-such-profile"])
            const show = find(list, "connectShowLog")
            verify(!show.visible, "no failure, no Show log")
            mouseClick(find(list, "connectGo"))
            tryVerify(() => show.visible, 5000, "Show log beside the bridge's words")
            verify(show.height >= Theme.touchTarget - 0.5, "a touch's size: " + show.height)
            const right = show.mapToItem(main.contentItem, show.width, 0).x
            verify(right <= main.contentItem.width + 0.5, "inside the glass: " + right)
            mouseClick(show)
            tryCompare(main, "currentPage", main.logPage, 2000, "the Log")
            verify(!log.showingFrames, "the activity, not the frames")
            const text = find(log, "activityLog")
            compare(text.cursorPosition, text.length, "at its newest line")
            verify(main.goBack(), "Back is taken")
            compare(main.currentPage, main.dashboardPage, "back on Dash")
        } finally {
            Theme.developerMode = false
            log.view = "activity"
        }
    }

    // The Log's back arrow on the tab-bar form: a touch's size at the left
    // edge, back to the page that opened the Log, and Dash from no tab
    function test_theLogsBackArrowReturnsWhereTheLogCameFrom_data() {
        return [{ tag: "Settings", from: 6, name: "Back to Settings" },
                { tag: "Dash", from: 1, name: "Back to Dash" },
                { tag: "no tab", from: 0, name: "Back to Dash" }]
    }
    function test_theLogsBackArrowReturnsWhereTheLogCameFrom(data) {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const log = find(main.contentItem, "logPage")
        main.gotoPage(data.from)
        main.openLog()
        compare(main.currentPage, main.logPage, "the Log")
        verify(waitForPolish(main), "laid out")
        const back = find(log, "logBack")
        verify(back.visible, "the arrow beside the tab bar")
        compare(back.Accessible.name, data.name)
        const top = back.mapToItem(main.contentItem, 0, 0)
        const bottom = back.mapToItem(main.contentItem, back.width, back.height)
        verify(bottom.y - top.y >= 47.5 && bottom.x - top.x >= 47.5,
               "a touch's size: " + (bottom.x - top.x) + " x " + (bottom.y - top.y))
        verify(top.x < 24, "at the left edge: " + top.x)
        mouseClick(back)
        compare(main.currentPage, data.from > 0 ? data.from : main.dashboardPage, "back")
        if (data.from === main.settingsPage)
            compare(settings.pageKey, "", "Settings' list")
    }

    // Beside the sidebar the Log is a listed page: nothing to go back to
    function test_theLogsBackArrowHidesBesideTheSidebar() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 720
        main.height = 800
        main.openLog()
        verify(waitForPolish(main), "laid out")
        verify(!find(main.contentItem, "logBack").visible, "no arrow beside the sidebar")
        main.width = 384
        tryVerify(() => find(main.contentItem, "logBack").visible, 2000, "the arrow with the tab bar")
    }

    // At 320 px in developer mode the arrow, the title and a compact Copy
    // share the title row and the Activity | Frames switch takes a row of
    // its own under it
    function test_theLogsHeaderFitsTheNarrowestPhone() {
        const main = phoneMain()
        main.width = 320
        Theme.developerMode = true
        const log = find(main.contentItem, "logPage")
        main.openLog()
        verify(waitForPolish(main), "laid out")
        const row = find(log, "logTitleRow")
        verify(row.implicitWidth <= log.availableWidth + 0.5,
               "the title row needs " + row.implicitWidth + " of " + log.availableWidth)
        verify(find(log, "copyLog").compact, "Copy is compact")
        const rowBottom = row.mapToItem(main.contentItem, 0, row.height).y
        for (const name of ["logBack", "copyLog", "view_activity", "view_frames"]) {
            const item = find(log, name)
            const right = item.mapToItem(main.contentItem, item.width, 0).x
            verify(item.visible && right <= main.contentItem.width + 0.5, name + " ends at " + right)
        }
        const toggle = find(log, "logViewSwitch")
        verify(toggle.mapToItem(main.contentItem, 0, 0).y >= rowBottom - 0.5,
               "the switch under the title row")
        compare(pastTheEdge(log, main.contentItem, []).length, 0, "nothing past the edge")
    }

    function test_aFailedConnectFromTheListSaysWhyAndChangesNothing() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const refused = "mock://demo?profile=no-such-profile"
        const list = connectListOn(main, [refused, "mock://demo"])
        const error = find(list, "connectError")
        verify(!error.visible, "nothing to say yet")
        mouseClick(find(list, "connectGo"))
        tryVerify(() => error.visible && error.text !== "", 5000, "the bridge's words")
        verify(!Bridge.connected)
        compare(JSON.stringify(settings.recentUris), JSON.stringify([refused, "mock://demo"]),
                "a failed connect writes nothing")
        const second = find(list, "connectRow_1")
        mouseClick(second)
        tryVerify(() => second.checked, 2000)
        mouseClick(find(list, "connectGo"))
        tryVerify(() => Bridge.connected, 5000, "the other connects")
    }

    function test_otherAddressOpensSettingsConnectField() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const list = connectListOn(main, ["mock://demo"])
        mouseClick(find(list, "connectOther"))
        tryCompare(main, "currentPage", main.settingsPage, 2000)
        tryCompare(settings, "shownKey", "connection", 2000)
    }

    function test_withNoRecentAddressConnectGoesToSettings() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        const list = connectListOn(main, [])
        verify(!find(list, "connectOther").visible, "one way to Settings")
        verify(findChild(list, "connectRow_0") === null, "no rows")
        const go = find(list, "connectGo")
        compare(go.text, "Connect…")
        verify(!go.highlighted, "nothing to connect to: it opens Settings, so it isn't primary")
        mouseClick(go)
        tryCompare(main, "currentPage", main.settingsPage, 2000)
        tryCompare(settings, "shownKey", "connection", 2000)
        verify(!Bridge.connecting && !Bridge.connected, "nothing to connect to")
    }

    function test_theConnectListGoesInEditModeAndOnTheDesktopForm() {
        const main = phoneMain()
        const list = connectListOn(main, ["mock://demo"])
        const dash = find(main.contentItem, "dashboardPage")
        verify(list.visible)
        dash.startEditing()
        tryVerify(() => findChild(dash, "dashConnectList") === null, 2000, "edit mode's own panels")
        dash.stopEditing()
        tryVerify(() => findChild(dash, "dashConnectList") !== null, 2000, "back after Done")
        dash.form = "desktop"
        tryVerify(() => findChild(dash, "dashConnectList") === null, 2000, "the desktop keeps Home")
    }

    // Five rows of long addresses fit a phone's width, every target a
    // touch's size on the glass, and the page scrolls to the gauges
    function test_theConnectListFitsAPhone_data() {
        return [{ tag: "384 upright", w: 384, h: 832 }, { tag: "320 upright", w: 320, h: 640 },
                { tag: "on its side", w: 832, h: 384 }]
    }

    function test_theConnectListFitsAPhone(data) {
        const main = phoneMain()
        main.width = data.w
        main.height = data.h
        verify(waitForPolish(main), "laid out")
        const uris = []
        for (let i = 0; i < 5; ++i)
            uris.push("mock://a-long-name-for-an-adapter-that-cannot-fit-in-its-row-" + i)
        const list = connectListOn(main, uris)
        tryVerify(() => list.width > 0 && find(list, "connectRow_4").width > 0, 2000)
        waitForRendering(main.contentItem)
        const out = pastTheEdge(list, main.contentItem, [])
        compare(out.length, 0, "past " + main.contentItem.width + " px: " + out.join(", "))
        const targets = [find(list, "connectGo"), find(list, "connectOther")]
        for (let i = 0; i < 5; ++i)
            targets.push(find(list, "connectRow_" + i))
        for (const t of targets) {
            const r = onGlass(t, main)
            verify(r.height >= 48 - 0.5, t.objectName + " is " + r.height + " tall")
            verify(r.width >= 48 - 0.5, t.objectName + " is " + r.width + " wide")
        }
        if (data.w < 500)
            verify(find(find(list, "connectRow_0"), "connectRowAddress").truncated, "the address elides")
        const view = find(main.contentItem, "dashPage_0")
        verify(view.contentHeight > list.height, "the gauges follow the list in the page")
        const under = find(view, "dashPanelSurface").mapToItem(list, 0, 0).y
        verify(under >= list.height, "the joined panel starts " + under + " down a list "
               + list.height + " tall")
    }

    // The readout's time: m:ss, then h:mm:ss past an hour
    function test_theLogsTimeReadsMinutesThenHours() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        compare(main.logTimeText(0), "0:00")
        compare(main.logTimeText(750 * 1000 + 999), "12:30")
        compare(main.logTimeText(3599 * 1000), "59:59")
        compare(main.logTimeText(3600 * 1000), "1:00:00")
        compare(main.logTimeText((10 * 3600 + 5 * 60 + 7) * 1000), "10:05:07")
        compare(main.logTimeText(-5000), "0:00", "a clock stepped back")
        compare(main.logTimeText(NaN), "0:00")
        compare(main.logTimeText(Infinity), "0:00")
        compare(main.logTimeText(undefined), "0:00")
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
        page.open("connection")   // the relay's box is in it
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

    // The minimum time between a log's sweeps is kept between runs: written
    // as it changes, and the next Main gives it to the bridge
    function test_theMinimumTimeBetweenSweepsIsKept() {
        Bridge.minSweepMs = 0
        const first = createTemporaryObject(mainComponent, testCase)
        verify(first)
        Bridge.minSweepMs = 500
        tryVerify(() => storedUi("minSweepMs") === 500, 2000, "written as it changes")
        first.destroy()
        wait(0)
        Bridge.minSweepMs = 0
        const next = createTemporaryObject(mainComponent, testCase)
        verify(next)
        compare(Bridge.minSweepMs, 500, "the next run starts with it")
        Bridge.minSweepMs = 0
        tryVerify(() => storedUi("minSweepMs") === 0, 2000, "and Off is kept too")
    }
}
