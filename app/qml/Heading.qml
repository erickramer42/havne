// File/layer: app/qml/Heading.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings (test_qml)
// The heading of a Settings part

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Label {
    color: Theme.text
    font.pixelSize: Theme.fontEmphasis
    font.weight: Font.DemiBold
    Layout.topMargin: Theme.spacing
}
