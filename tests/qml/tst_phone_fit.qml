// File/layer: tests/qml/tst_phone_fit.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.phone_fit (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

PhoneKit {
    id: testCase
    name: "Phone"

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
        main.gotoPage(stackIndex(page))
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
                { tag: "recordingsCompare" }, { tag: "recordingsComparePull" },
                { tag: "recordingsImportCsv" },
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
        const viewer = ["recordingsOpened", "recordingsViewer", "recordingsCompare",
                        "recordingsComparePull"].includes(data.tag)
        const importing = data.tag.startsWith("recordingsImport")
        // dashboardPage is the desktop form at a phone's width;
        // dashboardPhone* the phone's, in edit mode with each of its
        // panels, and outside it with the undo line (nothing left stored)
        const dashPhone = data.tag.startsWith("dashboardPhone")
        const name = data.tag === "logFrames" ? "logPage"
                   : viewer || importing ? "recordingsPage"
                   : dashPhone ? "dashboardPage" : data.tag
        const page = find(main.contentItem, name)
        main.gotoPage(stackIndex(page))
        verify(page.visible, name + " is on screen")
        if (dashPhone) {
            page.form = "phone"
            if (data.tag !== "dashboardPhone")
                page.startEditing()
            if (data.tag === "dashboardPhoneCell") {
                page.select(1)
                page.moreShown = true
            } else if (data.tag === "dashboardPhoneAdd") {
                page.addFlowPart.startAdd()
            } else if (data.tag === "dashboardPhoneSignal") {
                page.addFlowPart.pickKind("analog")
            } else if (data.tag === "dashboardPhoneReset") {
                page.moreShown = true
                page.confirmReset = true
            } else if (data.tag === "dashboardPhoneUndo") {
                page.stopEditing()
                page.editsPart.removeCell(1)
                page.historyPart.undo()
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
            page.openPart.chooseImportFile(Harness.writeScratchFile(
                "a-logger-export-with-a-long-file-name-2026-11-07.csv", guessFile))
            tryVerify(() => !page.importGuessing && !Bridge.recordingImport.busy, 10000,
                      "the guess ends")
            const preview = find(page, "guessPreview")
            verify(preview.visible, "the preview shows")
            preview.notesOpen = true
        } else if (importing) {
            const candump = data.tag === "recordingsImportCandump"
            page.openPart.chooseImportFile(Harness.writeScratchFile(
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
            page.openPart.open(row)
            tryVerify(() => find(page, "signalRows").visible, 10000, "the recording opens")
            if (data.tag !== "recordingsOpened")
                page.cursorsPart.setCursor(5000)
        }
        // Compared with itself, aligned on a rise: every field shown
        if (data.tag === "recordingsCompare") {
            page.comparePart.compareWith(row)
            tryVerify(() => page.compareAligned, 10000, "the compared one opens")
            page.comparePart.setAlignOn("rise")
            page.cursorsPart.setCursor(5000)
        }
        // Compared with itself, aligned on a pull: both pull choices
        // shown, in the phone viewer's About, where Compare lives
        if (data.tag === "recordingsComparePull") {
            page.comparePart.compareWith(row)
            tryVerify(() => page.compareAligned, 10000, "the compared one opens")
            page.comparePart.setAlignOn("pull")
            tryVerify(() => page.pullsHere !== null && page.pullsThere !== null, 10000, "the pulls arrive")
            if (page.pinnedViewer)
                page.detailView = "about"
            tryVerify(() => find(page, "alignPullHere").visible && find(page, "alignPullThere").visible,
                      2000, "both pull choices show")
        }
        if (data.tag === "diagnosticsPage") {
            tryVerify(() => find(page, "clearCodes").enabled, 2000, "the report is read")
            find(page, "clearCodes").clicked()
        }
        wait(300)
        waitForRendering(main.contentItem)
        // A value that changed since the last frame lays out on the next polish:
        // read the geometry after it, not between a text and its layout
        verify(waitForPolish(main), "laid out")
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

    // Every page, and each of Settings' pages, in each look at the two
    // narrowest phone widths: nothing paints past the right edge, whatever
    // the palette (a look changes the text and the summaries it shows)
    function test_everyPageFitsAPhonesWidthInEachLook_data() {
        const looks = [{ name: "dark", look: "dark", night: "off", style: "amber" },
                       { name: "daylight", look: "daylight", night: "off", style: "amber" },
                       { name: "nightAmber", look: "dark", night: "on", style: "amber" },
                       { name: "nightRed", look: "dark", night: "on", style: "red" }]
        const rows = []
        for (const l of looks) {
            for (const w of [384, 320])
                rows.push({ tag: l.name + " " + w, look: l.look, night: l.night, style: l.style, w: w })
        }
        return rows
    }

    function test_everyPageFitsAPhonesWidthInEachLook(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        Theme.look = data.look
        Theme.nightStyle = data.style
        Theme.night = data.night
        compare(Theme.active, data.night === "on" ? (data.style === "red" ? "nightRed" : "nightAmber")
                                                  : data.look, "the look in force")
        main.width = data.w
        main.height = 800
        main.font.pixelSize = 16
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const walked = []
        const over = []
        const walk = (page, what) => {
            main.gotoPage(stackIndex(page))
            verify(page.visible, what + " is on screen")
            verify(waitForPolish(main), what + " laid out")
            const out = pastTheEdge(page, main.contentItem, [])
            if (out.length > 0)
                over.push(what + " past " + main.contentItem.width + " px: " + out.slice(0, 4).join(", "))
            walked.push(what)
        }
        for (const name of ["homePage", "dashboardPage", "livePage", "diagnosticsPage", "logPage",
                            "recordingsPage"])
            walk(find(main.contentItem, name), name)
        const settings = find(main.contentItem, "settingsPage")
        for (const key of ["", "connection", "vehicle", "signals", "recording", "display", "advanced"]) {
            settings.pageKey = key
            walk(settings, "settings " + (key || "list"))
        }
        settings.pageKey = ""
        compare(walked.length, 13, "every page was walked")
        compare(over, [], "nothing past the edge")
    }

    // Main's alert band ON, its longest words (a long name, three alerts,
    // the stale line), on every page at the two narrowest phone widths: under
    // the header, above the page, nothing past the edge; Live's alert cell too
    Component { id: alertRowsC; ListModel {} }
    function test_theAlertBandFitsEveryPageOnAPhone_data() {
        return [{ tag: "384", w: 384 }, { tag: "320", w: 320 }]
    }
    function test_theAlertBandFitsEveryPageOnAPhone(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = 800
        main.font.pixelSize = 16
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const rows = createTemporaryObject(alertRowsC, testCase)
        const row = (key, level, name) => ({
            key: key, name: name, enabled: true, level: level, banner: true, live: true,
            state: "on", stale: true, unavailable: false, fired: 1, value: 112.5,
            unit: "°C", decimals: 1, staleSinceMs: new Date(2026, 10, 15, 8, 30, 5).getTime() })
        rows.append(row("a", "danger", "Coolant far past its limit while the engine still runs hard"))
        rows.append(row("b", "warning", "Oil warm"))
        rows.append(row("c", "warning", "Boost high"))
        const band = find(main.contentItem, "alertBand")
        band.conditions = rows
        tryVerify(() => band.shown && band.visible, 2000, "the band is ON")
        find(main.contentItem, "livePage").alertLevels = { "1001": 2 }
        const header = find(main.contentItem, "headerBar")
        const over = []
        const pages = ["homePage", "dashboardPage", "livePage", "diagnosticsPage", "logPage",
                       "recordingsPage", "settingsPage"]
        for (const name of pages) {
            const page = find(main.contentItem, name)
            main.gotoPage(stackIndex(page))
            verify(waitForPolish(main), name + " laid out")
            verify(band.visible, "the band on " + name)
            const top = band.mapToItem(main.contentItem, 0, 0).y
            const bottom = top + band.height
            verify(top >= header.mapToItem(main.contentItem, 0, header.height).y - 0.5,
                   "under the header on " + name)
            verify(page.mapToItem(main.contentItem, 0, 0).y >= bottom - 0.5,
                   name + " starts under the band")
            const out = pastTheEdge(page, main.contentItem, pastTheEdge(band, main.contentItem, []))
            if (out.length > 0)
                over.push(name + ": " + out.slice(0, 4).join(", "))
        }
        compare(over, [], "nothing past the edge")
        verify(find(band, "alertBandStale").visible, "the stale line shows")
    }

    // The Alerts page and an alert's editor at both phone widths, opened
    // from a gauge's Alert as on the phone: the list with a long name, the
    // editor with a signal term's picker open and a link term beside it
    function test_theAlertsPagesFitAPhone_data() {
        return [{ tag: "384", w: 384 }, { tag: "320", w: 320 }]
    }
    function test_theAlertsPagesFitAPhone(data) {
        const none = '{"havne_conditions":1,"conditions":[]}\n'
        verify(Bridge.saveConditions(JSON.stringify({ havne_conditions: 1, conditions: [
            { key: "long", name: "W".repeat(40), when: { signal: ["id:2001"], unit: "°C", above: 115 },
              alert: { level: "danger", banner: true } },
            { key: "link", name: "Link lost", when: { link: "hard_failure" }, alert: { level: "warning" } }] }))
            .ok, "two alerts saved")
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = 800
        main.font.pixelSize = 16
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const settings = find(main.contentItem, "settingsPage")
        const walk = (what) => {
            verify(settings.visible, what + " is on screen")
            verify(waitForPolish(main), what + " laid out")
            compare(pastTheEdge(settings, main.contentItem, []), [], what + " stays inside")
        }
        main.gotoPage(Array.from(settings.parent.children).indexOf(settings))
        settings.open("alerts")
        walk("the list")

        // A gauge's Alert opens the editor in Settings
        const dash = find(main.contentItem, "dashboardPage")
        dash.form = "phone"
        const doc = DashboardBook.builtIn("phone")
        doc.pages = [{ name: "Test", cells: [{ kind: "digital", w: 6, h: 12, label: "Coolant",
                                               refs: ["id:2001"], unit: "°C",
                                               options: { from: 0, to: 150, dangerFrom: 115 } }] }]
        dash.doc = doc
        main.gotoPage(stackIndex(dash))
        dash.select(0)
        find(dash, "panelAlert").clicked()
        verify(settings.visible, "Settings comes forward")
        compare(settings.pageKey, "alertedit", "on the alert's editor")
        const editor = find(settings, "settingsAlertEditor")
        compare(editor.nameField.text, "Coolant")
        find(editor, "alertTermSignal").clicked()
        find(editor, "alertTermAdd").clicked()
        find(find(editor, "alertTerm_1"), "alertTermKind_link").clicked()
        // A term with no signal in the profile offers all four comparisons
        find(editor, "alertTermAdd").clicked()
        const loose = find(editor, "alertTerm_2")
        for (const word of ["above", "below", "equals", "differs"])
            verify(find(loose, "alertComparison_" + word).visible, word + " is offered")
        verify(find(editor, "alertJoin").visible, "the join row shows")
        editor.recordBox.checked = true
        walk("the editor")

        // A second Alert leaves the edit waiting and says why on Dash
        main.gotoPage(stackIndex(dash))
        dash.select(0)
        find(dash, "panelAlert").clicked()
        verify(dash.visible, "Dash stays")
        verify(find(dash, "dashAlertNote").text.indexOf("save or cancel it first") >= 0,
               find(dash, "dashAlertNote").text)
        verify(editor.recordBox.checked, "the edit waits")
        main.gotoPage(Array.from(settings.parent.children).indexOf(settings))
        find(editor, "alertCancel").clicked()
        compare(settings.pageKey, "alerts")
        Bridge.saveConditions(none)
    }

    // A value too long for its row (a refusal's words, a text) is bounded by
    // its column, whatever its text: the name keeps its third and the value
    // ends at the row's margin, at 384 px and the narrowest phone's 320
    function test_aLongValueStaysInItsRowOnAPhone_data() {
        return [{ tag: "384", w: 384 }, { tag: "320", w: 320 }]
    }
    function test_aLongValueStaysInItsRowOnAPhone(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = 800
        main.font.pixelSize = 16
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const page = find(main.contentItem, "livePage")
        main.gotoPage(stackIndex(page))
        const list = find(page, "signalList")
        tryVerify(() => list.itemAtIndex(0) !== null && list.itemAtIndex(0).hasValue, 5000,
                  "a row has a value")
        const row = list.itemAtIndex(0)
        const value = find(row, "valueLabel")
        // The text assigned here ends the binding, so no later value
        // brings the short text back
        value.text = "W".repeat(300)
        verify(waitForPolish(main), "laid out")
        const name = find(row, "nameLabel")
        const right = value.mapToItem(row, value.width, 0).x
        verify(right <= row.width - 12 + 0.5, "the value ends at " + right + " of " + row.width)
        verify(value.x >= name.x + Math.min(name.implicitWidth, Math.round(row.width / 3)) - 0.5,
               "the value starts at " + value.x + ", the name at " + name.x)
        verify(value.width <= Math.round(row.width * 0.6) + 0.5, "the value is " + value.width)
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
        main.gotoPage(stackIndex(page))
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

    // The two cursors on a phone's width: each handle a touch wide and
    // tall, the span line inside the window with its targets a touch's
    // size, the rows still a touch tall with B and the change on their
    // second line, and the statistics' grid under the span line
    function test_theTwoCursorsFitAPhone_data() {
        return [{ tag: "320", w: 320 }, { tag: "384", w: 384 }]
    }
    function test_theTwoCursorsFitAPhone(data) {
        const v = uprightViewer({ title: "Early", uri: "replay://x?profile=obd2", source: "",
                                  folder: Harness.recordingFixtures + "/pull-early", profile: "obd2" })
        const main = v.main
        const page = v.page
        main.width = data.w
        verify(waitForPolish(main), "laid out")
        page.cursorsPart.setCursor(1000)
        page.cursorsPart.setCursorB(3000)
        tryVerify(() => Object.keys(page.spanStats).length > 0, 5000, "the statistics arrive")
        verify(waitForPolish(main), "laid out with the span")
        verify(page.pinnedViewer, "pinned")

        for (const name of ["cursorHandleA", "cursorHandleB"]) {
            const h = find(page, name)
            tryVerify(() => h.visible, 2000, name + " shows")
            verify(h.width >= Theme.touchTarget - 0.5, name + " is " + h.width + " wide")
            verify(h.height >= Theme.touchTarget - 0.5, name + " is " + h.height + " tall")
            verify(h.mapToItem(main.contentItem, h.width, 0).x <= data.w + 0.5, name + " inside the window")
        }
        const chart = find(page, "chart")
        compare(chart.drawn.axisH, ChartLayout.axisHCursor)

        const line = find(page, "spanLine")
        tryVerify(() => line.visible, 2000, "the span line shows")
        compare(find(page, "spanText").text, "A 1.0 s to B 3.0 s · 2.0 s")
        for (const name of ["zoomToSpan", "clearSpan"]) {
            const b = find(page, name)
            verify(b.height >= Theme.touchTarget - 0.5, name + " is " + b.height + " tall")
            verify(b.mapToItem(main.contentItem, b.width, 0).x <= data.w + 0.5, name + " inside the window")
        }
        const text = find(page, "spanText")
        verify(text.contentWidth <= text.width + 0.5, "the span's words fit their box")

        const grid = find(page, "spanStats")
        tryVerify(() => grid.visible, 2000, "the grid shows")
        const zoomRow = find(page, "zoomAll")
        const lineY = line.mapToItem(main.contentItem, 0, 0).y
        verify(lineY > zoomRow.mapToItem(main.contentItem, 0, 0).y, "the span line under the zoom row")
        verify(grid.mapToItem(main.contentItem, 0, 0).y >= lineY + line.height - 0.5, "the grid under the line")
        const rows = find(page, "signalRows")
        const first = rows.itemAtIndex(0)
        verify(first !== null, "a row")
        verify(grid.mapToItem(main.contentItem, 0, grid.height).y
               <= first.mapToItem(main.contentItem, 0, 0).y + 0.5, "the grid above the rows")

        const plotted = page.spanRows
        verify(plotted.length >= 2, "plotted: " + plotted.length)
        const cards = plotted.map(s => find(grid, "statCard_" + s.index))
        for (const c of cards) {
            verify(c.height >= Theme.touchTarget - 0.5, "a card is " + c.height + " tall")
            verify(c.mapToItem(main.contentItem, c.width, 0).x <= data.w + 0.5, "a card inside the window")
        }
        fuzzyCompare(cards[0].width, cards[1].width, 1, "two even columns")
        verify(cards[1].x > cards[0].x, "side by side")
        compare(cards[0].y, cards[1].y)
        const rpm = plotted.find(s => s.name === "RPM")
        const card = find(grid, "statCard_" + rpm.index)
        compare(find(card, "statChange").text, "-19 rpm")
        compare(find(card, "statLine").text, grouped(1982) + " · " + grouped(2012) + " · " + grouped(2003))

        const at = rows.model.findIndex(s => s.name === "RPM")
        tryVerify(() => rows.itemAtIndex(at) !== null, 2000, "the RPM row")
        const row = rows.itemAtIndex(at)
        verify(row.height >= Theme.touchTarget - 0.5 && row.height <= Theme.touchTarget + 0.5,
               "a row stays a touch tall: " + row.height)
        const second = find(row, "cursorSpanLine")
        verify(second.visible, "the row's second line")
        compare(second.text, "B " + grouped(1982) + " rpm · Δ -19 rpm")
        verify(second.mapToItem(row, 0, second.height).y <= row.height + 0.5, "inside the row")
        // Last, as it changes RPM's decimals: three values wider than a card
        // wrap, and the card beside grows with it
        const wide = Object.assign({}, page.spanStats)
        wide[rpm.index] = Object.assign({}, wide[rpm.index], { lowest: 1234567.5, highest: 6789012,
                                                               mean: 4000123.1, decimals: 1 })
        page.spanStats = wide
        const statLine = find(card, "statLine")
        tryVerify(() => statLine.text.indexOf(grouped(1234567) + Qt.locale().decimalPoint + "5") === 0, 2000,
                  statLine.text)
        verify(waitForPolish(main), "laid out")
        verify(!statLine.truncated, "the values are whole: " + statLine.text + " in " + statLine.width)
        verify(statLine.mapToItem(card, 0, statLine.height).y <= card.height + 0.5, "inside its card")
        compare(cards[0].y, cards[1].y)
        compare(cards[0].height, cards[1].height, "one height for the row")
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
        main.gotoPage(stackIndex(page))
        page.openPart.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
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

    // True once the item's place on the glass has held for `polls` checks
    // in a row (use with tryVerify): the page's chart takes its height from
    // the window and the fonts, so the layout can move after the rows show
    function holdsStill(item, root, polls) {
        let last = null
        let still = 0
        return () => {
            const y = item.mapToItem(root, 0, 0).y
            still = last !== null && y === last ? still + 1 : 0
            last = y
            return still >= polls
        }
    }

    function test_theViewersFramesViewFitsAPhone() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(stackIndex(page))
        page.openPart.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
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
        tryVerify(holdsStill(toggle, main.contentItem, 5), 10000, "the layout holds still")
        tap(toggle, toggle.width / 2, toggle.height / 2)
        tryVerify(() => find(page, "filterPanel").visible, 2000, "the filter opens")
        verify(waitForPolish(main), "laid out")
        const panel = find(page, "filterPanel")
        const panelRight = panel.mapToItem(scroll, panel.width, 0).x
        verify(panelRight <= scroll.width + 0.5,
               "the filter ends at " + panelRight + " of " + scroll.width)
    }

    // A plotted signal's Pane button is a touch's size and stays inside its
    // row on the narrowest phone, and its menu lists the choices
    function test_theViewersPaneButtonIsATouchsSizeAndFits_data() {
        return [{ tag: "384", w: 384 }, { tag: "320", w: 320 }]
    }

    function test_theViewersPaneButtonIsATouchsSizeAndFits(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = data.w
        main.height = 832
        const page = find(main.contentItem, "recordingsPage")
        main.gotoPage(Array.from(page.parent.children).indexOf(page))
        page.openPart.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
                    folder: Harness.recordingFixtures + "/findings", profile: "raw" })
        tryVerify(() => find(page, "chartFrame").visible, 10000, "the recording opens")
        verify(waitForPolish(main), "laid out")
        const rows = find(page, "signalRows")
        let button = null
        let at = -1
        tryVerify(() => {
            for (let i = 0; i < rows.count; ++i) {
                const item = rows.itemAtIndex(i)
                const b = item ? findChild(item, "paneButton") : null
                if (b && b.visible) {
                    button = b
                    at = i
                    return true
                }
            }
            return false
        }, 5000, "a plotted signal's row has the button")
        verify(button.height >= Theme.touchTarget - 0.5, "a touch's height: " + button.height)
        verify(button.width >= 40, "wide enough to hit: " + button.width)
        const row = rows.itemAtIndex(at)
        const right = button.mapToItem(row, button.width, 0).x
        verify(right <= row.width + 0.5, "inside its row: " + right + " of " + row.width)
        verify(button.Accessible.name.indexOf("Pane of ") === 0, button.Accessible.name)
        page.panesPart.openPaneMenu(row.signalIndex, button)
        verify(page.paneMenuItems.length >= 3, "by unit, a pane, a new one")
        compare(page.paneMenuItems[page.paneMenuItems.length - 1].text, "New pane")
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
        main.gotoPage(stackIndex(page))
        page.openPart.open({ title: "Findings", uri: "replay://x?profile=raw", source: "",
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
            page.cursorsPart.setCursor(5000)
            tryVerify(() => strip.visible, 2000, "a cursor: its values beside the chart")
            page.cursorsPart.setCursor(NaN)
            find(page, "maximizeChart").clicked()
        }
        tryVerify(() => find(page, "signalRows").visible, 2000, "the rows are back")
        verify(waitForPolish(main), "laid out")
        const chart = find(page, "chartFrame")
        verify(chart.height >= 200, "the chart is " + chart.height + " px")

        const scroll = find(page, "recordingsScroll")
        const check = find(page, "checkSummary")
        compare(page.pinnedViewer, !page.chartMaximized && data.h >= 480, "pinned upright")
        if (page.pinnedViewer) {
            // Upright the page stays put and About's list scrolls to its last finding
            find(page, "detailView_about").clicked()
            verify(waitForPolish(main), "About laid out")
            const findings = find(page, "findingList")
            verify(findings.visible && findings.count > 0, "About's list holds the findings")
            verify(findings.interactive, "About's list scrolls")
            const listBottom = findings.mapToItem(scroll, 0, findings.height).y
            verify(listBottom <= scroll.height + 0.5, "the list ends at " + listBottom + " of " + scroll.height)
            // Scrolled until the check's line is in the list's last row of view
            findings.contentY = Math.max(findings.originY, check.mapToItem(findings.contentItem, 0, check.height).y
                                                           - findings.height)
            verify(waitForPolish(main), "scrolled to the check")
            const checkBottom = check.mapToItem(scroll, 0, check.height).y
            verify(check.visible && checkBottom <= listBottom + 0.5,
                   "the frames check's line ends at " + checkBottom + " of " + listBottom)
            findings.positionViewAtEnd()
            verify(waitForPolish(main), "scrolled")
            const last = findings.itemAtIndex(findings.count - 1)
            verify(last !== null, "the last finding has a row")
            const lastBottom = last.mapToItem(scroll, 0, last.height).y
            verify(lastBottom <= listBottom + 0.5 && listBottom <= scroll.height + 0.5,
                   "the last finding ends at " + lastBottom + " of " + listBottom)
            findings.positionViewAtBeginning()
            find(page, "detailView_values").clicked()
            verify(waitForPolish(main), "Values laid out")
        } else {
            scroll.contentY = Math.max(0, scroll.contentHeight - scroll.height)
            verify(waitForPolish(main), "scrolled")
            const bottom = check.mapToItem(scroll, 0, check.height).y
            verify(check.visible && bottom <= scroll.height + 0.5,
                   "the frames check's line ends at " + bottom + " of " + scroll.height)
        }

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
        main.gotoPage(stackIndex(page))
        const kept = page.chartHeight
        page.chartHeight = -1
        page.openPart.open({ title: "Findings", uri: "replay://x?profile=obd2", source: "",
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
}
