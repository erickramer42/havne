// File/layer: app/qml/Main.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.phone, QmlTest.linkhealth, QmlTest.driveview (test_qml);
//          BENCH, Manual smoke

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

    // The drive view (ADR-0059) is the Dash page's state; the bars here
    // follow it, and in the view they slide in over the gauges
    readonly property bool driving: dashboardView.driving
    readonly property real barsShown: dashboardView.barsShown

    // Page indices, in the order of the sidebar and the stack
    readonly property int homePage: 0
    readonly property int dashboardPage: 1
    readonly property int livePage: 2
    readonly property int logPage: 4
    readonly property int recordingsPage: 5
    readonly property int settingsPage: 6

    // The tab bar's pages, in its order
    readonly property var tabs: [
        { label: qsTr("Dash"), page: dashboardPage },
        { label: qsTr("Live"), page: livePage },
        { label: qsTr("Codes"), page: 3 },
        { label: qsTr("Recs"), page: recordingsPage },
        { label: qsTr("Settings"), page: settingsPage }
    ]

    function gotoPage(index) { stack.currentIndex = index }
    // The tab bar lists no Home: a phone opens on Dash, and a window
    // narrowed past the sidebar's width leaves Home for it
    function leaveHomeForTheTabs() {
        if (isCompact && stack.currentIndex === 0)
            gotoPage(dashboardPage)
    }
    onIsCompactChanged: leaveHomeForTheTabs()
    // The page the one shown was reached from, for Back
    property int cameFrom: -1
    property int lastPage: 0
    // Where Settings was reached from while the Log, opened from it, is up:
    // the Log's way back must not make the Log what Settings goes back to
    property int cameFromBeforeLog: -1
    onCurrentPageChanged: {
        const backFromLog = lastPage === logPage && currentPage === settingsPage
                            && cameFrom === settingsPage
        if (currentPage === logPage && lastPage === settingsPage)
            cameFromBeforeLog = cameFrom
        cameFrom = backFromLog ? cameFromBeforeLog : lastPage
        lastPage = currentPage
        // Settings' Back leaving for the page that sent it survives a trip
        // to the Log (Show log beside a refusal), and nothing else
        if (settingsView && currentPage !== logPage && currentPage !== settingsPage)
            settingsView.backLeaves = false
        // A hidden page can keep the key focus, and then Back reaches nothing
        if (Theme.mobile || testMobile)
            zoomRoot.forceActiveFocus()
    }
    // Back out of Settings to the page that sent the user (Other address...)
    function leaveSettings() {
        const to = cameFrom
        settingsView.backLeaves = false
        settingsView.closeShown()
        if (to >= 0)
            gotoPage(to)
    }
    // Android's Back: true when it did something here. False leaves it to the
    // system, which goes to the launcher and keeps the session
    function goBack() {
        if (currentPage === dashboardPage)
            return dashboardView.goBack()
        if (currentPage === settingsPage) {
            if (!settingsView.backLeaves)
                return settingsView.closeShown()
            leaveSettings()
            return true
        }
        if (currentPage === logPage && cameFrom >= 0) {
            gotoPage(cameFrom)
            return true
        }
        if (currentPage === recordingsPage && recordingsView.viewing) {
            recordingsView.backToList()
            return true
        }
        return false
    }
    // A page shown again, or a control that hid, leaves no item with the key
    // focus and Back would never arrive. A window losing the front loses it
    // too, and a field gets it back only if nothing took it meanwhile
    onActiveFocusItemChanged: {
        const f = activeFocusItem
        if (active && (Theme.mobile || testMobile)
                && (f === null || f === contentItem || f.parent === null))
            zoomRoot.forceActiveFocus()
    }
    // Connect... from any page: Settings, with what to connect to open
    function openConnect() {
        settingsView.showAdapters()
        gotoPage(settingsPage)
    }
    // A connect list on screen: the ports, interfaces and Bluetooth adapters
    // read again, Bluetooth's without asking for its permission
    function listAdapters() {
        settingsView.refreshDevices()
        Bluetooth.refresh(false)
    }
    // Settings' Log row and Show log beside a failed connect: the Log at its
    // newest line; Back returns to the page that asked
    function openLog() {
        logView.showActivityEnd()
        gotoPage(logPage)
    }
    // The Log's back arrow goes where Back goes, and to Dash from no tab
    readonly property var logBackTab: tabs.find(t => t.page === cameFrom) ?? tabs[0]
    function logBack() {
        if (logBackTab.page !== cameFrom || !goBack())
            gotoPage(dashboardPage)
    }
    // A connect list's "Other address..." (Dash's, Home's): Back returns to
    // that page, not to Settings' list
    function openConnectFromDash() {
        openConnect()
        settingsView.backLeaves = true
    }
    // A recording's Replay connects it in one tap: the address goes through
    // Settings' field, as a typed one would, and the result picks the page:
    // Live once it connects, Settings' Connection page with the field's
    // error when the core refuses it (the folder gone, the adapter held)
    property string replayPending: ""
    function replayRecording(uri) {
        settingsView.pickRecording(uri)
        replayPending = uri
        settingsView.requestConnect(uri)
    }
    Connections {
        target: Bridge
        function onConnectResult(uri, error) {
            // A connect that worked on Connection, reached by Other address...,
            // returns to the page that sent it, where the gauges are
            if (error === "" && root.currentPage === root.settingsPage && settingsView.backLeaves
                    && uri !== root.replayPending) {
                root.leaveSettings()
                return
            }
            if (root.replayPending === "" || uri !== root.replayPending)
                return
            root.replayPending = ""
            if (error === "") {
                root.gotoPage(root.livePage)
                return
            }
            root.gotoPage(root.settingsPage)
            // Back from the refusal returns to Recordings, as from Other address
            settingsView.backLeaves = true
        }
    }
    // A log's time as m:ss, or h:mm:ss past an hour; 0:00 for a value
    // that isn't a time
    function logTimeText(ms) {
        const s = Number.isFinite(ms) ? Math.max(0, Math.floor(ms / 1000)) : 0
        const two = n => (n < 10 ? "0" : "") + n
        return s >= 3600 ? Math.floor(s / 3600) + ":" + two(Math.floor(s / 60) % 60) + ":" + two(s % 60)
                         : Math.floor(s / 60) + ":" + two(s % 60)
    }
    // The header's readout of a running log: its time, then its size from
    // 0.1 MB on (under that the size reads 0.0 and says nothing), beside
    // the time on one line or under it
    readonly property int logSizeShownBytes: 104858
    readonly property bool logReadoutOneLine: headerRow.width >= 560
    // The header's short state word: "Connected" ("Replaying" for a
    // replay) where the other pages' short label says "Live", which is a tab's name
    function shortStatusWord(state) {
        if (state !== "ok")
            return Theme.linkShortLabel(state)
        return Bridge.currentUri.toLowerCase().startsWith("replay://") ? qsTr("Replaying")
                                                                       : qsTr("Connected")
    }
    // From 720 px of header the readout says what the time is, and Mark
    // stands beside Stop log
    readonly property bool headerHasRoom: headerRow.width >= 720
    function logReadoutText(ms, bytes, oneLine) {
        const time = logTimeText(ms)
        const size = logSizeText(bytes)
        if (size === "")
            return time
        return oneLine ? qsTr("%1 · %2").arg(time).arg(size) : time + "\n" + size
    }
    // The log's size from 0.1 MB on, "" under it; the drive view's line too
    function logSizeText(bytes) {
        return bytes >= logSizeShownBytes ? qsTr("%1 MB").arg((bytes / (1024 * 1024)).toFixed(1)) : ""
    }
    // After Stop log the log opens in the viewer (ADR-0056 section 1); a
    // log the session's end or a failed write closed leaves the page as
    // it is, since the driver may be reading Live
    Connections {
        target: Bridge
        function onLogStopped(folder, byUser) {
            if (!byUser || root.driving)
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
        leaveHomeForTheTabs()
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

    // ---- A phone in developer mode logs its display's form and
    // orientation when either changes; sizes, scale and insets stay out
    // (a shared log or report would narrow the device)
    function orientationName(o) {
        return o === Qt.PortraitOrientation ? "portrait"
             : o === Qt.LandscapeOrientation ? "landscape"
             : o === Qt.InvertedPortraitOrientation ? "portrait, inverted"
             : o === Qt.InvertedLandscapeOrientation ? "landscape, inverted" : "unknown"
    }
    readonly property string displayLine: "Display: " + (root.phoneForm ? "phone" : "desktop")
        + " form, " + orientationName(Screen.orientation)
    onDisplayLineChanged: if (Theme.mobile && Theme.developerMode) displayTimer.restart()
    Timer {
        id: displayTimer
        interval: 500
        onTriggered: Bridge.logEvent(root.displayLine)
    }

    // On a phone a header button is a touch's size each way (headerRow's
    // touchMin) and draws smaller, so the bar's rule shows under it
    component HeaderButton: ThemedButton {
        compact: Theme.compact
        Layout.minimumWidth: Theme.compact ? Theme.touchTarget : 0
        topInset: Theme.compact ? 6 : 0
        bottomInset: Theme.compact ? 6 : 0
    }

    // Over a bar in the drive view: a press restarts its hide delay and goes on
    // to what's under it
    component BarTouch: MouseArea {
        enabled: root.driving
        onPressed: mouse => {
            dashboardView.barTouched()
            mouse.accepted = false
        }
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
        // The back key, from anything that holds the focus in here; the
        // window's focus is kept on this item when nothing else holds it
        focus: true
        Keys.onBackPressed: event => {
            // A held key repeats, and one press is one Back
            event.accepted = event.isAutoRepeat || root.goBack()
        }
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

            // The bar's place in the column: the drive view gives it to the
            // page and never shows the bar (its controls rise from the
            // bottom in the page's sheet)
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: root.driving ? 0 : headerBar.height
                z: 1
                ToolBar {
                    id: headerBar
                    objectName: "headerBar"
                    width: parent.width
                    height: Theme.toolBarHeight + zoomRoot.insetTop
                    visible: !root.driving
                    // The row below pads the inset itself; the style would pad it too
                    topPadding: 0
                    background: Rectangle {
                        color: Theme.surface
                        // The bar's rule: a hairline, as every other divider
                        Rectangle {
                            objectName: "headerRule"
                            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                            height: 1; color: Theme.hairline
                        }
                    }
                    RowLayout {
                        id: headerRow
                        objectName: "headerRow"
                        readonly property real touchMin: root.isCompact ? Theme.touchTarget : 0
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

                        // The link state and address: the address elides first, the
                        // word only at a narrow phone's top zoom. With the tab bar a
                        // tap opens the Log it omits
                        AbstractButton {
                            id: pill
                            objectName: "linkPill"
                            readonly property color tone: (Bridge.connecting || Bridge.disconnecting)
                                                        ? Theme.textDim
                                                        : Theme.linkColor(Bridge.linkState)
                            readonly property string stateWord:
                                Bridge.connecting ? qsTr("Connecting…")
                              : Bridge.disconnecting ? qsTr("Disconnecting…")
                              : root.isCompact || !headerTitle.visible
                                ? root.shortStatusWord(Bridge.linkState)
                                : Theme.linkLabel(Bridge.linkState)
                            readonly property string address: Bridge.connected ? Bridge.uriLabel : ""
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: implicitWidth
                            Layout.maximumWidth: implicitWidth
                            Layout.minimumWidth: 32
                            implicitWidth: implicitContentWidth + leftPadding + rightPadding
                            implicitHeight: implicitContentHeight
                            leftPadding: root.isCompact ? 0 : 12
                            rightPadding: root.isCompact ? 0 : 12
                            topPadding: 0
                            bottomPadding: 0
                            enabled: root.isCompact
                            text: address !== "" ? qsTr("%1 · %2").arg(stateWord).arg(address) : stateWord
                            // What the pill shows, the recording note included
                            readonly property string spoken: recordingNote.visible
                                                             ? text + " " + recordingNote.text : text
                            Accessible.name: enabled ? qsTr("%1: show the log").arg(spoken) : spoken
                            onClicked: root.gotoPage(root.logPage)
                            // A dot and a word at rest, no capsule
                            background: Rectangle {
                                color: pill.down ? Theme.elevated : "transparent"
                                radius: Theme.radius
                                border.width: pill.visualFocus ? 2 : 0
                                border.color: Theme.text
                            }
                            contentItem: RowLayout {
                                spacing: 8

                                Rectangle {
                                    implicitWidth: 8; implicitHeight: 8   // layout-managed
                                    radius: 4
                                    color: pill.tone
                                }
                                // The address gives way first; the word only alone in a
                                // row under 240 units (a narrow phone at the top zoom)
                                Label {
                                    objectName: "linkPillText"
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: pill.address !== "" || headerRow.width >= 240
                                                         ? implicitWidth : 0
                                    Layout.maximumWidth: implicitWidth
                                    elide: Text.ElideRight
                                    font.pixelSize: Theme.fontCaption
                                    color: Theme.text
                                    text: pill.stateWord
                                }
                                Label {
                                    objectName: "linkPillAddress"
                                    visible: pill.address !== ""
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    elide: Text.ElideRight
                                    font.pixelSize: Theme.fontCaption
                                    color: Theme.textDim
                                    text: qsTr("· %1").arg(pill.address)
                                }
                                // Record sessions on: the session is being recorded
                                // already, which is why Mark stands where Start log
                                // would; the address gives way to the word
                                Label {
                                    id: recordingNote
                                    objectName: "linkPillRecording"
                                    visible: Bridge.connected && Bridge.recordingActive && !Bridge.logActive
                                    font.pixelSize: Theme.fontCaption
                                    color: Theme.textDim
                                    text: qsTr("· recording")
                                }
                                // Marks the status as a button where it opens the Log
                                Label {
                                    objectName: "linkPillChevron"
                                    // Not where it would squeeze a connected phone's address
                                    visible: pill.enabled && (pill.address === "" || headerRow.width >= 420)
                                    Accessible.ignored: true
                                    font.pixelSize: Theme.fontCaption
                                    color: Theme.textDim
                                    text: "›"   // a right angle quotation mark
                                }
                            }
                        }

                        // Developer mode: the window's frames drawn a second, in
                        // the bar so it covers no page (root.framesPerSecond); not
                        // in a row too narrow to hold it beside the pill's word
                        Label {
                            id: frameRate
                            objectName: "frameRate"
                            visible: Theme.developerMode && headerRow.width >= 340
                            font.pixelSize: Theme.fontCaption
                            font.features: Theme.valueFeatures
                            color: Theme.textDim
                            text: qsTr("%1 frames/s").arg(root.framesPerSecond)
                        }

                        Item { Layout.fillWidth: true }  // Spacer

                        // A marker in the recording under way (a log is one: beside
                        // Stop log from 720 px of header), from any page
                        // (ADR-0049 section 4): "Mark N" at the tap, which the
                        // Recordings page aligns two runs on
                        HeaderButton {
                            id: markButton
                            objectName: "markButton"
                            visible: Bridge.recordingActive && (!Bridge.logActive || root.headerHasRoom)
                            text: Bridge.markCount > 0 ? qsTr("Mark (%1)").arg(Bridge.markCount)
                                                       : qsTr("Mark")
                            Accessible.name: qsTr("Mark the recording")
                            onClicked: Bridge.mark()
                        }

                        // The analysis log (ADR-0056 section 1): Live's ticked rows,
                        // recorded from now on and asked alone until Stop log. A
                        // replay plays a recording already; Record sessions' own
                        // recorder is the session's one
                        HeaderButton {
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
                        // While a log runs: its time, and its folder's size once
                        // there is one to read; one line where the row has room,
                        // else the size under the time, so a ten-hour log's
                        // readout fits beside the name at 420 px
                        Label {
                            objectName: "logReadout"
                            visible: Bridge.logActive
                            text: root.headerHasRoom
                                  ? qsTr("Logging %1").arg(root.logReadoutText(
                                        Bridge.logElapsedMs, Bridge.logBytes, true))
                                  : root.logReadoutText(Bridge.logElapsedMs, Bridge.logBytes,
                                                        root.logReadoutOneLine)
                            horizontalAlignment: Text.AlignRight
                            font.pixelSize: Theme.fontBody
                            color: Theme.text
                        }
                        HeaderButton {
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
                        HeaderButton {
                            objectName: "disconnectButton"
                            // Ending a session is quiet; Connect... is primary, unless
                            // Dash or Home shows its connect list, whose Connect is the primary
                            flat: Bridge.connected || Bridge.disconnecting
                            highlighted: !flat && !(root.currentPage === root.dashboardPage
                                                    && dashboardView.connectShown)
                                         && !(root.currentPage === root.homePage
                                              && !Bridge.connected)
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
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // ---- Sidebar (>= 720px): one full-height panel ----
                Rectangle {
                    visible: !root.isCompact && !root.driving
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
                        recents: settingsView.connectUris
                        vehicleUri: settingsView.vehicleUri
                        adapters: settingsView.connectAdapters
                        onConnectToUri: uri => settingsView.connectFromList(uri)
                        onOtherAddressRequested: root.openConnectFromDash()
                        onLogRequested: root.openLog()
                        onListShown: root.listAdapters()
                        onDashboardRequested: root.gotoPage(root.dashboardPage)
                    }
                    DashboardPage {
                        id: dashboardView
                        objectName: "dashboardPage"
                        offlineUri: settingsView.offlineUri
                        vehicles: settingsView.vehicles
                        connectUris: settingsView.connectUris
                        connectVehicleUri: settingsView.vehicleUri
                        connectAdapters: settingsView.connectAdapters
                        onConnectListShown: root.listAdapters()
                        onConnectToUri: uri => settingsView.connectFromList(uri)
                        onOtherAddressRequested: root.openConnectFromDash()
                        onLogRequested: root.openLog()
                        driveHideMs: settingsView.driveHideMs
                        driveOrientation: settingsView.driveOrientation
                        driveTabBarHeight: tabBar.height
                        driveInsetTop: zoomRoot.insetTop
                        driveInsetBottom: zoomRoot.insetBottom
                        driveLogTime: root.logTimeText(Bridge.logElapsedMs)
                        driveLogSize: root.logSizeText(Bridge.logBytes)
                    }
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
                    LogPage {
                        id: logView
                        objectName: "logPage"
                        backShown: root.isCompact
                        backName: qsTr("Back to %1").arg(root.logBackTab.label)
                        onBackRequested: root.logBack()
                    }
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
                        onLogRequested: root.openLog()
                        onLeaveRequested: root.leaveSettings()
                    }
                }
            }

            // A phone's tab bar: targets a touch's size at its top (each tab
            // keeps its own height), then a gap and the system's bottom
            // inset, all in the bar's color, so a tap low on a tab never
            // reaches Back or Home beside it
            Item {
                visible: root.isCompact
                Layout.fillWidth: true
                Layout.preferredHeight: root.driving ? 0 : tabBar.height
                z: 1
                TabBar {
                    id: tabBar
                    objectName: "tabBar"
                    visible: !root.driving || root.barsShown > 0
                    width: parent.width
                    height: Theme.touchTarget + Theme.barGap + zoomRoot.insetBottom
                    y: root.driving ? -height * root.barsShown : 0
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
                        model: root.tabs
                        delegate: TabButton {
                            id: btn
                            required property var modelData
                            objectName: "tab_" + modelData.page
                            text: modelData.label
                            implicitHeight: Theme.touchTarget
                            checked: stack.currentIndex === modelData.page
                            onClicked: {
                                // A tab in the drive view's revealed bar is Exit drive
                                if (root.driving)
                                    dashboardView.leaveDrive(true)
                                root.gotoPage(modelData.page)
                            }
                            // A narrow bar (a small window, or a phone zoomed far
                            // in) gets smaller labels, and one that still doesn't
                            // fit elides inside its tab: labels never run together
                            leftPadding: 4
                            rightPadding: 4
                            contentItem: Label {
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
            // With no tab bar (the sidebar's width), the pages keep off
            // the bottom inset
            Item {
                visible: !root.isCompact
                Layout.fillWidth: true
                Layout.preferredHeight: zoomRoot.insetBottom
            }
        }
    }

    // The phone's screen stays on by ADR-0059 section 6's rule, on the
    // phone form and in the drive view, which a tablet has too
    KeepAwake {
        mobile: root.phoneForm || root.driving
        connected: Bridge.connected
        dashShown: root.currentPage === root.dashboardPage
    }

    // The zoom level, for a moment after it changes
    Rectangle {
        id: zoomBadge
        objectName: "zoomBadge"
        function show() { opacity = 1; zoomBadgeTimer.restart() }
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        // The content item lies inside the system's bars already
        anchors.bottomMargin: 24
        width: zoomBadgeLabel.implicitWidth + 32
        height: 36
        radius: Theme.radius
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

    // ---- Developer mode: the window's drawn frames per second, read in
    // the header (frameRate). Qt draws only when something changed, so an
    // idle page reads 0 (the readout's own change is a frame at most each
    // second) and a chart being panned or pinched reads its rate: the
    // phone run's measure of the charts and the gauges. frameSwapped
    // comes from the render thread, queued to this one.
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
}
