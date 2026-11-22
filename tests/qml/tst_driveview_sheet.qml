// File/layer: tests/qml/tst_driveview_sheet.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.driveview_sheet (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

DriveViewKit {
    id: testCase
    name: "DriveView"

    // ---- The sheet (ADR-0059's appendix "the fill and the reveal from the bottom")

    // The delay at Never, so a long case's presses find the sheet still up
    function neverHide() {
        uiStore.setValue("driveHideMs", 0)
        uiStore.sync()
    }

    function withTheBarsHeld() {
        neverHide()
        return driven()
    }

    // The top row and the status line stay in view: the sheet rises from
    // the foot and the header never comes down
    function test_theRevealCoversNoTopRow_data() {
        return [{ tag: "upright", w: 384, h: 832 }, { tag: "on its side", w: 832, h: 384 }]
    }

    function test_theRevealCoversNoTopRow(data) {
        const s = withTheBarsHeld()
        s.main.width = data.w
        s.main.height = data.h
        enterOn(s)
        revealedOn(s)
        const b = bars(s)
        verify(waitForPolish(s.main), "laid out")
        const top = item => item.mapToItem(s.main.contentItem, 0, 0).y
        const first = find(s.dash, "dashCell_0_0")
        const line = find(s.dash, "driveStatusLine")
        verify(line.visible, "the status line shows")
        verify(top(first) + first.height <= top(b.sheet),
               "the first cell " + top(first) + " ends above the sheet at " + top(b.sheet))
        verify(top(line) + line.height <= top(b.sheet), "nothing covers the status line")
        verify(!b.head.visible, "no header")
        fuzzyCompare(top(b.sheet) + b.sheet.height, top(b.tab), 0.5, "the sheet sits on the tab bar")
    }

    // Upright the session's row over the view's, Exit drive nearest the
    // tabs; on its side one row. Each control is the header's or the tool
    // row's, bound to the same calls
    function test_theSheetHoldsTheSessionAndTheView_data() {
        return [{ tag: "upright", w: 384, h: 832, rows: 2 }, { tag: "narrowest", w: 320, h: 832, rows: 2 },
                { tag: "on its side", w: 832, h: 384, rows: 1 }]
    }

    function test_theSheetHoldsTheSessionAndTheView(data) {
        const s = withTheBarsHeld()
        s.main.width = data.w
        s.main.height = data.h
        enterOn(s)
        revealedOn(s)
        verify(waitForPolish(s.main), "laid out")
        const y = n => find(s.dash, n).mapToItem(s.main.contentItem, 0, 0).y
        if (data.rows === 2)
            verify(y("driveSessionRow") < y("driveViewRow"), "the view's row nearest the tabs")
        else
            fuzzyCompare(y("driveSessionRow"), y("driveViewRow"), 0.5, "one row")
        for (const n of ["driveStartLog", "driveDisconnect", "driveExit", "driveResetPeaks", "driveNight"])
            verify(find(s.dash, n).visible, n + " shows")
        verify(!find(s.dash, "driveStopLog").visible && !find(s.dash, "driveMark").visible)
        tryVerify(() => Bridge.signalModel.subscribedCount > 0, 3000, "signals ticked")
        mouseClick(find(s.dash, "driveStartLog"))
        tryCompare(Bridge, "logActive", true, 3000)
        verify(waitForPolish(s.main), "laid out")
        verify(find(s.dash, "driveStopLog").visible && find(s.dash, "driveLogReadout").visible)
        compare(find(s.dash, "driveLogReadout").text.indexOf(s.dash.driveLogTime), 0, "the log's time")
        verify(!find(s.dash, "driveStartLog").visible, "Stop log in its place")
        verify(!find(s.dash, "driveDisconnect").visible, "and in Disconnect's, as in the header")
        verify(s.dash.drivePart.driving, "Start log is no exit")
        // Mark offers itself during a log, and the row holds the readout, Stop log and
        // Mark inside the window, a touch's size and apart
        const mark = find(s.dash, "driveMark")
        verify(mark.visible, "Mark shows during a log")
        // The readout gives way first: Stop log and Mark keep their natural width
        const checkRow = () => {
            const placed = []
            for (const c of find(s.dash, "driveSessionRow").children) {
                if (!c.visible || c.objectName === "")
                    continue
                const at = c.mapToItem(s.main.contentItem, 0, 0)
                verify(at.x >= -0.5 && at.x + c.width <= s.main.contentItem.width + 0.5,
                       c.objectName + " inside the window: " + at.x + " + " + c.width)
                if (c.objectName !== "driveLogReadout") {
                    verify(c.width * Theme.zoom >= 47.5 && c.height * Theme.zoom >= 47.5,
                           c.objectName + " is " + c.width + " x " + c.height)
                    verify(c.width >= c.implicitWidth - 0.5, c.objectName + " keeps its width: "
                           + c.width + " of " + c.implicitWidth)
                }
                placed.push({ name: c.objectName, x: at.x, right: at.x + c.width })
            }
            placed.sort((a, b) => a.x - b.x)
            for (let i = 1; i < placed.length; ++i)
                verify(placed[i].x >= placed[i - 1].right - 0.5,
                       placed[i].name + " clear of " + placed[i - 1].name)
        }
        checkRow()
        compare(Bridge.markCount, 0, "a log starts from no marks")
        mouseClick(mark)
        tryCompare(Bridge, "markCount", 1, 3000)
        compare(mark.text, "Mark (1)")
        verify(waitForPolish(s.main), "laid out")
        checkRow()
        // A long readout (hours and a size) is the part that gives way
        s.dash.driveLogTime = "12:34:56"
        s.dash.driveLogSize = "1234.5 MB"
        verify(waitForPolish(s.main), "laid out")
        checkRow()
        Bridge.stopLog()
        tryCompare(Bridge, "logActive", false, 5000)
        verify(!find(s.dash, "driveMark").visible, "Mark goes with the log")
        verify(find(s.dash, "driveDisconnect").visible, "Disconnect comes back")
        verify(waitForPolish(s.main), "laid out")
        const d = find(s.dash, "driveDisconnect")
        mouseClick(d)
        tryCompare(s.dash.drivePart, "driving", false, 5000, "Disconnect ends the session and the view")
    }

    function test_theSheetMarksARecording() {
        neverHide()
        const s = phoneMain()
        Bridge.recording = true
        connect("mock://demo")
        tryCompare(Bridge, "recordingActive", true, 3000)
        enterOn(s)
        revealedOn(s)
        const mark = find(s.dash, "driveMark")
        verify(mark.visible, "Mark stands where Start log would")
        verify(!find(s.dash, "driveStartLog").visible)
        mouseClick(mark)
        tryCompare(Bridge, "markCount", 1, 3000)
        compare(mark.text, "Mark (1)")
    }

    // The armed recorder in the sheet (Main's armedButton is the header's): where
    // Start log stands, its states in words, Mark only inside a window
    function test_theSheetArmsTheRecorderAndMarksInAWindow() {
        neverHide()
        const s = phoneMain()
        const records = bound => '{"havne_conditions":1,"conditions":[{"key":"rec","name":"Engine stopped",'
                                + '"when":{"signal":["id:3001"],"unit":"km/h",' + bound + '},'
                                + '"record":{"before":0.2,"after":0.2}}]}'
        Bridge.saveConditions(records('"above":90000'))
        connect("mock://demo")
        enterOn(s)
        revealedOn(s)
        const armed = find(s.dash, "driveArmed")
        const mark = find(s.dash, "driveMark")
        verify(armed.visible && armed.enabled, "Armed offers itself")
        compare(armed.text, "Arm", "off, it says what a press does")
        verify(!mark.visible, "no Mark before a recorder")
        // Waiting: armed, between windows, no Mark
        mouseClick(armed)
        tryCompare(Bridge, "armedActive", true, 3000)
        compare(armed.text, "Armed")
        compare(find(s.dash, "driveStatusRun").text, "Armed")
        verify(!mark.visible, "no Mark between windows")
        mouseClick(armed)
        tryCompare(Bridge, "armedActive", false, 3000)
        Bridge.saveConditions(records('"below":1'))
        tryVerify(() => armed.enabled, 3000)
        mouseClick(armed)
        tryCompare(Bridge, "armedActive", true, 3000)
        tryVerify(() => Bridge.armedRecording, 10000, "a window opens")
        compare(armed.text, "Recording (window 1)")
        verify(mark.visible, "Mark inside the window")
        verify(!find(s.dash, "driveStartLog").visible, "Start log steps aside")
        compare(find(s.dash, "driveStatusRun").text, "Recording (window 1)")
        verify(waitForPolish(s.main), "laid out")
        for (const c of find(s.dash, "driveSessionRow").children) {
            if (!c.visible || c.objectName === "")
                continue
            const at = c.mapToItem(s.main.contentItem, 0, 0)
            verify(at.x >= -0.5 && at.x + c.width <= s.main.contentItem.width + 0.5,
                   c.objectName + " inside the window: " + at.x + " + " + c.width)
        }
        mouseClick(armed)
        tryCompare(Bridge, "armedActive", false, 3000)
        verify(!mark.visible, "Mark goes with the recorder")
    }

    // A tap on the disabled Armed says why in the notice, as a refused press does
    function test_aDisabledArmedExplainsItselfWhenTapped() {
        neverHide()
        const s = phoneMain()
        Bridge.saveConditions('{"havne_conditions":1,"conditions":[{"key":"c","name":"C",'
                              + '"when":{"signal":["id:1001"],"unit":"RPM","above":90000},"mark":true}]}')
        connect("mock://demo")
        enterOn(s)
        revealedOn(s)
        const armed = find(s.dash, "driveArmed")
        verify(armed.visible && !armed.enabled, "no condition records")
        Bridge.clearLogNotice()
        mouseClick(find(s.dash, "driveArmedHost"))
        tryCompare(Bridge, "logNotice", "Armed not started: no condition records", 3000)
        verify(!Bridge.armedActive)
    }

    // The header's disabled Armed explains itself the same way
    function test_aDisabledArmedInTheHeaderExplainsItselfWhenTapped() {
        const s = phoneMain()
        Bridge.saveConditions('{"havne_conditions":1,"conditions":[{"key":"c","name":"C",'
                              + '"when":{"signal":["id:1001"],"unit":"RPM","above":90000},"mark":true}]}')
        connect("mock://demo")
        const armed = find(s.main.contentItem, "armedButton")
        tryVerify(() => armed.visible, 5000, "Armed shows while connected")
        verify(!armed.enabled, "no condition records")
        Bridge.clearLogNotice()
        mouseClick(find(s.main.contentItem, "armedHost"))
        tryCompare(Bridge, "logNotice", "Armed not started: no condition records", 3000)
        verify(!Bridge.armedActive)
    }

    // Each control a touch's size on the glass, 8 from its neighbor, inside the window
    function test_theSheetsTargetsAreATouchsSize_data() {
        return [{ tag: "upright", w: 384, h: 832 }, { tag: "on its side", w: 832, h: 384 }]
    }

    function test_theSheetsTargetsAreATouchsSize(data) {
        const s = withTheBarsHeld()
        s.main.width = data.w
        s.main.height = data.h
        enterOn(s)
        revealedOn(s)
        verify(waitForPolish(s.main), "laid out")
        waitForRendering(s.main.contentItem)
        const shown = []
        for (const row of ["driveSessionRow", "driveViewRow"]) {
            for (const c of find(s.dash, row).children) {
                if (!c.visible || c.objectName === "")
                    continue
                const item = c.objectName === "driveNight" ? c.item : c
                const at = item.mapToItem(s.main.contentItem, 0, 0)
                verify(item.width * Theme.zoom >= 47.5 && item.height * Theme.zoom >= 47.5,
                       c.objectName + " is " + item.width + " x " + item.height)
                verify(at.x >= 0 && at.x + item.width <= s.main.contentItem.width + 0.5,
                       c.objectName + " inside the window: " + at.x)
                shown.push({ name: c.objectName, x: at.x, y: at.y, right: at.x + item.width })
            }
        }
        shown.sort((a, b) => a.y - b.y || a.x - b.x)
        for (let i = 1; i < shown.length; ++i) {
            if (Math.abs(shown[i].y - shown[i - 1].y) > 0.5)
                continue
            verify(shown[i].x - shown[i - 1].right >= 7.5,
                   shown[i - 1].name + " and " + shown[i].name + " are 8 apart")
        }
    }

    // ---- Leaving (sections 3 and 4)

    function test_disconnectingEndsTheViewAndKeepsTheSetting() {
        const s = driven()
        enterOn(s)
        verify(dashSaved(s), "remembered on entering")
        Bridge.disconnectSession()
        tryCompare(s.dash.drivePart, "driving", false, 5000)
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
        verify(s.dash.drivePart.driving, "a replay user may rewind")
    }

    function dashSaved(s) { return find(s.dash, "dashStore").phoneDrive }

    function test_exitDriveLeavesAndClearsTheSetting() {
        const s = driven()
        enterOn(s)
        revealedOn(s)
        mouseClick(find(s.dash, "driveExit"))
        compare(s.dash.drivePart.driving, false)
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
        compare(s.dash.drivePart.driving, false, "a tab is Exit drive")
        compare(s.main.currentPage, data.page)
        verify(!dashSaved(s))
    }

    function test_backRevealsThenLeaves() {
        const s = driven()
        enterOn(s)
        compare(back(s), true, "taken, never the launcher")
        verify(s.dash.drivePart.barsUp, "hidden bars: back reveals")
        verify(s.dash.drivePart.driving)
        compare(back(s), true)
        compare(s.dash.drivePart.driving, false, "revealed bars: back leaves")
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
}
