// File/layer: app/qml/HomePage.qml | Qt6 Quick + Shapes, UI layer
// Testing: manual smoke: logo renders centered at 96px, the tach needle
//          revs only while Home is on screen, status card centered
//          (the hint line below is the column's widest item), the
//          button opens Settings when disconnected and ends the session
//          when connected. The status card's dot and label in each link
//          state: test_qml (tests/qml/tst_linkhealth.qml), by objectName.
//          At a phone's sizes (tests/qml/tst_home.qml): inside the width
//          upright, the hint line wrapped; scrolling on its side

import QtQuick
import QtQuick.Layouts
import QtQuick.Shapes

pragma ComponentBehavior: Bound

Rectangle {
    id: home
    color: Theme.bg

    // Settings owns connecting; Main opens it
    signal connectRequested()

    // Scrolls when the page is shorter than the column: a phone on its
    // side (docs/bench-sessions/2026-10-28-android.md)
    Flickable {
        id: flick
        objectName: "homeFlick"
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(height, column.height + 2 * Theme.pageMargin)
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        ColumnLayout {
            id: column
            objectName: "homeColumn"
            // Centered while it fits; from the margin when it doesn't
            x: Math.max(Theme.pageMargin, (flick.width - width) / 2)
            y: Math.max(Theme.pageMargin, (flick.height - height) / 2)
            spacing: 24

            // Logo block: animated tachometer mark, 0..8 ×1000 r/min, red
            // from 6.5. No numerals or readout; at this size they'd crowd it.
            Rectangle {
                id: logo
                implicitWidth: 96; implicitHeight: 96   // layout-managed: implicit, not width/height
                radius: Theme.radius
                color: Theme.surface
                Layout.alignment: Qt.AlignHCenter

                readonly property real maxRpm: 8000
                readonly property real redline: 6500
                readonly property real cx: width / 2
                readonly property real cy: height / 2
                readonly property real r: 38
                readonly property real tickOuter: r * 0.76
                readonly property color litColor: rpm >= redline ? Theme.danger
                                                : rpm >= 4500 ? Theme.warning
                                                : Theme.success
                property real rpm: 800

                // Degrees; 0 = 3 o'clock, clockwise, as GaugeArc takes them
                function angleOf(v) { return 135 + v / maxRpm * 270 }

                // Idle hunting, a blip into the red and a smaller one. The
                // throttle snaps open, the revs fall off slower and idle
                // control catches the dip. Hidden pages stay alive in the
                // StackLayout, so it stops while Home isn't on screen.
                SequentialAnimation on rpm {
                    running: home.visible
                    loops: Animation.Infinite
                    NumberAnimation { to: 870; duration: 400; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 810; duration: 450; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 860; duration: 400; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 800; duration: 400; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 6800; duration: 380; easing.type: Easing.OutQuad }
                    PauseAnimation { duration: 80 }
                    NumberAnimation { to: 760; duration: 1300; easing.type: Easing.OutQuad }
                    NumberAnimation { to: 830; duration: 300; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 860; duration: 600; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 810; duration: 600; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 4200; duration: 280; easing.type: Easing.OutQuad }
                    NumberAnimation { to: 780; duration: 1100; easing.type: Easing.OutQuad }
                    NumberAnimation { to: 820; duration: 300; easing.type: Easing.InOutSine }
                }

                // Face, lit from above
                Shape {
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath {
                        strokeColor: Qt.alpha(Theme.text, 0.12)
                        strokeWidth: 1
                        fillGradient: RadialGradient {
                            centerX: logo.cx; centerY: logo.cy - logo.r * 0.35
                            centerRadius: logo.r * 1.25
                            focalX: logo.cx; focalY: logo.cy - logo.r * 0.35
                            GradientStop { position: 0.0; color: Theme.elevated }
                            GradientStop { position: 1.0; color: Theme.bg }
                        }
                        PathAngleArc {
                            centerX: logo.cx; centerY: logo.cy
                            radiusX: logo.r; radiusY: logo.r
                            startAngle: 0; sweepAngle: 360
                        }
                    }
                }

                // Redline wash behind the red ticks, brighter while the
                // needle is in it; lit ring from 0 to the needle on its track
                Shape {
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer
                    GaugeArc {
                        centerX: logo.cx; centerY: logo.cy; radius: logo.tickOuter - 3
                        startAngle: logo.angleOf(logo.redline)
                        sweepAngle: logo.angleOf(logo.maxRpm) - startAngle
                        strokeColor: Qt.alpha(Theme.danger, logo.rpm >= logo.redline ? 0.6 : 0.22)
                        strokeWidth: 6
                    }
                    GaugeArc {
                        centerX: logo.cx; centerY: logo.cy; radius: logo.r * 0.88
                        startAngle: logo.angleOf(0); sweepAngle: 270
                        strokeColor: Qt.alpha(Theme.text, 0.08)
                        strokeWidth: 3
                    }
                    GaugeArc {
                        centerX: logo.cx; centerY: logo.cy; radius: logo.r * 0.88
                        startAngle: logo.angleOf(0)
                        sweepAngle: logo.angleOf(logo.rpm) - startAngle
                        strokeColor: logo.litColor
                        strokeWidth: 3
                    }
                }

                // Ticks every 500 r/min, full-size items rotated about the center
                Repeater {
                    model: 17
                    delegate: Item {
                        id: tick
                        required property int index
                        readonly property real v: index * 500
                        readonly property bool major: index % 2 === 0

                        width: logo.width; height: logo.height
                        rotation: logo.angleOf(v) + 90   // 0° rotation = 12 o'clock

                        Rectangle {
                            antialiasing: true
                            width: tick.major ? 2 : 1
                            height: tick.major ? 6 : 3.5
                            x: logo.cx - width / 2
                            y: logo.cy - logo.tickOuter
                            color: tick.v >= logo.redline ? Theme.danger
                                 : tick.major ? Theme.text : Qt.alpha(Theme.text, 0.5)
                        }
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: logo.cy + logo.r * 0.42
                    text: qsTr("RPM")
                    font.pixelSize: 8
                    font.letterSpacing: 1
                    color: Theme.textDim
                }

                // Needle, drawn pointing at 3 o'clock
                Shape {
                    width: logo.width; height: logo.height
                    rotation: logo.angleOf(logo.rpm)
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath {
                        id: needle
                        readonly property real len: logo.r * 0.80
                        readonly property real tail: logo.r * 0.18
                        strokeColor: "transparent"
                        fillColor: Theme.danger
                        startX: logo.cx - needle.tail; startY: logo.cy - 1
                        PathLine { x: logo.cx; y: logo.cy - 1.6 }
                        PathLine { x: logo.cx + needle.len; y: logo.cy - 0.4 }
                        PathLine { x: logo.cx + needle.len; y: logo.cy + 0.4 }
                        PathLine { x: logo.cx; y: logo.cy + 1.6 }
                        PathLine { x: logo.cx - needle.tail; y: logo.cy + 1 }
                        PathLine { x: logo.cx - needle.tail; y: logo.cy - 1 }
                    }
                }

                // Hub cap
                Rectangle {
                    anchors.centerIn: parent
                    width: 9; height: width; radius: width / 2
                    border.color: Qt.alpha(Theme.text, 0.10)
                    border.width: 1
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Theme.elevatedLight }
                        GradientStop { position: 1.0; color: Theme.bg }
                    }
                }
            }

            // Branding text
            Text {
                text: qsTr("Havne")
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
                        objectName: "linkDot"
                        implicitWidth: 10; implicitHeight: 10   // layout-managed
                        radius: 5
                        color: Theme.linkColor(Bridge.linkState)
                    }
                    Text {
                        objectName: "linkLabel"
                        text: Theme.linkLabel(Bridge.linkState)
                        color: Theme.textDim
                        font.pixelSize: 13
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: qsTr("core %1").arg(Bridge.coreVersion)
                        color: Theme.textDim
                        font.pixelSize: 13
                    }
                }
            }

            ThemedButton {
                text: Bridge.connected ? qsTr("End session")
                    : Bridge.connecting ? qsTr("Connecting…")
                    : Bridge.disconnecting ? qsTr("Disconnecting…")
                    : qsTr("Connect…")
                highlighted: !Bridge.connected
                enabled: !Bridge.connecting && !Bridge.disconnecting
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 8
                onClicked: Bridge.connected ? Bridge.disconnectSession()
                                            : home.connectRequested()
            }

            Text {
                visible: !Bridge.connected
                text: Theme.developerMode
                      ? qsTr("Choose a J2534 interface or a serial port, or the mock://demo preset, in Settings")
                      : qsTr("Choose a J2534 interface or a serial port in Settings")
                font.pixelSize: 12
                color: Theme.textDim
                Layout.alignment: Qt.AlignHCenter
                // Wraps on a page narrower than the line
                Layout.maximumWidth: flick.width - 2 * Theme.pageMargin
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }
}
