import QtQuick
import QtQuick.Controls

Button {
    id: control

    // Expose so callers can tweak if needed. `highlighted` marks the
    // page's one primary action.
    property color baseColor: control.highlighted ? Theme.primary : Theme.elevated
    property color hoverColor: control.highlighted ? Qt.lighter(Theme.primary, 1.15)
                                                   : Theme.elevatedLight
    property color pressedColor: control.highlighted ? Qt.darker(Theme.primary, 1.2)
                                                     : Theme.surface

    opacity: enabled ? 1.0 : 0.45

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: 36
        radius: Theme.radius
        color: control.down    ? control.pressedColor
             : control.hovered ? control.hoverColor
             :                   control.baseColor
        // Keyboard focus ring; visualFocus stays false for mouse clicks
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.text
    }

    contentItem: Text {
        text: control.text
        font.pixelSize: 14
        color: Theme.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
