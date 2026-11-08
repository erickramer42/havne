// File/layer: app/qml/ThemedCheckBox.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings (test_qml)
// A CheckBox that is a phone's touch size inside a page that asks for one

import QtQuick
import QtQuick.Controls

CheckBox {
    id: control

    // The nearest ancestor's touchMin (SettingsSubpage sets one), as
    // ThemedButton takes it
    // qmllint disable missing-property
    readonly property real touchMin: {
        for (let p = control.parent; p; p = p.parent) {
            if (p.touchMin !== undefined)
                return p.touchMin
        }
        return 0
    }
    // qmllint enable missing-property
    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding, touchMin)
}
