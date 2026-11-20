// File/layer: app/qml/LogNotice.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard{,_pages,_panel}, QmlTest.dashedit, QmlTest.phone_*,
//          QmlTest.linkhealth, QmlTest.driveview{,_sheet,_platform} (test_qml); BENCH, Manual smoke

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// What the log did without being asked: Start log's refusal, or an
// end the user didn't ask for (a failed write, the session's end), in
// words beside a warning dot, until Dismiss or the next log. Start log
// sits in Main's header, so the bridge holds the words
// (Bridge.logNotice); the header and the drive view's sheet show them
RowLayout {
    id: notice
    required property string name
    readonly property real touchMin: Theme.touchTarget
    objectName: name
    Layout.fillWidth: true
    visible: Bridge.logNotice !== ""
    spacing: 10
    Rectangle {
        implicitWidth: 8; implicitHeight: 8   // layout-managed
        radius: 4
        color: Theme.warning
    }
    Label {
        objectName: notice.name + "Text"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.text
        font.pixelSize: Theme.fontCaption
        text: Bridge.logNotice
    }
    ThemedButton {
        objectName: notice.name + "Dismiss"
        compact: true
        text: qsTr("Dismiss")
        Accessible.name: qsTr("Dismiss the log's notice")
        onClicked: Bridge.clearLogNotice()
    }
}
