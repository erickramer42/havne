// File/layer: app/qml/PageHeader.qml | Qt6 Quick, UI layer
// Testing: manual smoke — Dashboard, Live, Log and Settings open with
//          it, the titles aligned at Theme.pageMargin

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A page's title row. Items declared inside it follow the title, so a
// page adds its status and actions in place.
RowLayout {
    id: header
    property string title: ""

    Layout.fillWidth: true
    spacing: 10

    Label {
        text: header.title
        font.pixelSize: 20
        font.weight: Font.DemiBold
        color: Theme.text
    }
}
