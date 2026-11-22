// File/layer: app/qml/RemoveQuestion.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone_* (test_qml)
// A removal's question, asked in place where it was asked: the words, then
// Remove and Cancel that wrap under them on a phone

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Flow {
    id: root
    // The question; the part shows while it isn't ""
    property string words: ""
    property bool canRemove: true
    signal removeChosen()
    signal cancelChosen()
    visible: words !== ""
    Layout.fillWidth: true
    spacing: Theme.spacing

    Label {
        textFormat: Text.PlainText
        objectName: "signalsConfirmText"
        text: root.words
        color: Theme.text
        wrapMode: Text.Wrap
        width: Math.min(implicitWidth, root.width)
    }
    ThemedButton {
        objectName: "signalsConfirmRemove"
        text: qsTr("Remove")
        highlighted: true
        enabled: root.canRemove
        onClicked: root.removeChosen()
    }
    ThemedButton {
        objectName: "signalsConfirmCancel"
        text: qsTr("Cancel")
        onClicked: root.cancelChosen()
    }
}
