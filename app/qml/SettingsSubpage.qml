// File/layer: app/qml/SettingsSubpage.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// One page of Settings: a scrolling column under a header with a back arrow

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The page's controls go in as children. The back arrow shows on a phone
// (when the page was pushed over the list), where it is the way back;
// nothing else depends on it, the back key only repeats it
Flickable {
    id: flick
    property string title
    property bool backShown: false
    property string backName: qsTr("Back to Settings")
    signal backRequested
    default property alias content: column.data

    // The buttons inside take at least this height (ThemedButton)
    readonly property Item body: column
    readonly property real touchMin: Theme.touchTarget
    contentWidth: width
    contentHeight: column.height + 2 * Theme.pageMargin
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {
        policy: flick.contentHeight > flick.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
    }

    ColumnLayout {
        id: column
        objectName: flick.objectName + "Column"
        x: Theme.pageMargin
        y: Theme.pageMargin
        width: flick.width - 2 * Theme.pageMargin
        // Its content's height, not the page's
        height: implicitHeight
        spacing: Theme.spacing * 2

        PageHeader {
            title: flick.title
            ThemedButton {
                objectName: "settingsBack"
                visible: flick.backShown
                text: "<"
                Accessible.name: flick.backName
                implicitWidth: Theme.touchTarget
                implicitHeight: Theme.touchTarget
                onClicked: flick.backRequested()
            }
        }
    }
}
