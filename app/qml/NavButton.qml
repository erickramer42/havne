import QtQuick
import QtQuick.Controls

AbstractButton {
    id: control
    property string label: ""
    property bool active: false

    implicitHeight: 44
    hoverEnabled: true

    background: Rectangle {
        color: control.active ? Theme.elevated
             : control.hovered ? Qt.alpha(Theme.primary, 0.08)
             : Theme.surface
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
