// File/layer: app/qml/LogPage.qml | Qt Quick, UI layer
// Testing: render verified manually; the arbId column by the smoke
//          recipe's "Frames + arbId column" row (docs/testing.md). The
//          role names this delegate binds are pinned by
//          BridgeTest.FrameRoleNamesMatchLogPageBindings. Oldest-first
//          follow: manual smoke — while mock:// streams, scroll up by
//          drag, wheel and scroll bar (the view holds), then "Latest"
//          or scrolling to the end resumes following.

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

                // Oldest-first reads as a tail: new rows land at the
                // bottom, and the view follows them while it's at the
                // end. Scrolling up stops following; scrolling back to
                // the end, or "Latest", resumes it. Newest-first needs no
                // follow, since new rows land at the top.
                property bool follow: true
                readonly property bool tailing: !QtTune.model.newestFirst

                // Position first, then follow: positioning cancels a flick
                // still coasting, and that flick's movementEnded would
                // otherwise clear follow before the view reaches the end.
                function jumpToLatest() {
                    positionViewAtEnd()
                    follow = true
                }
                // Named, so Qt.callLater folds a burst of frames into one
                // reposition per event-loop turn. Re-checks follow: a
                // scroll may have started since the call was queued.
                function followTail() { if (follow) jumpToLatest() }

                onCountChanged: {
                    if (count === 0) follow = true   // cleared or new session
                    else if (tailing && follow) Qt.callLater(frameList.followTail)
                }
                // User scrolling decides. Following stops as a drag, flick
                // or wheel starts, or the next frame would pull the view
                // back to the end; where the scroll comes to rest decides
                // whether it resumes. positionViewAtEnd emits neither.
                onMovementStarted: follow = false
                onMovementEnded: follow = atYEnd

                Connections {
                    target: QtTune.model
                    function onNewestFirstChanged() {
                        if (frameList.tailing) Qt.callLater(frameList.jumpToLatest)
                    }
                }

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                    onPressedChanged: frameList.follow = !pressed && frameList.atYEnd
                }

                delegate: RowLayout {
                    width: frameList.width
                    height: 26
                    spacing: 8

                    required property var model    // role access
                    required property int  index

                    // Roles from FrameListModel::roleNames(): the model
                    // registers "arbId" — bind exactly that name. A
                    // misspelled role is undefined at runtime, not a
                    // build error: the column would render empty and
                    // throw per delegate.
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
                        // Role name is "arbId" (quint32 via QVariantMap) —
                        // hex-format it for display.
                        text: "0x" + model.arbId.toString(16).toUpperCase()
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

            // Resumes following after a scroll up; on a phone there's
            // no scroll bar to drag back down
            ThemedButton {
                anchors { right: parent.right; bottom: parent.bottom; margins: 12 }
                z: 3
                visible: frameList.tailing && !frameList.follow && frameList.count > 0
                text: qsTr("Latest ↓")
                onClicked: frameList.jumpToLatest()
            }
        }
    }
}
