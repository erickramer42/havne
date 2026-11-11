// File/layer: app/qml/LogPage.qml | Qt Quick, UI layer
// Testing: BRIDGE: BridgeTest (test_bridge); QML: QmlTest.log,
//          QmlTest.linkhealth, QmlTest.phone (test_qml); Manual smoke

pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Pane {
    id: page
    // Buttons and segments reach a phone's touch size; the buttons read it up their parents
    readonly property real touchMin: Theme.touchTarget
    padding: Theme.pageMargin
    background: Rectangle { color: Theme.bg }

    // The page shows the activity log; in developer mode it can show the
    // raw frame table instead (Theme.developerMode)
    property string view: "activity"
    readonly property bool showingFrames: Theme.developerMode && view === "frames"
    readonly property int maxLogLines: 200

    // The tab-bar form's back arrow, where no tab is marked while the Log
    // shows; Main says where it leads
    property bool backShown: false
    property string backName: qsTr("Back")
    signal backRequested

    // ---- The Frames view's table (framefilter.h) ----
    // By ID instead of the frames; the filter's panel open
    property bool byId: false
    property bool filterOpen: false
    // Export CSV's last answer: the counts, or why nothing was written
    property string exportNote: ""
    property bool exportFailed: false

    // The filter's fields to the model (FrameFilterPanel)
    function applyFilter() { filterPanel.apply() }

    function showById() {
        page.byId = true
        Bridge.frameIds.refresh()
    }

    // A By ID row tapped: the table of that ID's frames alone
    function showIdFrames(id, extended) {
        filterPanel.showId(id, extended)
        page.filterOpen = true
        page.byId = false
    }

    // Frames a second, as the By ID view shows them
    function rateText(rate) { return idTable.rateText(rate) }

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

    // The activity at its newest line: where Show log and Settings' Log row
    // land, the developer's Frames view included
    function showActivityEnd() {
        page.view = "activity"
        activityLog.cursorPosition = activityLog.length
        // The cursor is already there after a line, so it may not scroll
        Qt.callLater(() => {
            const flick = activityScroll.contentItem
            flick.contentY = Math.max(0, flick.contentHeight - flick.height)
        })
    }

    // The Frames view's selected rows to the clipboard, a header line
    // first and one tab-separated line each (FrameListModel::selectedText)
    function copySelectedFrames() {
        clipboardBuffer.text = Bridge.model.selectedText()
        clipboardBuffer.selectAll()
        clipboardBuffer.copy()
        clipboardBuffer.text = ""
    }

    // Export CSV (SessionBridge::exportFrames): the frames held, scrubbed
    // by core, then those the filter shows, to the file the user chose
    function exportFramesTo(url) {
        const r = Bridge.exportFrames(url)
        exportFailed = !r.ok
        exportNote = r.ok ? qsTr("%n frame(s) exported; ", "", r.written)
                            + qsTr("%n left out (identification and text signals)", "", r.leftOut)
                          : r.words
        return r.ok
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        RowLayout {
            objectName: "logTitleRow"
            Layout.fillWidth: true
            spacing: 10

            // Quiet, at the left edge, as Settings' subpages have it
            ThemedButton {
                objectName: "logBack"
                visible: page.backShown
                flat: true
                text: "←"   // a left arrow
                Accessible.name: page.backName
                implicitWidth: Theme.touchTarget
                implicitHeight: Theme.touchTarget
                onClicked: page.backRequested()
            }

            PageHeader {
                title: qsTr("Log")

                // Beside the title, so it stays put when the buttons on the
                // right change with the view; on the tab-bar form a row of its own
                RowLayout {
                    id: wideSwitchSlot
                    visible: Theme.developerMode && !Theme.compact
                    spacing: 0
                }

                Item { Layout.fillWidth: true }

                // The whole activity log to the clipboard: a phone can't
                // select it (docs/bench-sessions/2026-10-28-android-2.md)
                ThemedButton {
                    objectName: "copyLog"
                    visible: !page.showingFrames
                    compact: Theme.compact
                    text: qsTr("Copy")
                    onClicked: {
                        activityLog.selectAll()
                        activityLog.copy()
                        activityLog.deselect()
                    }
                }
            }
        }

        // Developer mode's switch: under the title row on the tab-bar form,
        // where the arrow, the title, the switch and Copy don't fit 320 px
        RowLayout {
            id: narrowSwitchSlot
            visible: Theme.developerMode && Theme.compact
            spacing: 0

            SegmentRow {
                objectName: "logViewSwitch"
                parent: Theme.compact ? narrowSwitchSlot : wideSwitchSlot
                visible: Theme.developerMode
                SegmentButton {
                    tapHeight: page.touchMin
                    objectName: "view_activity"
                    text: qsTr("Activity")
                    selected: !page.showingFrames
                    onClicked: page.view = "activity"
                }
                SegmentButton {
                    tapHeight: page.touchMin
                    objectName: "view_frames"
                    text: qsTr("Frames")
                    selected: page.showingFrames
                    onClicked: page.view = "frames"
                }
            }
        }

        // QML has no clipboard of its own: the rows go through a text
        // field that never shows
        TextEdit {
            id: clipboardBuffer
            visible: false
            Layout.preferredWidth: 0
            Layout.preferredHeight: 0
        }
        Shortcut {
            sequences: [StandardKey.Copy]
            enabled: page.visible && page.showingFrames && Bridge.model.selectedCount > 0
            onActivated: page.copySelectedFrames()
        }

        // ---- The activity log: every connect, link change, ECU and
        // session line the bridge logs ----
        ScrollView {
            id: activityScroll
            objectName: "activityView"
            visible: !page.showingFrames
            Layout.fillWidth: true
            Layout.fillHeight: true

            TextArea {
                id: activityLog
                objectName: "activityLog"
                readOnly: true
                // Prose lines: they wrap at words, in the text face (hex
                // and raw frames are the frame table's, in mono)
                wrapMode: TextArea.WordWrap
                font.pixelSize: Theme.fontCaption
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

            // ---- Pause and Clear; while rows are selected, the selection
            // bar in their place (Copy and Select none take no room until
            // they're of use). A Flow, so on a phone's width they wrap
            // rather than lay the page out past its edge ----
            Flow {
                objectName: "frameTools"
                visible: Bridge.model.selectedCount === 0
                Layout.fillWidth: true
                spacing: Theme.spacing

                // Holds the table still so rows can be read. Frames that
                // arrive while paused are counted but not logged; decoding,
                // Live and the Dashboard carry on.
                ThemedButton {
                    objectName: "pauseFrames"
                    text: Bridge.logPaused ? qsTr("Resume") : qsTr("Pause")
                    checkable: true
                    checked: Bridge.logPaused
                    enabled: Bridge.connected
                    onClicked: Bridge.logPaused = !Bridge.logPaused
                }
                ThemedButton {
                    objectName: "clearFrames"
                    text: qsTr("Clear")
                    enabled: Bridge.frameCount > 0
                    onClicked: Bridge.clearFrames()
                }
                // A file to share: core leaves identification out
                // (ADR-0054), unlike Copy, which is the user's own pick
                ThemedButton {
                    objectName: "exportFrames"
                    text: qsTr("Export CSV")
                    enabled: Bridge.model.heldCount > 0
                    onClicked: exportDialog.open()
                }
            }
            Label {
                objectName: "exportWords"
                visible: Bridge.model.heldCount > 0 || page.exportNote !== ""
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                wrapMode: Text.WordWrap
                text: page.exportNote !== ""
                      ? page.exportNote
                      : qsTr("Export CSV leaves identification replies out; Copy copies the rows as they are.")
                color: page.exportFailed ? Theme.danger : Theme.textDim
                font.pixelSize: Theme.fontCaption
            }
            FileDialog {
                id: exportDialog
                title: qsTr("Export the frames as CSV")
                fileMode: FileDialog.SaveFile
                defaultSuffix: "csv"
                nameFilters: [qsTr("CSV files (*.csv)"), qsTr("All files (*)")]
                currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
                onAccepted: page.exportFramesTo(exportDialog.selectedFile)
            }
            Flow {
                objectName: "selectionBar"
                visible: Bridge.model.selectedCount > 0
                Layout.fillWidth: true
                spacing: Theme.spacing

                Label {
                    objectName: "selectedLabel"
                    height: Theme.controlHeight
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("%1 selected").arg(Bridge.model.selectedCount)
                    color: Theme.textDim
                    font.pixelSize: Theme.fontCaption
                }
                // The selected rows to the clipboard, as they are: the
                // user's own pick
                ThemedButton {
                    objectName: "copyFrames"
                    enabled: Bridge.model.selectedCount > 0
                    text: qsTr("Copy")
                    onClicked: page.copySelectedFrames()
                }
                ThemedButton {
                    objectName: "selectNone"
                    enabled: Bridge.model.selectedCount > 0
                    text: qsTr("Select none")
                    onClicked: Bridge.model.clearSelection()
                }
            }

            // ---- The table's view, and the filter ----
            Flow {
                Layout.fillWidth: true
                spacing: Theme.spacing

                SegmentRow {
                    SegmentButton {
                        tapHeight: page.touchMin
                        objectName: "table_frames"
                        text: qsTr("Frames")
                        selected: !page.byId
                        onClicked: page.byId = false
                    }
                    SegmentButton {
                        tapHeight: page.touchMin
                        objectName: "table_byId"
                        text: qsTr("By ID")
                        selected: page.byId
                        onClicked: page.showById()
                    }
                }
                ThemedButton {
                    objectName: "filterToggle"
                    text: Bridge.model.filterTerms > 0
                          ? qsTr("Filter (%1)").arg(Bridge.model.filterTerms)
                          : qsTr("Filter")
                    checkable: true
                    checked: page.filterOpen
                    onClicked: page.filterOpen = !page.filterOpen
                }
            }

            // ---- The filter: the columns a bus is read by ----
            FrameFilterPanel {
                id: filterPanel
                visible: page.filterOpen
                frames: Bridge.model
            }

            // ---- Status and bridge counters, wrapping on a phone ----
            Flow {
                Layout.fillWidth: true
                spacing: Theme.spacing

                // Paused, the table alone holds still: what a new frame
                // still reaches is said, so a pause never reads as frames
                // lost from the recording, nor kept when nothing records
                Label {
                    objectName: "frameStatus"
                    width: Math.min(implicitWidth, parent.width)
                    wrapMode: Text.Wrap
                    text: !Bridge.connected ? qsTr("Idle")
                        : Bridge.logPaused && Bridge.recordingActive
                          ? qsTr("Paused: %1 frames received. The table holds still; the recording keeps every frame")
                                .arg(Bridge.frameCount)
                        : Bridge.logPaused
                          ? qsTr("Paused: %1 frames received. The table holds still and leaves new frames out")
                                .arg(Bridge.frameCount)
                        : qsTr("Logging: %1 frames").arg(Bridge.frameCount)
                    color: !Bridge.connected ? Theme.textDim
                         : Bridge.logPaused ? Theme.warning
                         : Theme.success
                    font.pixelSize: Theme.fontCaption
                }

                // What the filter leaves of the table
                Label {
                    objectName: "filterStatus"
                    visible: Bridge.model.filterTerms > 0
                    width: Math.min(implicitWidth, parent.width)
                    wrapMode: Text.Wrap
                    text: qsTr("Showing %1 of %2").arg(Bridge.model.count).arg(Bridge.model.heldCount)
                    color: Theme.textDim
                    font.pixelSize: Theme.fontCaption
                }

                Label {
                    width: Math.min(implicitWidth, parent.width)
                    wrapMode: Text.Wrap
                    text: qsTr("%1 dropped").arg(Bridge.droppedFrames)
                    color: Theme.danger
                    font.pixelSize: Theme.fontCaption
                    visible: Bridge.droppedFrames > 0
                }

                // Frames that came after the adapter lost some (its buffer
                // overflowed), as against the ones dropped above
                Label {
                    width: Math.min(implicitWidth, parent.width)
                    wrapMode: Text.Wrap
                    objectName: "overflowLabel"
                    text: qsTr("%1 after a loss at the adapter")
                        .arg(Bridge.overflowFrames)
                    color: Theme.warning
                    font.pixelSize: Theme.fontCaption
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
                    font.pixelSize: Theme.fontCaption
                    visible: Bridge.frameCount > 0 && page.width >= 600
                }

            }

            // ---- Frame table ----
            FrameTable {
                visible: !page.byId
                frames: Bridge.model          // the bridge's FrameListModel
                emptyText: !Bridge.connected && Bridge.model.heldCount === 0
                           ? qsTr("No session: connect one from Settings")
                         : Bridge.model.heldCount > 0 ? qsTr("No frame the filter shows")
                         : Bridge.logPaused ? qsTr("Paused")
                         : qsTr("Waiting for frames…")
                // A tap selects the row or lets it go; a shift-click
                // selects from the last row tapped to this one
                onRowClicked: (index, modifiers, timestampUs) => {
                    if (modifiers & Qt.ShiftModifier)
                        Bridge.model.selectThrough(index)
                    else
                        Bridge.model.toggleSelected(index)
                }
            }

            // ---- By ID: one row per ID over the frames the table shows ----
            FrameIdList {
                id: idTable
                visible: page.byId
                ids: Bridge.frameIds
                refreshing: page.visible && page.showingFrames && page.byId
                emptyText: Bridge.model.heldCount > 0 ? qsTr("No frame the filter shows")
                                                      : qsTr("No frames yet")
                onIdTapped: (id, extended) => page.showIdFrames(id, extended)
            }
        }
    }
}
