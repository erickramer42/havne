// File/layer: app/qml/SettingsList.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The list of Settings' pages, each row with its summary on the right

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// pages: [{ key, title, summary }]; current is the key shown (the wide
// layout's selection, "" on a phone's list)
ColumnLayout {
    id: list
    property var pages: []
    property string current: ""
    signal chosen(string key)
    spacing: 0

    Repeater {
        model: list.pages
        ThemedButton {
            id: row
            required property var modelData
            objectName: "settingsRow_" + modelData.key
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.touchTarget
            checkable: true
            checked: list.current === modelData.key
            Accessible.name: modelData.title
            onClicked: list.chosen(modelData.key)
            contentItem: RowLayout {
                spacing: 10
                Label {
                    text: row.modelData.title
                    color: Theme.text
                    font.pixelSize: Theme.fontEmphasis
                    font.weight: Font.DemiBold
                }
                // The value gives way whole in a narrow row (a phone zoomed
                // far in): a few letters of it say nothing
                Label {
                    objectName: "settingsRowSummary"
                    visible: row.width >= 200
                    text: row.modelData.summary
                    color: Theme.textDim
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                    Layout.fillWidth: true
                }
            }
        }
    }
}
