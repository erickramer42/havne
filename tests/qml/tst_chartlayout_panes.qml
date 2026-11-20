// File/layer: tests/qml/tst_chartlayout_panes.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.chartlayout_panes (test_qml)

import QtQuick
import QtTest
import App

ChartLayoutKit {
    name: "ChartLayout"

    function paned(seriesList, height, panes, over) {
        // The ramps' samples are 10 s apart: one line, not three gaps
        const o = { panes: panes || {}, gapMs: 60000 }
        for (const k in over)
            o[k] = over[k]
        return ChartLayout.layout(seriesList, chartW, height, opts(o))
    }

    function inPane(l, row) {
        return l.traces.filter(t => t.row === row)[0].pane
    }

    // Without the panes option (Live) the chart is one pane with no axis,
    // the plot from the left edge, no seams
    function test_withoutPanesTheChartIsOnePaneWithNoAxis() {
        const l = layout([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)])
        compare(l.left, 0)
        compare(l.panes.length, 1)
        compare(l.panes[0].axis, false)
        compare(l.panes[0].label, "")
        compare(l.panes[0].ticks.length, 0)
        compare(l.seams.length, 0)
        compare(l.top, ChartLayout.top)
        compare(l.plotH, chartH - ChartLayout.axisH - ChartLayout.top - ChartLayout.bottomPad)
    }

    // Each trace goes to its unit's pane, the units of one quantity
    // sharing one; the panes stack in the order their first trace comes
    function test_tracesGoToTheirUnitsPaneInTheListsOrder() {
        const l = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100), ramp(2, "km/h", 0, 120),
                         ramp(3, "%", 10, 40), ramp(4, "mph", 0, 70)], 600)
        compare(l.panes.map(p => p.key).join("|"), "unit rpm|unit %|speed")
        compare(l.panes.map(p => p.label).join("|"), "rpm|%|km/h, mph")
        compare([0, 1, 2, 3, 4].map(r => inPane(l, r)).join(","), "0,1,2,1,2")
        for (let i = 1; i < l.panes.length; ++i) {
            near(l.panes[i].top, l.panes[i - 1].top + l.panes[i - 1].h + ChartLayout.paneGap, 1e-9,
                 "pane " + i + " under the one before")
            near(l.seams[i - 1], l.panes[i].top - ChartLayout.paneGap / 2, 1e-9, "seam " + i)
        }
        const last = l.panes[l.panes.length - 1]
        near(l.top + l.plotH, last.top + last.h, 1e-9, "the plot's height spans the panes")
        // Every point inside its own pane
        for (const tr of l.traces) {
            const p = l.panes[tr.pane]
            for (const piece of tr.pieces)
                for (const pt of piece.points)
                    verify(pt.y >= p.top - 1e-9 && pt.y <= p.top + p.h + 1e-9, "row " + tr.row + " in its pane")
        }
    }

    // A pane chosen for a row (a later per-row choice) wins over its unit
    function test_anAssignedPaneWinsOverTheUnit() {
        const l = paned([ramp(0, "rpm", 800, 6000), ramp(1, "km/h", 0, 120), ramp(2, "%", 0, 100)], 600,
                        { assign: { 0: "drive", 1: "drive" } })
        compare(l.panes.map(p => p.key).join("|"), "drive|unit %")
        compare(inPane(l, 1), 0)
    }

    // A series that names a pane of its own gets it, whatever its unit's
    // pane, with its own word in the pane's label (a comparison's
    // differences beside the signals they are of)
    function test_aSeriesOwnPaneWinsAndNamesItsLabel() {
        const own = (row, lo, hi) => Object.assign(ramp(row, "rpm", lo, hi),
                                                   { pane: "difference rpm", paneLabel: "D rpm" })
        const l = paned([ramp(0, "rpm", 800, 6000), own(1, -40, 25), ramp(2, "%", 0, 100), own(3, -5, 60)],
                        600, { assign: { 1: "drive" } })
        compare(l.panes.map(p => p.key).join("|"), "unit rpm|difference rpm|unit %")
        compare(l.panes.map(p => p.label).join("|"), "rpm|D rpm|%")
        compare([0, 1, 2, 3].map(r => inPane(l, r)).join(","), "0,1,2,1")
        // The pane's range is its own traces', not the signal's
        verify(l.panes[1].vMin <= -40 && l.panes[1].vMax >= 60 && l.panes[1].vMax < 800,
               "the difference's own range: " + l.panes[1].vMin + " to " + l.panes[1].vMax)
        // Folded past the limit, the mixed pane lists the word too
        const folded = paned([ramp(0, "rpm", 800, 6000), ramp(2, "%", 0, 100), own(1, -40, 25)],
                             600, { max: 2 })
        compare(folded.panes.length, 2)
        compare(folded.panes[1].label, "% \u00b7 D rpm")
    }

    // A pinned pane (a comparison's difference) is never folded into the
    // mixed pane while one ordinary pane stays: the ordinary panes give way,
    // then the pinned panes past the last but one share a pane of their own
    function pinnedRamp(row, unit, lo, hi) {
        return Object.assign(ramp(row, unit, lo, hi), { pane: "difference " + unit, paneLabel: "D " + unit,
                                                        pinned: true })
    }

    function test_pinnedPanesStayWhenTheOrdinaryOnesFold_data() {
        return [ { tag: "3 panes, two differences", max: 3, three: false, keys: "difference rpm|difference %|mixed" },
                 { tag: "4 panes", max: 4, three: false, keys: "unit rpm|difference rpm|difference %|mixed" },
                 { tag: "3 panes, three differences", max: 3, three: true, keys: "difference rpm|difference|mixed" } ]
    }

    function test_pinnedPanesStayWhenTheOrdinaryOnesFold(data) {
        const list = [ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100), ramp(2, "km/h", 0, 120),
                      pinnedRamp(3, "rpm", -40, 25), pinnedRamp(4, "%", -5, 60)]
        if (data.three)
            list.push(pinnedRamp(5, "km/h", -3, 3))
        const l = paned(list, 900, { max: data.max, mixedLabel: "mixed" })
        compare(l.panes.map(p => p.key).join("|"), data.keys)
        compare(l.panes.length, data.max)
        // The difference traces sit where they were put, never in "mixed"
        const mixed = l.panes.findIndex(p => p.key === "mixed")
        for (const r of [3, 4])
            verify(inPane(l, r) !== mixed, "row " + r + " is in a pane of its own")
        verify(l.traces.filter(t => t.pane === mixed).every(t => t.row < 3), "mixed holds the signals only")
    }

    // With no ordinary pane to keep, or no room for two, the old fold stands
    function test_aSingleOrdinaryPaneAndPinnedOnesNeedNoFold() {
        const l = paned([ramp(0, "rpm", 800, 6000), pinnedRamp(1, "rpm", -40, 25)], 600, { max: 3 })
        compare(l.panes.map(p => p.key).join("|"), "unit rpm|difference rpm")
    }

    // A difference ignores the assignment of its signal's row
    function test_aMovedSignalLeavesItsDifferenceWhereItIs() {
        const l = paned([ramp(0, "rpm", 800, 6000), pinnedRamp(0, "rpm", -40, 25), ramp(1, "%", 0, 100)],
                        600, { assign: { 0: "own:1" } })
        compare(l.panes.map(p => p.key).join("|"), "own:1|difference rpm|unit %")
    }

    // An assignment moves a signal to a pane of its own and the pane it left
    // goes when nothing is in it
    function test_aSignalMovedToANewPaneLeavesNoEmptyPaneBehind() {
        const list = [ramp(0, "rpm", 800, 6000), ramp(1, "km/h", 0, 120), ramp(2, "%", 0, 100)]
        const apart = paned(list, 600, { assign: { 1: "own:1" } })
        compare(apart.panes.map(p => p.key).join("|"), "unit rpm|own:1|unit %")
        const alone = paned(list, 600, { assign: { 0: "unit %" } })
        compare(alone.panes.map(p => p.key).join("|"), "unit %|speed")
        compare(inPane(alone, 0), 0)
        compare(inPane(alone, 2), 0)
    }

    // The views the viewer keeps for each recording
    function test_theViewsTextIsReadWithWhatIsDamagedLeftOut() {
        compare(JSON.stringify(ChartViews.viewsParse("")), "{}")
        compare(JSON.stringify(ChartViews.viewsParse("{ not json")), "{}")
        compare(JSON.stringify(ChartViews.viewsParse("[1, 2]")), "{}")
        compare(JSON.stringify(ChartViews.viewsParse(null)), "{}")
        const many = {}
        for (let i = 0; i < 65; ++i)
            many["s" + i] = "own:1"
        const good = { used: 5, assign: { "Engine speed": "own:1" }, shares: { "3": [1, 2, 1] } }
        const text = JSON.stringify({
            a: good,
            badAssign: { used: 1, assign: { x: 5 } },
            badShares: { used: 1, shares: { "3": [1, 2] } },
            zeroShare: { used: 1, shares: { "2": [1, 0] } },
            notAnObject: 7,
            tooMany: { used: 1, assign: many },
            fine: { used: 9 }
        })
        const got = ChartViews.viewsParse(text)
        compare(Object.keys(got).sort().join(","), "a,fine")
        compare(JSON.stringify(got.a), JSON.stringify(good))
        compare(JSON.stringify(got.fine.assign), "{}")
        // A text past the bound is none at all
        compare(JSON.stringify(ChartViews.viewsParse(" ".repeat(ChartViews.viewsMaxText + 1))), "{}")
    }

    function test_aRecordingsEntryIsWrittenStampedAndGoesWhenEmpty() {
        let v = ChartViews.viewsWith({}, "r1", { assign: { RPM: "own:1" } }, 100)
        compare(v.r1.used, 100)
        compare(v.r1.assign.RPM, "own:1")
        v = ChartViews.viewsWith(v, "r1", { shares: { "3": [1, 1, 1] } }, 200)
        compare(v.r1.used, 200)
        compare(v.r1.assign.RPM, "own:1", "a field left out stays")
        v = ChartViews.viewsTouched(v, "r1", 300)
        compare(v.r1.used, 300)
        compare(JSON.stringify(ChartViews.viewsTouched(v, "other", 400)), JSON.stringify(v), "no entry, no stamp")
        v = ChartViews.viewsWith(v, "r1", { assign: {} }, 500)
        verify(v.r1 !== undefined, "the heights still hold it")
        v = ChartViews.viewsWith(v, "r1", { shares: {} }, 600)
        compare(v.r1, undefined, "nothing left, no entry")
    }

    function test_theLeastRecentlyUsedViewsGoPastTheCap() {
        let v = {}
        for (let i = 0; i < ChartViews.viewsMax; ++i)
            v = ChartViews.viewsWith(v, "r" + i, { assign: { RPM: "own:1" } }, 1000 + i)
        compare(Object.keys(v).length, ChartViews.viewsMax)
        // r0 is the oldest: touched, it is the newest, and r1 goes first
        v = ChartViews.viewsTouched(v, "r0", 5000)
        v = ChartViews.viewsWith(v, "new", { assign: { RPM: "own:1" } }, 6000)
        compare(Object.keys(v).length, ChartViews.viewsMax)
        verify(v.r0 !== undefined && v.new !== undefined, "the used and the new stay")
        compare(v.r1, undefined, "the least recently used went")
    }

    // Past the limit, the last units fold into one mixed pane: no axis, its
    // traces each on its own scale as without panes, named at its axis
    function test_pastTheLimitTheLastUnitsFoldIntoAMixedPane_data() {
        return [ { tag: "4", max: 4, axes: 3 }, { tag: "3, a phone's page", max: 3, axes: 2 } ]
    }

    function test_pastTheLimitTheLastUnitsFoldIntoAMixedPane(data) {
        const list = [ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100), ramp(2, "km/h", 0, 120),
                      ramp(3, "°C", -10, 90), ramp(4, "V", 0, 50)]
        const l = paned(list, 800, { max: data.max, mixedLabel: "mixed" })
        compare(l.panes.length, data.max)
        for (let i = 0; i < data.axes; ++i)
            compare(l.panes[i].axis, true, "pane " + i)
        const mixed = l.panes[data.max - 1]
        compare(mixed.axis, false)
        compare(mixed.label, list.slice(data.axes).map(t => t.unit).join(" · "),
                "the folded units, joined")
        compare(mixed.ticks.length, 0)
        for (let r = data.axes; r < list.length; ++r)
            compare(inPane(l, r), data.max - 1, "row " + r + " folded")
        // Each folded trace fills the mixed pane on its own scale
        for (const tr of l.traces.filter(t => t.pane === data.max - 1)) {
            const ys = tr.pieces[0].points.map(p => p.y)
            near(Math.min.apply(null, ys), mixed.top, 1e-6, "row " + tr.row + "'s top")
            near(Math.max.apply(null, ys), mixed.top + mixed.h, 1e-6, "row " + tr.row + "'s foot")
        }
    }

    // Under paneMinH a pane the panes fold too, until they fit; one pane
    // left holds every trace mixed, or one unit with its axis
    function test_panesUnderTheirLeastHeightFold_data() {
        const room = n => n * ChartLayout.paneMinH + (n - 1) * ChartLayout.paneGap
                          + ChartLayout.top + ChartLayout.axisH + ChartLayout.bottomPad
        return [ { tag: "two fit", height: room(2), panes: 2, axes: 1 },
                 { tag: "one short of two", height: room(2) - 1, panes: 1, axes: 0 },
                 { tag: "three fit", height: room(3), panes: 3, axes: 3 } ]
    }

    function test_panesUnderTheirLeastHeightFold(data) {
        const l = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100), ramp(2, "km/h", 0, 120)],
                        data.height)
        compare(l.panes.length, data.panes)
        compare(l.panes.filter(p => p.axis).length, data.axes)
        for (const p of l.panes)
            verify(p.h >= ChartLayout.paneMinH - 1e-9, "a pane " + p.h + " px tall")
        const one = paned([ramp(0, "rpm", 800, 6000)], 100)
        compare(one.panes.length, 1)
        compare(one.panes[0].axis, true, "one unit keeps its axis in any height")
    }

    // A pane's range is its traces' lowest to highest, padded 5 % either
    // side, so traces of one unit share a scale and fill the pane
    function test_aPanesRangeIsItsTracesPadded() {
        const l = paned([ramp(0, "%", 10, 40), ramp(1, "%", 20, 90)], 400)
        const p = l.panes[0]
        near(p.vMin, 10 - 4, 1e-9, "low")
        near(p.vMax, 90 + 4, 1e-9, "high")
        compare(p.manual, false)
        const yOf = v => p.top + (p.vMax - v) / (p.vMax - p.vMin) * p.h
        const byRow = row => l.traces.filter(t => t.row === row)[0]
        near(byRow(0).endY, yOf(40), 1e-6, "40 % on the shared scale")
        near(byRow(1).endY, yOf(90), 1e-6, "90 % on the shared scale")
        // A flat one is widened about its value and drawn at it
        const flat = paned([series(0, [[0, 50], [20000, 50]], 1, "%")], 400).panes[0]
        near((flat.vMin + flat.vMax) / 2, 50, 1e-9, "centered")
        near(flat.vMax - flat.vMin, 20 * 0.1 * 1.1, 1e-9, "20 displayed digits, padded")
    }

    // A y-axis marks 2 to 4 round values: steps of 1, 2, 2.5 or 5 times a
    // power of ten, the finest that fits
    function test_theAxisTicksAreRound_data() {
        return [ { tag: "0-100", lo: 0, hi: 100, most: 4, values: "0,50,100", decimals: 0 },
                 { tag: "6-94", lo: 6, hi: 94, most: 3, values: "25,50,75", decimals: 0 },
                 { tag: "tenths", lo: 0.12, hi: 0.31, most: 4, values: "0.15,0.2,0.25,0.3", decimals: 2 },
                 { tag: "across zero", lo: -12, hi: 3, most: 2, values: "-10,0", decimals: 0 },
                 { tag: "a quarter step", lo: 0, hi: 1, most: 4, values: "0,0.25,0.5,0.75,1", decimals: 2,
                   most5: true } ]
    }

    function test_theAxisTicksAreRound(data) {
        const t = ChartLayout.valueTicks(data.lo, data.hi, data.most5 ? 5 : data.most)
        compare(t.values.join(","), data.values)
        compare(t.decimals, data.decimals)
    }

    // Each pane's ticks sit at their values' heights, captioned as the page
    // writes a value, the one too near the unit left uncaptioned; the
    // axes' column is the widest caption's, the plot to its right
    function test_theAxisColumnHoldsTheCaptions() {
        const l = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)], 400)
        let widest = 0
        for (const p of l.panes) {
            verify(p.ticks.length >= 2 && p.ticks.length <= 4, p.key + ": " + p.ticks.length + " ticks")
            widest = Math.max(widest, 7 * p.label.length)
            for (const tick of p.ticks) {
                near(tick.y, p.top + (p.vMax - tick.value) / (p.vMax - p.vMin) * p.h, 1e-9, "tick " + tick.value)
                compare(tick.caption, String(tick.value))
                compare(tick.captioned, tick.y - ChartLayout.labelPx / 2 >= p.top + ChartLayout.labelPx + 1)
                if (tick.captioned)
                    widest = Math.max(widest, 7 * tick.caption.length)
                verify(l.hGrid.indexOf(tick.y) >= 0, "a grid line at each tick")
            }
        }
        compare(l.left, widest + ChartLayout.axisGap)
        compare(l.labelX, l.left + l.plotW + 6)
        for (const tr of l.traces)
            verify(tr.pieces[0].points[0].x >= l.left, "the traces start at the plot")
    }

    // A range the user set is the pane's, and a trace past it is cut at the
    // pane's edges rather than drawn over its neighbor
    function test_aRangeTheUserSetCutsTheTraceToItsPane() {
        const l = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)], 400,
                        { ranges: { "unit %": { vMin: 20, vMax: 60 } } })
        const p = l.panes[1]
        compare(p.manual, true)
        compare(p.vMin, 20)
        compare(p.vMax, 60)
        compare(l.panes[0].manual, false)
        const tr = l.traces.filter(t => t.row === 1)[0]
        compare(tr.pieces.length, 1, "one run inside the pane")
        const pts = tr.pieces[0].points
        near(pts[0].y, p.top + p.h, 1e-6, "enters at the foot")
        near(pts[pts.length - 1].y, p.top, 1e-6, "leaves at the top")
        verify(tr.endY >= p.top && tr.endY <= p.top + p.h, "its label inside")
        // A range that isn't one is left out
        compare(paned([ramp(0, "%", 0, 100)], 400, { ranges: { "unit %": { vMin: 5, vMax: 5 } } })
                .panes[0].manual, false)
    }

    function test_aLineIsCutWhereItCrossesAnEdge() {
        const pts = [{ x: 0, y: 0 }, { x: 10, y: 100 }, { x: 20, y: 0 }, { x: 30, y: 50 }]
        const runs = ChartLayout.clipLine(pts, 25, 75)
        compare(runs.length, 3)
        compare(JSON.stringify(runs[0]), JSON.stringify([{ x: 2.5, y: 25 }, { x: 7.5, y: 75 }]))
        compare(JSON.stringify(runs[1]), JSON.stringify([{ x: 12.5, y: 75 }, { x: 17.5, y: 25 }]))
        compare(JSON.stringify(runs[2]), JSON.stringify([{ x: 25, y: 25 }, { x: 30, y: 50 }]))
        compare(ChartLayout.clipLine([{ x: 0, y: 10 }, { x: 5, y: 10 }], 25, 75).length, 0, "flat outside")
        compare(ChartLayout.clipLine([{ x: 0, y: 30 }, { x: 5, y: 40 }], 25, 75).length, 1, "inside whole")
        const band = ChartLayout.clipPieces([{ band: true, top: [{ x: 0, y: 0 }], bottom: [{ x: 0, y: 99 }] }],
                                            25, 75)[0]
        compare(band.top[0].y, 25)
        compare(band.bottom[0].y, 75)
    }

    // The seams' shares set the heights; none goes under paneMinH
    function test_theSharesSetTheHeights() {
        const two = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)], 400,
                          { shares: { 2: [3, 1] } })
        near(two.panes[0].h / two.panes[1].h, 3, 1e-9, "three to one")
        const fair = ChartLayout.paneHeights([1, 3], 2, 200, 72)
        compare(fair.join(","), "72,128")
        compare(ChartLayout.paneHeights(null, 3, 300, 72).join(","), "100,100,100")
        compare(ChartLayout.paneHeights([1, 1], 3, 300, 72).join(","), "100,100,100",
                "shares for another count are ignored")
        // Each count keeps its own shares
        const three = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100), ramp(2, "km/h", 0, 99)], 400,
                            { shares: { 2: [3, 1] } })
        near(three.panes[0].h, three.panes[2].h, 1e-9, "equal")
        const both = { 2: [3, 1], 3: [1, 1, 2] }
        const kept = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100), ramp(2, "km/h", 0, 99)], 400,
                           { shares: both })
        near(kept.panes[2].h / kept.panes[0].h, 2, 1e-9, "three's own shares")
        near(paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)], 400, { shares: both }).panes[0].h
             / 3, paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)], 400, { shares: both }).panes[1].h,
             1e-9, "two's own")
    }

    // A seam's drag trades its two neighbors' heights and no other's
    function test_aSeamTradesItsNeighborsHeights() {
        const s = ChartLayout.seamShares([100, 100, 100], 0, 20, 72)
        compare(s.map(v => Math.round(v * 300)).join(","), "120,80,100")
        const held = ChartLayout.seamShares([100, 100, 100], 1, 50, 72)
        compare(held.map(v => Math.round(v * 300)).join(","), "100,128,72", "the lower one keeps its least")
        const up = ChartLayout.seamShares([100, 100, 100], 0, -60, 72)
        compare(up.map(v => Math.round(v * 300)).join(","), "72,128,100", "the upper one keeps its least")
    }

    // The page's zoom and pan of a pane's range, by the pointer's value
    function test_aPanesRangeZoomsAboutThePointerAndPans() {
        const l = paned([ramp(0, "%", 0, 100), ramp(1, "rpm", 800, 6000)], 400)
        const p = l.panes[0]
        compare(ChartLayout.paneAt(l, p.top + 1), 0)
        compare(ChartLayout.paneAt(l, l.seams[0]), -1, "a seam is no pane")
        compare(ChartLayout.paneAt(l, l.panes[1].top + 1), 1)
        const y = p.top + p.h / 4
        const v = ChartLayout.valueAt(p, y)
        near(v, p.vMax - (p.vMax - p.vMin) / 4, 1e-9, "a quarter down")
        const zoomed = ChartLayout.scaledRange(p, y, 0.5)
        near(zoomed.vMax - zoomed.vMin, (p.vMax - p.vMin) / 2, 1e-9, "half the range")
        near(ChartLayout.valueAt({ top: p.top, h: p.h, vMin: zoomed.vMin, vMax: zoomed.vMax }, y), v, 1e-9,
             "the value under the pointer stays")
        const panned = ChartLayout.pannedRange({ vMin: 0, vMax: 100 }, 200, 50)
        compare(panned.vMin, 25)
        compare(panned.vMax, 125)
    }

    // The chart draws a paned layout: its panes are what it drew
    // The time axis's height is the page's to grow: the plot gives way
    function test_theAxisStripTakesItsHeightFromTheOption() {
        const s = [series(0, [[0, 0], [1000, 10]], 0, "rpm")]
        const plain = layout(s, { span: { t0: 0, t1: 1000 } })
        compare(plain.axisH, ChartLayout.axisH)
        const grown = layout(s, { span: { t0: 0, t1: 1000 }, axisH: ChartLayout.axisHCursor })
        compare(grown.axisH, ChartLayout.axisHCursor)
        compare(plain.plotH - grown.plotH, ChartLayout.axisHCursor - ChartLayout.axisH)
    }

    // A value's height on its trace: where the trace ends for its end
    // value, null outside a pane a user's range has narrowed, a flat
    // trace's lane for anything
    function test_aValuesHeightOnItsTrace() {
        const s = [series(0, [[0, 0], [500, 40], [1000, 100]], 0, "rpm")]
        const panes = { span: { t0: 0, t1: 1000 }, panes: { max: 4, minH: 72 } }
        const l = layout(s, panes)
        const tr = l.traces[0]
        fuzzyCompare(ChartLayout.traceYAt(l, tr, 100), tr.endY, 0.001)
        const p = l.panes[tr.pane]
        fuzzyCompare(ChartLayout.traceYAt(l, tr, (p.vMin + p.vMax) / 2), p.top + p.h / 2, 0.001)

        const narrow = layout(s, { span: { t0: 0, t1: 1000 },
                                   panes: { max: 4, minH: 72, ranges: { "unit rpm": { vMin: 20, vMax: 60 } } } })
        compare(ChartLayout.traceYAt(narrow, narrow.traces[0], 100), null, "over the range")
        compare(ChartLayout.traceYAt(narrow, narrow.traces[0], 0), null, "under the range")
        verify(ChartLayout.traceYAt(narrow, narrow.traces[0], 40) !== null, "inside it")

        const flat = layout([series(0, [[0, 5], [1000, 5]], 0, "")], { span: { t0: 0, t1: 1000 } })
        const f = flat.traces[0]
        verify(f.flat, "a flat trace")
        fuzzyCompare(ChartLayout.traceYAt(flat, f, 5), f.endY, 0.001)
    }

    // The extremes' dots: on the trace at their times, the highest filled;
    // out of view or past a user's range none; none on a compared trace
    function test_theExtremesDotsAreOnTheirTraces() {
        const s = [series(0, [[0, 0], [500, 40], [1000, 100]], 0, "rpm")]
        const l = layout(s, { span: { t0: 0, t1: 1000 }, panes: { max: 4, minH: 72 } })
        const stat = { 0: { lowestMs: 0, lowest: 0, highestMs: 1000, highest: 100 } }
        const dots = ChartLayout.extremeDots(l, stat, 0, 1000)
        compare(dots.length, 2)
        compare(dots[0].filled, false, "the lowest hollow")
        compare(dots[1].filled, true, "the highest filled")
        compare(dots[0].row, 0)
        fuzzyCompare(dots[0].x, l.left, 0.001)
        fuzzyCompare(dots[1].x, l.left + l.plotW - 1, 0.001)
        fuzzyCompare(dots[1].y, ChartLayout.traceYAt(l, l.traces[0], 100), 0.001)

        compare(ChartLayout.extremeDots(l, { 0: { lowestMs: -1, lowest: NaN, highestMs: -1, highest: NaN } },
                                        0, 1000).length, 0, "none with no samples")
        compare(ChartLayout.extremeDots(l, { 0: { lowestMs: 2000, lowest: 0, highestMs: 3000, highest: 100 } },
                                        0, 1000).length, 0, "none out of view")
        compare(ChartLayout.extremeDots(l, {}, 0, 1000).length, 0, "none with no statistics")

        const narrow = layout(s, { span: { t0: 0, t1: 1000 },
                                   panes: { max: 4, minH: 72, ranges: { "unit rpm": { vMin: 20, vMax: 60 } } } })
        compare(ChartLayout.extremeDots(narrow, stat, 0, 1000).length, 0, "none past the range")

        const dashed = [{ row: 0, samples: [0, 0, 1000, 100], decimals: 0, unit: "rpm", dash: [3, 3] }]
        const d = layout(dashed, { span: { t0: 0, t1: 1000 }, panes: { max: 4, minH: 72 } })
        compare(ChartLayout.extremeDots(d, stat, 0, 1000).length, 0, "none on a compared trace")
    }

    function test_aStripChartDrawsPanes() {
        const chart = createTemporaryObject(stripChart, this, {
            shownH: 400,
            fetch: w => [ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)],
            layoutOpts: opts({ panes: { mixedLabel: "mixed" } }) })
        tryVerify(() => chart.drawn !== null, 2000, "drawn")
        compare(chart.drawn.panes.length, 2)
        compare(chart.drawn.seams.length, 1)
        verify(chart.drawn.left > 0)
    }
}
