pragma Singleton
import QtQuick

// Central palette - dark mode
QtObject {
    readonly property color bg:             "#14141c"
    readonly property color surface:        "#1e1e2e"
    readonly property color elevated:       "#2a2a3e"
    readonly property color elevatedLight:  "#33334a"
    readonly property color primary:        "#6d4aff"
    readonly property color text:           "#e8e8f0"
    readonly property color textDim:        "#8888a0"
    readonly property color success:        "#4ade80"
    readonly property color danger:         "#f87171"
    readonly property color warning:        "#ffd166"
    readonly property color info:           "#60a5fa"
    readonly property int radius:           8
    readonly property int spacing:          8
}
