// File/layer: app/qml/SegmentRow.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.segments, QmlTest.settings, QmlTest.phone (test_qml)
// The one look of a closed choice: SegmentButtons on a surface track with
// the panel's hairline edge. Where one value is in force and a tap takes
// effect at once, use this; a checkable ThemedButton is a toggle on its own
// or one of a list that grows.

import QtQuick
import QtQuick.Layouts

Rectangle {
    id: track
    default property alias segments: row.data

    implicitWidth: row.implicitWidth + 4
    implicitHeight: row.implicitHeight + 4
    radius: Theme.radius
    color: Theme.surface
    border.width: 1
    border.color: Theme.hairline

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: 2
        spacing: 2
    }
}
