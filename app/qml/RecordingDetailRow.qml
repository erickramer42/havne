// File/layer: app/qml/RecordingDetailRow.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)

import QtQuick
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The signal area's switch (Values, Frames, About) and the signal filter, a part of RecordingsPage

GridLayout {
    id: root
    required property RecordingsPage host
    objectName: "detailRow"
    visible: root.host.recording.state === RecordingView.Open && !root.host.chartMaximized && !root.host.pinnedViewer
    Layout.fillWidth: true
    columns: root.host.phoneShape ? 1 : 2
    columnSpacing: Theme.spacing
    rowSpacing: 10
    RowLayout {
        id: detailSwitchSlot
        visible: root.host.recording.hasFrames
        // Maximized, it leads the zoom row where the row has room
        SegmentRow {
            parent: root.host.currentHead ? root.host.currentHead.switchSlot
                  : !root.host.chartMaximized ? detailSwitchSlot
                  : root.host.switchInZoomRow ? root.host.zoomSwitchSlot : root.host.switchRowSlot

            SegmentButton {
                tapHeight: root.host.segmentHeight
                objectName: "detailView_values"
                text: qsTr("Values")
                selected: !root.host.framesShown && !root.host.aboutShown
                onClicked: root.host.detailView = "values"
            }
            SegmentButton {
                tapHeight: root.host.segmentHeight
                objectName: "detailView_frames"
                visible: root.host.recording.hasFrames
                text: qsTr("Frames")
                selected: root.host.framesShown
                onClicked: root.host.detailView = "frames"
            }
            // On the pinned form About is a view of its own
            SegmentButton {
                tapHeight: root.host.segmentHeight
                objectName: "detailView_about"
                visible: root.host.pinnedViewer
                text: qsTr("About")
                selected: root.host.aboutShown
                onClicked: root.host.detailView = "about"
            }
        }
    }
    SignalFilterBar {
        objectName: "recordingFilter"
        parent: root.host.currentHead && root.host.currentHead === root.host.valuesHead
                ? root.host.valuesHead.toolsSlot : root
        visible: !root.host.framesShown
        Layout.fillWidth: true
        filter: root.host.signalFilter
        codes: root.host.recording.signalList.map(s => s.group)
    }
}
