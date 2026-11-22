// File/layer: app/qml/RecordingRefresh.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)

import QtQuick
import App

pragma ComponentBehavior: Bound

// Refreshing the list, reading the stored rules and the view heads for the Recordings page, a part of RecordingsPage

Item {
    id: part
    required property RecordingsPage host
    visible: false

    function noteHead(role, head) {
        if (role === "values") part.host.valuesHead = head
        else if (role === "frames") part.host.framesHead = head
        else if (role === "ids") part.host.idsHead = head
        else part.host.aboutHead = head
    }
    function readRules(text) {
        try {
            const v = JSON.parse(text)
            if (v === null || typeof v !== "object" || Array.isArray(v))
                return ({})
            // A stored end that isn't top speed reads as the lift
            if (v.pullEnd !== undefined && v.pullEnd !== "top-speed")
                delete v.pullEnd
            return v
        } catch (e) {
            return ({})
        }
    }
    function refresh() {
        part.host.recordings = Bridge.recentRecordings(part.host.recordingsListed)
        part.host.openPart.checkBaseline()
    }

    // A Replay button's spoken name: its own, then the reason it is off
    function replayName(name) {
        return part.host.replayWhy !== "" ? qsTr("%1: %2").arg(name).arg(part.host.replayWhy) : name
    }

    function toggleMaximized() {
        if (Theme.shortLayout)
            part.host.maximizedShort = !part.host.maximizedShort
        else
            part.host.maximizedTall = !part.host.maximizedTall
    }
}
