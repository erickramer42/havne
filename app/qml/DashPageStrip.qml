// File/layer: app/qml/DashPageStrip.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit (test_qml)

import QtQuick

pragma ComponentBehavior: Bound

// The desktop Dashboard's pages as tabs on the panel's upper edge
// (ADR-0057's appendix "the desktop takes the panel"). The shown tab is
// the open sheet: its fill and edge run down over the panel's top
// hairline, so tab and panel read as one. In edit mode a "+" tab ends the
// row and adds a page. A row wider than the strip scrolls sideways, by
// the wheel too, and follows the shown tab; there is no second row.
Item {
    id: strip
    objectName: "dashPageStrip"

    property var pages: []
    property int current: 0
    property bool editing: false
    property bool canAddPage: false
    property real hairlineWidth: 1

    signal pageChosen(int index)
    signal addRequested()

    // The touch size where Theme.compact asks for it (qml.md's density rule)
    readonly property real tabHeight: Math.max(36, Theme.touchTarget)
    implicitHeight: tabHeight

    function nameOf(i) {
        const p = pages[i]
        return p && p.name ? p.name : qsTr("Page %1").arg(i + 1)
    }
    function tab(i) { return tabs.itemAt(i) as Item }
    function holdsFocus() {
        for (let p = Window.activeFocusItem; p; p = p.parent) {
            if (p === strip)
                return true
        }
        return false
    }
    function focusTab(i) {
        const t = tab(i)
        if (t)
            t.forceActiveFocus()
    }
    // Scrolls the row just far enough to show tab i whole
    function ensureVisible(i) {
        const t = tab(i)
        if (!t || view.width <= 0)
            return
        const most = Math.max(0, view.contentWidth - view.width)
        let x = view.contentX
        if (t.x < x)
            x = t.x
        else if (t.x + t.width > x + view.width)
            x = t.x + t.width - view.width
        x = Math.max(0, Math.min(most, x))
        if (x === view.contentX)
            return
        slide.to = x
        slide.restart()
    }
    // After the row has laid out the change; the strip's own timer, so a
    // strip destroyed meanwhile (the form changed) never runs it
    onCurrentChanged: follow.restart()
    onWidthChanged: follow.restart()
    onPagesChanged: follow.restart()
    Timer {
        id: follow
        interval: 0
        onTriggered: strip.ensureVisible(strip.current)
    }

    Flickable {
        id: view
        objectName: "dashPageStripView"
        anchors { left: parent.left; right: parent.right; top: parent.top }
        // A hairline past the strip's foot, where the shown tab covers the panel's edge
        height: parent.height + strip.hairlineWidth
        clip: true
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        contentWidth: row.width
        contentHeight: height
        // The shown tab's bolder name widens the row after a change
        onContentWidthChanged: follow.restart()

        NumberAnimation {
            id: slide
            target: view
            property: "contentX"
            duration: 120
            easing.type: Easing.OutCubic
        }

        // The wheel moves the row sideways, either way it turns; in the view,
        // ahead of the Flickable, which takes a wheel it can't scroll by
        WheelHandler {
            objectName: "dashPageStripWheel"
            target: null
            onWheel: (event) => {
                const d = event.angleDelta.x !== 0 ? event.angleDelta.x : event.angleDelta.y
                const most = Math.max(0, view.contentWidth - view.width)
                slide.stop()
                view.contentX = Math.max(0, Math.min(most, view.contentX - d / 120 * 48))
            }
        }

        Row {
            id: row
            Repeater {
                id: tabs
                model: strip.pages.length
                ThemedButton {
                    id: tabButton
                    required property int index
                    objectName: "dashPageTab_" + index
                    checkable: true
                    checked: index === strip.current
                    text: strip.nameOf(index)
                    Accessible.name: text
                    width: Math.max(64, Math.min(220, label.implicitWidth + 32))
                    height: strip.tabHeight
                    leftPadding: 16
                    rightPadding: 16
                    onClicked: strip.pageChosen(index)

                    background: Item {
                        // The open sheet: the panel's surface and edge, its foot
                        // clipped a hairline below the tab, over the panel's top line
                        Item {
                            objectName: "tabSheet"
                            visible: tabButton.checked
                            width: parent.width
                            height: parent.height + strip.hairlineWidth
                            clip: true
                            Rectangle {
                                width: parent.width
                                height: parent.height + Theme.radiusCard + strip.hairlineWidth
                                color: Theme.surface
                                radius: Theme.radiusCard
                                border.color: Theme.hairline
                                border.width: strip.hairlineWidth
                            }
                        }
                        Rectangle {
                            anchors.fill: parent
                            visible: tabButton.visualFocus
                            color: "transparent"
                            radius: Theme.radius
                            border.color: Theme.text
                            border.width: 2
                        }
                    }
                    contentItem: Text {
                        id: label
                        text: tabButton.text
                        elide: Text.ElideRight
                        maximumLineCount: 1
                        wrapMode: Text.NoWrap
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: Theme.fontBody
                        font.weight: tabButton.checked ? Font.DemiBold : Font.Normal
                        color: tabButton.checked || tabButton.hovered ? Theme.text : Theme.textDim
                    }
                }
            }
            ThemedButton {
                objectName: "dashPageAdd"
                visible: strip.editing
                enabled: strip.canAddPage
                flat: true
                width: 48
                height: strip.tabHeight
                text: "+"
                Accessible.name: qsTr("Add a page")
                onClicked: strip.addRequested()
            }
        }
    }
}
