// File/layer: app/qml/HomePage.qml | Qt6 Quick, UI layer
// Testing: manual smoke — logo renders centered at 96px, bars animate
//          inside it only while Home is on screen, status card centered
//          (the hint line below is the column's widest item), status dot
//          reflects link state, the button opens Settings when
//          disconnected and ends the session when connected

import QtQuick
import QtQuick.Layouts

pragma ComponentBehavior: Bound

Rectangle {
    id: home
    color: Theme.bg

    // Settings owns connecting; Main opens it
    signal connectRequested()

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 24

        // Logo block — animated equalizer mark
        Rectangle {
            implicitWidth: 96; implicitHeight: 96   // layout-managed: implicit, not width/height
            radius: Theme.radius
            color: Theme.surface
            Layout.alignment: Qt.AlignHCenter

            // Bars anchored bottom: animated height extends the TOP upward
            Repeater {
                model: [
                    { x: 26, low: 16, high: 44, ms: 800 },
                    { x: 38, low: 28, high: 60, ms: 1100 },
                    { x: 50, low: 20, high: 52, ms: 950 },
                    { x: 62, low: 32, high: 48, ms: 1000 }
                ]
                delegate: Rectangle {
                    id: bar
                    required property var modelData
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 20
                    x: bar.modelData.x
                    width: 8; radius: 4
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "#f87171" }
                        GradientStop { position: 0.6; color: "#facc15" }
                        GradientStop { position: 1.0; color: "#4ade80" }
                    }
                    // Hidden pages stay alive in the StackLayout, so the
                    // animation stops while Home isn't on screen
                    NumberAnimation on height {
                        running: home.visible
                        from: bar.modelData.low; to: bar.modelData.high
                        duration: bar.modelData.ms
                        loops: Animation.Infinite; easing.type: Easing.InOutQuad
                    }
                }
            }
        }

        // Branding text
        Text {
            text: qsTr("QtTune")
            font.pixelSize: 32
            font.bold: true
            color: Theme.text
            Layout.alignment: Qt.AlignHCenter
        }
        // description text
        Text {
            text: qsTr("ECU reader / logger")
            font.pixelSize: 16
            color: Theme.textDim
            Layout.alignment: Qt.AlignHCenter
        }

        // Status card: session and link state, core version
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 16
            Layout.preferredWidth: 280
            Layout.preferredHeight: 48
            radius: Theme.radius
            color: Theme.surface

            RowLayout {
                anchors.fill: parent
                anchors.margins: 12

                Rectangle {
                    implicitWidth: 10; implicitHeight: 10   // layout-managed
                    radius: 5
                    color: Theme.linkColor(QtTune.linkState)
                }
                Text {
                    text: Theme.linkLabel(QtTune.linkState)
                    color: Theme.textDim
                    font.pixelSize: 13
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: qsTr("core %1").arg(QtTune.coreVersion)
                    color: Theme.textDim
                    font.pixelSize: 13
                }
            }
        }

        ThemedButton {
            text: QtTune.connected ? qsTr("End session")
                : QtTune.connecting ? qsTr("Connecting…")
                : qsTr("Connect…")
            highlighted: !QtTune.connected
            enabled: QtTune.coreInitialized && !QtTune.connecting
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 8
            onClicked: QtTune.connected ? QtTune.disconnectSession()
                                        : home.connectRequested()
        }

        Text {
            visible: !QtTune.connected
            text: qsTr("Choose a J2534 device, or the mock://demo preset, in Settings")
            font.pixelSize: 12
            color: Theme.textDim
            Layout.alignment: Qt.AlignHCenter
        }
    }
}
