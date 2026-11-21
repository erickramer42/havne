// File/layer: app/qml/PlainMenuItem.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.plaintext (test_qml)
// A MenuItem whose text is plain: the stock item draws AutoText, which reads a name from a file as markup

import QtQuick
import QtQuick.Controls

// Text only: it has no icon, no submenu arrow and no mirroring, which the
// pane menu's checkable items don't need
MenuItem {
    id: item

    contentItem: Text {
        leftPadding: item.checkable ? item.indicator.width + item.spacing : 0
        text: item.text
        textFormat: Text.PlainText
        font: item.font
        color: item.palette.text
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }
}
