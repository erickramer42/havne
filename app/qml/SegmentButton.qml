// File/layer: app/qml/SegmentButton.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.segments, QmlTest.log, QmlTest.recordings, QmlTest.phone, QmlTest.settings (test_qml)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One option of a SegmentRow. Every option stays visible and the current
// one is drawn as a selected ThemedButton is (fill and outline), so it
// doesn't read as a dropdown.
AbstractButton {
    id: seg
    property bool selected: false
    // A page on a phone sets Theme.touchTarget
    property real tapHeight: 30
    property alias wrapMode: segLabel.wrapMode
    // A setting's value is body text; a view switch's label stays caption
    property real labelSize: Theme.fontCaption
    // Equal shares of the row, for a phone
    property bool share: false
    Layout.fillWidth: seg.share
    Layout.preferredWidth: seg.share ? 0 : -1

    implicitWidth: segLabel.implicitWidth + 24
    implicitHeight: seg.tapHeight
    // A locked row (a session holds the choice) dims, as the buttons under it do
    opacity: seg.enabled ? 1 : 0.45
    hoverEnabled: true
    Accessible.checkable: true
    Accessible.checked: seg.selected

    background: Rectangle {
        radius: Theme.radius - 2
        color: seg.selected ? Theme.elevatedLight
             : seg.hovered ? Qt.alpha(Theme.text, 0.05)
             : "transparent"
        border.width: seg.visualFocus ? 2 : seg.selected ? 1 : 0
        border.color: seg.visualFocus ? Theme.text : Theme.outline
    }
    contentItem: Text {
        id: segLabel
        text: seg.text
        font.pixelSize: seg.labelSize
        color: seg.selected ? Theme.text : Theme.textDim
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
