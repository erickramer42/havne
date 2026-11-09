// File/layer: app/qml/ThemedTextField.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.fields, QmlTest.settings, QmlTest.phone (test_qml)
// A TextField drawn like the cards' controls: an elevated fill, an outline, a text-colored ring with the focus

import QtQuick
import QtQuick.Controls

TextField {
    id: control

    // The nearest ancestor's touchMin, as ThemedButton takes it
    // qmllint disable missing-property
    readonly property real touchMin: {
        for (let p = control.parent; p; p = p.parent) {
            if (p.touchMin !== undefined)
                return p.touchMin
        }
        return 0
    }
    // qmllint enable missing-property

    color: Theme.text
    placeholderTextColor: Theme.textDim
    selectionColor: Theme.textDim
    selectedTextColor: Theme.bg
    font.pixelSize: Theme.fontBody
    verticalAlignment: TextInput.AlignVCenter
    leftPadding: 8
    rightPadding: 8
    // The least a finger can hit on a phone (Theme.touchTarget is 36 on
    // the desktop, its control height)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding,
                             Theme.touchTarget, touchMin)
    opacity: enabled ? 1.0 : 0.45

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: Theme.controlHeight
        radius: Theme.radius
        color: Theme.elevated
        // Keyboard and pointer focus both show the ring: a field is where
        // typing goes
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.text : Theme.outline
    }
}
