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
    padding: Theme.pageMargin
    background: Rectangle { color: Theme.bg }

    // The page shows the activity log; in developer mode it can show the
    // raw frame table instead (Theme.developerMode)
    property string view: "activity"
    readonly property bool showingFrames: Theme.developerMode && view === "frames"
    // A phone's width: the table leaves out DLC, narrows its columns and
    // wraps the payload under itself rather than cut it off
    readonly property bool narrowTable: frameList.width < 480   // the rows' narrow too

    readonly property int maxLogLines: 200

    // ---- The Frames view's table (framefilter.h) ----
    // By ID instead of the frames; the filter's panel open; its
    // direction (FrameFilter::Direction's order)
    property bool byId: false
    property bool filterOpen: false
    // Export CSV's last answer: the counts, or why nothing was written
    property string exportNote: ""
    property bool exportFailed: false
    property int filterDirection: 0
    // The wide table's Flags column
    readonly property int flagsWidth: 96

    // The filter's fields to the model, which keeps the last good part of
    // a field that doesn't parse and says why
    function applyFilter() {
        const r = Bridge.model.setFilter(filterIds.text, filterData.text,
                                         page.filterDirection, filterFlag.currentIndex)
        filterIdsProblem.text = r.ids
        filterDataProblem.text = r.data
    }

    function clearFilter() {
        filterIds.text = ""
        filterData.text = ""
        filterFlag.currentIndex = 0
        page.filterDirection = 0
        applyFilter()
    }

    function showById() {
        page.byId = true
        Bridge.frameIds.refresh()
    }

    // A By ID row tapped: the table of that ID's frames alone. The ID
    // filter takes a number of either width, so the row's own width is
    // the flag: a standard 0x7E8's tap leaves an extended 0x7E8 out, as
    // an extended one's leaves the standard one out
    function showIdFrames(id, extended) {
        filterIds.text = id.toString(16).toUpperCase()
        filterFlag.currentIndex = extended ? 1 : 2   // Extended (29-bit), Standard (11-bit)
        applyFilter()
        page.filterOpen = true
        page.byId = false
    }

    // Frames a second, as the By ID view shows them
    function rateText(rate) {
        return rate <= 0 ? "-" : rate < 10 ? rate.toFixed(1) + "/s" : Math.round(rate) + "/s"
    }

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

            // Beside the title, so it stays put when the buttons on the
            // right change with the view
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

            Item { Layout.fillWidth: true }

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
                    highlighted: Bridge.logPaused
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
                font.pixelSize: 12
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
                    color: Theme.info
                    font.pixelSize: 13
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

                Rectangle {
                    implicitWidth: tableSwitch.implicitWidth + 4
                    implicitHeight: tableSwitch.implicitHeight + 4
                    radius: Theme.radius
                    color: Theme.surface

                    Row {
                        id: tableSwitch
                        anchors.centerIn: parent
                        spacing: 2
                        SegmentButton {
                            objectName: "table_frames"
                            text: qsTr("Frames")
                            selected: !page.byId
                            onClicked: page.byId = false
                        }
                        SegmentButton {
                            objectName: "table_byId"
                            text: qsTr("By ID")
                            selected: page.byId
                            onClicked: page.showById()
                        }
                    }
                }
                ThemedButton {
                    objectName: "filterToggle"
                    text: Bridge.model.filterTerms > 0
                          ? qsTr("Filter (%1)").arg(Bridge.model.filterTerms)
                          : qsTr("Filter")
                    highlighted: page.filterOpen
                    onClicked: page.filterOpen = !page.filterOpen
                }
            }

            // ---- The filter: the columns a bus is read by ----
            GridLayout {
                objectName: "filterPanel"
                visible: page.filterOpen
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.spacing
                rowSpacing: Theme.spacing / 2

                Label { text: qsTr("ID"); color: Theme.textDim; font.pixelSize: 13 }
                TextField {
                    id: filterIds
                    objectName: "filterIds"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0   // the panel's width, not its text's
                    placeholderText: qsTr("7E8, 7E0-7EF")
                    font.family: Theme.monoFamily
                    inputMethodHints: Qt.ImhPreferUppercase | Qt.ImhNoPredictiveText
                    onTextEdited: page.applyFilter()
                }
                Item { visible: filterIdsProblem.visible }  // the label column, empty
                Label {
                    id: filterIdsProblem
                    objectName: "filterIdsProblem"
                    visible: text !== ""
                    color: Theme.warning
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0   // the panel's width, not its text's
                }

                Label { text: qsTr("Data"); color: Theme.textDim; font.pixelSize: 13 }
                TextField {
                    id: filterData
                    objectName: "filterData"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0   // the panel's width, not its text's
                    placeholderText: qsTr("^62 F1 ??")
                    font.family: Theme.monoFamily
                    inputMethodHints: Qt.ImhPreferUppercase | Qt.ImhNoPredictiveText
                    onTextEdited: page.applyFilter()
                }
                Item { visible: filterDataProblem.visible }  // the label column, empty
                Label {
                    id: filterDataProblem
                    objectName: "filterDataProblem"
                    visible: text !== ""
                    color: Theme.warning
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0   // the panel's width, not its text's
                }

                Flow {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0   // the panel's width, not its text's
                    spacing: Theme.spacing

                    Rectangle {
                        implicitWidth: directionSwitch.implicitWidth + 4
                        implicitHeight: directionSwitch.implicitHeight + 4
                        radius: Theme.radius
                        color: Theme.surface

                        Row {
                            id: directionSwitch
                            anchors.centerIn: parent
                            spacing: 2
                            Repeater {
                                model: [qsTr("All"), qsTr("Received"), qsTr("Sent")]
                                SegmentButton {
                                    required property string modelData
                                    required property int index
                                    objectName: "direction_" + index
                                    text: modelData
                                    selected: page.filterDirection === index
                                    onClicked: {
                                        page.filterDirection = index
                                        page.applyFilter()
                                    }
                                }
                            }
                        }
                    }
                    ComboBox {
                        id: filterFlag
                        objectName: "filterFlag"
                        // FrameFilter::Flag's order
                        model: [qsTr("Any flag"), qsTr("Extended (29-bit)"),
                                qsTr("Standard (11-bit)"), qsTr("CAN FD"),
                                qsTr("Reassembled"), qsTr("Truncated"),
                                qsTr("After a loss"), qsTr("Error")]
                        implicitHeight: Theme.controlHeight
                        onActivated: page.applyFilter()
                    }
                    ThemedButton {
                        objectName: "filterClear"
                        text: qsTr("Clear the filter")
                        enabled: Bridge.model.filterTerms > 0 || filterIds.text !== ""
                                 || filterData.text !== ""
                        onClicked: page.clearFilter()
                    }
                }
                // No transport reports the frames it sends yet, so Sent
                // shows none: said, so an empty table doesn't read as lost
                Label {
                    objectName: "sentNote"
                    visible: page.filterDirection === 2
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0   // the panel's width, not its text's
                    text: qsTr("No adapter reports the frames the session sends yet, so Sent shows none")
                    color: Theme.textDim
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
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
                    font.pixelSize: 13
                }

                // What the filter leaves of the table
                Label {
                    objectName: "filterStatus"
                    visible: Bridge.model.filterTerms > 0
                    width: Math.min(implicitWidth, parent.width)
                    wrapMode: Text.Wrap
                    text: qsTr("Showing %1 of %2").arg(Bridge.model.count).arg(Bridge.model.heldCount)
                    color: Theme.info
                    font.pixelSize: 13
                }

                Label {
                    width: Math.min(implicitWidth, parent.width)
                    wrapMode: Text.Wrap
                    text: qsTr("%1 dropped").arg(Bridge.droppedFrames)
                    color: Theme.danger
                    font.pixelSize: 13
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

            }

            // ---- Frame table ----
            Rectangle {
                visible: !page.byId
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

                        // Widths MUST match delegate columns below. The time's
                        // header is the order: a tap turns it, and goes to the
                        // newest row (a new order resets the model, which
                        // leaves the view where it was)
                        AbstractButton {
                            id: orderToggle
                            objectName: "orderToggle"
                            Layout.preferredWidth: page.narrowTable ? 64 : 96
                            Layout.fillHeight: true
                            Accessible.name: Bridge.model.newestFirst
                                             ? qsTr("Time, newest first") : qsTr("Time, oldest first")
                            contentItem: Label {
                                text: (Bridge.model.newestFirst ? qsTr("Time ↓") : qsTr("Time ↑"))
                                      + (page.narrowTable ? "" : qsTr(" (s)"))
                                color: Theme.text
                                font.pixelSize: 12
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                            background: Rectangle {
                                color: "transparent"
                                border.width: orderToggle.visualFocus ? 2 : 0
                                border.color: Theme.text
                            }
                            onClicked: {
                                Bridge.model.newestFirst = !Bridge.model.newestFirst
                                Qt.callLater(frameList.jumpToLatest)
                            }
                        }
                        Label { text: qsTr("DLC"); color: Theme.textDim; font.pixelSize: 12;
                                visible: !page.narrowTable
                                Layout.preferredWidth: 36 }
                        Label { text: qsTr("Ext"); color: Theme.textDim; font.pixelSize: 12;
                                Layout.preferredWidth: page.narrowTable ? 20 : 36 }
                        // Fits a 29-bit ID ("0x18FF0C00") in the mono face
                        Label { text: qsTr("ID"); color: Theme.textDim; font.pixelSize: 12;
                                Layout.preferredWidth: page.narrowTable ? 80 : 96 }
                        Label { text: qsTr("Payload"); color: Theme.textDim; font.pixelSize: 12;
                                Layout.fillWidth: true }
                        Label { text: qsTr("Flags"); color: Theme.textDim; font.pixelSize: 12;
                                visible: !page.narrowTable
                                Layout.preferredWidth: page.flagsWidth }
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

                    delegate: Rectangle {
                        id: frameRow
                        // The page's narrowTable, read through the view
                        readonly property bool narrow: ListView.view !== null
                                                       && ListView.view.width < 480
                        width: ListView.view !== null ? ListView.view.width : 0
                        // Narrow, a long payload wraps onto more lines, its
                        // flags under it
                        height: narrow ? Math.max(26, payloadColumn.implicitHeight + 8) : 26
                        radius: 4
                        color: frameRow.model.selected ? Qt.alpha(Theme.info, 0.22) : "transparent"

                        required property var model    // role access
                        required property int  index

                        // A tap selects the row or lets it go; a
                        // shift-click selects from the last row tapped to
                        // this one. A drag still scrolls
                        MouseArea {
                            objectName: "frameRowArea"
                            anchors.fill: parent
                            onClicked: mouse => {
                                if (mouse.modifiers & Qt.ShiftModifier)
                                    Bridge.model.selectThrough(frameRow.index)
                                else
                                    Bridge.model.toggleSelected(frameRow.index)
                            }
                        }

                        RowLayout {
                            anchors.fill: parent
                            // Under the column headers: theirs is 12 px in,
                            // the list's 4
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 8

                            // Roles from FrameListModel::roleNames(): the model
                            // registers "arbId": bind exactly that name. A
                            // misspelled role is undefined at runtime, not a
                            // build error: the column would render empty and
                            // throw per delegate.
                            Label {
                                // Session-relative us, shown as s.mmm
                                text: (frameRow.model.timestampUs / 1e6).toFixed(3)
                                color: Theme.textDim
                                font.family: Theme.monoFamily; font.pixelSize: 12
                                Layout.preferredWidth: frameRow.narrow ? 64 : 96
                                elide: Text.ElideRight
                            }
                            Label {
                                text: frameRow.model.dlc
                                visible: !frameRow.narrow
                                color: Theme.text
                                font.family: Theme.monoFamily; font.pixelSize: 12
                                Layout.preferredWidth: 36
                            }
                            Label {
                                text: frameRow.model.extended ? "E" : "-"
                                color: frameRow.model.extended ? Theme.success : Theme.textDim
                                font.family: Theme.monoFamily; font.pixelSize: 12
                                Layout.preferredWidth: frameRow.narrow ? 20 : 36
                            }
                            Label {
                                // Role name is "arbId" (a quint32): hex-format
                                // it for display.
                                text: "0x" + frameRow.model.arbId.toString(16).toUpperCase()
                                color: Theme.textDim
                                font.family: Theme.monoFamily; font.pixelSize: 12
                                Layout.preferredWidth: frameRow.narrow ? 80 : 96
                                elide: Text.ElideRight
                            }
                            Column {
                                id: payloadColumn
                                Layout.fillWidth: true
                                Label {
                                    id: payloadLabel
                                    width: parent.width
                                    text: frameRow.model.payloadHex   // spaced uppercase bytes, from the bridge
                                    color: Theme.text
                                    font.family: Theme.monoFamily; font.pixelSize: 12
                                    wrapMode: frameRow.narrow ? Text.WrapAnywhere : Text.NoWrap
                                    elide: frameRow.narrow ? Text.ElideNone : Text.ElideRight
                                }
                                // A phone's flags, under the payload
                                Label {
                                    objectName: "rowFlagsNarrow"
                                    width: parent.width
                                    visible: frameRow.narrow && text !== ""
                                    text: frameRow.model.flagsText
                                    color: Theme.textDim
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }
                            Label {
                                objectName: "rowFlags"
                                visible: !frameRow.narrow
                                text: frameRow.model.flagsText
                                color: Theme.textDim
                                font.pixelSize: 12
                                Layout.preferredWidth: page.flagsWidth
                                elide: Text.ElideRight
                            }
                        }
                    }

                    // ---- Empty state ----
                    Label {
                        anchors.centerIn: parent
                        width: Math.min(implicitWidth, parent.width - 16)
                        wrapMode: Text.Wrap
                        horizontalAlignment: Text.AlignHCenter
                        visible: frameList.count === 0
                        text: !Bridge.connected && Bridge.model.heldCount === 0
                              ? qsTr("No session: connect one from Settings")
                            : Bridge.model.heldCount > 0 ? qsTr("No frame the filter shows")
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

            // ---- By ID: one row per ID over the frames the table shows ----
            Rectangle {
                visible: page.byId
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: Theme.radius
                color: Theme.surface
                clip: true

                // Read again twice a second while it shows (FrameIdModel)
                Timer {
                    interval: 500
                    repeat: true
                    running: page.visible && page.showingFrames && page.byId
                    onTriggered: Bridge.frameIds.refresh()
                }

                Rectangle {
                    id: idHeader
                    anchors { top: parent.top; left: parent.left; right: parent.right }
                    height: 32
                    z: 2
                    color: Theme.elevated

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 8
                        // Widths MUST match the delegate's columns below
                        Label { text: qsTr("ID"); color: Theme.textDim; font.pixelSize: 12;
                                Layout.preferredWidth: 104 }   // a 29-bit ID and its E
                        Label { text: qsTr("Frames"); color: Theme.textDim; font.pixelSize: 12;
                                Layout.preferredWidth: 52 }
                        Label { text: qsTr("Rate"); color: Theme.textDim; font.pixelSize: 12;
                                Layout.preferredWidth: 52 }
                        Label { text: qsTr("Last payload"); color: Theme.textDim; font.pixelSize: 12;
                                Layout.fillWidth: true; elide: Text.ElideRight }
                    }
                }

                ListView {
                    id: idList
                    objectName: "idList"
                    anchors { top: idHeader.bottom; left: parent.left
                              right: parent.right; bottom: parent.bottom }
                    anchors.margins: 4
                    clip: true
                    model: Bridge.frameIds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    delegate: Rectangle {
                        id: idRow
                        readonly property bool narrow: ListView.view !== null
                                                       && ListView.view.width < 480
                        width: ListView.view !== null ? ListView.view.width : 0
                        height: Math.max(Theme.rowHeight, idPayload.implicitHeight + 8)
                        radius: 4
                        color: "transparent"

                        required property var model
                        required property int index

                        // A tap reads this ID's frames: the filter takes it
                        MouseArea {
                            objectName: "idRowArea"
                            anchors.fill: parent
                            onClicked: page.showIdFrames(idRow.model.arbId, idRow.model.extended)
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 8
                            Label {
                                text: "0x" + idRow.model.arbId.toString(16).toUpperCase()
                                      + (idRow.model.extended ? " E" : "")
                                color: Theme.text
                                font.family: Theme.monoFamily; font.pixelSize: 12
                                Layout.preferredWidth: 104
                                elide: Text.ElideRight
                            }
                            Label {
                                text: idRow.model.frames
                                color: Theme.textDim
                                font.family: Theme.monoFamily; font.pixelSize: 12
                                Layout.preferredWidth: 52
                            }
                            Label {
                                text: page.rateText(idRow.model.rate)
                                color: Theme.textDim
                                font.family: Theme.monoFamily; font.pixelSize: 12
                                Layout.preferredWidth: 52
                            }
                            Label {
                                id: idPayload
                                text: idRow.model.payloadHex
                                color: Theme.text
                                font.family: Theme.monoFamily; font.pixelSize: 12
                                Layout.fillWidth: true
                                wrapMode: idRow.narrow ? Text.Wrap : Text.NoWrap   // between bytes
                                elide: idRow.narrow ? Text.ElideNone : Text.ElideRight
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: idList.count === 0
                        text: Bridge.model.heldCount > 0 ? qsTr("No frame the filter shows")
                                                         : qsTr("No frames yet")
                        color: Theme.textDim
                    }
                }
            }
        }
    }
}
