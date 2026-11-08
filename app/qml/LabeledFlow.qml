// File/layer: app/qml/LabeledFlow.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings (test_qml)
// A caption, then buttons that wrap: a row of them would hold the
// page at its natural width, past a phone's. The caption is the
// flow's first item, so a wrapped button has the whole width

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Flow {
    property alias label: caption.text
    Layout.fillWidth: true
    spacing: Theme.spacing

    Label {
        id: caption
        visible: text !== ""
        color: Theme.textDim
        topPadding: 8
    }
}
