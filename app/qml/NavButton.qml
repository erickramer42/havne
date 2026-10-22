import QtQuick
import QtQuick.Controls

// Sidebar item. Transparent on the sidebar panel: only the active item
// and a hovered one draw a background.
AbstractButton {
    id: control
    property string label: ""
    property bool active: false

    implicitHeight: 44
    hoverEnabled: true
    Accessible.name: control.label

    background: Rectangle {
        color: control.active ? Theme.elevated
             : control.hovered ? Qt.alpha(Theme.primary, 0.08)
             : "transparent"
        // Keyboard focus ring; visualFocus stays false for mouse clicks
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.text
        Rectangle { // left accent bar when active
            visible: control.active
            width: 3; height: parent.height
            color: Theme.primary
        }
    }
    contentItem: Label {
        text: control.label
        leftPadding: 16
        color: control.active ? Theme.text : Theme.textDim
        font.weight: control.active ? Font.DemiBold : Font.Normal
        verticalAlignment: Text.AlignVCenter
    }
}
