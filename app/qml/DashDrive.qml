// File/layer: app/qml/DashDrive.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit and its parts (QmlTest.dashedit_*), QmlTest.phone,
//          QmlTest.linkhealth, QmlTest.driveview (test_qml); BENCH, Manual smoke

import QtQuick

pragma ComponentBehavior: Bound

// The Dashboard's drive view (ADR-0059): its state and its enter and leave, a part of DashboardPage

Item {
    id: part
    required property DashboardPage host
    visible: false

    // ---- The drive view (ADR-0059): this page with every bar hidden

    property bool driving: false
    property bool barsUp: false          // the reveal
    property bool barsHeld: false        // a fault or a log's notice keeps them up
    // What Main hands over: Settings' two values, the tab bar's height (the
    // sheet rises above it) and the system's insets
    property int driveHideMs: 4000
    property string driveOrientation: "follow"
    property real driveTabBarHeight: 0
    property real driveInsetTop: 0
    property real driveInsetBottom: 0
    property string driveLogTime: ""
    property string driveLogSize: ""   // "" until the header shows one
    // The window made full screen; a test's stand-in keeps it off the desktop
    property var driveWindow: Window.window

    // How far the bars are in: 0 hidden, 1 revealed. Main's bars follow it
    property real barsShown: driving && barsUp ? 1 : 0
    Behavior on barsShown { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

    property bool driveHintUp: false
    function isReplay(uri) { return uri.toLowerCase().startsWith("replay://") }

    function enterDrive() {
        if (driving || !part.host.phoneForm || !Bridge.connected || part.host.editing || part.host.phoneEdit)
            return
        part.host.closePanel()
        part.host.cancelHold()
        barsUp = false
        barsHeld = false
        // A fault or a notice already up raises the bars through their handlers
        driving = true
        part.host.layoutStore.phoneDrive = true
        Immersive.enter(driveWindow, part.host.form)
        PhoneWindow.lockOrientation(driveOrientation)
        if (part.host.layoutStore.driveHints < 3) {
            ++part.host.layoutStore.driveHints
            driveHintUp = true
        }
    }
    // Exit drive and a tab are for good; a session's end comes back at the next connect
    function leaveDrive(forGood) {
        if (!driving)
            return
        driving = false
        barsUp = false
        barsHeld = false
        driveHintUp = false
        driveLogEnded = false
        if (forGood)
            part.host.layoutStore.phoneDrive = false
        Immersive.leave()
        PhoneWindow.lockOrientation("follow")
    }
    Component.onDestruction: leaveDrive(false)

    // A tap on the gauges: the bars in or out, and a held reveal is over
    function driveTap() {
        barsUp = !barsUp
        barsHeld = false
        driveHintUp = false
    }
    // Android's Back in the view: the bars first, then out for good
    function driveBack() {
        if (!driving)
            return false
        if (barsUp)
            leaveDrive(true)
        else
            barsUp = true
        return true
    }
    // Android's Back on Dash: the drive view, a gauge's panel, then edit
    // mode; false leaves it to the system
    function goBack() {
        if (driveBack())
            return true
        if (part.host.panel !== "") {
            part.host.closePanel()
            return true
        }
        if (part.host.editing) {
            part.host.stopEditing()
            return true
        }
        return false
    }
    function raiseBars(attention) {
        if (!driving)
            return
        if (attention) {
            barsUp = true
            barsHeld = true
        } else if (!driveFault && !driveNotice) {
            barsHeld = false
        }
    }
    // A fault may need Retry or Disconnect; the other states are for the status line
    function raisesBars(state) { return state === "fault" }
    readonly property bool driveFault: driving && raisesBars(Bridge.linkState)
    readonly property bool driveNotice: driving && Bridge.logNotice !== ""
    onDriveFaultChanged: raiseBars(driveFault)
    onDriveNoticeChanged: raiseBars(driveNotice)

    Timer {
        id: driveHideTimer
        objectName: "driveHideTimer"
        interval: part.driveHideMs
        running: part.driving && part.barsUp && !part.barsHeld && part.driveHideMs > 0
        onTriggered: part.barsUp = false
    }
    // A touch on a bar starts its delay again
    function barTouched() {
        ++barTouches
        if (driveHideTimer.running)
            driveHideTimer.restart()
    }
    property int barTouches: 0    // counted for the tests
    Timer {
        objectName: "driveHintTimer"
        interval: part.driveHideMs > 0 ? part.driveHideMs : 4000
        running: part.driveHintUp
        onTriggered: part.driveHintUp = false
    }
    // Stop log stays in the view, so the status line says it ended; the bars
    // stay as they are, since the driver asked for it
    property bool driveLogEnded: false
    Connections {
        target: Bridge
        function onLogStopped(folder, byUser) {
            if (part.driving && byUser)
                part.driveLogEnded = true
        }
    }
    // The bars rise from the bottom, so the status line is never covered
    Timer {
        objectName: "driveLogEndedTimer"
        interval: part.driveHideMs > 0 ? part.driveHideMs : 4000
        running: part.driveLogEnded
        onTriggered: part.driveLogEnded = false
    }
}
