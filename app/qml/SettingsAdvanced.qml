// File/layer: app/qml/SettingsAdvanced.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// Developer mode: one part of SettingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    // Developer mode (Theme.developerMode); Main.qml stores it
    ThemedCheckBox {
        id: developerBox
        objectName: "developerBox"
        text: qsTr("Developer mode: the mock:// preset, and the raw frames on the Log page")
        checked: Theme.developerMode
        Layout.fillWidth: true
        onToggled: {
            Theme.developerMode = checked
            checked = Qt.binding(() => Theme.developerMode)
        }
        contentItem: Label {
            text: developerBox.text
            color: Theme.text
            leftPadding: developerBox.indicator.width + developerBox.spacing
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
        }
    }
}
