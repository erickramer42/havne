// File/layer: tests/qml/ChartLayoutKit.qml | Qt Quick Test, QML tier
// Testing: QML: the base of the tst_chartlayout*.qml files (test_qml); not a test itself

import QtQuick
import QtTest
import App

// The shared setup and helpers of tst_chartlayout*.qml: a file derives from it and sets its `name`;
// `init` and `cleanup` are the kit's.
TestCase {
    property alias stripChart: stripChart

    readonly property int chartW: 400

    readonly property int chartH: 200

    // Seven pixels a character, and a plain fixed-point value
    function opts(over) {
        const o = { gapMs: 3000, minSpanUnits: 20,
                    measure: text => 7 * text.length,
                    format: (v, decimals, unit) => Number(v).toFixed(decimals)
                                                   + (unit !== "" ? " " + unit : "") }
        for (const k in over)
            o[k] = over[k]
        return o
    }

    // A series of (t, v) pairs (no Array.flat in the QML engine)
    function series(row, pairs, decimals, unit) {
        return { row: row, samples: [].concat.apply([], pairs), decimals: decimals || 0, unit: unit || "" }
    }

    function layout(seriesList, over) {
        return ChartLayout.layout(seriesList, chartW, chartH, opts(over))
    }

    function near(actual, expected, tolerance, what) {
        verify(Math.abs(actual - expected) <= tolerance,
               what + ": " + actual + ", expected " + expected + " ± " + tolerance)
    }

    // A StripChart draws a band on its Shape: the trace's fill, faint in
    // its row's color, and its two edges, beside its lines
    Component {
        id: stripChart
        StripChart { shownW: 400; shownH: 200 }
    }

    // Panes (the recordings viewer's): a ramp of one unit from lo to hi
    function ramp(row, unit, lo, hi) {
        return series(row, [[0, lo], [10000, (lo + hi) / 2], [20000, hi]], 0, unit)
    }
}
