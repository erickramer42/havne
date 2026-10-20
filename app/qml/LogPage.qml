import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    padding: 12
    background: Rectangle { color: Theme.bg }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        // ---- Toolbar: stats + actions ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Label {
                text: QtTune.connected
                      ? qsTr("Logging — %1 frames").arg(QtTune.frameCount)
                      : qsTr("Idle")
                color: QtTune.connected ? Theme.success : Theme.textDim
                font.pixelSize: 13
            }

            Label {
                text: qsTr("%1 dropped").arg(QtTune.droppedFrames)
                color: Theme.danger
                font.pixelSize: 13
                visible: QtTune.droppedFrames > 0
            }

            Item { Layout.fillWidth: true }

            ThemedButton {
                text: QtTune.model.newestFirst
                    ? qsTr("Newest first ▾")
                    : qsTr("Oldest first ▴")
                onClicked: QtTune.model.newestFirst = !QtTune.model.newestFirst
            }

            ThemedButton {
                text: qsTr("Clear")
                enabled: QtTune.frameCount > 0
                onClicked: QtTune.clearFrames()
            }
        }

        // ---- Frame table ----
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radius
            color: Theme.surface
            clip: true

            // Column header
            Rectangle {
                id: columnHeader
                anchors { top: parent.top; left: parent.left; right: parent.right }
                height: 32
                z: 2
                color: Theme.elevated

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 8

                    // Widths MUST match delegate columns below
                    Label { text: qsTr("Time (µs)"); color: Theme.textDim; font.pixelSize: 12;
                            Layout.preferredWidth: 130 }
                    Label { text: qsTr("DLC"); color: Theme.textDim; font.pixelSize: 12;
                            Layout.preferredWidth: 36 }
                    Label { text: qsTr("Ext"); color: Theme.textDim; font.pixelSize: 12;
                            Layout.preferredWidth: 36 }
                    Label { text: qsTr("ID"); color: Theme.textDim; font.pixelSize: 12;
                            Layout.preferredWidth: 64 }
                    Label { text: qsTr("Payload"); color: Theme.textDim; font.pixelSize: 12;
                            Layout.fillWidth: true }
                }
            }

            ListView {
                id: frameList
                anchors { top: columnHeader.bottom; left: parent.left
                          right: parent.right; bottom: parent.bottom }
                anchors.margins: 4
                clip: true
                model: QtTune.model          // FrameSortProxy over the bridge's FrameListModel

                // Auto-follow newest frames
                // onCountChanged: {
                //     if (atYEnd)
                //         positionViewAtEnd()
                // }

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: RowLayout {
                    width: frameList.width
                    height: 26
                    spacing: 8

                    required property var model    // role access
                    required property int  index

                    // Roles from FrameListModel::roleNames()
                    Label {
                        text: model.timestampUs
                        color: Theme.textDim
                        font.family: "monospace"; font.pixelSize: 12
                        Layout.preferredWidth: 130
                        elide: Text.ElideRight
                    }
                    Label {
                        text: model.dlc
                        color: Theme.text
                        font.family: "monospace"; font.pixelSize: 12
                        Layout.preferredWidth: 36
                    }
                    Label {
                        text: model.extended ? "E" : "-"
                        color: model.extended ? Theme.success : Theme.textDim
                        Layout.preferredWidth: 36
                    }
                    Label {
                        // ArbId arrives as a number (quint32 via QVariantMap) — hex-format it
                        text: "0x" + model.arbitrationId.toString(16).toUpperCase()
                        color: Theme.textDim
                        font.family: "monospace"; font.pixelSize: 12
                        Layout.preferredWidth: 64
                        elide: Text.ElideRight
                    }
                    Label {
                        text: model.payloadHex
                        color: Theme.text
                        font.family: "monospace"; font.pixelSize: 12
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                }

                // ---- Empty state ----
                Label {
                    anchors.centerIn: parent
                    visible: frameList.count === 0
                    text: QtTune.connected
                          ? qsTr("Waiting for frames…")
                          : qsTr("No session — start one from Home")
                    color: Theme.textDim
                }
            }
        }
    }
}
