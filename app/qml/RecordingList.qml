// File/layer: app/qml/RecordingList.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)
// The recordings a session saved, newest first, a row each with Open and
// Replay: one part of RecordingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ListView {
    id: root
    objectName: "recordingList"
    required property RecordingsPage host
    visible: !root.host.viewing
    Layout.fillWidth: true
    Layout.fillHeight: true
    clip: true
    spacing: 2
    currentIndex: -1
    highlightFollowsCurrentItem: false
    boundsBehavior: Flickable.StopAtBounds
    model: root.host.recordings
    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

    delegate: Rectangle {
        id: recordingRow
        required property var modelData
        width: root.width
        height: Math.max(56, rowLayout.implicitHeight + 2 * Theme.spacing)
        radius: Theme.radius
        color: rowArea.hovered ? Theme.elevatedLight : Theme.elevated

        // The row opens the recording: a button under the labels, so
        // UI Automation and a screen reader can press it
        AbstractButton {
            id: rowArea
            objectName: "recordingRowArea"
            anchors.fill: parent
            hoverEnabled: true
            Accessible.name: qsTr("Open %1").arg(recordingRow.modelData.title)
            onClicked: root.host.openPart.open(recordingRow.modelData)
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }

        RowLayout {
            id: rowLayout
            anchors.fill: parent
            anchors.margins: Theme.spacing
            spacing: Theme.spacing

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    objectName: "recordingTitle"
                    text: recordingRow.modelData.title
                    color: Theme.text
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    objectName: "recordingFacts"
                    visible: text !== ""
                    text: root.host.rowFacts(recordingRow.modelData)
                    color: Theme.textDim
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    objectName: "recordingDetail"
                    text: root.host.rowDetail(recordingRow.modelData)
                    color: Theme.textDim
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }
            }
            // The picked vehicle's baseline
            Label {
                objectName: "baselineBadge"
                visible: root.host.baselineFolder !== ""
                         && recordingRow.modelData.folder === root.host.baselineFolder
                text: qsTr("Baseline")
                color: Theme.text
                font.pixelSize: Theme.fontCaption
                leftPadding: 6
                rightPadding: 6
                topPadding: 2
                bottomPadding: 2
                background: Rectangle {
                    radius: Theme.radius
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.textDim
                }
                Accessible.name: qsTr("%1's baseline").arg(root.host.pickedVehicle !== null
                                                          ? root.host.pickedVehicle.label : "")
            }
            ThemedButton {
                objectName: "replayButton"
                visible: recordingRow.modelData.frames !== false
                // One tap connects; during a session it says what to do first
                text: root.host.replayText
                // Darker than its row, so it reads as a button there
                baseColor: Theme.surface
                enabled: root.host.sessionIdle
                Layout.maximumWidth: enabled ? 96 : 160
                Accessible.name: root.host.replayName(qsTr("Replay %1").arg(recordingRow.modelData.title))
                onClicked: root.host.replayRequested(recordingRow.modelData.uri)
            }
        }
    }
}
