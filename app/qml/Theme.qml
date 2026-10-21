// File/layer: app/qml/Theme.qml | Qt6 Quick singleton, UI layer
// Testing: manual smoke — palette is visual only; no unit test reads it.
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

    // Fixed-pitch family, so hex columns and digits keep their width row
    // to row. "monospace" is a fontconfig and Android alias; Windows and
    // the Apple platforms don't resolve it.
    readonly property string monoFamily: Qt.platform.os === "windows" ? "Consolas"
                                       : Qt.platform.os === "osx" || Qt.platform.os === "ios"
                                         ? "Menlo"
                                         : "monospace"

    // Chart trace colours, one per hue family so traces stay distinct on
    // the dark surface. LiveView keys them by model row. No red: it is
    // the danger colour, so a trace drawn in it would read as an alarm.
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
        case "requests":        return warning
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
        case "disconnected":    return qsTr("No session")
        default:                return qsTr("Link degraded")
        }
    }
}
