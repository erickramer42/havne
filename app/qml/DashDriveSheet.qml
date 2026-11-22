// File/layer: app/qml/DashDriveSheet.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard{,_pages,_panel}, QmlTest.dashedit, QmlTest.phone_*,
//          QmlTest.linkhealth, QmlTest.driveview{,_sheet,_platform} (test_qml); BENCH, Manual smoke

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The drive view's sheet of controls (ADR-0059), a part of DashboardPage

Rectangle {
    id: driveSheet
    required property DashboardPage host
    objectName: "driveToolsBackdrop"
    readonly property bool wide: driveSheet.host.width > driveSheet.host.height
    visible: driveSheet.host.drivePart.driving && driveSheet.host.drivePart.barsShown > 0
    z: 3
    x: 0
    y: driveSheet.host.height - driveSheet.host.drivePart.barsShown * (driveSheet.host.driveTabBarHeight + height)
    width: driveSheet.host.width
    height: sheetGrid.implicitHeight + 2 * Theme.barGap
    color: Theme.bg
    // Keeps a press between the controls off the gauges; driveToolsTouch counts it
    MouseArea {
        anchors.fill: parent
        enabled: driveSheet.host.drivePart.driving
    }
    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: driveSheet.host.hairlineWidth
        color: Theme.hairline
    }
    GridLayout {
        id: sheetGrid
        objectName: "driveSheet"
        readonly property real touchMin: Theme.touchTarget
        x: driveSheet.host.margin
        y: Theme.barGap
        width: parent.width - 2 * driveSheet.host.margin
        columns: driveSheet.wide ? 2 : 1
        columnSpacing: Theme.barGap
        rowSpacing: Theme.barGap

        LogNotice {
            name: "driveLogNotice"
            Layout.row: 0
            Layout.column: 0
            Layout.columnSpan: sheetGrid.columns
        }
        RowLayout {
            objectName: "driveSessionRow"
            Layout.row: 1
            Layout.column: driveSheet.wide ? 1 : 0
            Layout.fillWidth: true
            spacing: Theme.barGap
            Item { Layout.fillWidth: true; visible: driveSheet.wide }
            SheetButton {
                objectName: "driveStartLog"
                highlighted: true
                visible: Bridge.connected && !Bridge.recordingActive
                         && !driveSheet.host.drivePart.isReplay(Bridge.currentUri)
                enabled: !Bridge.disconnecting && Bridge.signalModel.tickedCount > 0
                text: qsTr("Start log")
                Accessible.name: qsTr("Start a log of the ticked signals")
                onClicked: Bridge.startLog()
            }
            Label {
                textFormat: Text.PlainText
                objectName: "driveLogReadout"
                visible: Bridge.logActive
                // Gives way first, so Stop log and Mark keep their size
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                font.family: Theme.valueFamily
                font.features: { "tnum": 1 }
                font.pixelSize: Theme.fontBody
                color: Theme.text
                text: driveSheet.host.driveLogSize === "" ? driveSheet.host.driveLogTime
                    : qsTr("%1 · %2").arg(driveSheet.host.driveLogTime).arg(driveSheet.host.driveLogSize)
            }
            SheetButton {
                objectName: "driveStopLog"
                highlighted: true
                visible: Bridge.logActive
                enabled: !Bridge.disconnecting
                text: qsTr("Stop log")
                onClicked: Bridge.stopLog()
            }
            // The armed recorder, where Start log stands (Main's armedButton):
            // an enabled item around it, so a tap on the disabled button says why
            Item {
                id: driveArmedHost
                objectName: "driveArmedHost"
                visible: Bridge.connected && !Bridge.logActive
                         && (Bridge.armedActive || !Bridge.recordingActive)
                         && !driveSheet.host.drivePart.isReplay(Bridge.currentUri)
                implicitWidth: driveArmedButton.implicitWidth
                implicitHeight: driveArmedButton.implicitHeight
                Layout.minimumWidth: Theme.touchTarget
                TapHandler {
                    objectName: "driveArmedExplain"
                    enabled: !driveArmedButton.enabled && Bridge.armedWhy !== ""
                    onTapped: Bridge.startArmed()
                }
            SheetButton {
                id: driveArmedButton
                objectName: "driveArmed"
                anchors.fill: parent
                enabled: !Bridge.disconnecting
                         && (Bridge.armedActive || Bridge.armedWhy === "")
                checkable: true
                checked: Bridge.armedActive
                text: Bridge.armedRecording
                      ? qsTr("Recording (window %1)").arg(Bridge.armedWindows)
                      : Bridge.armedActive ? qsTr("Armed") : qsTr("Arm")
                Accessible.name: Bridge.armedActive ? qsTr("Stop the armed recorder")
                                                    : qsTr("Arm a recorder for the conditions that record")
                Accessible.description: Bridge.armedActive ? "" : Bridge.armedWhy
                onToggled: toggle()
                onClicked: Bridge.armedActive ? Bridge.stopArmed() : Bridge.startArmed()
            }
            }
            SheetButton {
                objectName: "driveMark"
                visible: Bridge.recordingActive && (!Bridge.armedActive || Bridge.armedRecording)
                text: Bridge.markCount > 0 ? qsTr("Mark (%1)").arg(Bridge.markCount) : qsTr("Mark")
                Accessible.name: qsTr("Mark the recording")
                onClicked: Bridge.mark()
            }
            Item { Layout.fillWidth: true; visible: !driveSheet.wide }
            // Stop log takes its place while a log runs, as in the header
            SheetButton {
                objectName: "driveDisconnect"
                flat: true
                visible: !Bridge.logActive
                enabled: !Bridge.connecting && !Bridge.disconnecting
                text: Bridge.disconnecting ? qsTr("Disconnecting…") : qsTr("Disconnect")
                onClicked: Bridge.disconnectSession()
            }
        }
        RowLayout {
            objectName: "driveViewRow"
            Layout.row: driveSheet.wide ? 1 : 2
            Layout.column: 0
            Layout.fillWidth: true
            spacing: Theme.barGap
            SheetButton {
                objectName: "driveExit"
                flat: true
                text: qsTr("Exit drive")
                Accessible.name: qsTr("Exit drive: leave the full-screen gauges")
                onClicked: driveSheet.host.drivePart.leaveDrive(true)
            }
            Item { Layout.fillWidth: true; visible: !driveSheet.wide }
            SheetButton {
                objectName: "driveResetPeaks"
                flat: true
                text: qsTr("Reset peaks")
                onClicked: driveSheet.host.resetPeaks()
            }
            Loader {
                objectName: "driveNight"
                active: driveSheet.host.drivePart.driving
                sourceComponent: driveSheet.host.nightSwitch
            }
            Item { Layout.fillWidth: true; visible: driveSheet.wide }
        }
    }
    // Over the controls: a press restarts the delay and goes on to them
    MouseArea {
        objectName: "driveToolsTouch"
        anchors.fill: parent
        z: 1
        enabled: driveSheet.host.drivePart.driving
        onPressed: mouse => {
            driveSheet.host.drivePart.barTouched()
            mouse.accepted = false
        }
    }

    // A touch's size each way, drawn compact, as the header's buttons
    component SheetButton: ThemedButton {
        compact: !flat
        Layout.minimumWidth: Theme.touchTarget
    }
}
