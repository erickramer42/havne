// File/layer: app/qml/LogPage.qml | Qt Quick, UI layer
// Testing: test_qml (tests/qml/tst_log.qml): the activity log takes the
//          bridge's lines stamped, bounded to its newest lines, alone
//          with developer mode off; the Activity | Frames switch and the
//          frame table only in developer mode, back to the activity log
//          when it goes off; either order button, and Latest, at the newest
//          row from a scrolled view (the top newest-first, the end
//          oldest-first); the link lines in each link state
//          (tests/qml/tst_linkhealth.qml). The frame table: render
//          verified manually; the arbId column by the smoke
//          recipe's "Frames + arbId column" row (docs/testing.md). The
//          role names this delegate binds are pinned by
//          BridgeTest.FrameRoleNamesMatchLogPageBindings, Pause's
//          bridge side by BridgeTest.LogPauseStopsFrameLogNotDecode, and
//          the row order (arrival; a connect empties the table) by
//          BridgeTest.FrameLogOrdersByArrivalNotTimestamp and
//          .ConnectStartsAnEmptyFrameLog.
//          Oldest-first follow: manual smoke while mock:// streams,
//          scroll up by drag, wheel and scroll bar (the view holds), then
//          "Latest" or scrolling to the end resumes following.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: page
    padding: Theme.pageMargin
    background: Rectangle { color: Theme.bg }

    // The page shows the activity log; in developer mode it can show the
    // raw frame table instead (Theme.developerMode)
    property string view: "activity"
    readonly property bool showingFrames: Theme.developerMode && view === "frames"

    readonly property int maxLogLines: 200

    // Each line is stamped with local time in the surrogate session
    // log's format (HH:MM:SS.mmm), so the two line up, and a disconnect
    // or a recovery can be timed from here.
    function appendLog(message) {
        activityLog.append(Qt.formatTime(new Date(), "hh:mm:ss.zzz") + "  " + message)
        // Bound the log: QML TextArea.append keeps growing otherwise.
        // Removes whole oldest LINES (split on "\n"), not visual rows.
        // lineCount counts wrapped rows, so a single long wrapped
        // message can exceed maxLogLines while the text has no newline
        // at all; indexOf("\n") then returns -1, and remove(0, 0) would
        // remove nothing and spin forever. The guard breaks the loop
        // instead: such a message stays until a newline-bearing line can
        // be trimmed.
        while (activityLog.lineCount > page.maxLogLines) {
            const nl = activityLog.text.indexOf("\n")
            if (nl < 0)
                break
            activityLog.remove(0, nl + 1)
        }
        activityLog.cursorPosition = activityLog.length
    }

    Connections {
        target: Bridge
        function onLogMessage(message) { page.appendLog(message) }
    }

    // One option of a two-option toggle. Both options stay visible and
    // the current one is filled, so it doesn't read as a dropdown.
    component SegmentButton: AbstractButton {
        id: seg
        property bool selected: false

        implicitWidth: segLabel.implicitWidth + 24
        implicitHeight: 30
        hoverEnabled: true
        Accessible.checkable: true
        Accessible.checked: seg.selected

        background: Rectangle {
            radius: Theme.radius - 2
            color: seg.selected ? Theme.elevatedLight
                 : seg.hovered ? Qt.alpha(Theme.text, 0.05)
                 : "transparent"
            border.width: seg.visualFocus ? 2 : 0
            border.color: Theme.text
        }
        contentItem: Text {
            id: segLabel
            text: seg.text
            font.pixelSize: 13
            color: seg.selected ? Theme.text : Theme.textDim
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        PageHeader {
            title: qsTr("Log")

            Item { Layout.fillWidth: true }

            Rectangle {
                visible: Theme.developerMode
                implicitWidth: viewSwitch.implicitWidth + 4
                implicitHeight: viewSwitch.implicitHeight + 4
                radius: Theme.radius
                color: Theme.surface

                Row {
                    id: viewSwitch
                    anchors.centerIn: parent
                    spacing: 2

                    SegmentButton {
                        objectName: "view_activity"
                        text: qsTr("Activity")
                        selected: !page.showingFrames
                        onClicked: page.view = "activity"
                    }
                    SegmentButton {
                        objectName: "view_frames"
                        text: qsTr("Frames")
                        selected: page.showingFrames
                        onClicked: page.view = "frames"
                    }
                }
            }

            // The whole activity log to the clipboard: a phone can't
            // select it (docs/bench-sessions/2026-10-28-android-2.md)
            ThemedButton {
                objectName: "copyLog"
                visible: !page.showingFrames
                text: qsTr("Copy")
                onClicked: {
                    activityLog.selectAll()
                    activityLog.copy()
                    activityLog.deselect()
                }
            }

            // Holds the table still so rows can be read. Frames that
            // arrive while paused are counted but not logged; decoding,
            // Live and the Dashboard carry on.
            ThemedButton {
                visible: page.showingFrames
                text: Bridge.logPaused ? qsTr("Resume") : qsTr("Pause")
                highlighted: Bridge.logPaused
                enabled: Bridge.connected
                onClicked: Bridge.logPaused = !Bridge.logPaused
            }
            ThemedButton {
                visible: page.showingFrames
                text: qsTr("Clear")
                enabled: Bridge.frameCount > 0
                onClicked: Bridge.clearFrames()
            }
        }

        // ---- The activity log: every connect, link change, ECU and
        // session line the bridge logs ----
        ScrollView {
            objectName: "activityView"
            visible: !page.showingFrames
            Layout.fillWidth: true
            Layout.fillHeight: true

            TextArea {
                id: activityLog
                objectName: "activityLog"
                readOnly: true
                wrapMode: TextArea.WrapAnywhere
                font.family: Theme.monoFamily
                font.pixelSize: 12
                color: Theme.text
                background: Rectangle {
                    radius: Theme.radius
                    color: Theme.surface
                }
                text: qsTr("Ready.")
            }
        }

        // ---- Developer mode: the raw frames ----
        ColumnLayout {
            objectName: "frameView"
            visible: page.showingFrames
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spacing

            // ---- Status, bridge counters and row order ----
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing

                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: !Bridge.connected ? qsTr("Idle")
                        : Bridge.logPaused
                          ? qsTr("Paused: %1 frames received; new frames aren't logged")
                                .arg(Bridge.frameCount)
                        : qsTr("Logging: %1 frames").arg(Bridge.frameCount)
                    color: !Bridge.connected ? Theme.textDim
                         : Bridge.logPaused ? Theme.warning
                         : Theme.success
                    font.pixelSize: 13
                }

                Label {
                    text: qsTr("%1 dropped").arg(Bridge.droppedFrames)
                    color: Theme.danger
                    font.pixelSize: 13
                    visible: Bridge.droppedFrames > 0
                }

                // Frames that came after the adapter lost some (its buffer
                // overflowed), as against the ones dropped above
                Label {
                    objectName: "overflowLabel"
                    text: qsTr("%1 after a loss at the adapter")
                        .arg(Bridge.overflowFrames)
                    color: Theme.warning
                    font.pixelSize: 13
                    visible: Bridge.overflowFrames > 0
                }

                // Bridge diagnostics: frames delivered to the GUI thread (one
                // queued call each, ADR-0005's per-frame baseline) and the
                // coalesced signal batches. Left out at phone width.
                Label {
                    text: qsTr("notify %1 · batch %2")
                        .arg(Bridge.guiNotifyCount)
                        .arg(Bridge.signalBatchCount)
                    color: Theme.textDim
                    font.pixelSize: 13
                    visible: Bridge.frameCount > 0 && page.width >= 600
                }

                Rectangle {
                    implicitWidth: order.implicitWidth + 4
                    implicitHeight: order.implicitHeight + 4
                    radius: Theme.radius
                    color: Theme.surface

                    Row {
                        id: order
                        anchors.centerIn: parent
                        spacing: 2

                        // A click goes to the newest row: a new order resets
                        // the model, which leaves the view where it was, and
                        // the same order brings a scrolled view back
                        SegmentButton {
                            objectName: "order_newest"
                            text: qsTr("Newest first")
                            selected: Bridge.model.newestFirst
                            onClicked: {
                                Bridge.model.newestFirst = true
                                Qt.callLater(frameList.jumpToLatest)
                            }
                        }
                        SegmentButton {
                            objectName: "order_oldest"
                            text: qsTr("Oldest first")
                            selected: !Bridge.model.newestFirst
                            onClicked: {
                                Bridge.model.newestFirst = false
                                Qt.callLater(frameList.jumpToLatest)
                            }
                        }
                    }
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
                        Label { text: qsTr("Time (s)"); color: Theme.textDim; font.pixelSize: 12;
                                Layout.preferredWidth: 96 }
                        Label { text: qsTr("DLC"); color: Theme.textDim; font.pixelSize: 12;
                                Layout.preferredWidth: 36 }
                        Label { text: qsTr("Ext"); color: Theme.textDim; font.pixelSize: 12;
                                Layout.preferredWidth: 36 }
                        // Fits a 29-bit ID ("0x18FF0C00") in the mono face
                        Label { text: qsTr("ID"); color: Theme.textDim; font.pixelSize: 12;
                                Layout.preferredWidth: 96 }
                        Label { text: qsTr("Payload"); color: Theme.textDim; font.pixelSize: 12;
                                Layout.fillWidth: true }
                    }
                }

                ListView {
                    id: frameList
                    objectName: "frameList"
                    anchors { top: columnHeader.bottom; left: parent.left
                              right: parent.right; bottom: parent.bottom }
                    anchors.margins: 4
                    clip: true
                    model: Bridge.model          // the bridge's FrameListModel

                    // Oldest-first reads as a tail: new rows land at the
                    // bottom, and the view follows them while it's at the
                    // end. Scrolling up stops following; scrolling back to
                    // the end, or "Latest", resumes it. Newest-first needs no
                    // follow, since new rows land at the top.
                    property bool follow: true
                    readonly property bool tailing: !Bridge.model.newestFirst

                    // The newest row: the top in newest-first order, the end
                    // in oldest-first. Position first, then follow:
                    // positioning cancels a flick still coasting, and that
                    // flick's movementEnded would otherwise clear follow
                    // before the view reaches the end.
                    function jumpToLatest() {
                        if (tailing)
                            positionViewAtEnd()
                        else
                            positionViewAtBeginning()
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
                        // registers "arbId": bind exactly that name. A
                        // misspelled role is undefined at runtime, not a
                        // build error: the column would render empty and
                        // throw per delegate.
                        Label {
                            // Session-relative µs, shown as s.mmm
                            text: (model.timestampUs / 1e6).toFixed(3)
                            color: Theme.textDim
                            font.family: Theme.monoFamily; font.pixelSize: 12
                            Layout.preferredWidth: 96
                            elide: Text.ElideRight
                        }
                        Label {
                            text: model.dlc
                            color: Theme.text
                            font.family: Theme.monoFamily; font.pixelSize: 12
                            Layout.preferredWidth: 36
                        }
                        Label {
                            text: model.extended ? "E" : "-"
                            color: model.extended ? Theme.success : Theme.textDim
                            font.family: Theme.monoFamily; font.pixelSize: 12
                            Layout.preferredWidth: 36
                        }
                        Label {
                            // Role name is "arbId" (a quint32): hex-format
                            // it for display.
                            text: "0x" + model.arbId.toString(16).toUpperCase()
                            color: Theme.textDim
                            font.family: Theme.monoFamily; font.pixelSize: 12
                            Layout.preferredWidth: 96
                            elide: Text.ElideRight
                        }
                        Label {
                            text: model.payloadHex   // spaced uppercase bytes, from the bridge
                            color: Theme.text
                            font.family: Theme.monoFamily; font.pixelSize: 12
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }

                    // ---- Empty state ----
                    Label {
                        anchors.centerIn: parent
                        visible: frameList.count === 0
                        text: !Bridge.connected ? qsTr("No session: connect one from Settings")
                            : Bridge.logPaused ? qsTr("Paused")
                            : qsTr("Waiting for frames…")
                        color: Theme.textDim
                    }
                }

                // Back to the newest row after a scroll away (and following
                // again, oldest-first); on a phone there's no scroll bar to
                // drag
                ThemedButton {
                    objectName: "latestButton"
                    anchors { right: parent.right; bottom: parent.bottom; margins: 12 }
                    z: 3
                    visible: frameList.count > 0
                             && (frameList.tailing ? !frameList.follow : !frameList.atYBeginning)
                    text: frameList.tailing ? qsTr("Latest ↓") : qsTr("Latest ↑")
                    onClicked: frameList.jumpToLatest()
                }
            }
        }
    }
}
