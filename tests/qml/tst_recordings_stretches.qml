// File/layer: tests/qml/tst_recordings_stretches.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.recordings_stretches (test_qml)

import QtQuick
import QtTest
import App
import AppTest

RecordingsKit {
    id: testCase
    name: "Recordings"

    // The Jump row steps between a recording's pulls: the stretch in view
    // with a margin, the cursor at its start, and where it is said
    function test_theJumpRowStepsBetweenPulls() {
        const page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        compare(page.comparePart.stretchesOf("pull").length, 2)
        const status = find(page, "jumpStatus")
        compare(status.text, "2 pulls · No cruise found")
        verify(!find(page, "jumpPrevious_pull").enabled, "nothing before the view's start")
        verify(find(page, "jumpNext_pull").enabled)

        find(page, "jumpNext_pull").clicked()
        compare(page.cursorMs, 18002, "the cursor at the pull's start")
        verify(page.viewT0 <= 18002 && page.viewT1 >= 22002, "the pull is in view: " + page.viewT0 + " to " + page.viewT1)
        verify(page.viewT1 - page.viewT0 < 20000, "zoomed to it")
        compare(status.text, "Pull 1 of 2 · 18.0 s")
        verify(!find(page, "jumpPrevious_pull").enabled, "no pull before the first")

        find(page, "jumpNext_pull").clicked()
        compare(page.cursorMs, 31002)
        compare(status.text, "Pull 2 of 2 · 31.0 s")
        verify(!find(page, "jumpNext_pull").enabled, "no pull after the last")

        find(page, "jumpPrevious_pull").clicked()
        compare(page.cursorMs, 18002)
        compare(status.text, "Pull 1 of 2 · 18.0 s")

        // A cursor moved off the stretch's start puts the counts back
        page.cursorsPart.setCursor(25000)
        compare(status.text, "2 pulls · No cruise found")
    }

    // A recording with neither a throttle nor an engine-load signal has no
    // pull to find at any level, and the Jump row says that, not just "No pull
    // found" (the findings fixture holds an engine speed alone)
    function test_theJumpRowSaysWhyARecordingWithNoThrottleOrLoadHasNoPull() {
        const page = createPage()
        page.recordings = [findingsRow]
        openRow(page, findingsRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        verify(page.recording.pullsLackSignals, "core finds no throttle or load")
        compare(page.comparePart.stretchesOf("pull").length, 0)
        compare(find(page, "jumpStatus").text,
                "No pull found (no throttle or load signal) · No cruise found")
    }

    // On a phone the row keeps its plain words, so it stays one line at any
    // font, and the reason is the label's spoken name
    function test_thePhonesJumpRowKeepsItsPlainWordsAndSpeaksTheReason() {
        const page = createPage()
        page.recordings = [findingsRow]
        openRow(page, findingsRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        Theme.compact = true
        try {
            const status = find(page, "jumpStatus")
            compare(status.text, "No pull found · No cruise found")
            compare(status.Accessible.name,
                    "No pull found (no throttle or load signal) · No cruise found")
        } finally {
            Theme.compact = false
        }
    }

    // The Stretches panel asks again with its levels, shows core's defaults in
    // its fields, and goes back to them
    function test_theStretchesPanelReasksWithItsLevels() {
        const page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        compare(page.comparePart.stretchesOf("pull").length, 2)
        find(page, "jumpStretches").clicked()
        verify(page.stretchesOpen)
        verify(waitForPolish(page.Window.window), "laid out")
        verify(find(page, "stretchesPanel").visible, "the panel is up")
        verify(find(page, "chartFrame").visible, "the desktop keeps its chart")
        const rise = find(page, "rule_pullRise")
        compare(rise.placeholderText, "750", "core's default shows in the field")
        rise.text = "5000"
        find(page, "stretchesApply").clicked()
        verify(page.stretchList === null, "looking again after Apply")
        compare(find(page, "jumpStatus").text, "Looking for stretches…")
        tryVerify(() => page.stretchList !== null, 10000, "the new answer")
        compare(page.comparePart.stretchesOf("pull").length, 0, "no pull climbs 5000 rpm")
        compare(find(page, "jumpStatus").text, "No pull found · No cruise found")
        compare(page.stretchRules.pullRise, 5000)
        verify(page.stretchesOpen, "the desktop's panel stays up")
        find(page, "stretchesReset").clicked()
        compare(rise.text, "", "the field is empty again")
        tryVerify(() => page.stretchList !== null && page.comparePart.stretchesOf("pull").length === 2, 10000,
                  "the defaults find the two again")
    }

    // Where a pull ends: at the throttle's lift unless the panel's choice
    // says top speed, asked at its tap, kept with the levels, and Defaults
    // puts the lift back. pull-twice's throttle lifts 2 ms after speed tops
    function test_thePanelChoosesWhereAPullEnds() {
        const page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        compare(page.comparePart.stretchesOf("pull").map(s => s.toMs), [25002, 35002], "the lift")
        find(page, "jumpStretches").clicked()
        verify(waitForPolish(page.Window.window), "laid out")
        const lift = find(page, "pullEnd_lift")
        const top = find(page, "pullEnd_topSpeed")
        verify(lift.selected && !top.selected, "the lift is the default")
        top.clicked()
        verify(top.selected && !lift.selected)
        tryVerify(() => page.stretchList !== null, 10000, "asked again at the tap")
        compare(page.comparePart.stretchesOf("pull").map(s => s.toMs), [25000, 35000], "top speed")
        compare(page.stretchRules.pullEnd, "top-speed", "kept with the levels")
        find(page, "stretchesReset").clicked()
        verify(lift.selected, "Defaults puts the lift back")
        tryVerify(() => page.stretchList !== null
                  && page.comparePart.stretchesOf("pull").map(s => s.toMs).join() === "25002,35002", 10000,
                  "the lift's ends again")
        compare(page.stretchRules.pullEnd, undefined)
    }

    // A stored end that is no word of the choice reads as the lift, with no
    // refusal; a stored top speed opens selected
    function test_aStoredPullEndIsReadAsTheLiftUnlessItIsTopSpeed() {
        setStoredRules("{\"pullEnd\": \"sideways\"}")
        let page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        compare(page.rulesProblem, "", "no refusal")
        compare(page.comparePart.stretchesOf("pull").map(s => s.toMs), [25002, 35002])
        page.destroy()
        setStoredRules("{\"pullEnd\": \"top-speed\"}")
        page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        compare(page.comparePart.stretchesOf("pull").map(s => s.toMs), [25000, 35000])
        find(page, "jumpStretches").clicked()
        verify(waitForPolish(page.Window.window), "laid out")
        verify(find(page, "pullEnd_topSpeed").selected, "the panel shows the stored end")
    }

    // The name the Stretches panel gives a rule: core's refusal and the panel's own
    // words say the rule by it
    function fieldName(page, key) {
        return find(page, "stretchesPanel").fields.find(f => f.key === key).name
    }

    // A refused level says why under the fields and leaves the earlier answer;
    // text that is no number is said at once, with no ask
    function test_aRefusedLevelSaysWhyAndLeavesTheListAlone() {
        const page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        find(page, "jumpStretches").clicked()
        verify(waitForPolish(page.Window.window), "laid out")
        const problem = find(page, "stretchesProblem")
        verify(!problem.visible, "no words before a refusal")
        find(page, "rule_pullLevel").text = "150"
        find(page, "stretchesApply").clicked()
        compare(problem.text, fieldName(page, "pullLevel") + " must be a number from 0 to 100")
        verify(problem.visible)
        compare(page.comparePart.stretchesOf("pull").length, 2, "the earlier answer stands")
        compare(page.stretchRules.pullLevel, undefined, "and nothing was kept")
        find(page, "rule_pullLevel").text = ""
        find(page, "rule_pullRise").text = "abc"
        find(page, "stretchesApply").clicked()
        compare(problem.text, "Engine speed rises must be a number")
        verify(page.stretchList !== null, "no ask was made")
    }

    function storedViews() {
        const store = Qt.createQmlObject("import QtCore; Settings { category: \"recordings\" }", testCase)
        store.sync()
        const text = String(store.value("views", "{}"))
        store.destroy()
        return text
    }

    function nameOfRow(page, row) {
        return page.recording.signalList.find(s => s.index === row).name
    }

    // A plotted signal can be moved to a pane of its own from its row or its
    // label; the arrangement is kept with the recording, another recording
    // opens with the panes its units make, and By unit puts the signal back
    function test_aSignalMovedToANewPaneIsKeptWithItsRecording() {
        const page = openEarly()
        const restore = growWindow(page)
        try {
            const chart = panedChart(page)
            const row = chart.drawn.traces[0].row
            const name = nameOfRow(page, row)
            page.panesPart.openPaneMenu(row, chart)
            compare(page.paneMenuItems.length, 5, "by unit, three panes, a new one")
            compare(page.paneMenuItems[0].key, "")
            compare(page.paneMenuItems[0].text, "By unit")
            verify(page.paneMenuItems[0].checked, "by unit is in force")
            compare(page.paneMenuItems[4].key, "new")
            compare(page.paneMenuItems[4].text, "New pane")
            const mine = chart.drawn.panes[chart.drawn.traces[0].pane].key
            page.panesPart.choosePane("new")
            // The signal was alone in its pane: the pane it left goes
            tryVerify(() => chart.drawn.panes.some(p => p.key === "own:1"), 5000, "a pane of its own")
            compare(chart.drawn.panes.length, 3)
            verify(!chart.drawn.panes.some(p => p.key === mine), "its old pane is gone")
            compare(page.viewAssign[name], "own:1")
            tryVerify(() => storedViews().indexOf("own:1") >= 0, 3000, "kept in the settings")

            // Closed and opened again: the arrangement is back
            page.openPart.backToList()
            page.openPart.open(earlyRow)
            tryVerify(() => find(page, "signalRows").visible, 10000, "the recording opens")
            tryVerify(() => chart.drawn && chart.drawn.panes.some(p => p.key === "own:1"), 5000, "the pane again")
            compare(page.viewAssign[name], "own:1")

            // Another recording opens with the default panes
            page.openPart.backToList()
            page.openPart.open(lateRow)
            tryVerify(() => find(page, "signalRows").visible, 10000, "the other recording opens")
            // The first recording's chart also has three panes, one of them own:1:
            // only the other recording's chart has three and none of that key
            tryVerify(() => chart.drawn && chart.drawn.panes.length === 3
                      && !chart.drawn.panes.some(p => p.key === "own:1"), 5000, "the units' three panes")
            compare(Object.keys(page.viewAssign).length, 0)

            // By unit puts the signal back
            page.openPart.backToList()
            page.openPart.open(earlyRow)
            tryVerify(() => find(page, "signalRows").visible, 10000, "the first opens again")
            tryVerify(() => chart.drawn && chart.drawn.panes.some(p => p.key === "own:1"), 5000)
            page.panesPart.openPaneMenu(row, chart)
            page.panesPart.choosePane("")
            tryVerify(() => chart.drawn.panes.some(p => p.key === mine), 5000, "its unit's pane is back")
            verify(!chart.drawn.panes.some(p => p.key === "own:1"))
            compare(page.viewAssign[name], undefined)
        } finally {
            restore()
        }
    }

    // A signal joins another's pane by its key
    function test_aSignalCanJoinAnotherPane() {
        const page = openEarly()
        const restore = growWindow(page)
        try {
            const chart = panedChart(page)
            const first = chart.drawn.traces[0]
            page.panesPart.openPaneMenu(first.row, chart)
            const mine = chart.drawn.panes[first.pane].key
            const target = page.paneMenuItems.find(i => i.key !== "" && i.key !== "new" && i.key !== mine)
            verify(target !== undefined, "another pane is offered")
            verify(target.text.indexOf("With ") === 0)
            page.panesPart.choosePane(target.key)
            tryVerify(() => chart.drawn.panes.length === 2, 5000, "two panes")
        } finally {
            restore()
        }
    }

    // A damaged text, an entry with a bad share and a name the recording
    // lacks leave the panes the units make
    function test_aDamagedViewsTextOrAMissingNameLeavesTheDefaultPanes_data() {
        return [ { tag: "not json", text: "not json" },
                 { tag: "a bad share", text: JSON.stringify({ "pull-early": { used: 1, shares: { "3": [1, 2] } } }) },
                 { tag: "a name the recording lacks",
                   text: JSON.stringify({ "pull-early": { used: 1, assign: { "No such signal": "own:1" } } }) } ]
    }

    function test_aDamagedViewsTextOrAMissingNameLeavesTheDefaultPanes(data) {
        setStoredViews(data.text)
        const page = openEarly()
        const restore = growWindow(page)
        try {
            panedChart(page)
        } finally {
            restore()
        }
    }

    // An open of a recording with a kept arrangement stamps it used; a
    // recording with none writes nothing
    function test_openingARecordingStampsItsKeptViewOnly() {
        setStoredViews(JSON.stringify({ "pull-early": { used: 1, assign: { RPM: "own:1" } } }))
        const page = openEarly()
        verify(page.viewEntry !== undefined)
        tryVerify(() => page.viewEntry.used > 1000000000000, 3000, "stamped now: " + page.viewEntry.used)
        page.openPart.backToList()
        const before = storedViews()
        page.openPart.open(lateRow)
        tryVerify(() => find(page, "signalRows").visible, 10000, "the other opens")
        compare(storedViews(), before, "no entry, no write")
    }

    // A pull's end chosen with a bad number typed asks nothing and keeps the
    // end in force
    function test_aPullEndWithABadNumberKeepsTheEndInForce() {
        const page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        find(page, "jumpStretches").clicked()
        verify(waitForPolish(page.Window.window), "laid out")
        const lift = find(page, "pullEnd_lift")
        const top = find(page, "pullEnd_topSpeed")
        find(page, "rule_pullRise").text = "abc"
        top.clicked()
        verify(lift.selected && !top.selected, "the end in force stays selected")
        verify(find(page, "stretchesPanel").typedProblem.indexOf("must be a number") >= 0)
        verify(page.stretchList !== null, "nothing was asked")
        compare(page.stretchRules.pullEnd, undefined)
    }

    // A pull's end chosen while a level core refuses is typed keeps the end
    // in force, with the refusal's words showing
    function test_aPullEndWithALevelCoreRefusesKeepsTheEndInForce() {
        const page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        find(page, "jumpStretches").clicked()
        verify(waitForPolish(page.Window.window), "laid out")
        const lift = find(page, "pullEnd_lift")
        const top = find(page, "pullEnd_topSpeed")
        find(page, "rule_pullLevel").text = "150"
        top.clicked()
        verify(lift.selected && !top.selected, "the end in force stays selected")
        verify(page.rulesProblem !== "", "core's refusal is said")
        compare(page.stretchRules.pullEnd, undefined)
    }

    // The note says when panes are folded, and a value label is found by a point
    function test_foldedPanesAreSaidAndALabelIsFoundByItsPoint() {
        const page = openEarly()
        const note = find(page, "paneFoldNote")
        const chart = find(page, "chart")
        tryVerify(() => chart.drawn !== null, 5000, "drawn")
        if (chart.drawn.panes.length < 3) {
            verify(note.visible, "folded panes are said")
            verify(page.paneFoldNote.indexOf("folded") >= 0)
            verify(/^(1 pane is|[2-9] panes are) folded/.test(page.paneFoldNote), "one or many: " + page.paneFoldNote)
        }
        const restore = growWindow(page)
        try {
            panedChart(page)
            tryVerify(() => page.paneFoldNote === "", 5000, "nothing folded, nothing said")
            verify(!note.visible)
            const l = chart.drawn
            const tr = l.traces.find(t => t.labeled)
            verify(tr !== undefined, "a labeled trace")
            compare(page.panesPart.labelRowAt(l.labelX + 2, tr.labelY), tr.row)
            compare(page.panesPart.labelRowAt(l.left + 5, tr.labelY), -1, "the plot is no label")
        } finally {
            restore()
        }
    }

    // A comparison's difference traces are pinned, so no fold takes their pane
    function test_aDifferenceTraceIsPinned() {
        const page = swingCompared("pinned-differences")
        page.seriesPart.series(400)
        find(page, "compareDifference").toggled()
        differenceRead(page)
        const all = page.seriesPart.series(400)
        const diffs = all.filter(s => s.pane !== undefined)
        verify(diffs.length > 0, "a difference trace")
        verify(diffs.every(s => s.pinned === true))
        verify(all.filter(s => s.pane === undefined).every(s => s.pinned !== true))
    }

    // Every rule's refusal names the field the panel shows: one table gives
    // both the words and the field's name
    function test_eachRulesRefusalNamesItsField() {
        const page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        find(page, "jumpStretches").clicked()
        verify(waitForPolish(page.Window.window), "laid out")
        const panel = find(page, "stretchesPanel")
        const problem = find(page, "stretchesProblem")
        compare(panel.fields.length, 10)
        for (const f of panel.fields) {
            for (const other of panel.fields)
                find(page, "rule_" + other.key).text = ""
            const field = find(page, "rule_" + f.key)
            compare(field.Accessible.name, f.name, f.key + "'s field")
            verify(f.name !== undefined && f.name !== "", f.key + " has a name")
            field.text = "-1"
            find(page, "stretchesApply").clicked()
            verify(problem.visible, f.key + " refused")
            verify(problem.text.indexOf(f.name + " must be a number") === 0,
                   f.key + ": " + problem.text)
        }
    }

    // A stored level that is none today reads as the defaults, the words
    // shown, never a refused ask on open
    function test_aStoredLevelThatIsNoneTodayReadsAsTheDefaults() {
        setStoredRules("{\"pullLevel\": 500}")
        const page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        compare(page.comparePart.stretchesOf("pull").length, 2, "the defaults found them")
        verify(!page.stretchFailed, "not a failed read")
        compare(page.rulesProblem, fieldName(page, "pullLevel") + " must be a number from 0 to 100")
        find(page, "jumpStretches").clicked()
        verify(waitForPolish(page.Window.window), "laid out")
        compare(find(page, "stretchesProblem").text,
                fieldName(page, "pullLevel") + " must be a number from 0 to 100")
        setStoredRules("not json")
        compare(page.refreshPart.readRules("not json").pullLevel, undefined, "damaged text is no levels")
    }

    // The compared recording's pulls are counted by the same levels
    function test_theComparedRecordingTakesTheSameLevels() {
        const page = createPage()
        page.recordings = [twiceRow, earlyRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        find(page, "jumpStretches").clicked()
        find(page, "rule_pullRise").text = "5000"
        find(page, "stretchesApply").clicked()
        tryVerify(() => page.stretchList !== null, 10000, "the new answer")
        page.comparePart.compareWith(earlyRow)
        page.comparePart.setAlignOn("pull")
        tryVerify(() => page.pullsHere !== null && page.pullsThere !== null, 10000, "the pulls arrive")
        compare(page.pullsHere.length, 0, "none here under that rise")
        compare(page.pullsThere.length, 0, "and none there: one rule for both")
    }

    // The defaults a bad stored level fell back to are the compared
    // recording's too: its ask is never the refused one
    function test_aBadStoredLevelDoesNotRefuseTheComparedRecordingsAsk() {
        setStoredRules("{\"pullLevel\": 500}")
        const page = createPage()
        page.recordings = [twiceRow, earlyRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        page.comparePart.compareWith(earlyRow)
        page.comparePart.setAlignOn("pull")
        tryVerify(() => page.pullsHere !== null && page.pullsThere !== null, 10000, "the pulls arrive")
        compare(page.pullsHere.length, 2)
        compare(page.pullsThere.length, 1, "asked under the defaults")
    }

    // Each open starts the colors over: a recording's slots are the order it
    // is plotted in, never the history of the one before
    function test_aRecordingsColorsStartOverAtEachOpen() {
        const page = createPage()
        page.recordings = [twiceRow, earlyRow]
        openRow(page, twiceRow)
        for (const i of [0, 1, 2]) {
            if (page.plotted[i] === true)
                page.cursorsPart.togglePlotted(i)
        }
        page.cursorsPart.togglePlotted(2)
        page.cursorsPart.togglePlotted(0)
        page.cursorsPart.togglePlotted(1)
        compare(page.panesPart.slotOf(2), 0, "the first recording's order")
        page.openPart.backToList()
        page.recordings = [twiceRow, earlyRow]
        openRow(page, earlyRow)
        const plotted = [0, 1, 2].filter(i => page.plotted[i] === true)
        verify(plotted.length >= 2, "the second recording plots some numbers")
        for (let k = 0; k < plotted.length; ++k)
            compare(page.panesPart.slotOf(plotted[k]), k, "the lowest slots, in order")
    }

    // A refused Apply leaves the levels in force as they were, and the
    // compared recording is asked with those, not with the defaults
    function test_aRefusedApplyLeavesTheComparedRecordingsLevelsAlone() {
        const page = createPage()
        page.recordings = [twiceRow, earlyRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        page.comparePart.applyStretchRules({ pullRise: 5000 })
        tryVerify(() => page.stretchList !== null, 10000, "the new answer")
        page.comparePart.applyStretchRules({ pullLevel: 150 })
        verify(page.rulesProblem !== "", "the refusal is said")
        page.comparePart.compareWith(earlyRow)
        page.comparePart.setAlignOn("pull")
        tryVerify(() => page.pullsHere !== null && page.pullsThere !== null, 10000, "the pulls arrive")
        compare(page.pullsHere.length, 0, "here by the levels in force")
        compare(page.pullsThere.length, 0, "and there by the same")
    }

    // The levels are typed in display units: a change of units asks again,
    // here and there, reading the same numbers in the other unit
    function test_aChangeOfUnitsAsksTheStretchesAgain() {
        const page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        // 700 is a floor in km/h, and more than a mile an hour can be in mph
        page.comparePart.applyStretchRules({ cruiseFloor: 700 })
        tryVerify(() => page.stretchList !== null, 10000, "the new answer")
        compare(page.rulesProblem, "")
        compare(page.stretchUnits, 0)
        Bridge.unitSystem = 1
        tryVerify(() => page.stretchUnits === 1, 5000, "asked again in the other unit")
        tryVerify(() => page.stretchList !== null, 10000, "and answered")
        verify(page.rulesProblem.indexOf("621 mph") >= 0, "700 is no floor in mph: " + page.rulesProblem)
        compare(page.comparePart.stretchesOf("pull").length, 2, "the defaults found them")
        Bridge.unitSystem = 0
        tryVerify(() => page.stretchUnits === 0, 5000, "and back")
    }

    // A change of units remakes no cell: the fields keep the levels in
    // force, so the next Apply sends them with the one typed
    function test_aChangeOfUnitsKeepsTheStretchesFieldsFilled() {
        const page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        find(page, "jumpStretches").clicked()
        verify(waitForPolish(page.Window.window), "laid out")
        find(page, "rule_pullRise").text = "5000"
        find(page, "rule_cruiseFloor").text = "40"
        find(page, "stretchesApply").clicked()
        tryVerify(() => page.stretchList !== null, 10000, "the answer")
        try {
            Bridge.unitSystem = 1
            tryVerify(() => page.stretchUnits === 1, 5000, "asked again in the other unit")
            compare(find(page, "rule_pullRise").text, "5000", "the field keeps its level")
            compare(find(page, "rule_cruiseFloor").text, "40", "a speed field too")
            find(page, "rule_pullLevel").text = "50"
            find(page, "stretchesApply").clicked()
            compare(page.stretchRules.pullRise, 5000, "Apply sends the kept level with the typed one")
            compare(page.stretchRules.pullLevel, 50)
        } finally {
            Bridge.unitSystem = 0
        }
    }

    // A level is written and read in one locale: a comma-decimal one reads
    // back what it wrote, never 1.125 as 1125
    function test_theStretchesFieldsRoundTripInACommaLocale() {
        const page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        find(page, "jumpStretches").clicked()
        verify(waitForPolish(page.Window.window), "laid out")
        const panel = find(page, "stretchesPanel")
        panel.numberLocale = Qt.locale("de_DE")
        compare(panel.shown(1.125), "1,125")
        compare(panel.shown(750), "750")
        panel.rules = ({ pullHoldS: 1.125, pullRise: 5000 })
        compare(find(page, "rule_pullHoldS").text, "1,125", "written in the locale")
        compare(panel.typedRules().pullHoldS, 1.125, "and read back whole")
        compare(panel.typedRules().pullRise, 5000)
        panel.numberLocale = Qt.locale("C")
    }

    // A phone's panel stands in the chart's place, and Apply returns to it
    function test_aPhonesStretchesPanelTakesTheChartsPlaceUntilApplied() {
        const page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        Theme.compact = true
        try {
            page.width = 384
            page.height = 832
            find(page, "jumpStretches").clicked()
            verify(waitForPolish(page.Window.window), "laid out")
            verify(find(page, "stretchesPanel").visible, "the panel is up")
            verify(!find(page, "chartFrame").visible, "in the chart's place")
            find(page, "rule_pullRise").text = "5000"
            find(page, "stretchesApply").clicked()
            verify(!page.stretchesOpen, "Apply goes back")
            verify(waitForPolish(page.Window.window), "laid out")
            verify(find(page, "chartFrame").visible, "to the chart")
        } finally {
            Theme.compact = false
            page.width = 900
            page.height = 900
        }
    }

    // A kind with none is said so, and its buttons are off; a failed pass
    // is never "none found"
    function test_aKindWithNoStretchIsSaidSoAndAFailedPassIsNot() {
        const page = createPage()
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        verify(!find(page, "jumpPrevious_cruise").enabled)
        verify(!find(page, "jumpNext_cruise").enabled)
        verify(find(page, "jumpStatus").text.indexOf("No cruise found") >= 0)
        page.stretchFailed = true
        compare(find(page, "jumpStatus").text, "Its stretches couldn't be read")
        page.stretchFailed = false
        page.stretchList = null
        compare(find(page, "jumpStatus").text, "Looking for stretches…")
    }

    // The bands come and go with the switch, and a new open asks again
    function test_theBandsFollowTheSwitchAndAReopenAsksAgain() {
        const page = createPage()
        page.recordings = [twiceRow, earlyRow]
        openRow(page, twiceRow)
        tryVerify(() => page.stretchList !== null, 10000, "the stretches arrive")
        verify(page.bandsShown)
        find(page, "jumpBands").toggled()
        verify(!page.bandsShown, "switched off")
        find(page, "jumpBands").toggled()
        verify(page.bandsShown, "and on")
        page.openPart.backToList()
        verify(page.stretchList === null, "the list is the open's")
        page.recordings = [twiceRow, earlyRow]
        openRow(page, earlyRow)
        tryVerify(() => page.stretchList !== null, 10000, "the next recording's arrive")
        compare(page.comparePart.stretchesOf("pull").length, 1)
    }
}
