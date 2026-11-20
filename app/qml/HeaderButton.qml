// File/layer: app/qml/HeaderButton.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.phone (test_qml)

import QtQuick
import QtQuick.Layouts

// On a phone a header button is a touch's size each way (headerRow's
// touchMin) and draws smaller, so the bar's rule shows under it
ThemedButton {
    compact: Theme.compact
    Layout.minimumWidth: Theme.compact ? Theme.touchTarget : 0
    topInset: Theme.compact ? 6 : 0
    bottomInset: Theme.compact ? 6 : 0
}
