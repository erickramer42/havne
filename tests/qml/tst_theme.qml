// File/layer: tests/qml/tst_theme.qml | Qt Quick Test, QML tier
// Testing: Theme's link-state color and label for every state the
//          bridge publishes and an unknown one, and the chart trace
//          palette. No session.

import QtQuick
import QtTest
import QtTune

TestCase {
    name: "Theme"

    function sameColor(actual, expected, what) {
        verify(Qt.colorEqual(actual, expected), what + ": " + actual + ", expected " + expected)
    }

    function test_linkColor_data() {
        return [
            { tag: "ok",           state: "ok",           color: Theme.success },
            { tag: "quiet",        state: "quiet",        color: Theme.warning },
            { tag: "requests",     state: "requests",     color: Theme.warning },
            { tag: "stopped",      state: "stopped",      color: Theme.warning },
            { tag: "disconnected", state: "disconnected", color: Theme.textDim },
            { tag: "fault",        state: "fault",        color: Theme.danger },
            { tag: "degraded",     state: "degraded",     color: Theme.danger },
            { tag: "unknown",      state: "a later state", color: Theme.danger },
        ]
    }
    function test_linkColor(data) {
        sameColor(Theme.linkColor(data.state), data.color, data.state)
    }

    function test_linkLabel_data() {
        return [
            { tag: "ok",           state: "ok",           label: "Session active" },
            { tag: "quiet",        state: "quiet",        label: "Link quiet" },
            { tag: "requests",     state: "requests",     label: "Requests failing" },
            { tag: "stopped",      state: "stopped",      label: "Replay stopped" },
            { tag: "disconnected", state: "disconnected", label: "No session" },
            { tag: "fault",        state: "fault",        label: "Adapter fault" },
            { tag: "degraded",     state: "degraded",     label: "Link degraded" },
            { tag: "unknown",      state: "a later state", label: "Link degraded" },
        ]
    }
    function test_linkLabel(data) {
        compare(Theme.linkLabel(data.state), data.label)
    }

    function test_seriesColorsAreDistinct() {
        const colors = Theme.seriesColors
        verify(colors.length > 1)
        for (let i = 0; i < colors.length; ++i) {
            for (let j = i + 1; j < colors.length; ++j)
                verify(!Qt.colorEqual(colors[i], colors[j]),
                       "series colors " + i + " and " + j + " are both " + colors[i])
        }
    }

    // A trace in the danger color would read as an alarm, so no series
    // color is a saturated hue within 20° of it
    function test_seriesColorsLeaveOutTheDangerHue() {
        const danger = Qt.color(Theme.danger).hsvHue
        for (const c of Theme.seriesColors) {
            const color = Qt.color(c)
            if (color.hsvSaturation < 0.2) continue   // grays and near-white
            const d = Math.abs(color.hsvHue - danger)
            const degrees = Math.min(d, 1 - d) * 360
            verify(degrees > 20, c + " is " + degrees.toFixed(0) + "° from the danger color")
        }
    }
}
