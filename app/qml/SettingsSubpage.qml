// File/layer: app/qml/SettingsSubpage.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// One page of Settings: a scrolling column under a pinned title row with
// a back arrow

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
    // The title row's height: the content starts under it, and a scroll
    // that brings an item into view leaves this much above it
    readonly property real headerHeight: headerBar.height
    contentWidth: width
    contentHeight: headerBar.height + column.height + Theme.pageMargin
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {
        policy: flick.contentHeight > flick.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
    }

    // The title row stays at the top while the page scrolls under it: it
    // rides with contentY, over the column, so a long page's way back is
    // never scrolled away
    Rectangle {
        id: headerBar
        objectName: flick.objectName + "Header"
        y: flick.contentY
        z: 1
        width: flick.width
        height: Theme.pageMargin + headerRow.implicitHeight + Theme.spacing * 2
        color: Theme.bg

        RowLayout {
            id: headerRow
            x: Theme.pageMargin
            y: Theme.pageMargin
            width: parent.width - 2 * Theme.pageMargin
            spacing: 10

            // Quiet, at the left edge, where a back control is looked for
            ThemedButton {
                objectName: "settingsBack"
                visible: flick.backShown
                flat: true
                text: "←"   // a left arrow
                Accessible.name: flick.backName
                implicitWidth: Theme.touchTarget
                implicitHeight: Theme.touchTarget
                onClicked: flick.backRequested()
            }
            PageHeader {
                title: flick.title
            }
        }
    }

    ColumnLayout {
        id: column
        objectName: flick.objectName + "Column"
        x: Theme.pageMargin
        y: headerBar.height
        width: flick.width - 2 * Theme.pageMargin
        // Its content's height, not the page's
        height: implicitHeight
        spacing: Theme.spacing * 2
    }
}
