// File/layer: app/qml/ViewHead.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)
// A list head of RecordingsPage's pinned phone viewer, which holds the state

import QtQuick
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// A view's list head on the pinned form: the zoom row, the switch and
// the view's own controls move into its slots, so they scroll away
// with the list; elsewhere it is empty and takes no height
ColumnLayout {
    id: head
    required property RecordingsPage host
    // values, frames, ids or about
    required property string role
    readonly property alias zoomSlot: zoomSlotItem
    readonly property alias switchSlot: switchSlotItem
    readonly property alias toolsSlot: toolsSlotItem
    // The list it heads. A ListView keeps its contentY when its header's
    // height changes, so a head that grows at the list's start would
    // slide under the list's top: the list goes back to its start then,
    // unless it was scrolled on. While a change waits for that, a contentY
    // within the head's height of the origin is the change's, not a scroll
    property ListView list: null
    property bool listAtStart: true
    property bool settling: false
    visible: head.host.pinnedViewer
    height: visible ? implicitHeight + 10 : 0
    spacing: 10
    function settle() {
        head.settling = true
        Qt.callLater(head.toStart)
    }
    function toStart() {
        head.settling = false
        if (head.visible && head.list !== null && head.list.visible && head.listAtStart)
            head.list.positionViewAtBeginning()
    }
    onHeightChanged: head.settle()
    Connections {
        target: head.list
        function onContentYChanged() {
            head.listAtStart = head.list.contentY
                               <= head.list.originY + 1 + (head.settling ? head.height : 0)
        }
        function onHeightChanged() { head.settle() }
        function onOriginYChanged() { head.settle() }
        function onVisibleChanged() { head.settle() }
    }
    ColumnLayout {
        id: zoomSlotItem
        Layout.fillWidth: true
        spacing: 0
    }
    RowLayout {
        id: switchSlotItem
        Layout.fillWidth: true
    }
    ColumnLayout {
        id: toolsSlotItem
        Layout.fillWidth: true
        spacing: 0
    }
    Component.onCompleted: head.host.refreshPart.noteHead(role, head)
}
