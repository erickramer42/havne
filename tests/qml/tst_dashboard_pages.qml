// File/layer: tests/qml/tst_dashboard_pages.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashboard_pages (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

DashboardKit {
    id: testCase
    name: "Dashboard"

    // A palette by its name: Dark is Night off, the others Night on in a style
    function usePalette(name) {
        const night = Theme.nightPalettes.indexOf(name) >= 0
        Theme.night = night ? "on" : "off"
        if (night)
            Theme.nightStyle = name === "nightRed" ? "red" : "amber"
        else
            Theme.look = name
    }

    Component { id: fieldComponent; TextField { text: "abc" } }

    Component { id: readOnlyFieldComponent; TextField { text: "abc"; readOnly: true } }

    // An item that takes Right when it has the focus
    Component {
        id: keyTakerComponent
        Item {
            property int rights: 0
            Keys.onRightPressed: ++rights
        }
    }

    // Every gauge on the page, each page's own
    function allGauges(page) {
        const found = []
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i]
                if (child.sweeping !== undefined && child.valueText !== undefined)
                    found.push(child)
                walk(child)
            }
        }
        walk(page)
        return found
    }

    // Section 4's grid: the phone's 6 columns upright and 12 on its
    // side, the desktop's one per 220 px from 2 to 12, each cell where
    // DashboardBook.place puts it, a row a quarter of a column (the
    // desktop's 2 x 8 tach square), and no two cells overlapping
    function test_theGridFollowsTheForm_data() {
        return [
            { tag: "a desktop window", form: "desktop", w: 1280, h: 800, columns: 6 },
            { tag: "a narrow desktop window", form: "desktop", w: 500, h: 900, columns: 2 },
            { tag: "a desktop window under 2 columns", form: "desktop", w: 300, h: 900, columns: 2 },
            { tag: "a wide desktop window", form: "desktop", w: 1900, h: 1400, columns: 8 },
            { tag: "a 4K desktop window", form: "desktop", w: 3840, h: 2000, columns: 12 },
            { tag: "a desktop window over 12 columns", form: "desktop", w: 5000, h: 1400,
              columns: 12 },
            { tag: "a phone upright", form: "phone", w: 384, h: 832, columns: 6 },
            { tag: "a phone on its side", form: "phone", w: 832, h: 384, columns: 12 }
        ]
    }

    function test_theGridFollowsTheForm(data) {
        const page = createTemporaryObject(pageComponent, testCase,
                                           { form: data.form, width: data.w, height: data.h })
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 2000, "the page is laid out")
        compare(view.columns, data.columns)
        const cells = page.pages[0].cells
        // Where place puts each cell, its width as the built-in layout is drawn
        const boxes = DashboardBook.stretched(cells, DashboardBook.place(cells, data.columns),
                                              data.columns, true)
        const pitchX = view.columnWidth + page.cellGap
        const shown = []
        for (let i = 0; i < cells.length; ++i) {
            const cell = findChild(page, "dashCell_0_" + i)
            verify(cell, "cell " + i)
            fuzzyCompare(cell.x, boxes[i].x * pitchX, 1e-6, "cell " + i + " x")
            fuzzyCompare(cell.y, boxes[i].y * view.pitch, 1e-6, "cell " + i + " y")
            fuzzyCompare(cell.width, boxes[i].w * view.columnWidth + (boxes[i].w - 1) * page.cellGap,
                         1e-6, "cell " + i + " width")
            verify(cell.x + cell.width <= view.gridWidth + 1e-6, "cell " + i + " in the grid")
            shown.push(cell)
        }
        for (let i = 0; i < shown.length; ++i) {
            for (let j = i + 1; j < shown.length; ++j) {
                const a = shown[i], b = shown[j]
                verify(a.x + a.width <= b.x + 1e-6 || b.x + b.width <= a.x + 1e-6
                       || a.y + a.height <= b.y + 1e-6 || b.y + b.height <= a.y + 1e-6,
                       "cells " + i + " and " + j + " don't overlap")
            }
        }
        const tach = findChild(page, "dashCell_0_1")
        const tallest = cells.reduce((m, c) => Math.max(m, c.h), 1)
        const cap = (Math.max(160, view.height - page.gridTop - page.margin) + page.cellGap) / tallest
        fuzzyCompare(view.pitch, Math.min(pitchX / 4, cap), 1e-6,
                     "a row is a quarter of a column, unless the tallest cell wouldn't fit")
        if (data.form === "desktop")
            fuzzyCompare(tach.height, tach.width, 1e-6, "the tach is square")
    }

    // The phone's dial and its first readout share the row under the
    // shift lights
    function test_thePhonesDialsShareARow() {
        const page = createTemporaryObject(pageComponent, testCase,
                                           { form: "phone", width: 384, height: 832 })
        const tach = findChild(page, "dashCell_0_1")
        const speedo = findChild(page, "dashCell_0_2")
        tryVerify(() => tach.width > 0, 2000, "the grid lays the cells out")
        compare(tach.y, speedo.y, "side by side")
        verify(tach.x < speedo.x)
    }

    // The desktop's pages as tabs, and Left and Right; one page outside
    // edit mode has neither
    function tabOf(page, i) {
        return findChild(page, "dashPageTab_" + i)
    }

    function test_thePageStripAndKeys() {
        const page = pageOf({}, pagesDoc(3), true)
        const host = findChild(page, "dashPageStripHost")
        const swipe = findChild(page, "dashPages")
        verify(host.visible, "the strip shows with three pages")
        const strip = findChild(page, "dashPageStrip")
        verify(strip, "the strip")
        verify(!findChild(page, "dashIndicator").visible, "no indicator on the desktop")
        verify(!swipe.interactive, "the desktop's pages don't swipe")
        compare(tabOf(page, 0).text, "Engine")
        compare(tabOf(page, 2).text, "Page 3")
        verify(!tabOf(page, 3), "a tab per page")
        verify(!findChild(page, "dashPageAdd").visible, "no + tab outside edit mode")
        compare(page.pageIndex, 0)
        verify(tabOf(page, 0).checked)

        verify(waitForPolish(page.Window.window), "laid out")
        mouseClick(tabOf(page, 2))
        compare(page.pageIndex, 2)
        tryCompare(swipe, "currentIndex", 2)
        verify(tabOf(page, 2).checked)
        verify(!tabOf(page, 0).checked)

        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 2, "Right on the last page stays")
        keyClick(Qt.Key_Left)
        compare(page.pageIndex, 1)
        tryCompare(swipe, "currentIndex", 1)
        keyClick(Qt.Key_Left)
        keyClick(Qt.Key_Left)
        compare(page.pageIndex, 0, "Left on the first page stays")

        // With a tab focused the keys still page, one step each, and the
        // focus moves to the shown page's tab
        tabOf(page, 0).forceActiveFocus()
        verify(tabOf(page, 0).activeFocus)
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 1, "one step from a focused tab")
        verify(tabOf(page, 1).activeFocus, "the focus follows to the shown tab")
        keyClick(Qt.Key_Left)
        compare(page.pageIndex, 0)
        verify(tabOf(page, 0).activeFocus)
        tabOf(page, 0).focus = false

        // A focused text field keeps its keys, at its text's end too
        const field = createTemporaryObject(fieldComponent, testCase.Window.window.contentItem)
        field.forceActiveFocus()
        verify(field.activeFocus)
        field.cursorPosition = field.text.length
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 0, "a text field's Right isn't the page's")
        field.cursorPosition = 0
        keyClick(Qt.Key_Left)
        compare(page.pageIndex, 0, "nor its Left at its start")
        field.focus = false
        const readOnly = createTemporaryObject(readOnlyFieldComponent,
                                               testCase.Window.window.contentItem)
        readOnly.forceActiveFocus()
        verify(readOnly.activeFocus)
        readOnly.cursorPosition = readOnly.text.length
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 0, "a read-only field's Right isn't the page's either")
        readOnly.focus = false
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 1, "the field let go")

        // A page added keeps the one shown
        page.doc = pagesDoc(4)
        compare(page.pageIndex, 1)
        tryCompare(swipe, "count", 4)
        compare(swipe.currentIndex, 1, "the page shown stays shown")

        // With more than one page the keys are the page's, ahead of
        // the item that has the focus
        const taker = createTemporaryObject(keyTakerComponent, testCase.Window.window.contentItem)
        taker.forceActiveFocus()
        verify(taker.activeFocus)
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 2)
        compare(taker.rights, 0, "the page took Right")

        // A document of fewer pages keeps the index on one of them, and
        // with one the keys go on to the item that has the focus
        page.doc = pagesDoc(1)
        compare(page.pageIndex, 0)
        verify(!host.visible, "one page has no strip")
        page.startEditing()
        verify(host.visible, "edit mode shows it with one page")
        verify(findChild(page, "dashPageAdd").visible, "with the + tab")
        page.stopEditing()
        verify(!host.visible, "Done hides it again")
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 0)
        compare(taker.rights, 1, "one page leaves Right to the focused item")
    }

    // The phone's pages swipe, with the indicator above the tab bar
    function test_thePhonesPagesSwipe() {
        const page = pageOf({ form: "phone", width: 384, height: 832 }, pagesDoc(3), true)
        const swipe = findChild(page, "dashPages")
        const indicator = findChild(page, "dashIndicator")
        verify(swipe.interactive)
        verify(indicator.visible)
        verify(!findChild(page, "dashPageStrip"), "no strip on the phone")
        compare(indicator.count, 3)
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 0, "the phone has no page keys")

        swipe.incrementCurrentIndex()
        compare(page.pageIndex, 1, "a swipe moves the page")
        compare(indicator.currentIndex, 1)
        page.goTo(2)
        tryCompare(swipe, "currentIndex", 2)
        page.doc = pagesDoc(1)
        verify(!indicator.visible, "one page has no indicator")
    }

    // Each form keeps the page it last showed; one the document no
    // longer has is its last page
    function test_theLastPageShownIsKept() {
        const stored = createTemporaryObject(storeComponent, testCase)
        stored.setValue("desktop", DashboardBook.serialize(pagesDoc(3)))
        const first = pageOf({}, null, false)
        compare(first.pages.length, 3, "the stored document")
        first.goTo(2)
        const store = findChild(first, "dashStore")
        compare(store.desktopPage, 2)
        compare(store.phonePage, 0, "the phone's is its own")
        first.destroy()
        wait(0)
        compare(Number(stored.value("desktopPage")), 2, "stored as the page goes")

        const again = pageOf({}, null, false)
        compare(again.pageIndex, 2, "the same page")
        tryCompare(findChild(again, "dashPages"), "currentIndex", 2)
        const phone = pageOf({ form: "phone", width: 384, height: 832 }, null, false)
        compare(phone.pageIndex, 0, "the phone's document has one page")
        phone.destroy()
        wait(0)

        // The phone's page goes under its own key
        stored.setValue("phone", DashboardBook.serialize(pagesDoc(3)))
        const phoneMany = pageOf({ form: "phone", width: 384, height: 832 }, null, false)
        phoneMany.goTo(1)
        const phoneStore = findChild(phoneMany, "dashStore")
        compare(phoneStore.phonePage, 1, "the phone's page")
        compare(phoneStore.desktopPage, 2, "the desktop's left as it was")
        phoneMany.destroy()
        wait(0)
        compare(Number(stored.value("phonePage")), 1, "stored as the page goes")
        compare(Number(stored.value("desktopPage")), 2)

        stored.setValue("desktopPage", 7)
        const past = pageOf({}, null, false)
        compare(past.pageIndex, 2, "past the end: the last page")
    }

    // A stored document this version can't use: the built-in layout,
    // and a line saying why until it's dismissed
    function test_aStoredDocumentThatCantBeUsedSaysSo_data() {
        return [
            { tag: "damaged", text: "{", words: "couldn't be read" },
            { tag: "newer", text: JSON.stringify({ format: 3, pages: [] }),
              words: "a newer version of Havne" }
        ]
    }

    function test_aStoredDocumentThatCantBeUsedSaysSo(data) {
        const plain = pageOf({}, null, true)
        verify(!findChild(plain, "dashProblem").visible, "nothing stored, nothing to say")
        compare(plain.problem, "none")
        plain.destroy()

        const stored = createTemporaryObject(storeComponent, testCase)
        stored.setValue("desktop", data.text)
        const page = pageOf({}, null, true)
        const line = findChild(page, "dashProblem")
        compare(page.problem, data.tag)
        verify(line.visible, "the line shows")
        verify(findChild(page, "dashProblemText").text.indexOf(data.words) >= 0,
               findChild(page, "dashProblemText").text)
        compare(JSON.stringify(page.doc), JSON.stringify(DashboardBook.builtIn("desktop")),
                "the built-in layout")
        mouseClick(findChild(page, "dashProblemDismiss"))
        verify(!line.visible, "dismissed")
        compare(stored.value("desktop"), data.text, "the stored text is kept")
    }

    // Dismissing hides the line for that text; a different stored text
    // is a different problem and shows it again
    function test_aChangedStoredTextShowsItsProblemAgain() {
        const stored = createTemporaryObject(storeComponent, testCase)
        stored.setValue("desktop", "{")
        const page = pageOf({}, null, true)
        const line = findChild(page, "dashProblem")
        verify(line.visible, "the line shows")
        mouseClick(findChild(page, "dashProblemDismiss"))
        verify(!line.visible, "dismissed")
        findChild(page, "dashStore").desktop = "["
        compare(page.problem, "damaged")
        verify(line.visible, "another damaged text shows the line again")
    }

    // A kind this version doesn't draw is an empty card saying so,
    // under the cell's label; the eight it draws load their gauge
    function test_aKindThisVersionDoesntDrawIsAnEmptyCard_data() {
        return [
            { tag: "unknown", kind: "gizmo", drawn: false },
            { tag: "segments", kind: "segments", drawn: true },
            { tag: "seven-segment", kind: "seven-segment", drawn: true },
            { tag: "sparkline", kind: "sparkline", drawn: true },
            { tag: "analog", kind: "analog", drawn: true },
            { tag: "ring", kind: "ring", drawn: true },
            { tag: "bar", kind: "bar", drawn: true },
            { tag: "digital", kind: "digital", drawn: true },
            { tag: "shift-light", kind: "shift-light", drawn: true }
        ]
    }

    function test_aKindThisVersionDoesntDrawIsAnEmptyCard(data) {
        const page = pageOf({}, docOf([{ kind: data.kind, w: 2, h: 4, label: "Odd", refs: ["id:1001"],
                          unit: "RPM" }]), true)
        const cell = findChild(page, "dashCell_0_0")
        verify(cell, "the cell is kept")
        const empty = findChild(cell, "unknownKind")
        compare(empty.visible, !data.drawn)
        compare(gaugeOf(cell) !== null, data.drawn)
        if (!data.drawn)
            compare(empty.text, "a gauge this version of Havne doesn't have")
        else
            compare(gaugeOf(cell).label, "Odd")
    }

    // Step 5's kinds and options reach their gauges: the cells' count and
    // curve, the digits, the sparkline's window, the bar standing up and
    // without its ticks, and the held peak on every kind
    function test_theNewKindsTakeTheirOptions_data() {
        return [
            { tag: "segments", kind: "segments", options: { cells: 12, rise: 2, peak: true },
              check: g => g.count === 12 && g.rise === 2 && g.showPeak },
            { tag: "seven-segment", kind: "seven-segment", options: { digits: 3 },
              check: g => g.count === 3 && !g.showPeak },
            { tag: "sparkline", kind: "sparkline", options: { window: 60 },
              check: g => g.span === 60 && g.windowMs === 60000 },
            { tag: "a vertical bar", kind: "bar", options: { vertical: true, marks: false, peak: true },
              check: g => g.vertical && !g.marks && g.showPeak },
            { tag: "a bar's defaults", kind: "bar", options: {},
              check: g => !g.vertical && g.marks && !g.showPeak },
            { tag: "a ring's peak", kind: "ring", options: { peak: true }, check: g => g.showPeak },
            { tag: "a dial's peak", kind: "analog", options: { peak: true }, check: g => g.showPeak }
        ]
    }

    function test_theNewKindsTakeTheirOptions(data) {
        const page = pageOf({}, docOf([{ kind: data.kind, w: 2, h: 4, label: "Odd", refs: ["id:1001"],
                          unit: "RPM", options: data.options }]), true)
        const cell = findChild(page, "dashCell_0_0")
        verify(cell, "the cell is there")
        const g = gaugeOf(cell)
        verify(g !== null, "the gauge loads")
        verify(data.check(g), "the options reach the gauge")
    }

    // A sparkline reads the model's history only while its page is the one
    // shown (a SwipeView keeps the other visible), and a page left keeps its
    // trace: back on it, the trace holds what came while it was away
    function test_aSparklineKeepsItsTraceAcrossAPageChange() {
        const cell = label => ({ kind: "sparkline", w: 2, h: 3, label: label,
                                 refs: refsOf("Engine speed"), unit: "RPM", options: { window: 10 } })
        const d = docOf([cell("First")])
        d.pages.push({ name: "Second", cells: [cell("Second")] })
        const page = pageOf({}, d, true)
        connect("mock://demo")
        const a = gaugeOf(findChild(page, "dashCell_0_0"))
        const b = gaugeOf(findChild(page, "dashCell_1_0"))
        verify(a && b, "both load (the neighbor loads at once)")
        verify(a.traceSource === Bridge.signalModel && a.signalId >= 0, "it reads the model's row")
        tryVerify(() => a.samples.length > 6, 5000, "the shown page reads")
        wait(300)
        compare(b.samples.length, 0, "the other page reads nothing")
        page.goTo(1)
        tryVerify(() => b.samples.length > 6, 5000, "the page reached reads, its history whole")
        const kept = a.samples.length
        const leftAt = a.samples[kept - 2]
        verify(kept > 6, "the page left keeps its trace")
        wait(600)
        compare(a.samples.length, kept, "and reads nothing meanwhile")
        page.goTo(0)
        tryVerify(() => {
            const s = a.samples
            for (let i = 0; i < s.length; i += 2)
                if (s[i] > leftAt && s[i] < s[s.length - 2])
                    return true
            return false
        }, 5000, "back on it, the trace holds what came while it was away")
    }

    // Dismiss is a touch's size on the phone: the line's words are the only
    // way to learn the log ended, and Dismiss the only way to clear them
    function test_theLogNoticesDismissIsATouchTarget() {
        Bridge.clearLogNotice()
        Theme.compact = true
        try {
            const page = pageOf({ form: "phone", width: 384, height: 800 }, undefined, true)
            compare(Bridge.startLog(), false)
            const dismiss = findChild(page, "dashLogNoticeDismiss")
            tryVerify(() => dismiss.visible && dismiss.height > 0, 2000, "the line shows")
            verify(dismiss.height >= Theme.touchTarget,
                   "Dismiss is " + dismiss.height + " tall, a touch is " + Theme.touchTarget)
        } finally {
            Theme.compact = false
            Bridge.clearLogNotice()
        }
    }

    // The header's line about the log (Bridge.logNotice): a refusal of
    // Start log and an end the user didn't ask for show in words, whole
    // at a phone's width, until Dismiss or the next log
    function test_theLogsNoticeShowsInTheHeaderUntilDismissed_data() {
        return [{ tag: "phone", form: "phone", width: 384 },
                { tag: "desktop", form: "desktop", width: 1280 }]
    }

    function test_theLogsNoticeShowsInTheHeaderUntilDismissed(data) {
        Bridge.clearLogNotice()
        const page = pageOf({ form: data.form, width: data.width, height: 800 }, undefined, true)
        const line = findChild(page, "dashLogNotice")
        const words = findChild(page, "dashLogNoticeText")
        verify(line && words, "the line is there")
        verify(!line.visible, "nothing to say")

        // A refusal: no session
        compare(Bridge.startLog(), false)
        compare(Bridge.logNotice, "Log not started: not connected")
        tryVerify(() => line.visible && words.width > 0, 2000, "the line shows")
        compare(words.text, Bridge.logNotice)
        compare(words.truncated, false, "whole at " + data.width + " px")
        verify(words.mapToItem(page, 0, 0).y > 0, "in the header, under the title row")
        findChild(page, "dashLogNoticeDismiss").clicked()
        compare(Bridge.logNotice, "")
        tryVerify(() => !line.visible, 2000, "Dismiss hides it")

        // A log the user's Disconnect closes, told; the next log clears it
        connect("mock://demo")
        compare(Bridge.startLog(), true)
        compare(Bridge.logNotice, "")
        verify(!line.visible, "a running log says nothing")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting && !Bridge.connected, 5000, "disconnect ends")
        compare(Bridge.logNotice, "Log ended")
        tryVerify(() => line.visible, 2000, "the end shows")
        compare(words.text, "Log ended")
        connect("mock://demo")
        compare(Bridge.startLog(), true)
        compare(Bridge.logNotice, "", "the next log clears it")
        tryVerify(() => !line.visible, 2000)
        Bridge.stopLog()
        compare(Bridge.logNotice, "", "Stop log says nothing")
    }

    // Section 6: a cell over a row of another unit takes the row's
    // range and no zones, and its note names the unit it expected
    function test_aCellOverAnotherUnitTakesTheRowsRange() {
        const page = pageOf({}, docOf([{ kind: "analog", w: 2, h: 8, label: "Wrong", refs: ["id:1001"],
                          unit: "°C",
                          options: { from: 0, to: 130, majorStep: 10, dangerFrom: 115 } }]), true)
        const hub = createTemporaryObject(hubComponent, testCase)
        const g = gaugeOf(findChild(page, "dashCell_0_0"))
        compare(g.to, 130, "no row yet: the cell's own scale")
        compare(g.dangerFrom, 115)
        connect("mock://demo")
        const rpm = hub.lookup(["id:1001"])
        compare(g.note, "waiting for data (expected °C)")
        compare(g.from, rpm.rangeLow)
        compare(g.to, rpm.rangeHigh)
        verify(isNaN(g.dangerFrom), "no zones under another unit")
        compare(g.unit, "RPM")
        tryVerify(() => g.hasValue, 3000, "the value shows")
        compare(g.note, "")
    }

    // An enumeration's row shows its label on a digital gauge
    function test_anEnumerationShowsItsLabel() {
        const values = {}
        for (let raw = 0; raw < 256; ++raw)
            values[String(raw)] = "v" + raw
        const r = keptFile(JSON.stringify({
            havne_signals: 1, name: "Modes", source: { name: "t" },
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [{ key: "mode", name: "Mode", read: { service: "22", number: "1E1C" },
                        field: { byte: 0, bits: 8 }, kind: "enum", values: values }]
        }))
        verify(r.ok, r.words)
        const page = pageOf({}, docOf([{ kind: "digital", w: 2, h: 4, label: "Mode", refs: ["key:mode"] }]), true)
        const hub = createTemporaryObject(hubComponent, testCase)
        const g = gaugeOf(findChild(page, "dashCell_0_0"))
        connect("mock://demo?profile=" + r.profile)
        tryVerify(() => g.hasValue, 5000, "the mode arrives")
        const entry = hub.byRef["key:mode"]
        verify(entry.isEnum)
        verify(entry.label !== "", "the row has a label")
        compare(g.valueText, entry.label)
    }

    // A value that stops coming dims (Theme.staleMs), and a session's
    // end clears it
    function test_aValueThatStopsComingDims() {
        const page = createTemporaryObject(pageComponent, testCase)
        const tach = gauges(page)["Engine speed"]
        connect("replay://" + Harness.replayFixtures
                + "/2026-10-25-engine-start?profile=obd2&speed=100")
        tryVerify(() => tach.hasValue, 5000, "the tach reads")
        tryVerify(() => tach.stale, 20000, "the replay's end leaves it stale")
        Bridge.disconnectSession()
        tryVerify(() => !tach.stale && !tach.hasValue, 5000, "no session, no value")
    }

    // A row the recording never holds reads "not in this recording" once
    // the replay's last frame has played
    function test_aRowTheRecordingNeverHoldsSaysSoAtTheReplaysEnd() {
        const r = keptFile(JSON.stringify({
            havne_signals: 1, name: "Never recorded", source: { name: "t" }, extends: "obd2",
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [{ key: "never", name: "Never", read: { service: "22", number: "F1FE" },
                        field: { byte: 0, bits: 8 } }]
        }))
        verify(r.ok, r.words)
        const page = pageOf({}, docOf([{ kind: "digital", w: 2, h: 4, label: "Never", refs: ["key:never"] }]), true)
        const never = gaugeOf(findChild(page, "dashCell_0_0"))
        compare(never.note, "no session")
        connect("replay://" + Harness.replayFixtures
                + "/2026-10-25-engine-start?profile=" + r.profile + "&speed=max")
        tryCompare(Bridge, "replayFinished", true, 10000)
        tryCompare(never, "note", "not in this recording", 5000)
        Bridge.disconnectSession()
        tryCompare(Bridge, "replayFinished", false, 5000)
    }

    // The ECU's own answers: a PID its bitmaps leave out, and a DID it
    // refuses (the fake ECU, through the fake J2534 DLL)
    function test_theEcusAnswersAreTheNotes() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const r = keptFile(JSON.stringify({
            havne_signals: 1, name: "Refusals", source: { name: "t" }, extends: "obd2",
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [{ key: "gone", name: "Gone", read: { service: "22", number: "F1FE" },
                        field: { byte: 0, bits: 8 } }]
        }))
        verify(r.ok, r.words)
        Harness.armFakeKnob("ECUIO_FAKE_WITHHELD_PIDS", "0A,5C")
        const doc = DashboardBook.builtIn("desktop")
        doc.pages[0].cells.push({ kind: "digital", w: 2, h: 4, label: "Gone",
                                  refs: ["key:gone"] })
        const page = pageOf({}, doc, false)
        const all = gauges(page)
        const gone = gaugeOf(findChild(page, "dashCell_0_" + (doc.pages[0].cells.length - 1)))
        // Main's binding: the gauges join the pick, so the unranked DID is asked
        Bridge.pickGauges = page.pickRefs
        compare(Bridge.connectSession("j2534://" + Harness.fakeDllPath + "?profile="
                                      + r.profile + "&identify=0&diagnose=0"), "")
        tryCompare(all["Oil temp"], "note", "not sent by this ECU", 5000)
        tryCompare(gone, "note", "refused (7F 22 31)", 5000)
        tryVerify(() => all["Coolant"].hasValue, 5000, "the rest read")
    }

    // The sweep plays on the dials and the shift lights, and Reset
    // peaks clears every page's
    function test_theSweepAndThePeaksReachEveryPage() {
        const page = pageOf({}, pagesDoc(2), true)
        tryVerify(() => allLoaded(page), 10000, "the second page loads")
        const every = allGauges(page)
        connect("mock://demo")
        const swept = every.filter(g => g.sweeping)
        verify(swept.length > 0, "the sweep plays")
        for (let p = 0; p < 2; ++p) {
            const cells = page.pages[p].cells
            for (let i = 0; i < cells.length; ++i) {
                const g = gaugeOf(findChild(page, "dashCell_" + p + "_" + i))
                const dial = cells[i].kind !== "bar" && cells[i].kind !== "digital"
                compare(g.sweeping, dial, "page " + p + " " + cells[i].label)
            }
        }
        tryVerify(() => every.every(g => !g.sweeping), 5000, "the sweep ends")
        const tachs = every.filter(g => g.label === "Engine speed")
        compare(tachs.length, 2, "a tach on each page")
        tryVerify(() => tachs.every(g => !isNaN(g.peakValue)), 5000, "each holds a peak")
        mouseClick(findChild(page, "resetPeaks"))
        verify(tachs.every(g => isNaN(g.peakValue)), "both cleared")
    }

    // A page whose gauges load after the sweep was requested sweeps them
    // as they load while the sweep is on, and shows them as they are once
    // it is over. The last page holds one dial, so it has one gauge to watch
    function test_aGaugeLoadedAfterTheSweepSweeps_data() {
        return [
            { tag: "the sweep is on", sweptAt: "now", sweeps: true },
            { tag: "the sweep is over", sweptAt: "never", sweeps: false }
        ]
    }

    function test_aGaugeLoadedAfterTheSweepSweeps(data) {
        const d = pagesDoc(4)
        d.pages[3].cells = d.pages[3].cells.filter(c => c.kind === "analog").slice(0, 1)
        const page = pageOf({}, d, true)
        const loader = findChild(findChild(page, "dashCell_3_0"), "gauge")
        verify(loader.asynchronous, "the last page loads later")
        // Set before the event loop runs, so the load is still to come
        page.sweepMs = 600000
        page.sweptAt = data.sweptAt === "now" ? Date.now() : 0
        tryCompare(loader, "status", Loader.Ready, 10000)
        if (data.sweeps) {
            verify(loader.item.sweeping, "the gauge sweeps as it loads")
        } else {
            wait(100)
            verify(!loader.item.sweeping, "the gauge shows its value")
        }
    }

    // A page and its neighbors load at once, so a swipe never drags in
    // an empty card; a page two or more away loads between frames
    function test_theNeighborsOfTheShownPageLoadAtOnce() {
        const page = pageOf({ form: "phone", width: 384, height: 832 }, pagesDoc(5), true)
        const at = p => findChild(findChild(page, "dashCell_" + p + "_0"), "gauge")
        for (let p = 0; p < 5; ++p)
            compare(at(p).asynchronous, p > 1, "page " + p + " from page 0")
        compare(at(0).status, Loader.Ready, "the shown page")
        compare(at(1).status, Loader.Ready, "the next page, at once")
        page.goTo(2)
        for (let p = 0; p < 5; ++p)
            compare(at(p).asynchronous, p === 0 || p === 4, "page " + p + " from page 2")
        for (let p = 1; p <= 3; ++p)
            tryCompare(at(p), "status", Loader.Ready, 5000, "page " + p)
    }

    // A full document (every page, every cell) draws its first frame
    // with only the near pages' gauges made; the rest load between
    // frames, and a page moved to stops loading later
    function test_aFullDocumentsOtherPagesLoadLater() {
        const d = DashboardBook.builtIn("desktop")
        const base = d.pages[0].cells
        d.pages = []
        for (let p = 0; p < DashboardBook.maxPages; ++p) {
            const cells = []
            for (let i = 0; i < DashboardBook.maxCells; ++i)
                cells.push(JSON.parse(JSON.stringify(base[i % base.length])))
            d.pages.push({ name: "Page " + (p + 1), cells: cells })
        }
        compare(DashboardBook.documentProblem(d), "", "a document the book takes")
        const page = pageOf({}, d, true)
        const at = (p, i) => findChild(findChild(page, "dashCell_" + p + "_" + i), "gauge")
        for (let i = 0; i < DashboardBook.maxCells; ++i)
            compare(at(0, i).status, Loader.Ready, "the shown page's cell " + i)
        const last = DashboardBook.maxPages - 1
        verify(at(last, 0).asynchronous, "a far page's loads later")
        verify(at(5, 0).asynchronous, "so does a middle one's")
        page.goTo(5)
        verify(!at(5, 0).asynchronous, "the page moved to loads at once")
        for (let i = 0; i < DashboardBook.maxCells; ++i)
            tryCompare(at(5, i), "status", Loader.Ready, 10000, "the page moved to, cell " + i)
        tryVerify(() => allLoaded(page), 30000, "every page loads")
        compare(loaders(page).length, DashboardBook.maxPages * DashboardBook.maxCells)
    }

    // The lit parts take Theme's gaugeValue in every theme, not a color
    // derived from the primary
    function test_gaugesTakeTheirValueColorFromTheTheme() {
        for (const name of Theme.paletteNames) {
            usePalette(name)
            const page = createTemporaryObject(pageComponent, testCase)
            const tach = gauges(page)["Engine speed"]
            verify(Qt.colorEqual(tach.accent, Theme.gaugeValue),
                   name + ": " + tach.accent + ", expected " + Theme.gaugeValue)
        }
        usePalette("dark")
    }

    // A ref that names an inherited object key is no signal
    function test_hubIgnoresInheritedKeys() {
        const hub = createTemporaryObject(hubComponent, testCase)
        compare(hub.lookup(["constructor"]), null)
        compare(hub.lookup(["toString", "__proto__"]), null)
    }
}
