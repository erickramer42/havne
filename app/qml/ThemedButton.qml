// File/layer: app/qml/ThemedButton.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)

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

    // Compact, for a phone's crowded rows: as wide as its text with
    // narrow sides (40 px at least), not the 120 px a button otherwise
    // starts from
    property bool compact: false
    horizontalPadding: compact ? 6 : 8
    implicitWidth: compact ? Math.max(40, implicitContentWidth + leftPadding + rightPadding)
                           : Math.max(implicitBackgroundWidth + leftInset + rightInset,
                                      implicitContentWidth + leftPadding + rightPadding)

    // A page that needs its buttons a phone's touch size (Settings) has a
    // touchMin property; the button takes the nearest one up its parents
    // qmllint disable missing-property
    readonly property real touchMin: {
        for (let p = control.parent; p; p = p.parent) {
            if (p.touchMin !== undefined)
                return p.touchMin
        }
        return 0
    }
    // qmllint enable missing-property
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding, touchMin)

    opacity: enabled ? 1.0 : 0.45

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: Theme.controlHeight
        radius: Theme.radius
        color: control.down    ? control.pressedColor
             : control.hovered ? control.hoverColor
             :                   control.baseColor
        // Keyboard focus ring; visualFocus stays false for mouse clicks
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.text
    }

    // Given less than its text's width, the text wraps and the button
    // grows taller
    contentItem: Text {
        text: control.text
        font.pixelSize: 14
        color: Theme.text
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
