// File/layer: app/qml/Main.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.phone, QmlTest.linkhealth (test_qml); BENCH, Manual
//          smoke

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
    // opens at 1280 x 800, or less so it fits the screen (a 1080p laptop
    // at 150 % scaling has 1280 x 720, the taskbar included), and keeps
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
    palette.highlightedText: Theme.textOnPrimary
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

    // A phone keeps its tab bar upright and on its side: the form follows
    // the platform (ADR-0057), not a width that a turn would cross.
    // testMobile stands in for Theme.mobile in a test. A tablet (a short
    // side of 600 dp or more) and a desktop go by the layout's width
    property bool testMobile: false
    // The short side is the window's own dp, not the layout's: a phone
    // zoomed out is still a phone
    readonly property bool phoneForm: (Theme.mobile || testMobile)
                                      && Math.min(root.width, root.height) < 600
    // The layout width, after the zoom
    readonly property bool isCompact: phoneForm || zoomRoot.width < 720
    // A phone on its side, or a window as short (the bars' insets apart)
    readonly property bool isShort: zoomRoot.height - zoomRoot.insetTop - zoomRoot.insetBottom < 480
    Binding { target: Theme; property: "dense"; value: root.isCompact || root.isShort }
    Binding { target: Theme; property: "shortLayout"; value: root.isShort }
    Binding { target: Theme; property: "compact"; value: root.isCompact }

    // The system's bars over the window, per side: Qt's safe area, or a
    // test's stand-in where larger (a test can't set SafeArea). The
    // header and the tab bar paint theirs in their own color.
    // The window's own safe area (an item's SafeArea reads its own overlap)
    // testWindowSafe stands in for it where larger (a test's window has no
    // bars), so zoomRoot's reach over them can be pinned
    property var testWindowSafe: ({ left: 0, top: 0, right: 0, bottom: 0 })
    readonly property var windowSafe: {
        const s = SafeArea.margins
        const t = testWindowSafe
        return { left: Math.max(s.left, t.left), top: Math.max(s.top, t.top),
                 right: Math.max(s.right, t.right), bottom: Math.max(s.bottom, t.bottom) }
    }
    property var testInsets: ({ left: 0, top: 0, right: 0, bottom: 0 })
    readonly property var insets: {
        const s = SafeArea.margins
        const t = testInsets
        return { left: Math.max(s.left, t.left), top: Math.max(s.top, t.top),
                 right: Math.max(s.right, t.right), bottom: Math.max(s.bottom, t.bottom) }
    }
    readonly property int currentPage: stack.currentIndex

    // Page indices, in the order of the sidebar, stack and tab bar
    readonly property int recordingsPage: 5
    readonly property int settingsPage: 6

    function gotoPage(index) { stack.currentIndex = index }
    // Connect... from any page: Settings, with what to connect to open
    function openConnect() {
        settingsView.showAdapters()
        gotoPage(settingsPage)
    }
    // A recording's Replay: its URI in Settings' field, ready to connect
    function replayRecording(uri) {
        settingsView.pickRecording(uri)
        gotoPage(settingsPage)
    }
    // A log's time as m:ss, or h:mm:ss past an hour; 0:00 for a value
    // that isn't a time
    function logTimeText(ms) {
        const s = Number.isFinite(ms) ? Math.max(0, Math.floor(ms / 1000)) : 0
        const two = n => (n < 10 ? "0" : "") + n
        return s >= 3600 ? Math.floor(s / 3600) + ":" + two(Math.floor(s / 60) % 60) + ":" + two(s % 60)
                         : Math.floor(s / 60) + ":" + two(s % 60)
    }
    // After Stop log the log opens in the viewer (ADR-0056 section 1); a
    // log the session's end or a failed write closed leaves the page as
    // it is, since the driver may be reading Live
    Connections {
        target: Bridge
        function onLogStopped(folder, byUser) {
            if (!byUser)
                return
            root.gotoPage(root.recordingsPage)
            recordingsView.openFolder(folder)
        }
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
        property int minSweepMs: 0
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
        Bridge.minSweepMs = uiSettings.minSweepMs   // anything but a choice is ignored
    }
    Connections {
        target: Bridge
        function onUnitSystemChanged() { uiSettings.unitSystem = Bridge.unitSystem }
        function onMinSweepMsChanged() { uiSettings.minSweepMs = Bridge.minSweepMs }
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
        const s = root.insets
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
    // It keeps out of the insets' sides; at the top and the bottom the
    // header and the tab bar reach the window's edge and pad their insets
    // in their own color (Android can draw the window under its status
    // and navigation bars, edge to edge; elsewhere the insets are 0).
    Item {
        id: zoomRoot
        objectName: "zoomRoot"
        readonly property var safe: root.insets
        // The top and bottom insets in the layout's units
        readonly property real insetTop: safe.top / Theme.zoom
        readonly property real insetBottom: safe.bottom / Theme.zoom
        // ApplicationWindow's content item already lies inside Qt's safe
        // area, so this item reaches back out over the top and bottom
        // bars (the header and the tab bar paint them) and takes no side
        // inset a second time
        readonly property var qtSafe: root.windowSafe
        x: 0
        y: -qtSafe.top
        width: parent.width / Theme.zoom
        height: (parent.height + qtSafe.top + qtSafe.bottom) / Theme.zoom
        scale: Theme.zoom
        transformOrigin: Item.TopLeft

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            ToolBar {
                objectName: "headerBar"
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.toolBarHeight + zoomRoot.insetTop
                // The row below pads the inset itself; the style would pad it too
                topPadding: 0
                background: Rectangle {
                    color: Theme.surface
                    Rectangle { // 2px accent underline
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: 2; color: Theme.primary
                    }
                }
                RowLayout {
                    id: headerRow
                    objectName: "headerRow"
                    anchors.fill: parent
                    anchors.topMargin: zoomRoot.insetTop
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    spacing: 12

                    // The name gives way to Mark or the log's controls on a
                    // phone's width
                    Label {
                        id: headerTitle
                        objectName: "headerTitle"
                        visible: !(markButton.visible || startLogButton.visible || Bridge.logActive)
                                 || headerRow.width >= 420
                        text: qsTr("Havne")
                        font.pixelSize: Theme.fontEmphasis
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
                        // A dot and a word, no capsule
                        color: "transparent"

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
                                font.pixelSize: Theme.fontCaption
                                color: Theme.text
                                text: Bridge.connecting ? qsTr("Connecting…")
                                    : Bridge.disconnecting ? qsTr("Disconnecting…")
                                    : !headerTitle.visible ? Theme.linkShortLabel(Bridge.linkState)
                                    : Bridge.connected
                                      ? qsTr("%1 · %2").arg(Theme.linkLabel(Bridge.linkState))
                                                        .arg(Bridge.uriLabel)
                                      : Theme.linkLabel(Bridge.linkState)
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }  // Spacer

                    // A marker in the recording under way, from any page
                    // (ADR-0049 section 4): "Mark N" at the tap, which the
                    // Recordings page aligns two runs on
                    ThemedButton {
                        id: markButton
                        objectName: "markButton"
                        visible: Bridge.recordingActive && !Bridge.logActive
                        text: Bridge.markCount > 0 ? qsTr("Mark (%1)").arg(Bridge.markCount)
                                                   : qsTr("Mark")
                        Accessible.name: qsTr("Mark the recording")
                        onClicked: Bridge.mark()
                    }

                    // The analysis log (ADR-0056 section 1): Live's ticked rows,
                    // recorded from now on and asked alone until Stop log. A
                    // replay plays a recording already; Record sessions' own
                    // recorder is the session's one
                    ThemedButton {
                        id: startLogButton
                        objectName: "startLogButton"
                        highlighted: true
                        visible: Bridge.connected && !Bridge.recordingActive
                                 && !Bridge.currentUri.toLowerCase().startsWith("replay://")
                        enabled: !Bridge.disconnecting && Bridge.signalModel.subscribedCount > 0
                        text: qsTr("Start log")
                        Accessible.name: qsTr("Start a log of the ticked signals")
                        onClicked: Bridge.startLog()
                    }
                    // While a log runs: its time over its folder's size, two
                    // short lines, so a ten-hour log's readout fits beside
                    // the name at 420 px
                    Label {
                        objectName: "logReadout"
                        visible: Bridge.logActive
                        text: qsTr("%1\n%2 MB").arg(root.logTimeText(Bridge.logElapsedMs))
                                                .arg((Bridge.logBytes / (1024 * 1024)).toFixed(1))
                        horizontalAlignment: Text.AlignRight
                        font.pixelSize: Theme.fontCaption
                        color: Theme.text
                    }
                    ThemedButton {
                        objectName: "stopLogButton"
                        highlighted: true
                        visible: Bridge.logActive
                        enabled: !Bridge.disconnecting
                        text: qsTr("Stop log")
                        onClicked: Bridge.stopLog()
                    }

                    // Settings owns connecting: this button opens it, or ends the
                    // session from any page. On Settings itself, its Connect
                    // button is the one to use.
                    // A log's Stop log takes its place while the log runs
                    ThemedButton {
                        objectName: "disconnectButton"
                        // Ending a session is quiet; Connect... is primary
                        flat: Bridge.connected || Bridge.disconnecting
                        highlighted: !flat
                        visible: !Bridge.logActive
                                 && (Bridge.connected || root.currentPage !== root.settingsPage)
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

                // ---- Sidebar (>= 720px): one full-height panel ----
                Rectangle {
                    visible: !root.isCompact
                    Layout.fillHeight: true
                    Layout.preferredWidth: Theme.sidebarWidth
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
                                { label: qsTr("Diagnostics"), icon: "" },
                                { label: qsTr("Log"),       icon: "" },
                                { label: qsTr("Recordings"), icon: "" },
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
                    DiagnosticsPage {
                        objectName: "diagnosticsPage"
                        // The report names the picked vehicle's baseline
                        vehicles: settingsView.vehicles
                        onConnectRequested: root.openConnect()
                    }
                    LogPage { objectName: "logPage" }
                    RecordingsPage {
                        id: recordingsView
                        objectName: "recordingsPage"
                        // A vehicle's baseline: Settings keeps the vehicles
                        vehicles: settingsView.vehicles
                        onReplayRequested: uri => root.replayRecording(uri)
                        onVehiclesEdited: doc => settingsView.saveVehicles(doc)
                    }
                    SettingsPage {
                        id: settingsView
                        objectName: "settingsPage"
                    }
                }
            }

            // A phone's tab bar: targets a touch's size at its top (each tab
            // keeps its own height), then a gap and the system's bottom
            // inset, all in the bar's color, so a tap low on a tab never
            // reaches Back or Home beside it
            TabBar {
                id: tabBar
                objectName: "tabBar"
                visible: root.isCompact
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.touchTarget + Theme.barGap + zoomRoot.insetBottom
                spacing: 0
                // The bar's height already holds the bottom inset, painted in
                // its color: the style pads none of it again
                topPadding: 0
                bottomPadding: 0
                leftPadding: 0
                rightPadding: 0
                currentIndex: stack.currentIndex
                onCurrentIndexChanged: stack.currentIndex = currentIndex

                background: Rectangle { objectName: "tabBarBackground"; color: Theme.surface }

                // per-button styling since TabBar doesn't propagate backgrounds
                component ThemedTabButton: TabButton {
                    id: btn
                    implicitHeight: Theme.touchTarget
                    contentItem: Label {
                        text: btn.text
                        // 13px keeps "Settings" inside a fifth of a 375pt phone
                        font.pixelSize: Theme.fontCaption
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
                ThemedTabButton { text: qsTr("Codes") }
                ThemedTabButton { text: qsTr("Log") }
                ThemedTabButton { text: qsTr("Recs") }
                ThemedTabButton { text: qsTr("Settings") }
            }
            // With no tab bar (the sidebar's width), the pages keep off
            // the bottom inset
            Item {
                visible: !tabBar.visible
                Layout.fillWidth: true
                Layout.preferredHeight: zoomRoot.insetBottom
            }
        }
    }

    // The zoom level, for a moment after it changes
    Rectangle {
        id: zoomBadge
        function show() { opacity = 1; zoomBadgeTimer.restart() }
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        // The content item lies inside the system's bars already
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

    // ---- Developer mode: the window's drawn frames per second, in a
    // corner badge that takes no input. Qt draws only when something
    // changed, so an idle page reads 0 (the badge's own change is a frame
    // at most each second) and a chart being panned or pinched reads its
    // rate: the phone run's measure of the charts and the gauges.
    // frameSwapped comes from the render thread, queued to this one.
    property int framesThisSecond: 0
    property int framesPerSecond: 0
    Connections {
        target: root
        enabled: Theme.developerMode
        function onFrameSwapped() { root.framesThisSecond += 1 }
    }
    Timer {
        interval: 1000
        repeat: true
        running: Theme.developerMode
        onTriggered: {
            root.framesPerSecond = root.framesThisSecond
            root.framesThisSecond = 0
        }
        onRunningChanged: {
            root.framesThisSecond = 0
            root.framesPerSecond = 0
        }
    }
    Rectangle {
        objectName: "frameRate"
        visible: Theme.developerMode
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 8
        // Above the tab bar on a phone, so it covers none of the tabs: the
        // bar's part under the navigation bar lies outside this item's parent
        anchors.bottomMargin: 8 + (tabBar.visible ? tabBar.height * Theme.zoom - root.windowSafe.bottom : 0)
        width: frameRateLabel.implicitWidth + 12
        height: frameRateLabel.implicitHeight + 6
        radius: 4
        color: Theme.elevatedLight
        border.color: Theme.outline
        opacity: 0.85
        Label {
            id: frameRateLabel
            anchors.centerIn: parent
            text: qsTr("%1 frames/s").arg(root.framesPerSecond)
            font.pixelSize: Theme.fontCaption
            color: Theme.textDim
        }
    }
}
