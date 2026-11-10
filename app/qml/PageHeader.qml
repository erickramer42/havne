// File/layer: app/qml/PageHeader.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.phone (test_qml); manual smoke: the titles
//          aligned at Theme.pageMargin

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A page's title row. Items declared inside it follow the title, so a
// page adds its status and actions in place.
RowLayout {
    id: header
    property string title: ""
    // The title gives way, eliding, when the row's other items leave it
    // less than its width; it never grows past its own width. Off, the
    // title keeps its width and the row's fill items give way alone.
    property bool titleShrinks: false

    Layout.fillWidth: true
    spacing: 10

    Label {
        visible: header.title !== ""
        Layout.fillWidth: header.titleShrinks
        // A whole pixel: the layout rounds a fractional maximum down, which
        // leaves the text short of its own width and elides a title that fits
        Layout.maximumWidth: header.titleShrinks ? Math.ceil(implicitWidth)
                                                 : Number.POSITIVE_INFINITY
        elide: header.titleShrinks ? Text.ElideRight : Text.ElideNone
        text: header.title
        font.pixelSize: Theme.fontTitle
        font.weight: Font.DemiBold
        color: Theme.text
    }
}
