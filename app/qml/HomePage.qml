import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    color: Theme.bg

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 24

        // Logo block — animated equalizer mark
        Rectangle {
            width: 96; height: 96; radius: Theme.radius
            color: Theme.surface
            Layout.alignment: Qt.AlignHCenter

            // Bars anchored bottom: animated height extends the TOP upward
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 20
                x: 26
                width: 8; radius: 4
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#f87171" }
                    GradientStop { position: 0.6; color: "#facc15" }
                    GradientStop { position: 1.0; color: "#4ade80" }
                }
                NumberAnimation on height {
                    from: 16; to: 44; duration: 800
                    loops: Animation.Infinite; easing.type: Easing.InOutQuad
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 20
                x: 38
                width: 8; radius: 4
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#f87171" }
                    GradientStop { position: 0.6; color: "#facc15" }
                    GradientStop { position: 1.0; color: "#4ade80" }
                }
                NumberAnimation on height {
                    from: 28; to: 60; duration: 1100
                    loops: Animation.Infinite; easing.type: Easing.InOutQuad
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 20
                x: 50
                width: 8; radius: 4
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#f87171" }
                    GradientStop { position: 0.6; color: "#facc15" }
                    GradientStop { position: 1.0; color: "#4ade80" }
                }
                NumberAnimation on height {
                    from: 20; to: 52; duration: 950
                    loops: Animation.Infinite; easing.type: Easing.InOutQuad
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 20
                x: 62
                width: 8; radius: 4
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#f87171" }
                    GradientStop { position: 0.6; color: "#facc15" }
                    GradientStop { position: 1.0; color: "#4ade80" }
                }
                NumberAnimation on height {
                    from: 32; to: 48; duration: 1000
                    loops: Animation.Infinite; easing.type: Easing.InOutQuad
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

        // Status card — wires up nicely to the bridge later
        Rectangle {
            Layout.topMargin: 16
            Layout.preferredWidth: 280
            Layout.preferredHeight: 48
            radius: Theme.radius
            color: Theme.surface

            RowLayout {
                anchors.fill: parent
                anchors.margins: 12

                Rectangle {
                    width: 10; height: 10; radius: 5
                    color: QtTune.connected ? Theme.success : Theme.danger
                }
                Text {
                    text: QtTune.connected ? qsTr("Session active") : qsTr("No session")
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
            text: QtTune.connected ? qsTr("End session") : qsTr("Start session")
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 8
            onClicked: {
                if (QtTune.connected) {
                    QtTune.disconnectSession()
                } else {
                    var err = QtTune.connectSession("mock://demo")
                    if (err !== "") {
                        console.error("connect failed:", err)
                        return
                    }
                    gotoPage(3)   // jump to Log
                }
            }
        }

        Text {
            text: qsTr("Connect a J2534 device for live vehicle data (planned)")
            font.pixelSize: 12
            color: Theme.textDim
            opacity: 0.7
            Layout.alignment: Qt.AlignHCenter
        }
    }
}
