// File/layer: app/qml/FrameTable.qml | Qt Quick, UI layer
// Testing: QML: QmlTest.log, QmlTest.recordings, QmlTest.phone (test_qml)

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A FrameListModel's rows as a table. The Log page's live frames and the
// recording viewer's frames share it; what a tap does is the user's.
Rectangle {
    id: table
    objectName: "frameTable"
    // The FrameListModel the rows come from
    required property var frames
    // True for the live session: new rows land at one end and the view
    // follows them. A recording's rows are a fixed set
    property bool live: true
    // Shown while the table has no rows
    property string emptyText: ""
    // The nearest ancestor's touchMin (the recordings page asks for a phone's
    // touch size; the Log page has none), as ThemedButton takes it
    // qmllint disable missing-property
    readonly property real touchMin: {
        for (let p = table.parent; p; p = p.parent) {
            if (p.touchMin !== undefined)
                return p.touchMin
        }
        return 0
    }
    // qmllint enable missing-property
    // A phone's width: the table leaves out DLC, narrows its columns and
    // wraps the payload under itself rather than cut it off
    readonly property bool narrowTable: frameList.width < 480   // the rows' narrow too
    // The wide table's Flags column
    readonly property int flagsWidth: 96
    // A row tapped: its index, the keyboard modifiers held and its time in
    // microseconds, as the table shows it
    signal rowClicked(int index, int modifiers, real timestampUs)
    // A head over the rows (the viewer's controls on a phone), and whether
    // it shows: then it and the column names scroll with the rows, so the
    // table is the one thing that scrolls
    property Component head: null
    property bool headScrolls: false
    // Where the column names go while the head scrolls: made with the head
    property Item namesSlot: null
    readonly property ListView list: frameList

    // The time in microseconds of a view row, -1 for no such row
    function timeAt(row) { return table.frames.timestampAt(row) }
    // The first view row at or after a time in the view's order (later in
    // time oldest-first, earlier newest-first), the last row when the time
    // lies past them all: the row the table shows that time at
    function rowAtTime(timeUs) {
        const newest = table.frames.newestFirst
        let lo = 0
        let hi = table.frames.count
        while (lo < hi) {
            const mid = (lo + hi) >> 1
            const t = table.timeAt(mid)
            if (newest ? t > timeUs : t < timeUs)
                lo = mid + 1
            else
                hi = mid
        }
        return Math.min(lo, table.frames.count - 1)
    }

    Layout.fillWidth: true
    Layout.fillHeight: true
    radius: Theme.radius
    color: Theme.surface
    clip: true

    // Column header: at the top, or under the head while it scrolls
    Rectangle {
        id: columnHeader
        objectName: "frameColumnNames"
        parent: table.headScrolls && table.namesSlot ? table.namesSlot : table
        width: parent ? parent.width : 0
        height: Math.max(32, table.touchMin)
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
                Layout.preferredWidth: table.narrowTable ? 64 : 96
                Layout.fillHeight: true
                Accessible.name: table.frames.newestFirst
                                 ? qsTr("Time, newest first") : qsTr("Time, oldest first")
                contentItem: Label {
                    text: (table.frames.newestFirst ? qsTr("Time ↓") : qsTr("Time ↑"))
                          + (table.narrowTable ? "" : qsTr(" (s)"))
                    color: Theme.text
                    font.pixelSize: Theme.fontCaption
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
                background: Rectangle {
                    color: "transparent"
                    border.width: orderToggle.visualFocus ? 2 : 0
                    border.color: Theme.text
                }
                onClicked: {
                    table.frames.newestFirst = !table.frames.newestFirst
                    Qt.callLater(table.live ? frameList.jumpToLatest : frameList.positionViewAtBeginning)
                }
            }
            Label { text: qsTr("DLC"); color: Theme.textDim; font.pixelSize: Theme.fontCaption;
                    visible: !table.narrowTable
                    Layout.preferredWidth: 36 }
            Label { text: qsTr("Ext"); color: Theme.textDim; font.pixelSize: Theme.fontCaption;
                    Layout.preferredWidth: table.narrowTable ? 20 : 36 }
            // Fits a 29-bit ID ("0x18FF0C00") in the mono face
            Label { text: qsTr("ID"); color: Theme.textDim; font.pixelSize: Theme.fontCaption;
                    Layout.preferredWidth: table.narrowTable ? 80 : 96 }
            Label { text: qsTr("Payload"); color: Theme.textDim; font.pixelSize: Theme.fontCaption;
                    Layout.fillWidth: true }
            Label { text: qsTr("Flags"); color: Theme.textDim; font.pixelSize: Theme.fontCaption;
                    visible: !table.narrowTable
                    Layout.preferredWidth: table.flagsWidth }
        }
    }

    ListView {
        id: frameList
        objectName: "frameList"
        anchors { top: parent.top; left: parent.left
                  right: parent.right; bottom: parent.bottom }
        anchors.margins: 4
        anchors.topMargin: table.headScrolls ? 4 : columnHeader.height + 4
        clip: true
        header: table.head !== null ? headHolder : null
        Component {
            id: headHolder
            Column {
                width: frameList.width
                Loader {
                    width: parent.width
                    sourceComponent: table.head
                }
                Item {
                    id: names
                    width: parent.width
                    height: table.headScrolls ? columnHeader.height : 0
                    Component.onCompleted: table.namesSlot = names
                }
            }
        }
        // Bound only while shown: each live frame inserts a row, and a hidden
        // table would make and destroy a delegate for it on every page
        model: table.visible ? table.frames : null
        Accessible.name: qsTr("Frames")

        // Oldest-first reads as a tail: new rows land at the
        // bottom, and the view follows them while it's at the
        // end. Scrolling up stops following; scrolling back to
        // the end, or "Latest", resumes it. Newest-first needs no
        // follow, since new rows land at the top.
        property bool follow: true
        readonly property bool tailing: !table.frames.newestFirst

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

        // A hidden table keeps its place: the time of the first row shown
        // (rows arrive and leave while it's hidden, so an index would land
        // on another frame) and whether it followed, noted while shown and
        // put back once it's bound again
        property real keptTimeUs: -1
        property bool keptFollow: true
        property bool rebinding: false
        onContentYChanged: if (table.visible && count > 0) keptTimeUs = table.timeAt(indexAt(1, contentY + 1))
        onFollowChanged: if (table.visible && count > 0) keptFollow = follow
        function restoreTop() {
            if (!follow && count > 0 && keptTimeUs >= 0)
                positionViewAtIndex(table.rowAtTime(keptTimeUs), ListView.Beginning)
        }

        onCountChanged: {
            if (!table.visible) {
                rebinding = true   // unbound while hidden, not cleared
            } else if (count === 0) {
                // Cleared or a new session: the place noted before is gone too
                follow = true
                keptFollow = true
                keptTimeUs = -1
            } else if (rebinding) {
                rebinding = false
                follow = keptFollow
                if (!follow)
                    Qt.callLater(frameList.restoreTop)
                else if (table.live && tailing)
                    Qt.callLater(frameList.followTail)
            } else if (table.live && tailing && follow) {
                Qt.callLater(frameList.followTail)
            }
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
            color: frameRow.model.selected ? Qt.alpha(Theme.text, 0.12) : "transparent"

            required property var model    // role access
            required property int  index

            // A tap is the table's user's to answer (rowClicked); a drag
            // still scrolls
            MouseArea {
                objectName: "frameRowArea"
                anchors.fill: parent
                onClicked: mouse => table.rowClicked(frameRow.index, mouse.modifiers,
                                                     frameRow.model.timestampUs)
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
                // The labels stay out of the accessibility tree: on
                // Android each row's coming and going rebuilds a node
                // on the main thread
                Label {
                    // Session-relative us, shown as s.mmm
                    text: (frameRow.model.timestampUs / 1e6).toFixed(3)
                    Accessible.ignored: true
                    color: Theme.textDim
                    font.family: Theme.monoFamily; font.pixelSize: Theme.fontCaption
                    Layout.preferredWidth: frameRow.narrow ? 64 : 96
                    elide: Text.ElideRight
                }
                Label {
                    text: frameRow.model.dlc
                    Accessible.ignored: true
                    visible: !frameRow.narrow
                    color: Theme.text
                    font.family: Theme.monoFamily; font.pixelSize: Theme.fontCaption
                    Layout.preferredWidth: 36
                }
                Label {
                    text: frameRow.model.extended ? "E" : "-"
                    Accessible.ignored: true
                    color: frameRow.model.extended ? Theme.success : Theme.textDim
                    font.family: Theme.monoFamily; font.pixelSize: Theme.fontCaption
                    Layout.preferredWidth: frameRow.narrow ? 20 : 36
                }
                Label {
                    // Role name is "arbId" (a quint32): hex-format
                    // it for display.
                    text: "0x" + frameRow.model.arbId.toString(16).toUpperCase()
                    Accessible.ignored: true
                    color: Theme.textDim
                    font.family: Theme.monoFamily; font.pixelSize: Theme.fontCaption
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
                        Accessible.ignored: true
                        color: Theme.text
                        font.family: Theme.monoFamily; font.pixelSize: Theme.fontCaption
                        wrapMode: frameRow.narrow ? Text.WrapAnywhere : Text.NoWrap
                        elide: frameRow.narrow ? Text.ElideNone : Text.ElideRight
                    }
                    // A phone's flags, under the payload
                    Label {
                        objectName: "rowFlagsNarrow"
                        width: parent.width
                        visible: frameRow.narrow && text !== ""
                        text: frameRow.model.flagsText
                        Accessible.ignored: true
                        color: Theme.textDim
                        font.pixelSize: Theme.fontCaption
                        elide: Text.ElideRight
                    }
                }
                Label {
                    objectName: "rowFlags"
                    visible: !frameRow.narrow
                    text: frameRow.model.flagsText
                    Accessible.ignored: true
                    color: Theme.textDim
                    font.pixelSize: Theme.fontCaption
                    Layout.preferredWidth: table.flagsWidth
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
            text: table.emptyText
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
        visible: table.live && frameList.count > 0
                 && (frameList.tailing ? !frameList.follow : !frameList.atYBeginning)
        text: frameList.tailing ? qsTr("Latest ↓") : qsTr("Latest ↑")
        onClicked: frameList.jumpToLatest()
    }
}
