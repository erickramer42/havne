// File/layer: app/qml/Captioned.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings (test_qml)
// A form's box with its name above it: a placeholder alone goes once
// something is typed, and the box no longer says what it holds

import QtQuick
import QtQuick.Controls

Column {
    property alias caption: captionText.text
    spacing: 2
    Label {
        id: captionText
        color: Theme.textDim
    }
}
