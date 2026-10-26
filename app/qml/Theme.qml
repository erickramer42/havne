// File/layer: app/qml/Theme.qml | Qt6 Quick singleton, UI layer
// Testing: test_qml (tests/qml/tst_theme.qml) pins linkColor and
//          linkLabel for every state and the trace palette's rules;
//          how the palette looks is manual smoke.
pragma Singleton
import QtQuick

// Central palette - dark mode
QtObject {
    readonly property color bg:             "#14141c"
    readonly property color surface:        "#1e1e2e"
    readonly property color elevated:       "#2a2a3e"
    readonly property color elevatedLight:  "#33334a"
    readonly property color outline:        "#64647e"   // control borders; 3:1 on bg
    readonly property color primary:        "#6d4aff"
    readonly property color text:           "#e8e8f0"
    readonly property color textDim:        "#8888a0"
    readonly property color success:        "#4ade80"
    readonly property color warning:        "#ffd166"
    readonly property color danger:         "#f87171"
    readonly property color info:           "#60a5fa"
    readonly property int radius:           8
    readonly property int spacing:          8
    readonly property int pageMargin:       16

    // A value this old is stale: Live dims its row and the chart breaks
    // its trace across the gap, and the Dashboard dims its gauge. 3 s is
    // generous against a full rotation of the requests, at most the
    // engine's 100 ms wait each, so about 1 s for the obd2 set.
    readonly property int staleMs:          3000

    // The whole UI's zoom, set with Ctrl + / Ctrl − / Ctrl 0 or from
    // Settings. Main.qml lays the UI out at the window's size divided by
    // it and scales the result, so every page reflows as if the window
    // were that size, and stores it between runs.
    property real zoom: 1.0
    readonly property var zoomSteps: [0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0]
    function clampZoom(z) {
        const v = Number(z)
        return isFinite(v) && v > 0 ? Math.max(zoomSteps[0], Math.min(zoomSteps[zoomSteps.length - 1], v))
                                    : 1.0
    }
    // To the next step up (direction > 0) or down from the current zoom
    function stepZoom(direction) {
        if (direction > 0) {
            for (const s of zoomSteps)
                if (s > zoom + 0.001) { zoom = s; return }
        } else {
            for (let i = zoomSteps.length - 1; i >= 0; --i)
                if (zoomSteps[i] < zoom - 0.001) { zoom = zoomSteps[i]; return }
        }
    }

    // Fixed-pitch family, so hex columns and digits keep their width row
    // to row. "monospace" is a fontconfig and Android alias; Windows and
    // the Apple platforms don't resolve it.
    readonly property string monoFamily: Qt.platform.os === "windows" ? "Consolas"
                                       : Qt.platform.os === "osx" || Qt.platform.os === "ios"
                                         ? "Menlo"
                                         : "monospace"

    // Chart trace colors, one per hue family so traces stay distinct on
    // the dark surface. LiveView keys them by model row. No red: it is
    // the danger color, so a trace drawn in it would read as an alarm.
    readonly property var seriesColors: [
        "#4aa3ff",   // blue
        "#31d08c",   // green
        "#5dd9d0",   // aqua
        "#f0e442",   // yellow
        "#e39ac9",   // reddish purple
        "#ffa94d",   // orange
        "#f2f2f2",   // near-white
        "#b08cff",   // violet
    ]

    // Link state → color. Whitelists the healthy and the explained
    // states; fault, degraded and any future state fall to danger.
    function linkColor(state: string): color {
        switch (state) {
        case "ok":              return success
        case "quiet":
        case "requests":
        case "stopped":         return warning
        case "disconnected":    return textDim
        default:                return danger
        }
    }

    // Link state → short status label (header pill, Home card). Unknown
    // states fall to "Link degraded", mirroring linkColor. LiveView's
    // banner keeps its own longer messages, which carry the episode count
    // and error code.
    function linkLabel(state: string): string {
        switch (state) {
        case "ok":              return qsTr("Session active")
        case "fault":           return qsTr("Adapter fault")
        case "requests":        return qsTr("Requests failing")
        case "quiet":           return qsTr("Link quiet")
        case "stopped":         return qsTr("Replay stopped")
        case "disconnected":    return qsTr("No session")
        default:                return qsTr("Link degraded")
        }
    }
}
