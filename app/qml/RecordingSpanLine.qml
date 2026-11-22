// File/layer: app/qml/RecordingSpanLine.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The span line: the two cursors and how far apart, with Zoom and Clear, a part of RecordingsPage

RowLayout {
    id: root
    required property RecordingsPage host
    objectName: "spanLine"
    parent: root.host.phoneShape ? root.host.spanRowPhone : root.host.spanRowDesktop
    visible: root.host.spanLineUp
    Layout.fillWidth: true
    spacing: 6
    Label {
        textFormat: Text.PlainText
        objectName: "spanText"
        Layout.fillWidth: true
        horizontalAlignment: root.host.phoneShape ? Text.AlignLeft : Text.AlignRight
        verticalAlignment: Text.AlignVCenter
        wrapMode: root.host.phoneShape ? Text.WordWrap : Text.NoWrap
        maximumLineCount: root.host.phoneShape ? 2 : 1
        elide: Text.ElideRight
        font.pixelSize: root.host.phoneShape ? Theme.fontCaption : Theme.fontBody
        color: Theme.textDim
        text: root.host.hasSpan ? qsTr("A %1 to B %2 · %3").arg(root.host.formatPart.timeText(root.host.spanFromMs, true))
                               .arg(root.host.formatPart.timeText(root.host.spanToMs, true))
                               .arg(root.host.formatPart.timeText(root.host.spanToMs - root.host.spanFromMs, true))
              : root.host.phoneShape ? qsTr("Hold to place the second cursor")
              : qsTr("At %1. Shift+click to place the second cursor")
                    .arg(root.host.formatPart.timeText(root.host.cursorMs, true))
    }
    ThemedButton {
        objectName: "zoomToSpan"
        visible: root.host.hasSpan
        compact: true
        text: root.host.phoneShape ? qsTr("Zoom") : qsTr("Zoom to span")
        Accessible.name: qsTr("Zoom to the span")
        onClicked: root.host.cursorsPart.zoomToSpan()
    }
    ThemedButton {
        objectName: "clearSpan"
        visible: root.host.hasSpan
        flat: true
        text: qsTr("Clear")
        Accessible.name: qsTr("Remove the second cursor")
        onClicked: root.host.cursorsPart.clearCursorB()
    }
}
