// File/layer: app/qml/KeepAwake.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.driveplatform (test_qml); BENCH for the phone's flag

import QtQuick

// The screen-stays-awake rule of ADR-0059 section 6: while a session is
// connected and the app is in front, the phone's screen doesn't sleep if it
// has external power or the Dash page is showing. Power is read every 5 s
// rather than through a receiver. The desktop form never asks.
Item {
    id: root
    visible: false

    property bool connected: false
    property bool dashShown: false
    property bool mobile: Theme.mobile
    // qmllint disable missing-property
    property bool active: Qt.application.state === Qt.ApplicationActive
    // qmllint enable missing-property
    property bool hasPower: false
    // A test's stand-in for the phone's reading
    property var readPower: () => PhoneWindow.externalPower()

    function keepAwake(isConnected, isPowered, isOnDash) {
        return isConnected && (isPowered || isOnDash)
    }

    readonly property bool wanted: mobile && active && keepAwake(connected, hasPower, dashShown)
    onWantedChanged: PhoneWindow.setKeepScreenOn(wanted)

    Timer {
        objectName: "powerTimer"
        interval: 5000
        repeat: true
        triggeredOnStart: true
        running: root.mobile && root.connected && root.active
        onTriggered: root.hasPower = root.readPower()
    }
}
