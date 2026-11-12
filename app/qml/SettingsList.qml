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
    // The Log row's summary: a line of its own, so a new one doesn't remake
    // the rows `pages` makes (a press in flight, the focus)
    property string logSummary: ""
    // True beside the pages, where the list is narrow: the value goes under
    // the title, on up to two lines, so it reads whole
    property bool stacked: false
    signal chosen(string key)
    spacing: 0

    Repeater {
        model: list.pages
        ThemedButton {
            id: row
            required property var modelData
            objectName: "settingsRow_" + modelData.key
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(Theme.touchTarget,
                                             list.stacked ? stackedContent.implicitHeight + 16 : 0)
            checkable: true
            checked: list.current === modelData.key
            Accessible.name: modelData.title
            onClicked: list.chosen(modelData.key)
            contentItem: GridLayout {
                id: stackedContent
                columns: list.stacked ? 1 : 2
                columnSpacing: 10
                rowSpacing: 2
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
                    text: row.modelData.key === "log" ? list.logSummary : row.modelData.summary
                    color: row.modelData.warn === true ? Theme.danger : Theme.textDim
                    wrapMode: list.stacked ? Text.WordWrap : Text.NoWrap
                    maximumLineCount: list.stacked ? 2 : 1
                    elide: Text.ElideRight
                    horizontalAlignment: list.stacked ? Text.AlignLeft : Text.AlignRight
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                }
            }
        }
    }
}
