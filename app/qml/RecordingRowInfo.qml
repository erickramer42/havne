// File/layer: app/qml/RecordingRowInfo.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)

import QtQuick
import App

pragma ComponentBehavior: Bound

// Row facts for the Recordings page, a part of RecordingsPage

Item {
    id: part
    required property RecordingsPage host
    visible: false

    // A row's length, whether it is a log of chosen signals or a whole
    // session, its signal count and its marks
    function rowFacts(row) {
        const parts = []
        if (row.durationMs >= 0)
            parts.push(part.host.formatPart.lengthText(row.durationMs))
        if (row.log)
            parts.push(row.signals === 1 ? qsTr("log, 1 signal") : qsTr("log, %1 signals").arg(row.signals))
        else if (row.frames !== false)
            parts.push(qsTr("session"))
        else if (row.signals > 0)
            parts.push(row.signals === 1 ? qsTr("1 signal") : qsTr("%1 signals").arg(row.signals))
        if (row.marks > 0)
            parts.push(row.marks === 1 ? qsTr("1 mark") : qsTr("%1 marks").arg(row.marks))
        return parts.join(" · ")
    }

    // A row's second line: the address it was recorded from and the
    // profile it replays under, the profile only where the address
    // doesn't name it already. A row without frames (a CSV import's
    // values) has nothing to replay; a row that doesn't say has
    function rowDetail(row) {
        if (row.frames === false)
            return qsTr("values alone: the recording has no frames to replay")
        if (row.source === "")
            return qsTr("replays as %1").arg(row.profile)
        // The address's own profile, or its scheme's default (mock:// is mock)
        const m = /[?&]profile=([^&]*)/.exec(row.source)
        const own = m !== null ? decodeURIComponent(m[1])
                  : row.source.toLowerCase().startsWith("mock://") ? "mock" : ""
        if (own === row.profile)
            return row.source
        return qsTr("%1 · replays as %2").arg(row.source).arg(row.profile)
    }
}
