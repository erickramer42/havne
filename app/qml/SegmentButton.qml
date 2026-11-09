// File/layer: app/qml/SegmentButton.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.log, QmlTest.recordings, QmlTest.phone, QmlTest.settings (test_qml)

import QtQuick
import QtQuick.Controls

// One option of a toggle of a few. Every option stays visible and the
// current one is filled, so it doesn't read as a dropdown.
AbstractButton {
    id: seg
    property bool selected: false
    property alias wrapMode: segLabel.wrapMode

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
        font.pixelSize: Theme.fontCaption
        color: seg.selected ? Theme.text : Theme.textDim
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
