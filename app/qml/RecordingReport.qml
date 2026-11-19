// File/layer: app/qml/RecordingReport.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick
import App

pragma ComponentBehavior: Bound

// Export and the saved report: opening and closing the panel, saving, the plotted names, a part of RecordingsPage

Item {
    id: part
    required property RecordingsPage host
    visible: false

    function draws(s) {
        if (s.samples > 0)
            return true
        if (!part.host.compareAligned)
            return false
        const theirs = part.host.compare.signalList[ComparePairs.pairOf(part.host.comparePairs, s.index)]
        return theirs !== undefined && !theirs.isText && theirs.samples > 0
    }
    function noValuesText(names) {
        const list = names.length <= 2 ? names.join(qsTr(" and "))
                   : names.slice(0, -1).join(", ") + qsTr(" and ") + names[names.length - 1]
        return names.length === 1 ? qsTr("%1 has no values in this recording").arg(list)
                                  : qsTr("%1 have no values in this recording").arg(list)
    }
    // The panel opens under the title row: the page goes to its top first
    function openExport() {
        part.host.exportOpen = true
        part.host.exportPanel.begin()
        part.host.scroller.contentY = 0
    }
    function closeExport() {
        part.host.exportPanel.end()
        part.host.exportOpen = false
    }

    // The whole recording's health report to url, its format by the
    // dialog's filter or the file's name (ADR-0054's appendix "the health
    // report as built"), naming the picked vehicle's baseline; the view's
    // zoom never narrows it. The result comes back in onReportSaved; false
    // when none started
    function saveReportTo(url, filterIndex) {
        const started = part.host.recording.saveHealthReport(url, filterIndex, part.host.baselineFolder)
        if (started) {
            part.host.reportFailed = false
            part.host.reportNote = qsTr("Saving the report…")
        } else if (!part.host.recording.reporting) {
            // Not a report already under way: the note says this one didn't start
            part.host.reportFailed = true
            part.host.reportNote = qsTr("Report not saved: the report couldn't start")
        }
        return started
    }

    function reportSaved(r) {
        Bridge.logEvent(r.line)
        part.host.reportFailed = !r.ok
        part.host.reportNote = r.ok ? qsTr("Report saved: %1").arg(r.summary)
                          : qsTr("Report not saved: %1").arg(r.words)
    }
}
