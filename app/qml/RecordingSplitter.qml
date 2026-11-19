// File/layer: app/qml/RecordingSplitter.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The splitter below the chart, a part of RecordingsPage

Item {
    id: root
    required property RecordingsPage host
    objectName: "chartSplitter"
    visible: root.host.recording.state === RecordingView.Open && !root.host.chartMaximized
             && !root.host.exportShown
    Layout.fillWidth: true
    implicitHeight: 12
    Rectangle {
        anchors.centerIn: parent
        width: 40
        height: 4
        radius: 2
        color: chartDrag.active || chartSplitHover.hovered ? Theme.textDim : Theme.outline
    }
    HoverHandler { id: chartSplitHover; cursorShape: Qt.SizeVerCursor }
    DragHandler {
        id: chartDrag
        target: null
        margin: root.host.phoneShape ? Math.max(0, (Theme.touchTarget - parent.height) / 2) : 0
        xAxis.enabled: false
        property real startHeight: 0
        onActiveChanged: if (active) startHeight = root.host.chartFrame.height
        onActiveTranslationChanged: {
            if (active)
                root.host.viewerSettings.chartHeight = Math.max(200, startHeight + activeTranslation.y)
        }
    }
    DoubleTapHandler { onDoubleTap: root.host.viewerSettings.chartHeight = -1 }
}
