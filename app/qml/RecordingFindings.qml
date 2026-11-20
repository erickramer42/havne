// File/layer: app/qml/RecordingFindings.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)
// The frames check's findings, a row each, a tap centering the view on one:
// one part of RecordingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ListView {
    id: root
    objectName: "findingList"
    required property RecordingsPage host
    visible: root.host.recording.state === RecordingView.Open && !root.host.chartMaximized
             && (root.host.pinnedViewer ? root.host.aboutShown : count > 0)
    Layout.fillWidth: true
    Layout.preferredHeight: root.host.pinnedViewer ? 0 : Math.min(count, 3) * Math.max(32, root.host.touchMin)
    Layout.minimumHeight: root.host.pinnedViewer ? root.host.pinnedListMin : 0
    Layout.fillHeight: root.host.pinnedViewer
    clip: true
    currentIndex: -1
    highlightFollowsCurrentItem: false
    boundsBehavior: Flickable.StopAtBounds
    model: root.host.recording.findings
    header: Component {
        ViewHead {
            host: root.host
            role: "about"
            width: root.width
            list: root
        }
    }
    ScrollBar.vertical: ScrollBar {
        policy: root.host.pinnedViewer && root.contentHeight > root.height
                ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
    }

    delegate: Rectangle {
        id: findingRow
        required property var modelData
        width: root.width
        height: Math.max(32, root.host.touchMin)
        color: findingArea.hovered ? Theme.elevatedLight : "transparent"
        AbstractButton {
            id: findingArea
            objectName: "findingRowArea"
            anchors.fill: parent
            hoverEnabled: true
            Accessible.name: qsTr("Show %1").arg(root.host.formatPart.findingText(findingRow.modelData))
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            onClicked: {
                const f = findingRow.modelData
                root.host.cursorsPart.centerOn(f.atMs + f.lengthMs / 2)
                root.host.cursorsPart.setCursor(f.atMs)
            }
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            Rectangle {
                implicitWidth: 10; implicitHeight: 4
                color: root.host.formatPart.findingColor(findingRow.modelData.kind)
            }
            Label {
                text: root.host.formatPart.timeText(findingRow.modelData.atMs, true)
                color: Theme.textDim
                font.family: Theme.monoFamily
            }
            Label {
                objectName: "findingText"
                text: root.host.formatPart.findingText(findingRow.modelData)
                color: Theme.text
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }
    }
}
