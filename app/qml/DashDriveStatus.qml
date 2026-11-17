// File/layer: app/qml/DashDriveStatus.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit, QmlTest.phone,
//          QmlTest.linkhealth, QmlTest.driveview (test_qml); BENCH, Manual smoke

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The drive view's status line (ADR-0059), a part of DashboardPage

Item {
    id: driveStatus
    required property DashboardPage host
    objectName: "driveStatusLine"
    visible: driveStatus.host.driving
    x: 0
    y: driveStatus.host.driveInsetTop
    width: driveStatus.host.width
    height: 28
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: driveStatus.host.margin
        anchors.rightMargin: driveStatus.host.margin
        spacing: 6
        Rectangle {
            objectName: "driveStatusDot"
            implicitWidth: 8; implicitHeight: 8   // layout-managed
            radius: 4
            color: Theme.linkColor(Bridge.linkState)
        }
        Label {
            objectName: "driveStatusWord"
            font.pixelSize: Theme.fontCaption
            color: Theme.text
            text: Theme.linkShortLabel(Bridge.linkState)
        }
        Label {
            objectName: "driveStatusAddress"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            elide: Text.ElideRight
            font.pixelSize: Theme.fontCaption
            color: Theme.textDim
            text: qsTr("· %1").arg(SessionLabel.address(Bridge.uriLabel))
        }
        Label {
            objectName: "driveStatusRun"
            visible: Bridge.logActive || Bridge.recordingActive || driveStatus.host.driveLogEnded
            font.pixelSize: Theme.fontCaption
            color: Theme.text
            text: Bridge.logActive ? qsTr("Log")
                : Bridge.armedRecording ? qsTr("Recording (window %1)").arg(Bridge.armedWindows)
                : Bridge.armedActive ? qsTr("Armed")
                : driveStatus.host.driveLogEnded ? qsTr("Log ended") : qsTr("Recording")
        }
        Label {
            objectName: "driveStatusTime"
            visible: Bridge.logActive
            font.family: Theme.valueFamily
            font.features: { "tnum": 1 }
            font.pixelSize: Theme.fontCaption
            color: Theme.text
            text: driveStatus.host.driveLogTime
        }
        Label {
            objectName: "driveStatusDetail"
            readonly property string detail:
                Bridge.logActive ? driveStatus.host.driveLogSize
              : !Bridge.recordingActive || Bridge.markCount === 0 ? ""
              : Bridge.markCount === 1 ? qsTr("1 mark")
              : qsTr("%1 marks").arg(Bridge.markCount)
            visible: detail !== ""
            font.pixelSize: Theme.fontCaption
            color: Theme.textDim
            text: qsTr("· %1").arg(detail)
        }
    }
}
