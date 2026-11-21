// File/layer: app/qml/PlainComboBox.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.plaintext (test_qml)
// A ComboBox whose shown text and rows are plain text: the stock delegate and content draw AutoText, which reads a name from a file as markup

import QtQuick
import QtQuick.Controls

ComboBox {
    id: control

    // The delegate is a Component of its own: it reaches the box by its id
    // qmllint disable unqualified
    delegate: ItemDelegate {
        id: row
        required property var modelData
        required property int index
        width: ListView.view ? ListView.view.width : control.width
        highlighted: control.highlightedIndex === row.index
        hoverEnabled: control.hoverEnabled
        // The text is the row's own, whichever way the model is shaped
        readonly property string rowText: control.textRole
                                          ? (Array.isArray(control.model) ? row.modelData[control.textRole]
                                                                          : (row.modelData?.[control.textRole] ?? ""))
                                          : String(row.modelData)
        text: row.rowText
        contentItem: Text {
            text: row.rowText
            textFormat: Text.PlainText
            font: control.font
            color: row.highlighted ? control.palette.highlightedText : control.palette.text
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
    }

    // qmllint enable unqualified

    contentItem: Text {
        leftPadding: 12
        rightPadding: control.indicator ? control.indicator.width + control.spacing : 0
        text: control.displayText
        textFormat: Text.PlainText
        font: control.font
        color: control.palette.buttonText
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }
}
