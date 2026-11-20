// File/layer: app/qml/RecordingDecode.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)

import QtQuick
import App

pragma ComponentBehavior: Bound

// Decode choices and the info and check texts for the Recordings page, a part of RecordingsPage

Item {
    id: part
    required property RecordingsPage host
    visible: false

    // Where the values can come from (ADR-0049's appendix of 2026-11-06):
    // as recorded (signals.csv), or the frames decoded through the
    // recording's own signal file or a profile the app has loaded
    function decodeChoices() {
        const out = [{ text: qsTr("Values as recorded"), value: "" }]
        if (part.host.recording.ownSignalFile)
            out.push({ text: qsTr("Its frames through its own signal file"), value: "signals.json" })
        for (const p of Bridge.decodeProfiles())
            out.push({ text: qsTr("Its frames through %1").arg(p.name !== "" ? p.name : p.profile),
                       value: p.profile })
        return out
    }
    function decodeWith(value) {
        if (value === part.host.recording.decodedWith && part.host.recording.state === RecordingView.Open)
            return
        part.host.decodeReopen = true
        if (value === "")
            part.host.recording.open(part.host.recording.folder)
        else
            part.host.recording.openFrames(part.host.recording.folder, value)
    }

    function infoText() {
        const info = part.host.recording.info
        let text = qsTr("%1 long").arg(part.host.formatPart.lengthText(part.host.recording.durationMs))
        if (part.host.recording.decodedWith === "signals.json")
            text += qsTr(" · values decoded from its frames through its own signal file")
        else if (part.host.recording.decodedWith !== "")
            text += qsTr(" · values decoded from its frames through %1").arg(part.host.recording.decodedWith)
        else if (part.host.recording.signalList.every(s => s.samples === 0))
            text += qsTr(" · no values were recorded: decode its frames with a profile")
        if (info.utcStartMs > 0)
            text += qsTr(" · started %1").arg(new Date(info.utcStartMs).toLocaleString(Qt.locale(), Locale.ShortFormat))
        if (info.recordsDropped > 0)
            text += qsTr(" · %1 records dropped while recording").arg(info.recordsDropped)
        if (info.noSummary)
            text += qsTr(" · it ended without its summary (the app or the power stopped)")
        return text
    }
    function checkText() {
        const check = part.host.recording.check
        // A folder of values alone (a CSV import): nothing to check
        if (!part.host.recording.hasFrames)
            return qsTr("Frames check: the recording has no frames")
        if (!check.ran)
            return qsTr("Frames check: didn't run (%1)").arg(check.error)
        const n = part.host.recording.findings.length
        return n === 0 ? qsTr("Frames check: nothing found in %1 frames").arg(check.frames)
                       : n === 1 ? qsTr("Frames check: 1 finding")
                                 : qsTr("Frames check: %1 findings").arg(n)
    }
}
