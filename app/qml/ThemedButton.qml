// File/layer: app/qml/ThemedButton.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.buttons, QmlTest.settings, QmlTest.phone (test_qml)

import QtQuick
import QtQuick.Controls

Button {
    id: control

    // The ranks (Qt's own properties): `highlighted` is the one action a
    // page or bar is for (primary), the default is every other action
    // (secondary), `flat` a frequent-but-minor or destructive one (quiet),
    // and `checkable` with `checked` a choice among a few or an open panel
    // (selected, which wins over the others). `checked` is the page's
    // state: bind it. A click that toggled it is undone here, so the
    // binding stays the truth (a click on the chosen one leaves it chosen),
    // and the page's onClicked changes what the binding reads.
    readonly property bool quiet: control.flat && !control.checked
    readonly property bool primary: control.highlighted

    // Exposed so callers can tweak if needed
    property color baseColor: control.checked ? Theme.elevatedLight
                            : control.primary ? Theme.primary
                            : control.quiet ? "transparent"
                            : Theme.elevated
    property color hoverColor: control.checked ? Qt.lighter(Theme.elevatedLight, 1.15)
                             : control.primary ? Qt.lighter(Theme.primary, 1.15)
                             : control.quiet ? Theme.elevated
                             : Theme.elevatedLight
    property color pressedColor: control.checked ? Qt.darker(Theme.elevatedLight, 1.2)
                               : control.primary ? Qt.darker(Theme.primary, 1.2)
                               : control.quiet ? Theme.elevated
                               : Theme.surface
    property color textColor: control.quiet ? Theme.textDim : Theme.text

    onToggled: control.toggle()

    // Compact, for a phone's crowded rows: as wide as its text with
    // narrow sides (40 px at least), not the 120 px a button otherwise
    // starts from; a quiet button is as wide as its text too, with
    // `touchMin` the least
    property bool compact: false
    horizontalPadding: compact || quiet ? 6 : 8
    implicitWidth: compact ? Math.max(40, implicitContentWidth + leftPadding + rightPadding)
                 : quiet ? Math.max(touchMin, implicitContentWidth + leftPadding + rightPadding)
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
        font.pixelSize: Theme.fontBody
        color: control.textColor
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
