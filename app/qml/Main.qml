// File/layer: app/qml/Main.qml | Qt6 Quick, UI layer
// Testing: manual smoke — navigation, sidebar↔TabBar at 720px, header
//          pill elides at 400 px, keyboard focus rings, the window opens
//          within a 1280 × 720 screen, zoom steps with Ctrl + / Ctrl − /
//          Ctrl 0 and Settings' Display row and survives a restart, as
//          does the unit system (2026-10-24) and developer mode
//          (2026-10-27); Home's Connect… opens Settings with Adapter
//          unfolded (2026-10-28). The safe area's margins (Android's system
//          bars), the window filling a phone's screen after it turns,
//          and the "Display:" line a phone logs in developer mode:
//          BENCH, the phone run sheet.
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
    // A phone's window fills the screen, and follows it when the phone
    // turns: a size short of the screen leaves bands at the right and the
    // bottom (docs/bench-sessions/2026-10-28-android-4.md). A desktop's
    // opens at 1280 × 800, or less so it fits the screen (a 1080p laptop
    // at 150 % scaling has 1280 × 720, the taskbar included), and keeps
    // it: Component.onCompleted ends the binding, which would resize the
    // window on another monitor
    width: Theme.mobile ? Screen.desktopAvailableWidth
                        : Math.min(1280, Screen.desktopAvailableWidth - 16)
    height: Theme.mobile ? Screen.desktopAvailableHeight
                         : Math.min(800, Screen.desktopAvailableHeight - 48)
    visible: true
    title: qsTr("Havne")
    color: Theme.bg

    // Every role the Basic style reads, so stock controls (CheckBox,
    // TextField, ScrollBar) draw from Theme whatever the OS color
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
    // Connect… from any page: Settings, with what to connect to open
    function openConnect() {
        settingsView.showAdapters()
        gotoPage(settingsPage)
    }

    // ---- Zoom (Theme.zoom), units (Bridge.unitSystem) and developer mode
    // (Theme.developerMode), kept between runs
    Settings {
        id: uiSettings
        category: "ui"
        property real zoom: 1.0
        // Imperial until changed (ADR-0022's appendix of 2026-10-25); the
        // bridge and recordings stay in core's decoded, metric units
        property int unitSystem: 1
        property bool developerMode: false
    }
    property bool zoomRestored: false
    Component.onCompleted: {
        if (!Theme.mobile) {
            // The same size, as a value: the bindings above end
            width = width
            height = height
        }
        Theme.zoom = Theme.clampZoom(uiSettings.zoom)
        zoomRestored = true
        Bridge.unitSystem = uiSettings.unitSystem   // anything but 0 or 1 is ignored
        Theme.developerMode = uiSettings.developerMode
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
        function onDeveloperModeChanged() {
            uiSettings.developerMode = Theme.developerMode
            if (Theme.mobile && Theme.developerMode)
                displayTimer.restart()
        }
    }
    // Ctrl + is Ctrl Shift = on most layouts, so Ctrl = counts too
    Shortcut { sequences: [StandardKey.ZoomIn, "Ctrl+="]; onActivated: Theme.stepZoom(1) }
    Shortcut { sequences: [StandardKey.ZoomOut]; onActivated: Theme.stepZoom(-1) }
    Shortcut { sequence: "Ctrl+0"; onActivated: Theme.zoom = 1 }

    // ---- A phone in developer mode logs its display: the screen, the
    // window and the safe area's margins, once they settle after a change.
    // After a rotation the window stopped short of the screen
    // (docs/bench-sessions/2026-10-28-android-3.md); this says which
    // size didn't follow
    function orientationName(o) {
        return o === Qt.PortraitOrientation ? "portrait"
             : o === Qt.LandscapeOrientation ? "landscape"
             : o === Qt.InvertedPortraitOrientation ? "portrait, inverted"
             : o === Qt.InvertedLandscapeOrientation ? "landscape, inverted" : "unknown"
    }
    readonly property string displayLine: {
        const s = zoomRoot.safe
        return "Display: screen " + Screen.width + " × " + Screen.height
             + " (" + orientationName(Screen.orientation) + ", " + Screen.devicePixelRatio + "x)"
             + ", available " + Screen.desktopAvailableWidth + " × " + Screen.desktopAvailableHeight
             + ", window " + root.width + " × " + root.height + " at " + root.x + ", " + root.y
             + ", safe area left " + Math.round(s.left) + ", top " + Math.round(s.top)
             + ", right " + Math.round(s.right) + ", bottom " + Math.round(s.bottom)
    }
    onDisplayLineChanged: if (Theme.mobile && Theme.developerMode) displayTimer.restart()
    Timer {
        id: displayTimer
        interval: 500
        onTriggered: Bridge.logEvent(root.displayLine)
    }

    // The whole UI, laid out at the window's size divided by the zoom and
    // scaled by it, so pages reflow as if the window were that size.
    // Shapes and text are vector-drawn, so they stay sharp at any zoom.
    // It keeps out of the safe area's margins: Android draws the window
    // under its status and navigation bars (edge to edge), where the
    // window's color shows; elsewhere the margins are 0.
    Item {
        id: zoomRoot
        readonly property var safe: parent.SafeArea.margins
        x: safe.left
        y: safe.top
        width: (parent.width - safe.left - safe.right) / Theme.zoom
        height: (parent.height - safe.top - safe.bottom) / Theme.zoom
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
                        text: qsTr("Havne")
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
                                                        .arg(Bridge.uriLabel)
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
                                                    : root.openConnect()
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
                        onConnectRequested: root.openConnect()
                    }
                    DashboardPage { objectName: "dashboardPage" }
                    LiveViewPage {
                        objectName: "livePage"
                        onConnectRequested: root.openConnect()
                    }
                    LogPage { objectName: "logPage" }
                    SettingsPage {
                        id: settingsView
                        objectName: "settingsPage"
                    }
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
                    // on color alone
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
        anchors.bottomMargin: 24 + zoomRoot.safe.bottom
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
