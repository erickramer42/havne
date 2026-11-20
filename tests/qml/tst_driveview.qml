// File/layer: tests/qml/tst_driveview.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.driveview (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

DriveViewKit {
    id: testCase
    name: "DriveView"

    Component { id: spyComponent; SignalSpy {} }

    // A session whose adapter fails reads, so its link state is `fault`,
    // or another knob's state; the fake DLL is Windows' alone
    function drivenOnTheFake(knob, value, query) {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        Harness.armFakeKnob(knob, value)
        const s = phoneMain()
        connect("j2534://" + Harness.fakeDllPath + query)
        return s
    }

    function faulted() {
        const s = drivenOnTheFake("ECUIO_FAKE_READ_STATUS", "7", "?profile=raw&filter=0x7E8")
        enterOn(s)
        // The fault can land before or after the entry (the polish wait
        // under load decides), so only the end state is judged; a fault
        // up at the entry has its own case
        tryCompare(Bridge, "linkState", "fault", 5000)
        tryVerify(() => s.dash.drivePart.barsUp, 2000, "the fault raises them")
        return s
    }

    // A log that's running and a second Start log's refusal
    function raiseANotice() {
        tryVerify(() => Bridge.signalModel.subscribedCount > 0, 3000, "signals ticked")
        compare(Bridge.startLog(), true)
        compare(Bridge.startLog(), false)
        verify(Bridge.logNotice !== "", "the refusal is the notice")
    }

    function hideTimer(s) { return find(s.dash, "driveHideTimer") }

    // ---- Entering and what it hides (sections 1 and 2)

    function test_driveIsOffWithoutASession() {
        const s = phoneMain()
        const drive = find(s.dash, "dashDrive")
        verify(drive.visible, "on the phone's row")
        compare(drive.enabled, false)
        compare(drive.text, "Connect first", "and it says what it waits for")
        compare(drive.Accessible.name, "Connect first: a session shows the gauges full screen")
        s.dash.drivePart.enterDrive()
        compare(s.dash.drivePart.driving, false, "and nothing else enters it")
    }

    function test_theDesktopFormHasNoDriveButton() {
        const s = phoneMain()
        s.dash.form = "desktop"
        verify(!find(s.dash, "dashDrive").visible)
    }

    function test_theDesktopFormNeverEnters() {
        dashStore.setValue("phoneDrive", true)
        dashStore.sync()
        const s = phoneMain()
        s.dash.form = "desktop"
        connect("mock://demo")
        wait(50)
        compare(s.dash.drivePart.driving, false, "a remembered view stays on the phone")
        s.dash.drivePart.enterDrive()
        compare(s.dash.drivePart.driving, false, "and nothing else enters it")
    }

    function test_driveIsATouchsSize() {
        const s = driven()
        const drive = find(s.dash, "dashDrive")
        // The row lays its buttons out at the next polish after Drive's
        // words change with the connect
        tryVerify(() => drive.width >= Theme.touchTarget - 0.5, 2000,
                  "Drive is " + drive.width + " wide (implicit " + drive.implicitWidth + ")")
        verify(drive.height >= Theme.touchTarget - 0.5, "Drive is " + drive.height + " tall")
    }

    function test_thePageIndicatorGoesInTheView() {
        const s = driven()
        const doc = JSON.parse(JSON.stringify(s.dash.doc))
        doc.pages.push({ name: "Second", cells: JSON.parse(JSON.stringify(doc.pages[0].cells)) })
        s.dash.doc = doc
        const indicator = find(s.dash, "dashIndicator")
        verify(indicator.visible, "two pages show it")
        enterOn(s)
        verify(!indicator.visible, "the view has none")
        s.dash.drivePart.leaveDrive(true)
        verify(indicator.visible, "and it comes back")
    }

    function test_enterHidesTheBarsAndKeepsTheStatusLine() {
        const s = driven()
        const b = bars(s)
        verify(b.head.visible && b.tab.visible, "both show before")
        verify(!find(s.dash, "driveStatusLine").visible, "no line outside the view")
        mouseClick(find(s.dash, "dashDrive"))
        verify(s.dash.drivePart.driving, "Drive enters")
        tryCompare(b.head, "visible", false, 2000, "the top bar goes")
        tryCompare(b.tab, "visible", false, 2000, "the tab bar goes")
        verify(!find(s.dash, "resetPeaks").visible, "the tool row goes")
        verify(!b.tools.visible, "no sheet")
        verify(!find(s.dash, "dashIndicator").visible, "the page indicator goes")
        const line = find(s.dash, "driveStatusLine")
        verify(line.visible, "the status line stays")
        compare(line.height, 28)
        compare(s.stand.visibility, Window.FullScreen, "the system bars go")
    }

    function test_thePanelReachesTheScreensEdgesAndKeepsItsSeams() {
        const s = driven()
        enterOn(s)
        const surface = find(s.dash, "dashPanelSurface")
        tryVerify(() => surface.width > 0, 2000, "the panel is laid out")
        const left = surface.mapToItem(s.main.contentItem, 0, 0).x
        compare(left, 0, "to the left edge")
        compare(surface.width, s.main.contentItem.width, "and the whole width")
        compare(surface.radius, 0, "square corners at the screen's edge")
        compare(surface.border.width, 0, "no edge of its own")
        verify(findChild(s.dash, "dashSeam"), "the hairlines between cells stay")
    }

    function test_theGaugesGetTheBarsHeight() {
        const s = driven()
        const swipe = find(s.dash, "dashPages")
        const before = swipe.height
        enterOn(s)
        verify(swipe.height > before + 100, "more height: " + before + " to " + swipe.height)
        compare(swipe.y, 28, "right under the status line")
    }

    // A page of twelve 3 x 6 cells: 36 rows upright (six columns), 18 on its
    // side (twelve), short of the screen at the square pitch either way.
    // Its last cell is Digits with no signal, so its note shows
    function fillingPage() {
        const cells = []
        for (let i = 0; i < 12; ++i)
            cells.push({ kind: i === 11 ? "seven-segment" : "digital", w: 3, h: 6, label: "Cell " + i,
                         refs: [i === 11 ? "id:9999" : "id:1001"], unit: "RPM",
                         options: { from: 0, to: 8000 } })
        return { format: 2, pages: [{ name: "P", cells: cells }] }
    }

    // The panel's foot, in the window
    function panelFoot(s) {
        const surface = find(s.dash, "dashPanelSurface")
        return surface.mapToItem(s.main.contentItem, 0, surface.height).y
    }

    function test_thePanelFillsTheHeightInTheView_data() {
        return [{ tag: "upright", w: 384, h: 832 }, { tag: "on its side", w: 832, h: 384 }]
    }

    function test_thePanelFillsTheHeightInTheView(data) {
        const s = driven()
        s.main.width = data.w
        s.main.height = data.h
        s.dash.doc = fillingPage()
        const heights = s.dash.doc.pages[0].cells.map(c => c.h)
        const view = find(s.dash, "dashPage_0")
        verify(waitForPolish(s.main), "laid out")
        tryVerify(() => view.columnWidth > 0, 2000, "laid out")
        compare(view.pitch, view.squarePitch, "outside the view the square pitch")
        enterOn(s)
        tryVerify(() => view.pitch > view.squarePitch, 2000, "taller rows in the view")
        verify(view.pitch <= 2 * view.squarePitch + 0.01, "at most twice the square")
        const bottom = s.main.contentItem.height - s.dash.driveInsetBottom
        verify(Math.abs(panelFoot(s) - bottom) <= view.pitch,
               "the foot " + panelFoot(s) + " within a row of " + bottom)
        compare(s.dash.doc.pages[0].cells.map(c => c.h), heights, "the document's heights unchanged")
        const note = find(find(s.dash, "dashCell_0_11"), "gaugeNote")
        // The note elides by design, to the room the title and value leave it
        const cell = find(s.dash, "dashCell_0_11")
        verify(!note.visible || note.mapToItem(cell, note.width, 0).x <= cell.width + 0.5,
               "the Digits note stays in its cell: " + note.text)
        s.dash.drivePart.leaveDrive(true)
        tryCompare(view, "pitch", view.squarePitch, 2000, "the square pitch again")
    }

    // A page of one short row stops at twice the square, and the ground shows under it
    function test_aShortPageStopsAtTwiceTheSquare() {
        const s = driven()
        const page = fillingPage()
        page.pages[0].cells = page.pages[0].cells.slice(0, 2)
        s.dash.doc = page
        enterOn(s)
        const view = find(s.dash, "dashPage_0")
        tryVerify(() => view.columnWidth > 0, 2000, "laid out")
        fuzzyCompare(view.pitch, 2 * view.squarePitch, 0.01)
        verify(panelFoot(s) < s.main.contentItem.height / 2, "the panel stops: " + panelFoot(s))
    }

    // A page taller than the screen keeps its pitch in the view, and scrolls
    function test_aLongPageKeepsItsPitch() {
        const s = driven()
        const page = fillingPage()
        page.pages[0].cells = page.pages[0].cells.concat(fillingPage().pages[0].cells)
        s.dash.doc = page
        const view = find(s.dash, "dashPage_0")
        enterOn(s)
        tryVerify(() => view.columnWidth > 0, 2000, "laid out")
        verify(waitForPolish(s.main), "laid out")
        compare(view.pitch, view.squarePitch, "no squeeze to fit")
        verify(view.contentHeight > view.height, "it scrolls")
    }

    readonly property int holdMs: Application.styleHints.mousePressAndHoldInterval

    function test_editModeIsNotReachable() {
        const s = driven()
        enterOn(s)
        revealedOn(s)
        verify(!find(s.dash, "dashEdit").visible, "Edit isn't offered")
        s.dash.drivePart.driveTap()    // back to the gauges alone
        tryCompare(s.dash.drivePart, "barsShown", 0, 2000)
        const cell = find(s.dash, "dashCell_0_0")
        verify(waitForPolish(s.main))
        mousePress(cell)
        wait(holdMs + 300)
        compare(s.dash.held, -1, "the hold lifts nothing")
        mouseRelease(cell)
        compare(s.dash.panel, "", "and opens no menu")
        verify(!s.dash.drivePart.barsUp, "nor counts as a tap")
    }

    // ---- The status line (section 2)

    function statusWords(s) {
        return find(s.dash, "driveStatusWord").text + "|" + find(s.dash, "driveStatusAddress").text
    }

    function test_theStatusLineNamesTheStateAndTheAddress() {
        const s = driven("mock://demo?profile=mock")
        enterOn(s)
        tryCompare(Bridge, "linkState", "ok", 3000)
        compare(statusWords(s), Theme.linkShortLabel("ok") + "|· " + "mock://demo")
        compare(find(s.dash, "driveStatusDot").color, Theme.linkColor("ok"))
        verify(!find(s.dash, "driveStatusRun").visible, "nothing runs, so it never says No log")
    }

    function test_theStatusLineShowsALog() {
        const s = driven()
        enterOn(s)
        tryVerify(() => Bridge.signalModel.subscribedCount > 0, 3000, "signals ticked")
        Bridge.startLog()
        tryCompare(Bridge, "logActive", true, 3000)
        verify(waitForPolish(s.main))
        compare(find(s.dash, "driveStatusRun").text, qsTr("Log"))
        verify(find(s.dash, "driveStatusTime").visible, "the elapsed time")
        // logElapsedMs reads the clock live; the label follows its once-a-second notify
        tryVerify(() => find(s.dash, "driveStatusTime").text === s.main.logTimeText(Bridge.logElapsedMs),
                  2500, "the label agrees with the elapsed time")
        // The size as the header gives it, none under 0.1 MB
        const detail = find(s.dash, "driveStatusDetail")
        compare(detail.detail, s.main.logSizeText(Bridge.logBytes))
        compare(detail.visible, detail.detail !== "")
        compare(s.main.logSizeText(104857), "", "none under 0.1 MB")
        compare(s.main.logSizeText(104858), "0.1 MB")
        Bridge.stopLog()
        tryCompare(Bridge, "logActive", false, 5000)
    }

    function test_aStoppedLogStaysInTheView() {
        const s = driven()
        enterOn(s)
        tryVerify(() => Bridge.signalModel.subscribedCount > 0, 3000, "signals ticked")
        Bridge.startLog()
        tryCompare(Bridge, "logActive", true, 3000)
        Bridge.stopLog()
        tryCompare(Bridge, "logActive", false, 5000)
        wait(50)
        compare(s.main.currentPage, s.main.dashboardPage, "no jump to the viewer mid-drive")
        verify(s.dash.drivePart.driving, "the view stays on")
    }

    // The stop isn't silent, and the driver who asked for it isn't interrupted
    function test_stopLogSaysSoOnTheStatusLineAlone() {
        const s = driven()
        enterOn(s)
        tryVerify(() => Bridge.signalModel.subscribedCount > 0, 3000, "signals ticked")
        compare(Bridge.startLog(), true)
        tryCompare(Bridge, "logActive", true, 3000)
        const run = find(s.dash, "driveStatusRun")
        Bridge.stopLog()
        tryCompare(run, "text", qsTr("Log ended"), 5000)
        verify(run.visible, "it shows")
        verify(!s.dash.drivePart.barsUp, "hidden bars stay hidden")
        compare(Bridge.logNotice, "", "no notice to dismiss")
        const timer = find(s.dash, "driveLogEndedTimer")
        compare(timer.interval, s.dash.driveHideMs, "for as long as the bars would stay")
        timer.triggered()
        verify(!run.visible, "then it goes")
    }

    // Pressed in the sheet: the status line above it says so at once
    function test_stopLogFromTheSheetSaysSoAtOnce() {
        const s = driven()
        enterOn(s)
        tryVerify(() => Bridge.signalModel.subscribedCount > 0, 3000, "signals ticked")
        compare(Bridge.startLog(), true)
        tryCompare(Bridge, "logActive", true, 3000)
        revealedOn(s)
        const timer = find(s.dash, "driveLogEndedTimer")
        const run = find(s.dash, "driveStatusRun")
        mouseClick(find(s.dash, "driveStopLog"))
        tryVerify(() => s.dash.drivePart.driveLogEnded, 5000, "said")
        verify(s.dash.drivePart.barsUp, "the bars stay")
        verify(timer.running, "its time runs while the bars are up")
        compare(run.text, qsTr("Log ended"))
        verify(run.visible)
    }

    // A full disk: the driver didn't ask, so it comes with the notice and the bars
    function test_aLogThatEndsByItselfRaisesTheNotice() {
        const s = driven()
        enterOn(s)
        tryVerify(() => Bridge.signalModel.subscribedCount > 0, 3000, "signals ticked")
        compare(Bridge.startLog(), true)
        tryCompare(Bridge, "logActive", true, 3000)
        verify(!s.dash.drivePart.barsUp, "hidden at first")
        Harness.failRecordingWrites()
        tryCompare(Bridge, "logActive", false, 5000)
        compare(Bridge.logNotice, "Log ended: the disk is full")
        tryVerify(() => s.dash.drivePart.barsUp, 2000, "the notice raises them")
        verify(s.dash.drivePart.barsHeld, "and holds them")
        verify(!s.dash.drivePart.driveLogEnded, "the status line's words are for Stop log")
        compare(s.main.currentPage, s.main.dashboardPage)
        verify(s.dash.drivePart.driving, "the view stays on")
    }

    function test_leavingClearsTheWords() {
        const s = driven()
        enterOn(s)
        s.dash.drivePart.driveLogEnded = true
        s.dash.drivePart.leaveDrive(true)
        compare(s.dash.drivePart.driveLogEnded, false)
    }

    function test_theStatusLineShowsARecordingAndItsMarks() {
        const s = phoneMain()
        Bridge.recording = true
        connect("mock://demo")
        tryCompare(Bridge, "recordingActive", true, 3000)
        enterOn(s)
        compare(find(s.dash, "driveStatusRun").text, qsTr("Recording"))
        verify(!find(s.dash, "driveStatusTime").visible, "a recording has no clock here")
        verify(!find(s.dash, "driveStatusDetail").visible, "no marks yet")
        Bridge.mark()
        tryCompare(Bridge, "markCount", 1, 3000)
        compare(find(s.dash, "driveStatusDetail").text, "· 1 mark")
        Bridge.mark()
        tryCompare(Bridge, "markCount", 2, 3000)
        compare(find(s.dash, "driveStatusDetail").text, "· 2 marks")
    }

    function test_aLongAddressElidesAndTheStateWordStays() {
        const s = driven("mock://demo?profile=uds-generic&did=F190:text,1E1C:u16,F18C:text,F187:text")
        s.main.width = 280
        enterOn(s)
        const word = find(s.dash, "driveStatusWord")
        const address = find(s.dash, "driveStatusAddress")
        tryVerify(() => address.truncated, 2000, "the address elides")
        compare(word.truncated, false)
        verify(word.width >= word.implicitWidth, "the word keeps its width")
    }

    // ---- The reveal (section 3)

    function test_aTapRevealsTheBarsOverTheGauges() {
        const s = driven()
        enterOn(s)
        const swipe = find(s.dash, "dashPages")
        const height = swipe.height
        const b = bars(s)
        mouseClick(find(s.dash, "dashCell_0_0"))
        verify(s.dash.drivePart.barsUp, "a tap on a gauge reveals")
        tryCompare(s.dash.drivePart, "barsShown", 1, 2000)
        verify(b.tab.visible && b.tools.visible, "the tab bar and the sheet show")
        verify(!b.head.visible, "the header stays hidden")
        compare(swipe.height, height, "the gauges don't reflow")
        compare(find(s.dash, "driveExit").text, qsTr("Exit drive"))
    }

    function test_aTapOffTheCellsAlsoReveals() {
        const s = driven()
        s.dash.doc = { format: 2, pages: [{ name: "P", cells: [
            { kind: "digital", w: 3, h: 6, label: "A", refs: ["id:1001"], unit: "RPM", options: {} }] }] }
        enterOn(s)
        const area = find(s.dash, "driveTapArea")
        tryVerify(() => area.height > 200, 2000, "laid out")
        mouseClick(area, area.width - 12, area.height - 12)
        verify(s.dash.drivePart.barsUp, "the bottom of an emptier page")
    }

    function test_theBarsHideAfterTheDelay() {
        const s = driven()
        enterOn(s)
        revealedOn(s)
        const timer = hideTimer(s)
        verify(timer.running, "the delay runs")
        compare(timer.interval, 4000, "the default")
        timer.triggered()
        verify(!s.dash.drivePart.barsUp, "they slide out")
        tryCompare(s.dash.drivePart, "barsShown", 0, 2000)
    }

    // Pressed over the delay's first part, the delay starts over: the bars
    // outlast the first delay by the press's own time
    function test_aTouchRestartsTheDelayAndTheButtonStillWorks() {
        const s = driven()
        enterOn(s)
        revealedOn(s)
        const timer = hideTimer(s)
        let resets = 0
        s.dash.peaksResetRequested.connect(() => ++resets)
        const reset = find(s.dash, "driveResetPeaks")
        const spy = createTemporaryObject(spyComponent, testCase,
                                          { target: timer, signalName: "runningChanged" })
        mouseClick(reset)
        compare(resets, 1, "the press went on to Reset peaks")
        verify(s.dash.drivePart.driving, "which is no exit")
        compare(s.dash.drivePart.barTouches, 1, "a touch on the bars")
        compare(spy.count, 2, "the delay stopped and started again at the touch")
        verify(timer.running)
        timer.interval = 20
        tryCompare(s.dash.drivePart, "barsUp", false, 2000, "and ended after it")
    }

    // The gaps in the bars aren't gauges: no toggle, and the delay starts again
    function test_aTouchInTheBarsGapsStaysOutOfTheGauges() {
        const s = driven()
        enterOn(s)
        revealedOn(s)
        const b = bars(s)
        mouseClick(b.tab, b.tab.width / 2, b.tab.height - 3)
        verify(s.dash.drivePart.barsUp, "below the tabs")
        const backdrop = find(s.dash, "driveToolsBackdrop")
        const before = s.dash.drivePart.barTouches
        const spy = createTemporaryObject(spyComponent, testCase,
                                          { target: hideTimer(s), signalName: "runningChanged" })
        mouseClick(backdrop, backdrop.width / 2, 1)
        verify(s.dash.drivePart.barsUp, "around the tools")
        compare(s.dash.drivePart.barTouches, before + 1, "a touch on the bars")
        compare(spy.count, 2, "the delay started again")
    }

    function test_aTouchOnEachBarIsCounted() {
        const s = driven()
        enterOn(s)
        revealedOn(s)
        const b = bars(s)
        verify(waitForPolish(s.main), "laid out")
        const before = s.dash.drivePart.barTouches
        mouseClick(b.sheet, b.sheet.width / 2, 2)
        compare(s.dash.drivePart.barTouches, before + 1, "the sheet")
        mouseClick(b.tab, b.tab.width / 2, b.tab.height - 3)
        compare(s.dash.drivePart.barTouches, before + 2, "the tab bar")
        verify(s.dash.drivePart.driving, "neither is an exit")
    }

    function test_neverKeepsTheBarsUp() {
        uiStore.setValue("driveHideMs", 0)
        uiStore.sync()
        const s = driven()
        compare(s.dash.driveHideMs, 0, "Settings' value reaches the page")
        enterOn(s)
        revealedOn(s)
        verify(!hideTimer(s).running, "no delay to run")
        verify(s.dash.drivePart.barsUp)
    }

    function test_theDelayIsSettings() {
        uiStore.setValue("driveHideMs", 8000)
        uiStore.sync()
        const s = driven()
        compare(hideTimer(s).interval, 8000)
    }

    function test_aFaultRevealsAndHoldsTheBars() {
        const s = faulted()
        verify(!hideTimer(s).running, "and the delay is suspended")
    }

    function test_aFaultTappedAwayStaysAway() {
        const s = faulted()
        s.dash.drivePart.driveTap()
        verify(!s.dash.drivePart.barsUp, "tapped away")
        verify(!s.dash.drivePart.barsHeld, "the hold ends with it")
    }

    function test_aRepeatedTapStartsTheDelayAfterAFault() {
        const s = faulted()
        s.dash.drivePart.driveTap()
        s.dash.drivePart.driveTap()
        verify(s.dash.drivePart.barsUp, "revealed by hand")
        verify(hideTimer(s).running, "a tap's reveal has the delay, fault or no fault")
    }

    function test_aLogNoticeRevealsAndHoldsTheBars() {
        const s = driven()
        enterOn(s)
        raiseANotice()
        tryCompare(s.dash.drivePart, "barsUp", true, 2000, "a notice raises them")
        verify(!hideTimer(s).running, "held")
        tryCompare(s.dash.drivePart, "barsShown", 1, 2000)
        verify(find(s.dash, "driveLogNotice").visible, "with its row and Dismiss, in the sheet")
        verify(find(s.dash, "driveLogNoticeDismiss").visible)
        Bridge.clearLogNotice()
        tryCompare(s.dash.drivePart, "barsHeld", false, 2000, "its end lets the delay run")
        verify(hideTimer(s).running)
    }

    function test_aFaultAlreadyUpRevealsOnEntering() {
        const s = drivenOnTheFake("ECUIO_FAKE_READ_STATUS", "7", "?profile=raw&filter=0x7E8")
        tryCompare(Bridge, "linkState", "fault", 5000)
        enterOn(s)
        verify(s.dash.drivePart.barsUp, "not hidden behind the view")
        verify(s.dash.drivePart.barsHeld, "and held")
        verify(!hideTimer(s).running)
    }

    function test_aNoticeAlreadyUpRevealsOnEntering() {
        const s = driven()
        raiseANotice()
        enterOn(s)
        verify(s.dash.drivePart.barsUp, "not hidden behind the view")
    }

    function test_onlyAFaultRaisesTheBars_data() {
        return [{ tag: "fault", state: "fault", want: true }, { tag: "ok", state: "ok", want: false },
                { tag: "quiet", state: "quiet", want: false },
                { tag: "requests", state: "requests", want: false },
                { tag: "stopped", state: "stopped", want: false },
                { tag: "degraded", state: "degraded", want: false }]
    }

    function test_onlyAFaultRaisesTheBars(data) {
        const s = phoneMain()
        compare(s.dash.drivePart.raisesBars(data.state), data.want)
    }

    function test_quietDoesntReveal() {
        const s = drivenOnTheFake("ECUIO_FAKE_READ_STATUS", "9", "?profile=raw&filter=0x7E8")
        enterOn(s)
        tryCompare(Bridge, "linkState", "quiet", 5000)
        wait(20)
        verify(!s.dash.drivePart.barsUp, "the status line says it")
        verify(s.dash.drivePart.driving)
    }
}
