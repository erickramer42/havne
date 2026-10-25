// File/layer: app/qml/Main.qml | Qt6 Quick, UI layer
// Testing: manual smoke — navigation, sidebar↔TabBar at 720px, header
//          pill elides at 400 px, keyboard focus rings, the window opens
//          within a 1280 × 720 screen, zoom steps with Ctrl + / Ctrl − /
//          Ctrl 0 and Settings' Display row and survives a restart, as
//          does the unit system (2026-10-24).
//          test_qml (tests/qml/tst_linkhealth.qml) checks the pill and
//          each page's link surfaces in every link state, finding the
//          pages and the pill by objectName

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ApplicationWindow {
    id: root
    // 1280 × 800, or less so the window opens within the screen: a 1080p
    // laptop at 150 % scaling has 1280 × 720, the taskbar included
    width: Math.min(1280, Screen.desktopAvailableWidth - 16)
    height: Math.min(800, Screen.desktopAvailableHeight - 48)
    visible: true
    title: qsTr("QtTune")
    color: Theme.bg

    // Every role the Basic style reads, so stock controls (CheckBox,
    // TextField, ScrollBar) draw from Theme whatever the OS colour
    // scheme is
    palette.window: Theme.bg
    palette.windowText: Theme.text
    palette.base: Theme.elevated
    palette.alternateBase: Theme.surface
    palette.text: Theme.text
    palette.button: Theme.elevated
    palette.buttonText: Theme.text
    palette.brightText: Theme.text
    palette.highlight: Theme.primary
    palette.highlightedText: Theme.text
    palette.placeholderText: Theme.textDim
    palette.toolTipBase: Theme.elevatedLight
    palette.toolTipText: Theme.text
    palette.light: Theme.elevatedLight
    palette.midlight: Theme.elevatedLight
    palette.mid: Theme.outline
    palette.dark: Theme.textDim
    palette.shadow: "#000000"
    palette.link: Theme.info
    palette.linkVisited: Theme.info
    palette.disabled.windowText: Theme.textDim
    palette.disabled.text: Theme.textDim
    palette.disabled.buttonText: Theme.textDim

    // The layout width, after the zoom
    readonly property bool isCompact: zoomRoot.width < 720
    readonly property int currentPage: stack.currentIndex

    // Page indices, in the order of the sidebar, stack and tab bar
    readonly property int settingsPage: 4

    function gotoPage(index) { stack.currentIndex = index }

    // ---- Zoom (Theme.zoom) and units (Bridge.unitSystem), kept between runs
    Settings {
        id: uiSettings
        category: "ui"
        property real zoom: 1.0
        // Imperial until changed (ADR-0022's appendix of 2026-10-25); the
        // bridge and recordings stay in core's decoded, metric units
        property int unitSystem: 1
    }
    property bool zoomRestored: false
    Component.onCompleted: {
        Theme.zoom = Theme.clampZoom(uiSettings.zoom)
        zoomRestored = true
        Bridge.unitSystem = uiSettings.unitSystem   // anything but 0 or 1 is ignored
    }
    Connections {
        target: Bridge
        function onUnitSystemChanged() { uiSettings.unitSystem = Bridge.unitSystem }
    }
    Connections {
        target: Theme
        function onZoomChanged() {
            uiSettings.zoom = Theme.zoom
            if (root.zoomRestored)
                zoomBadge.show()
        }
    }
    // Ctrl + is Ctrl Shift = on most layouts, so Ctrl = counts too
    Shortcut { sequences: [StandardKey.ZoomIn, "Ctrl+="]; onActivated: Theme.stepZoom(1) }
    Shortcut { sequences: [StandardKey.ZoomOut]; onActivated: Theme.stepZoom(-1) }
    Shortcut { sequence: "Ctrl+0"; onActivated: Theme.zoom = 1 }

    // The whole UI, laid out at the window's size divided by the zoom and
    // scaled by it, so pages reflow as if the window were that size.
    // Shapes and text are vector-drawn, so they stay sharp at any zoom.
    Item {
        id: zoomRoot
        width: parent.width / Theme.zoom
        height: parent.height / Theme.zoom
        scale: Theme.zoom
        transformOrigin: Item.TopLeft

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            ToolBar {
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                background: Rectangle {
                    color: Theme.surface
                    Rectangle { // 2px accent underline
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: 2; color: Theme.primary
                    }
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    spacing: 12

                    Label {
                        text: qsTr("QtTune")
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: Theme.text
                    }

                    // Connection pill: link state and URI, from Theme like every
                    // other surface. It shrinks and elides before anything else
                    // moves, so the header fits a phone.
                    Rectangle {
                        id: pill
                        objectName: "linkPill"
                        readonly property color tone: (Bridge.connecting || Bridge.disconnecting)
                                                    ? Theme.textDim
                                                    : Theme.linkColor(Bridge.linkState)
                        Layout.fillWidth: true
                        Layout.preferredWidth: pillRow.implicitWidth + 24
                        Layout.maximumWidth: pillRow.implicitWidth + 24
                        Layout.minimumWidth: 32
                        implicitHeight: 28
                        radius: height / 2
                        color: Qt.alpha(pill.tone, 0.14)

                        RowLayout {
                            id: pillRow
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 8

                            Rectangle {
                                implicitWidth: 8; implicitHeight: 8   // layout-managed
                                radius: 4
                                color: pill.tone
                            }
                            Label {
                                objectName: "linkPillText"
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                font.pixelSize: 13
                                color: pill.tone
                                text: Bridge.connecting ? qsTr("Connecting…")
                                    : Bridge.disconnecting ? qsTr("Disconnecting…")
                                    : Bridge.connected
                                      ? qsTr("%1 · %2").arg(Theme.linkLabel(Bridge.linkState))
                                                        .arg(Bridge.currentUri)
                                      : Theme.linkLabel(Bridge.linkState)
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }  // Spacer

                    // Settings owns connecting: this button opens it, or ends the
                    // session from any page. On Settings itself, its Connect
                    // button is the one to use.
                    ThemedButton {
                        visible: Bridge.connected || root.currentPage !== root.settingsPage
                        // A teardown can take seconds with a wedged adapter; no
                        // connect starts until it ends
                        enabled: !Bridge.connecting && !Bridge.disconnecting
                        text: Bridge.connected ? qsTr("Disconnect")
                            : Bridge.disconnecting ? qsTr("Disconnecting…")
                            : qsTr("Connect…")
                        onClicked: Bridge.connected ? Bridge.disconnectSession()
                                                    : root.gotoPage(root.settingsPage)
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // ---- Sidebar (≥720px): one full-height panel ----
                Rectangle {
                    visible: !root.isCompact
                    Layout.fillHeight: true
                    Layout.preferredWidth: 220
                    color: Theme.surface

                    Rectangle {   // hairline against the page
                        anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
                        width: 1
                        color: Theme.elevated
                    }

                    ColumnLayout {
                        anchors { left: parent.left; right: parent.right; top: parent.top }
                        anchors.topMargin: 8
                        anchors.rightMargin: 1
                        spacing: 2

                        Repeater {
                            model: [
                                { label: qsTr("Home"), icon: "" },
                                { label: qsTr("Dashboard"), icon: "" },
                                { label: qsTr("Live"),   icon: "" },
                                { label: qsTr("Log"),       icon: "" },
                                { label: qsTr("Settings"),  icon: "" }
                            ]
                            delegate: NavButton {
                                id: navBtn
                                required property var modelData
                                required property int index
                                Layout.fillWidth: true
                                label: modelData.label
                                active: stack.currentIndex === index
                                onClicked: stack.currentIndex = index
                            }
                        }
                    }
                }

                StackLayout {
                    id: stack
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    HomePage {
                        objectName: "homePage"
                        onConnectRequested: root.gotoPage(root.settingsPage)
                    }
                    DashboardPage { objectName: "dashboardPage" }
                    LiveViewPage {
                        objectName: "livePage"
                        onConnectRequested: root.gotoPage(root.settingsPage)
                    }
                    LogPage { objectName: "logPage" }
                    SettingsPage { objectName: "settingsPage" }
                }
            }

            TabBar {
                visible: root.isCompact
                Layout.fillWidth: true
                currentIndex: stack.currentIndex
                onCurrentIndexChanged: stack.currentIndex = currentIndex

                background: Rectangle { color: Theme.surface }

                // per-button styling since TabBar doesn't propagate backgrounds
                component ThemedTabButton: TabButton {
                    id: btn
                    contentItem: Label {
                        text: btn.text
                        // 13px keeps "Settings" inside a fifth of a 375pt phone
                        font.pixelSize: 13
                        font.weight: btn.checked ? Font.DemiBold : Font.Normal
                        color: btn.checked ? Theme.text : Theme.textDim
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    // The selected tab is underlined, so selection doesn't rest
                    // on colour alone
                    background: Rectangle {
                        color: Theme.surface
                        border.width: btn.visualFocus ? 2 : 0
                        border.color: Theme.text
                        Rectangle {
                            visible: btn.checked
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 6
                            width: Math.min(parent.width - 16, btn.contentItem.implicitWidth + 8)
                            height: 3
                            radius: 1.5
                            color: Theme.primary
                        }
                    }
                }

                ThemedTabButton { text: qsTr("Home") }
                ThemedTabButton { text: qsTr("Dash") }
                ThemedTabButton { text: qsTr("Live") }
                ThemedTabButton { text: qsTr("Log") }
                ThemedTabButton { text: qsTr("Settings") }
            }
        }
    }

    // The zoom level, for a moment after it changes
    Rectangle {
        id: zoomBadge
        function show() { opacity = 1; zoomBadgeTimer.restart() }
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        width: zoomBadgeLabel.implicitWidth + 32
        height: 36
        radius: 18
        color: Theme.elevatedLight
        border.color: Theme.outline
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        Label {
            id: zoomBadgeLabel
            anchors.centerIn: parent
            text: qsTr("Zoom %1 %").arg(Math.round(Theme.zoom * 100))
            color: Theme.text
        }
        Timer { id: zoomBadgeTimer; interval: 1200; onTriggered: zoomBadge.opacity = 0 }
    }
}
