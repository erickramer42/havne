// File/layer: tests/qml/tst_driveview.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.driveview (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

// The drive view (ADR-0059): a state of the Dash page, driven in Main so the
// top bar and the tab bar are the real ones, at the phone's size
TestCase {
    id: testCase
    name: "DriveView"
    when: windowShown

    Component { id: mainComponent; Main {} }
    // Stands in for the window, so a test never takes the desktop full screen
    Component { id: windowComponent; QtObject { property int visibility: Window.Maximized } }
    Component { id: otherWindow; Window { width: 100; height: 100; visible: true } }
    Component { id: fieldComponent; TextInput { width: 100; height: 30 } }
    Component { id: spyComponent; SignalSpy {} }

    Settings {
        id: dashStore
        category: "dashboards"
        property bool phoneDrive: false
        property int driveHints: 0
    }
    Settings {
        id: uiStore
        category: "ui"
        property int driveHideMs: 4000
        property string driveOrientation: "follow"
    }

    function resetStores() {
        dashStore.setValue("phoneDrive", false)
        dashStore.setValue("driveHints", 3)    // the hint has its own cases
        dashStore.sync()
        uiStore.setValue("driveHideMs", 4000)
        uiStore.setValue("driveOrientation", "follow")
        uiStore.sync()
    }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        resetStores()
    }

    function cleanup() {
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Bridge.recording = false
        Bridge.clearLogNotice()
        Harness.clearFakeKnobs()
        PhoneWindow.lockOrientation("follow")
        PhoneWindow.setKeepScreenOn(false)
        Immersive.leave()
        resetStores()
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    function phoneMain(props) {
        const main = createTemporaryObject(mainComponent, testCase, props)
        verify(main, "Main loads")
        main.testMobile = true
        main.width = 384
        main.height = 832
        main.font.pixelSize = 16
        const dash = find(main.contentItem, "dashboardPage")
        dash.form = "phone"
        dash.driveWindow = createTemporaryObject(windowComponent, testCase)
        verify(waitForPolish(main), "laid out")
        return { main: main, dash: dash, stand: dash.driveWindow }
    }

    function connect(uri) {
        compare(Bridge.connectSession(uri), "", uri + " connects")
        tryVerify(() => Bridge.connected, 5000, "connected to " + uri)
    }

    function replayUri() {
        return "replay://" + Harness.replayFixtures + "/2026-10-25-engine-start?profile=obd2&speed=max"
    }

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
        verify(!s.dash.barsUp, "hidden at first")
        tryCompare(Bridge, "linkState", "fault", 5000)
        tryVerify(() => s.dash.barsUp, 2000, "the fault raises them")
        return s
    }

    // A log that's running and a second Start log's refusal
    function raiseANotice() {
        tryVerify(() => Bridge.signalModel.subscribedCount > 0, 3000, "signals ticked")
        compare(Bridge.startLog(), true)
        compare(Bridge.startLog(), false)
        verify(Bridge.logNotice !== "", "the refusal is the notice")
    }

    // A phone Main with a mock session, the Dash page showing
    function driven(uri) {
        const s = phoneMain()
        connect(uri === undefined ? "mock://demo" : uri)
        tryCompare(s.main, "currentPage", s.main.dashboardPage, 2000)
        return s
    }

    function enterOn(s) {
        s.dash.enterDrive()
        verify(s.dash.driving, "the view is on")
        verify(waitForPolish(s.main), "laid out")
    }

    function bars(s) {
        return { head: find(s.main.contentItem, "headerBar"), tab: find(s.main.contentItem, "tabBar"),
                 tools: find(s.dash, "resetPeaks") }
    }

    function revealedOn(s) {
        s.dash.driveTap()
        verify(s.dash.barsUp, "revealed")
        tryCompare(s.dash, "barsShown", 1, 2000, "slid in")
    }

    function hideTimer(s) { return find(s.dash, "driveHideTimer") }

    // The focus is somewhere the back key reaches zoomRoot's handler from
    function focusReachesTheHandler(s) {
        for (let i = s.main.activeFocusItem; i; i = i.parent) {
            if (i.objectName === "zoomRoot")
                return true
        }
        return false
    }
    // Answers whether QML took the key, as Android's system reads it
    function back(s) {
        const taken = Harness.pressBack(s.main)
        verify(focusReachesTheHandler(s), "the next Back arrives too")
        return taken
    }
    function fronted(s) {
        s.main.requestActivate()
        tryVerify(() => s.main.active, 3000, "the window is in front")
    }

    // ---- Entering and what it hides (sections 1 and 2)

    function test_driveIsOffWithoutASession() {
        const s = phoneMain()
        const drive = find(s.dash, "dashDrive")
        verify(drive.visible, "on the phone's row")
        compare(drive.enabled, false)
        compare(drive.text, "Connect first", "and it says what it waits for")
        compare(drive.Accessible.name, "Connect first: a session shows the gauges full screen")
        s.dash.enterDrive()
        compare(s.dash.driving, false, "and nothing else enters it")
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
        compare(s.dash.driving, false, "a remembered view stays on the phone")
        s.dash.enterDrive()
        compare(s.dash.driving, false, "and nothing else enters it")
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
        s.dash.leaveDrive(true)
        verify(indicator.visible, "and it comes back")
    }

    function test_enterHidesTheBarsAndKeepsTheStatusLine() {
        const s = driven()
        const b = bars(s)
        verify(b.head.visible && b.tab.visible, "both show before")
        verify(!find(s.dash, "driveStatusLine").visible, "no line outside the view")
        mouseClick(find(s.dash, "dashDrive"))
        verify(s.dash.driving, "Drive enters")
        tryCompare(b.head, "visible", false, 2000, "the top bar goes")
        tryCompare(b.tab, "visible", false, 2000, "the tab bar goes")
        verify(!b.tools.visible, "the tool row goes")
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
        verify(!note.visible || !note.truncated, "the Digits note is whole or hidden: " + note.text)
        s.dash.leaveDrive(true)
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
        s.dash.driveTap()    // back to the gauges alone
        tryCompare(s.dash, "barsShown", 0, 2000)
        const cell = find(s.dash, "dashCell_0_0")
        verify(waitForPolish(s.main))
        mousePress(cell)
        wait(holdMs + 300)
        compare(s.dash.held, -1, "the hold lifts nothing")
        mouseRelease(cell)
        compare(s.dash.panel, "", "and opens no menu")
        verify(!s.dash.barsUp, "nor counts as a tap")
    }

    // ---- The status line (section 2)

    function statusWords(s) {
        return find(s.dash, "driveStatusWord").text + "|" + find(s.dash, "driveStatusAddress").text
    }

    function test_theStatusLineNamesTheStateAndTheAddress() {
        const s = driven()
        enterOn(s)
        tryCompare(Bridge, "linkState", "ok", 3000)
        compare(statusWords(s), Theme.linkShortLabel("ok") + "|· " + Bridge.uriLabel)
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
        compare(find(s.dash, "driveStatusTime").text, s.main.logTimeText(Bridge.logElapsedMs))
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
        verify(s.dash.driving, "the view stays on")
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
        verify(!s.dash.barsUp, "hidden bars stay hidden")
        compare(Bridge.logNotice, "", "no notice to dismiss")
        const timer = find(s.dash, "driveLogEndedTimer")
        compare(timer.interval, s.dash.driveHideMs, "for as long as the bars would stay")
        timer.triggered()
        verify(!run.visible, "then it goes")
    }

    // Pressed in the revealed bars, where the header covers the status line
    function test_stopLogFromTheBarsWaitsForThemToGo() {
        const s = driven()
        enterOn(s)
        tryVerify(() => Bridge.signalModel.subscribedCount > 0, 3000, "signals ticked")
        compare(Bridge.startLog(), true)
        tryCompare(Bridge, "logActive", true, 3000)
        revealedOn(s)
        const timer = find(s.dash, "driveLogEndedTimer")
        const run = find(s.dash, "driveStatusRun")
        Bridge.stopLog()
        tryVerify(() => s.dash.driveLogEnded, 5000, "said")
        verify(s.dash.barsUp, "the bars stay")
        verify(!timer.running, "no time spent behind the header")
        s.dash.driveTap()
        verify(timer.running, "it starts as they go")
        compare(run.text, qsTr("Log ended"))
        verify(run.visible)
        s.dash.driveTap()
        verify(!timer.running, "and waits again behind them")
    }

    // A full disk: the driver didn't ask, so it comes with the notice and the bars
    function test_aLogThatEndsByItselfRaisesTheNotice() {
        const s = driven()
        enterOn(s)
        tryVerify(() => Bridge.signalModel.subscribedCount > 0, 3000, "signals ticked")
        compare(Bridge.startLog(), true)
        tryCompare(Bridge, "logActive", true, 3000)
        verify(!s.dash.barsUp, "hidden at first")
        Harness.failRecordingWrites()
        tryCompare(Bridge, "logActive", false, 5000)
        compare(Bridge.logNotice, "Log ended: the disk is full")
        tryVerify(() => s.dash.barsUp, 2000, "the notice raises them")
        verify(s.dash.barsHeld, "and holds them")
        verify(!s.dash.driveLogEnded, "the status line's words are for Stop log")
        compare(s.main.currentPage, s.main.dashboardPage)
        verify(s.dash.driving, "the view stays on")
    }

    function test_leavingClearsTheWords() {
        const s = driven()
        enterOn(s)
        s.dash.driveLogEnded = true
        s.dash.leaveDrive(true)
        compare(s.dash.driveLogEnded, false)
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
        verify(s.dash.barsUp, "a tap on a gauge reveals")
        tryCompare(s.dash, "barsShown", 1, 2000)
        verify(b.head.visible && b.tab.visible && b.tools.visible, "every bar shows")
        compare(swipe.height, height, "the gauges don't reflow")
        compare(find(s.dash, "dashDrive").text, qsTr("Exit drive"))
    }

    function test_aTapOffTheCellsAlsoReveals() {
        const s = driven()
        s.dash.doc = { format: 2, pages: [{ name: "P", cells: [
            { kind: "digital", w: 3, h: 6, label: "A", refs: ["id:1001"], unit: "RPM", options: {} }] }] }
        enterOn(s)
        const area = find(s.dash, "driveTapArea")
        tryVerify(() => area.height > 200, 2000, "laid out")
        mouseClick(area, area.width - 12, area.height - 12)
        verify(s.dash.barsUp, "the bottom of an emptier page")
    }

    function test_theBarsHideAfterTheDelay() {
        const s = driven()
        enterOn(s)
        revealedOn(s)
        const timer = hideTimer(s)
        verify(timer.running, "the delay runs")
        compare(timer.interval, 4000, "the default")
        timer.triggered()
        verify(!s.dash.barsUp, "they slide out")
        tryCompare(s.dash, "barsShown", 0, 2000)
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
        const reset = find(s.dash, "resetPeaks")
        const spy = createTemporaryObject(spyComponent, testCase,
                                          { target: timer, signalName: "runningChanged" })
        mouseClick(reset)
        compare(resets, 1, "the press went on to Reset peaks")
        verify(s.dash.driving, "which is no exit")
        compare(s.dash.barTouches, 1, "a touch on the bars")
        compare(spy.count, 2, "the delay stopped and started again at the touch")
        verify(timer.running)
        timer.interval = 20
        tryCompare(s.dash, "barsUp", false, 2000, "and ended after it")
    }

    // The gaps in the bars aren't gauges: no toggle, and the delay starts again
    function test_aTouchInTheBarsGapsStaysOutOfTheGauges() {
        const s = driven()
        enterOn(s)
        revealedOn(s)
        const b = bars(s)
        mouseClick(b.tab, b.tab.width / 2, b.tab.height - 3)
        verify(s.dash.barsUp, "below the tabs")
        const backdrop = find(s.dash, "driveToolsBackdrop")
        const before = s.dash.barTouches
        const spy = createTemporaryObject(spyComponent, testCase,
                                          { target: hideTimer(s), signalName: "runningChanged" })
        mouseClick(backdrop, backdrop.width / 2, 1)
        verify(s.dash.barsUp, "around the tools")
        compare(s.dash.barTouches, before + 1, "a touch on the bars")
        compare(spy.count, 2, "the delay started again")
    }

    function test_aTouchOnEachBarIsCounted() {
        const s = driven()
        enterOn(s)
        revealedOn(s)
        const b = bars(s)
        verify(waitForPolish(s.main), "laid out")
        const before = s.dash.barTouches
        mouseClick(b.head, b.head.width / 2, b.head.height / 2)
        compare(s.dash.barTouches, before + 1, "the top bar")
        mouseClick(b.tab, b.tab.width / 2, b.tab.height - 3)
        compare(s.dash.barTouches, before + 2, "the tab bar")
        verify(s.dash.driving, "neither is an exit")
    }

    function test_neverKeepsTheBarsUp() {
        uiStore.setValue("driveHideMs", 0)
        uiStore.sync()
        const s = driven()
        compare(s.dash.driveHideMs, 0, "Settings' value reaches the page")
        enterOn(s)
        revealedOn(s)
        verify(!hideTimer(s).running, "no delay to run")
        verify(s.dash.barsUp)
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
        s.dash.driveTap()
        verify(!s.dash.barsUp, "tapped away")
        verify(!s.dash.barsHeld, "the hold ends with it")
    }

    function test_aRepeatedTapStartsTheDelayAfterAFault() {
        const s = faulted()
        s.dash.driveTap()
        s.dash.driveTap()
        verify(s.dash.barsUp, "revealed by hand")
        verify(hideTimer(s).running, "a tap's reveal has the delay, fault or no fault")
    }

    function test_aLogNoticeRevealsAndHoldsTheBars() {
        const s = driven()
        enterOn(s)
        raiseANotice()
        tryCompare(s.dash, "barsUp", true, 2000, "a notice raises them")
        verify(!hideTimer(s).running, "held")
        tryCompare(s.dash, "barsShown", 1, 2000)
        verify(find(s.dash, "dashLogNotice").visible, "with its row and Dismiss")
        Bridge.clearLogNotice()
        tryCompare(s.dash, "barsHeld", false, 2000, "its end lets the delay run")
        verify(hideTimer(s).running)
    }

    function test_aFaultAlreadyUpRevealsOnEntering() {
        const s = drivenOnTheFake("ECUIO_FAKE_READ_STATUS", "7", "?profile=raw&filter=0x7E8")
        tryCompare(Bridge, "linkState", "fault", 5000)
        enterOn(s)
        verify(s.dash.barsUp, "not hidden behind the view")
        verify(s.dash.barsHeld, "and held")
        verify(!hideTimer(s).running)
    }

    function test_aNoticeAlreadyUpRevealsOnEntering() {
        const s = driven()
        raiseANotice()
        enterOn(s)
        verify(s.dash.barsUp, "not hidden behind the view")
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
        compare(s.dash.raisesBars(data.state), data.want)
    }

    function test_quietDoesntReveal() {
        const s = drivenOnTheFake("ECUIO_FAKE_READ_STATUS", "9", "?profile=raw&filter=0x7E8")
        enterOn(s)
        tryCompare(Bridge, "linkState", "quiet", 5000)
        wait(20)
        verify(!s.dash.barsUp, "the status line says it")
        verify(s.dash.driving)
    }

    // ---- Leaving (sections 3 and 4)

    function test_disconnectingEndsTheViewAndKeepsTheSetting() {
        const s = driven()
        enterOn(s)
        verify(dashSaved(s), "remembered on entering")
        Bridge.disconnectSession()
        tryCompare(s.dash, "driving", false, 5000)
        compare(s.stand.visibility, Window.Maximized, "the system bars return")
        verify(dashSaved(s), "the session's end doesn't clear it")
    }

    // The bridge's "Replay finished" line is the recording's end
    function test_aRecordingThatEndsKeepsTheView() {
        const s = phoneMain()
        let finished = false
        const heard = line => { if (line.indexOf("Replay finished") === 0) finished = true }
        Bridge.logMessage.connect(heard)
        connect(replayUri())
        enterOn(s)
        tryVerify(() => finished, 10000, "the replay plays to its end")
        Bridge.logMessage.disconnect(heard)
        verify(Bridge.connected, "still connected at the recording's end")
        verify(s.dash.driving, "a replay user may rewind")
    }

    function dashSaved(s) { return find(s.dash, "dashStore").phoneDrive }

    function test_exitDriveLeavesAndClearsTheSetting() {
        const s = driven()
        enterOn(s)
        revealedOn(s)
        mouseClick(find(s.dash, "dashDrive"))
        compare(s.dash.driving, false)
        compare(s.stand.visibility, Window.Maximized)
        verify(!dashSaved(s), "Exit drive clears it")
        const b = bars(s)
        tryCompare(b.head, "visible", true, 2000, "the bars are back")
        compare(b.head.y, 0, "at their place")
        compare(find(s.dash, "dashDrive").text, qsTr("Drive"))
    }

    function test_aTabLeavesTheViewForGood_data() {
        return [{ tag: "another page", index: 1, page: 2 }, { tag: "Dash's own tab", index: 0, page: 1 }]
    }
    function test_aTabLeavesTheViewForGood(data) {
        const s = driven()
        enterOn(s)
        revealedOn(s)
        const bar = find(s.main.contentItem, "tabBar")
        mouseClick(bar.itemAt(data.index))
        compare(s.dash.driving, false, "a tab is Exit drive")
        compare(s.main.currentPage, data.page)
        verify(!dashSaved(s))
    }

    function test_theStatusOpensTheLogAndLeaves() {
        const s = driven()
        enterOn(s)
        revealedOn(s)
        mouseClick(find(s.main.contentItem, "linkPill"))
        compare(s.dash.driving, false)
        compare(s.main.currentPage, s.main.logPage)
    }

    function test_backRevealsThenLeaves() {
        const s = driven()
        enterOn(s)
        compare(back(s), true, "taken, never the launcher")
        verify(s.dash.barsUp, "hidden bars: back reveals")
        verify(s.dash.driving)
        compare(back(s), true)
        compare(s.dash.driving, false, "revealed bars: back leaves")
        verify(!dashSaved(s))
    }

    function test_backOnDashWithoutTheViewIsLeftToTheSystem() {
        const s = driven()
        compare(back(s), false, "nowhere to go back to")
    }

    // Edit mode is Dash's own view: Back closes a gauge's panel, then
    // leaves edit mode, then leaves the app
    function test_backClosesThePanelThenEditModeThenLeaves() {
        const s = driven()
        verify(s.dash.shownCells.length > 0, "a gauge to edit")
        s.dash.startEditing()
        s.dash.select(0)
        compare(s.dash.panel, "cell", "its panel is open")
        compare(back(s), true, "taken")
        compare(s.dash.panel, "", "the panel closes first")
        verify(s.dash.editing, "edit mode stays")
        compare(s.dash.selected, 0, "and the gauge stays selected")
        compare(back(s), true, "taken")
        verify(!s.dash.editing, "then edit mode ends")
        compare(back(s), false, "then the system's")

        // A panel outside edit mode closes alone
        s.dash.select(0)
        compare(back(s), true)
        compare(s.dash.panel, "")
        compare(back(s), false)
    }

    // ---- Remembered once connected (section 7)

    function test_aConnectEntersWithDashShowing() {
        dashStore.setValue("phoneDrive", true)
        dashStore.sync()
        const s = phoneMain()
        compare(s.dash.driving, false, "never opens in the view")
        connect("mock://demo")
        tryVerify(() => s.dash.driving, 3000, "it comes back with the session")
    }

    function test_aConnectFromAnotherPageStaysThere() {
        dashStore.setValue("phoneDrive", true)
        dashStore.sync()
        const s = phoneMain()
        s.main.gotoPage(2)
        connect("mock://demo")
        wait(50)
        compare(s.main.currentPage, 2)
        compare(s.dash.driving, false)
    }

    function test_aReplayNeverEntersByItself() {
        dashStore.setValue("phoneDrive", true)
        dashStore.sync()
        const s = phoneMain()
        verify(s.dash.isReplay("REPLAY://x"), "whatever its case")
        verify(!s.dash.isReplay("mock://demo"))
        connect(replayUri())
        wait(50)
        compare(s.dash.driving, false)
    }

    function test_aConnectLeavesEditModeAlone() {
        dashStore.setValue("phoneDrive", true)
        dashStore.sync()
        const s = phoneMain()
        s.dash.startEditing()
        connect("mock://demo")
        wait(50)
        compare(s.dash.driving, false, "edit mode is never interrupted")
        s.dash.stopEditing()
    }

    function test_aSettingLeftOffDoesntEnter() {
        const s = phoneMain()
        connect("mock://demo")
        wait(50)
        compare(s.dash.driving, false)
    }

    // ---- The hint (section 3)

    function test_theHintShowsOnTheFirstThreeEntries() {
        dashStore.setValue("driveHints", 0)
        dashStore.sync()
        const s = driven()
        const hint = find(s.dash, "driveHint")
        for (let i = 1; i <= 3; ++i) {
            s.dash.enterDrive()
            verify(hint.visible, "entry " + i)
            s.dash.leaveDrive(true)
        }
        s.dash.enterDrive()
        verify(!hint.visible, "not on the fourth")
    }

    function test_theHintLastsTheDelay_data() {
        return [{ tag: "two seconds", ms: 2000, want: 2000 }, { tag: "never", ms: 0, want: 4000 }]
    }
    function test_theHintLastsTheDelay(data) {
        dashStore.setValue("driveHints", 0)
        dashStore.sync()
        uiStore.setValue("driveHideMs", data.ms)
        uiStore.sync()
        const s = driven()
        s.dash.enterDrive()
        compare(find(s.dash, "driveHintTimer").interval, data.want)
    }

    function test_theHintGoesAtItsTimeOrATap() {
        dashStore.setValue("driveHints", 0)
        dashStore.sync()
        const s = driven()
        s.dash.enterDrive()
        const hint = find(s.dash, "driveHint")
        find(s.dash, "driveHintTimer").triggered()
        verify(!hint.visible, "its time")
        s.dash.leaveDrive(true)
        dashStore.setValue("driveHints", 0)
        s.dash.enterDrive()
        s.dash.driveTap()
        verify(!hint.visible, "a tap")
    }

    // ---- The platform (sections 5, 6 and 8)

    function test_theOrientationIsLockedInTheViewAlone() {
        uiStore.setValue("driveOrientation", "side")
        uiStore.sync()
        const s = driven()
        compare(PhoneWindow.orientation, "follow", "before")
        s.dash.enterDrive()
        compare(PhoneWindow.orientation, "side", "held while the view is on")
        s.dash.leaveDrive(true)
        compare(PhoneWindow.orientation, "follow", "released on leaving")
    }

    function test_theScreenStaysOnWithDashShown() {
        const s = driven()
        tryCompare(PhoneWindow, "keepScreenOn", true, 2000, "a session and Dash, on battery")
        s.main.gotoPage(2)
        tryCompare(PhoneWindow, "keepScreenOn", false, 2000, "another page on battery sleeps")
        s.main.gotoPage(s.main.dashboardPage)
        tryCompare(PhoneWindow, "keepScreenOn", true, 2000)
    }

    // A tablet has the view but not the phone form's rule
    function test_aTabletStaysAwakeInTheView() {
        const s = driven()
        s.main.width = 1000
        s.main.height = 900
        verify(waitForPolish(s.main), "laid out")
        compare(s.main.phoneForm, false)
        tryCompare(PhoneWindow, "keepScreenOn", false, 2000, "not the phone form")
        enterOn(s)
        tryCompare(PhoneWindow, "keepScreenOn", true, 2000, "the view holds it")
        s.dash.leaveDrive(true)
        tryCompare(PhoneWindow, "keepScreenOn", false, 2000)
    }

    function test_theKeepAwakeFormIsThePhonesAlone() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main)
        main.gotoPage(main.dashboardPage)
        connect("mock://demo")
        wait(50)
        compare(PhoneWindow.keepScreenOn, false, "a desktop's Main never asks, even on Dash")
    }

    function test_theScreenSleepsWithoutASession() {
        const s = phoneMain()
        s.main.gotoPage(s.main.dashboardPage)
        wait(50)
        compare(PhoneWindow.keepScreenOn, false, "Dash alone doesn't hold it")
        connect("mock://demo")
        tryCompare(PhoneWindow, "keepScreenOn", true, 2000)
        Bridge.disconnectSession()
        tryCompare(PhoneWindow, "keepScreenOn", false, 5000, "the session's end lets it go")
    }

    function test_theInsetsBoundTheView_data() {
        return [{ tag: "gone", bottom: 0 }, { tag: "kept", bottom: 24 }]
    }
    function test_theInsetsBoundTheView(data) {
        const s = phoneMain({ testInsets: { left: 0, top: 34, right: 0, bottom: data.bottom } })
        connect("mock://demo")
        enterOn(s)
        const line = find(s.dash, "driveStatusLine")
        compare(line.mapToItem(s.main.contentItem, 0, 0).y, 34, "the status line sits under the cutout")
        const pages = find(s.dash, "dashPages")
        compare(pages.mapToItem(s.main.contentItem, 0, pages.height).y,
                s.main.contentItem.height - data.bottom, "the gauges reach the bottom inset")
    }

    function test_aRotationKeepsTheView() {
        const s = driven()
        enterOn(s)
        s.main.width = 832
        s.main.height = 384
        verify(waitForPolish(s.main), "laid out")
        verify(s.dash.driving, "the view stays")
        tryVerify(() => find(s.dash, "dashPage_0").columns === 12, 2000, "twelve columns on its side")
        compare(find(s.dash, "driveStatusLine").y, 0, "the line stays at the top")
    }

    // ---- Android's Back

    function test_backFromTheLogReturnsToTheTabItCameFrom() {
        const s = phoneMain()
        const pill = find(s.main.contentItem, "linkPill")
        s.main.gotoPage(3)
        mouseClick(pill)
        compare(s.main.currentPage, s.main.logPage)
        compare(back(s), true)
        compare(s.main.currentPage, 3, "Codes, where it came from")
        compare(back(s), false, "and nothing further back")
    }

    function test_otherAddressBackReturnsToDash() {
        const s = phoneMain()
        const settings = find(s.main.contentItem, "settingsPage")
        compare(s.main.currentPage, s.main.dashboardPage)
        s.dash.otherAddressRequested()
        compare(s.main.currentPage, s.main.settingsPage)
        compare(settings.pageKey, "connection")
        compare(back(s), true)
        compare(s.main.currentPage, s.main.dashboardPage, "back where it came from")
        compare(settings.pageKey, "", "and Settings' list is what it opens to next")
    }

    function test_otherAddressAfterTheListIsBackInTheList() {
        const s = phoneMain()
        const settings = find(s.main.contentItem, "settingsPage")
        s.dash.otherAddressRequested()
        settings.pageKey = ""
        compare(settings.backLeaves, false, "the list ends it")
        settings.open("connection")
        compare(back(s), true)
        compare(s.main.currentPage, s.main.settingsPage, "Back closes Connection as usual")
    }

    function test_aSettingsSubpageClosesAfterALeaveAndReturn() {
        const s = phoneMain()
        const settings = find(s.main.contentItem, "settingsPage")
        s.main.gotoPage(s.main.settingsPage)
        settings.open("display")
        s.main.gotoPage(2)
        s.main.gotoPage(s.main.settingsPage)
        compare(back(s), true, "taken")
        compare(settings.pageKey, "")
        compare(back(s), false, "at the list: left to the system")
    }

    function test_aHeldBackIsOneBack() {
        const s = driven()
        enterOn(s)
        compare(Harness.pressBack(s.main), true)
        verify(s.dash.barsUp, "the press reveals")
        compare(Harness.pressBack(s.main, true), true, "the repeat is taken, or the system would act on it")
        verify(s.dash.driving, "and doesn't go on to leave")
        verify(s.dash.barsUp)
    }

    function test_backFromAnOpenRecordingGoesToTheList() {
        const s = driven()
        const recordings = find(s.main.contentItem, "recordingsPage")
        tryVerify(() => Bridge.signalModel.subscribedCount > 0, 3000, "signals ticked")
        compare(Bridge.startLog(), true)
        tryCompare(Bridge, "logActive", true, 3000)
        Bridge.stopLog()
        tryVerify(() => recordings.viewing, 5000, "Stop log opens the viewer")
        compare(s.main.currentPage, s.main.recordingsPage)
        compare(back(s), true, "there's a list to go back to")
        compare(recordings.viewing, false)
        compare(s.main.currentPage, s.main.recordingsPage)
        compare(back(s), false, "at the list: left to the system")
    }

    // Two panes have no subpage to close: a phone on its side can be this wide
    function test_backInTwoPanesIsLeftToTheSystem() {
        const s = phoneMain()
        s.main.width = 1000
        s.main.height = 500
        const settings = find(s.main.contentItem, "settingsPage")
        s.main.gotoPage(s.main.settingsPage)
        settings.open("display")
        tryCompare(settings, "twoPane", true, 2000)
        compare(back(s), false, "nothing to close")
        compare(settings.pageKey, "display", "the page stays")
    }

    function test_keyFocusComesBackWhenNothingHoldsIt() {
        const s = phoneMain()
        const zoomRoot = find(s.main.contentItem, "zoomRoot")
        s.main.gotoPage(s.main.settingsPage)
        tryVerify(() => s.main.activeFocusItem !== null, 2000)
        // A control that takes the focus and goes away leaves nothing with it
        const settings = find(s.main.contentItem, "settingsPage")
        settings.open("display")
        settings.forceActiveFocus()
        tryVerify(() => settings.activeFocus, 2000)
        s.main.gotoPage(2)
        tryVerify(() => zoomRoot.activeFocus, 2000, "held where the back key is heard")
    }

    // Qt leaves the active focus on an item that hides
    function test_backStillWorksWithAFocusedControlHidden() {
        const s = phoneMain()
        fronted(s)
        s.main.gotoPage(s.main.logPage)
        const field = createTemporaryObject(fieldComponent, find(s.main.contentItem, "zoomRoot"))
        field.forceActiveFocus()
        tryVerify(() => field.activeFocus, 2000)
        field.visible = false
        compare(back(s), true, "with no page change")
        compare(s.main.currentPage, s.main.dashboardPage)
    }

    // A control that goes away drops the focus to the window's root
    function test_aFocusedControlDestroyedLeavesBackWorking() {
        const s = phoneMain()
        fronted(s)
        const field = fieldComponent.createObject(find(s.main.contentItem, "zoomRoot"))
        field.forceActiveFocus()
        tryVerify(() => field.activeFocus, 2000)
        field.destroy()
        wait(0)
        verify(s.main.activeFocusItem !== field, "gone")
        tryVerify(() => focusReachesTheHandler(s), 2000, "held where the back key is heard")
    }

    function test_leavingTheFrontKeepsAFieldsFocus() {
        const s = phoneMain()
        fronted(s)
        const field = createTemporaryObject(fieldComponent, s.dash)
        field.forceActiveFocus()
        tryVerify(() => field.activeFocus, 2000)
        // No parent: a window counts as active while a transient child of its parent is
        const other = otherWindow.createObject(null)
        other.requestActivate()
        tryVerify(() => !s.main.active, 3000, "another window in front")
        verify(!field.activeFocus)
        fronted(s)
        other.destroy()
        tryVerify(() => field.activeFocus, 2000, "the field has it again")
    }
}
