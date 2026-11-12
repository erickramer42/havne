// File/layer: tests/qml/tst_chartfonts.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.chartfonts (test_qml)

import QtQuick
import QtTest
import App

TestCase {
    name: "ChartFonts"
    width: 400
    height: 300
    visible: true
    when: windowShown

    // Two panes' worth of units; short units, so the axis column is led by
    // the captions' digits and not by a unit's width
    function seriesFor(widthPx) {
        const a = [], b = []
        for (let i = 0; i < 40; ++i) {
            a.push(i * 500, 1110 + 100 * Math.abs(Math.sin(i / 5)))
            b.push(i * 500, -20 + 120 * Math.abs(Math.cos(i / 7)))
        }
        return [{ row: 0, samples: a, decimals: 1, unit: "V" },
                { row: 1, samples: b, decimals: 1, unit: "A" }]
    }

    Component {
        id: chartMaker
        StripChart {
            fetch: seriesFor
            layoutOpts: ({ gapMs: 3000, minSpanUnits: 20,
                           panes: { max: 3, minH: ChartLayout.paneMinH },
                           format: (v, d, u) => Number(v).toFixed(d) + (u !== "" ? " " + u : "") })
        }
    }

    // The same, with a unit that is free text
    function longSeries(widthPx) {
        return seriesFor(widthPx).map(s => Object.assign({}, s, { unit: "degrees BTDC" }))
    }
    Component {
        id: longMaker
        StripChart {
            fetch: longSeries
            layoutOpts: ({ gapMs: 3000, minSpanUnits: 20,
                           panes: { max: 3, minH: ChartLayout.paneMinH },
                           format: (v, d, u) => Number(v).toFixed(d) + (u !== "" ? " " + u : "") })
        }
    }

    function chartAt(w, h) {
        const c = createTemporaryObject(chartMaker, this, { shownW: w, shownH: h })
        verify(c)
        tryVerify(() => c.drawn !== null, 2000, "drawn")
        return c
    }

    // The chart's own font, with its digit cell set as draw() sets it
    function fontedContext(chart) {
        const ctx = chart.getContext("2d")
        ctx.font = ChartLayout.labelPx + "px " + (Theme.fontFamily || "sans-serif")
        chart.setDigitW(ctx)
        return ctx
    }

    // Measured as the chart does, in the face it draws
    function widthOf(chart, text) {
        return chart.tabularWidth(fontedContext(chart), text)
    }

    function near(actual, expected, tolerance, what) {
        verify(Math.abs(actual - expected) <= tolerance, what + ": " + actual + ", expected " + expected)
    }

    function test_theFaceIsTheTextFaceNotMono() {
        compare(typeof Theme.fontFamily, "string")
        verify(Theme.fontFamily !== Theme.monoFamily)
    }

    function test_digitsKeepOneWidthWhateverTheValue() {
        const c = chartAt(360, 220)
        const w = widthOf(c, "0000.0")
        for (const t of ["1111.1", "8888.8", "5050.5", "9179.4"])
            compare(widthOf(c, t), w, t)
    }

    // A non-digit run is measured whole, so kerning and combining marks stay
    function test_textAroundDigitsIsMeasuredWhole() {
        const c = chartAt(360, 220)
        const ctx = fontedContext(c)
        for (const t of ["Coolant temp 12", "AVAWA 7", "é 5"]) {
            const run = t.replace(/[0-9]+/g, "")
            const digits = t.length - run.length
            near(c.tabularWidth(ctx, t), ctx.measureText(run).width + digits * c.digitW, 0.001, t)
        }
        // the case is worth something only where whole differs from the sum
        let apart = 0
        for (const ch of "AVAWA")
            apart += ctx.measureText(ch).width
        verify(Math.abs(apart - ctx.measureText("AVAWA").width) > 0.01
               || Math.abs(ctx.measureText("é").width - ctx.measureText("e").width
                           - ctx.measureText("́").width) > 0.01,
               "the face kerns or composes in these labels")
    }

    // An imported column's unit is free text: a pane's name and a value's
    // label are cut to their columns, never drawn past them
    function test_aLongUnitIsCutToItsColumn() {
        for (const w of [200, 320, 384, 600]) {
            const c = createTemporaryObject(longMaker, this, { shownW: w, shownH: 320 })
            tryVerify(() => c.drawn !== null && c.drawn.traces.length === 2, 2000, "drawn at " + w)
            const l = c.drawn
            for (const tr of l.traces) {
                if (!tr.labeled) continue
                const right = l.labelX + ChartLayout.swatchW + 4 + widthOf(c, tr.label)
                verify(right <= w + 0.5, "label '" + tr.label + "' ends at " + right + " of " + w)
            }
            for (const pane of l.panes) {
                if (pane.label !== "")
                    verify(widthOf(c, pane.label) <= l.left + 0.5,
                           "pane label '" + pane.label + "' is " + widthOf(c, pane.label) + " of " + l.left + " at " + w)
            }
        }
    }

    function test_labelsFitEveryWidth() {
        for (const w of [200, 280, 320, 384, 600]) {
            const c = chartAt(w, 320)
            const l = c.drawn
            verify(l !== null, "layout at " + w)
            verify(l.left > 0 && l.panes.length === 2, "the axes are drawn at " + w)
            for (const tr of l.traces) {
                if (!tr.labeled) continue
                const right = l.labelX + ChartLayout.swatchW + 4 + widthOf(c, tr.label)
                verify(right <= w + 0.5, "label '" + tr.label + "' ends at " + right + " of " + w)
            }
            for (const pane of l.panes) {
                if (pane.label !== "")
                    verify(widthOf(c, pane.label) <= l.left, "pane label '" + pane.label + "' clipped at " + w)
                for (const tick of pane.ticks)
                    if (tick.captioned)
                        verify(widthOf(c, tick.caption) + ChartLayout.axisGap <= l.left,
                               "axis caption '" + tick.caption + "' clipped at " + w)
            }
            const shown = l.ticks.filter(t => t.captioned)
            for (let i = 1; i < shown.length; ++i) {
                // neighbors in time order never overlap
                const a = shown[i - 1], b = shown[i]
                const left = a.x < b.x ? a : b, right = a.x < b.x ? b : a
                const lEnd = left.x + (left.align === "right" ? 0 : widthOf(c, left.caption) / 2)
                const rStart = right.x - (right.align === "right" ? widthOf(c, right.caption)
                                                                  : widthOf(c, right.caption) / 2)
                verify(lEnd <= rStart + 0.5, "time captions '" + left.caption + "' and '"
                       + right.caption + "' overlap at " + w)
            }
        }
    }
}
