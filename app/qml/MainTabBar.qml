// File/layer: app/qml/MainTabBar.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.phone_*, QmlTest.driveview{,_sheet,_platform} (test_qml)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The phone's tab bar and its slot in Main's column, with the press-through
// that keeps the drive view's revealed bar from hiding under a touch.
// Main reads tabBar's height through `bar`.

Item {
    id: tabSlot
    required property Main host
    property alias bar: tabBar

    // Over a bar in the drive view: a press restarts its hide delay and goes on
    // to what's under it
    component BarTouch: MouseArea {
        enabled: tabSlot.host.driving
        onPressed: mouse => {
            tabSlot.host.dashboardItem.drivePart.barTouched()
            mouse.accepted = false
        }
    }

    visible: tabSlot.host.isCompact
    Layout.fillWidth: true
    Layout.preferredHeight: tabSlot.host.driving ? 0 : tabBar.height
    z: 1
    TabBar {
        id: tabBar
        objectName: "tabBar"
        visible: !tabSlot.host.driving || tabSlot.host.barsShown > 0
        width: parent.width
        height: Theme.touchTarget + Theme.barGap + tabSlot.host.insetBottom
        y: tabSlot.host.driving ? -height * tabSlot.host.barsShown : 0
        spacing: 0
        // The bar's height already holds the bottom inset, painted in
        // its color: the style pads none of it again
        topPadding: 0
        bottomPadding: 0
        leftPadding: 0
        rightPadding: 0

        background: Rectangle {
            objectName: "tabBarBackground"
            color: Theme.surface
            Rectangle {
                objectName: "tabBarHairline"
                anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 1
                color: Theme.hairline
            }
        }

        // The page decides the mark: on the Log no tab is marked, and
        // TabBar's index at -1 leaves the last checked and deaf to a tap
        Repeater {
            model: tabSlot.host.tabs
            delegate: TabButton {
                id: btn
                required property var modelData
                objectName: "tab_" + modelData.page
                text: modelData.label
                implicitHeight: Theme.touchTarget
                checked: tabSlot.host.currentPage === modelData.page
                onClicked: {
                    // A tab in the drive view's revealed bar is Exit drive
                    if (tabSlot.host.driving)
                        tabSlot.host.dashboardItem.drivePart.leaveDrive(true)
                    tabSlot.host.gotoPage(modelData.page)
                }
                // A narrow bar (a small window, or a phone zoomed far
                // in) gets smaller labels, and one that still doesn't
                // fit elides inside its tab: labels never run together
                leftPadding: 4
                rightPadding: 4
                contentItem: Label {
                    textFormat: Text.PlainText
                    objectName: "tabLabel_" + btn.modelData.page
                    text: btn.text
                    font.pixelSize: btn.width < 64 ? Theme.fontCaption : Theme.fontBody
                    font.weight: btn.checked ? Font.DemiBold : Font.Normal
                    color: btn.checked ? Theme.text : Theme.textDim
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                // The active tab is marked, so selection doesn't rest
                // on color alone
                background: Rectangle {
                    color: "transparent"
                    border.width: btn.visualFocus ? 2 : 0
                    border.color: Theme.text
                    Rectangle {
                        objectName: "tabMark_" + btn.modelData.page
                        visible: btn.checked
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        width: 32
                        height: 3
                        color: Theme.nightActive ? Theme.text : Theme.primary
                    }
                }
            }
        }
    }
    BarTouch { y: tabBar.y; width: tabBar.width; height: tabBar.height; visible: tabBar.visible }
}
