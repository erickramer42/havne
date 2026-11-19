// File/layer: app/qml/RecordingFormat.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick
import App

pragma ComponentBehavior: Bound

// Time, length, value and finding texts for the Recordings page, a part of RecordingsPage

Item {
    id: part
    required property RecordingsPage host
    visible: false

    function valueText(v, decimals, unit) {
        const n = Number(v).toLocaleString(Qt.locale(), 'f', decimals)
        return unit !== "" ? n + " " + unit : n
    }

    // 12.3 s, 1:02.3, 1:02:03; before the start (an aligned axis), -12.3 s
    function timeText(ms, tenths) {
        if (ms < 0)
            return "-" + part.timeText(-ms, tenths)
        // Tenths are rounded as a whole first: 119.96 s is 2:00.0, never 1:60.0
        const s = tenths ? Math.round(ms / 100) / 10 : ms / 1000
        if (s < 60)
            return tenths ? qsTr("%1 s").arg(s.toFixed(1)) : qsTr("%1 s").arg(Math.round(s))
        const whole = Math.floor(s)
        const sec = tenths ? (s % 60).toFixed(1).padStart(4, "0") : String(whole % 60).padStart(2, "0")
        if (s < 3600)
            return Math.floor(s / 60) + ":" + sec
        return Math.floor(s / 3600) + ":" + String(Math.floor(s / 60) % 60).padStart(2, "0") + ":" + sec
    }
    function captionOf(seconds) {
        return part.timeText(seconds * 1000, false)
    }
    function lengthText(ms) {
        const s = Math.round(ms / 1000)
        if (s < 60) return ms < 10000 ? qsTr("%1 s").arg((ms / 1000).toFixed(1)) : qsTr("%1 s").arg(s)
        if (s < 3600) return qsTr("%1 min %2 s").arg(Math.floor(s / 60)).arg(s % 60)
        return qsTr("%1 h %2 min").arg(Math.floor(s / 3600)).arg(Math.floor(s / 60) % 60)
    }

    function idText(f) {
        return "0x" + f.id.toString(16).toUpperCase().padStart(f.extended ? 8 : 3, "0")
    }
    // A finding in a few words (ECUIO_CHECK_* kinds)
    function findingText(f) {
        switch (f.kind) {
        case 1: return qsTr("No frames for %1").arg(part.lengthText(f.lengthMs))
        case 2: return qsTr("%1 slowed for %2").arg(part.idText(f)).arg(part.lengthText(f.lengthMs))
        case 3: return qsTr("%1 first heard late").arg(part.idText(f))
        case 4: return qsTr("Adapter overflow: frames lost")
        case 5: return qsTr("%1: a message broken off").arg(part.idText(f))
        case 6: return qsTr("Link: %1").arg(Bridge.statusString(f.code))
        case 7: return qsTr("Asleep for %1").arg(part.lengthText(f.lengthMs))
        default: return qsTr("Finding of kind %1").arg(f.kind)
        }
    }
    function findingColor(kind) {
        return kind === 7 ? Theme.textDim : kind === 1 || kind === 4 || kind === 5 ? Theme.danger : Theme.warning
    }
}
