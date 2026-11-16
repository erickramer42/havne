// File/layer: app/qml/RecordingZoomRow.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)
// Zoom, the cursor's time, the span line and the frame test: one part of
// RecordingsPage, which holds the state; on the pinned form it leads the shown
// view's list

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property RecordingsPage host
    property alias spanRowDesktop: spanRowDesktop
    property alias spanRowPhone: spanRowPhone
    property alias spanStatsPinned: spanStatsPinned
    property alias zoomSwitchSlot: zoomSwitchSlot
    parent: root.host.currentHead ? root.host.currentHead.zoomSlot : root.host.zoomHome
    Layout.fillWidth: true
    spacing: 10
    Label {
        objectName: "paneFoldNote"
        visible: root.host.paneFoldNote !== "" && root.host.recording.state === RecordingView.Open
        text: root.host.paneFoldNote
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
    }
    // Zoom, and where the cursor is
    // Compact on a phone: at their desktop widths the buttons, Frame
    // test among them, outgrow its width
    RowLayout {
        visible: root.host.recording.state === RecordingView.Open
        Layout.fillWidth: true
        spacing: 6
        // Maximized, the views' switch leads the row, so either view
        // leads back to the other
        RowLayout {
            id: zoomSwitchSlot
            visible: root.host.chartMaximized && root.host.recording.hasFrames && root.host.switchInZoomRow
        }
        ThemedButton {
            objectName: "zoomAll"
            compact: Theme.dense
            text: qsTr("All")
            Layout.maximumWidth: 72
            onClicked: root.host.showAll()
        }
        ThemedButton {
            objectName: "zoomOut"
            compact: Theme.dense
            text: "−"
            Layout.maximumWidth: 48
            Accessible.name: qsTr("Zoom out")
            onClicked: root.host.zoomAt((root.host.viewT0 + root.host.viewT1) / 2, 2)
        }
        ThemedButton {
            objectName: "zoomIn"
            compact: Theme.dense
            text: "+"
            Layout.maximumWidth: 48
            Accessible.name: qsTr("Zoom in")
            onClicked: root.host.zoomAt((root.host.viewT0 + root.host.viewT1) / 2, 0.5)
        }
        ThemedButton {
            objectName: "frameTest"
            compact: Theme.dense
            visible: Theme.developerMode
            enabled: !root.host.frameTesting
            text: qsTr("Frame test")
            Layout.maximumWidth: 120
            onClicked: root.host.startFrameTest()
        }
        // On a phone the hint wraps onto two caption lines where the
        // row leaves it little width, instead of eliding to "Tap for …"
        Label {
            objectName: "cursorTime"
            visible: root.host.phoneShape || !root.host.spanLineUp
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter
            wrapMode: Theme.compact ? Text.WordWrap : Text.NoWrap
            maximumLineCount: Theme.compact ? 2 : 1
            elide: Text.ElideRight
            font.pixelSize: Theme.compact ? Theme.fontCaption : Theme.fontBody
            color: Theme.textDim
            text: !root.host.hasCursor ? (Theme.compact ? qsTr("Tap for values")
                                                   : qsTr("Click for values · drag to pan · wheel to zoom · double-click a pane to enlarge it"))
                                  : qsTr("At %1").arg(root.host.timeText(root.host.cursorMs, true))
        }
        RowLayout {
            id: spanRowDesktop
            visible: !root.host.phoneShape && root.host.spanLineUp
            Layout.fillWidth: true
        }
        ThemedButton {
            objectName: "maximizeChart"
            text: root.host.chartMaximized ? qsTr("Signals") : qsTr("Chart")
            compact: Theme.dense
            Layout.minimumWidth: 52
            Layout.maximumWidth: 84
            Accessible.name: root.host.chartMaximized ? qsTr("Show the signals again")
                                                 : qsTr("Give the chart the page")
            onClicked: root.host.toggleMaximized()
        }
    }

    // The span line, under the zoom row on a phone
    RowLayout {
        id: spanRowPhone
        visible: root.host.phoneShape && root.host.spanLineUp
        Layout.fillWidth: true
    }
    ColumnLayout {
        id: spanStatsPinned
        visible: root.host.spanStatsBlock.parent === spanStatsPinned && root.host.spanStatsBlock.shown
        Layout.fillWidth: true
        Layout.fillHeight: false
    }

    Label {
        objectName: "frameTestResult"
        visible: Theme.developerMode && root.host.frameTestResult !== "" && !root.host.chartMaximized
                 && root.host.recording.state === RecordingView.Open
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        text: root.host.frameTestResult
    }
}
