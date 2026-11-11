// File/layer: app/qml/HomePage.qml | Qt6 Quick + Shapes, UI layer
// Testing: QML: QmlTest.home, QmlTest.gauges, QmlTest.linkhealth, QmlTest.phone (test_qml); Manual smoke

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes

pragma ComponentBehavior: Bound

// The desktop's first page: with no session, what can be connected to
// (the connect list Dash shows on a phone); with one, the session's card.
// Settings owns connecting; Main hands it the chosen address
Rectangle {
    id: home
    color: Theme.bg

    // Settings' connectUris, vehicleUri and connectAdapters
    property var recents: []
    property string vehicleUri: ""
    property var adapters: []
    // The tapped row, until the recent list changes
    property string tapped: ""
    onRecentsChanged: tapped = ""
    onVehicleUriChanged: tapped = ""

    signal connectToUri(string uri)
    signal otherAddressRequested()
    signal logRequested()
    signal listShown()
    signal dashboardRequested()

    // When the session on the card started, as Home saw it connect
    property var connectedAt: null
    Connections {
        target: Bridge
        function onConnectedChanged() { home.connectedAt = Bridge.connected ? new Date() : null }
    }
    Component.onCompleted: if (Bridge.connected) connectedAt = new Date()

    // The address's profile in words: profile= or the default
    function profileOf(uri) {
        const found = /[?&]profile=([^&]+)/.exec(uri)
        return found ? decodeURIComponent(found[1]) : qsTr("default")
    }

    // Scrolls when the page is shorter than the column: a window on its
    // side, or a long list of adapters
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
            width: Math.min(560, flick.width - 2 * Theme.pageMargin)
            x: (flick.width - width) / 2
            y: Theme.pageMargin
            spacing: Theme.spacing * 2

            RowLayout {
                objectName: "homeHeader"
                Layout.fillWidth: true
                spacing: 16

                // Logo block: animated tachometer mark, 0..8 x1000 r/min, red
                // from 6.5. No numerals or readout; at this size they'd crowd it.
                Rectangle {
                    id: logo
                    objectName: "homeLogo"
                    implicitWidth: 96; implicitHeight: 96   // layout-managed: implicit, not width/height
                    radius: Theme.radius
                    color: Theme.surface

                    readonly property real maxRpm: 8000
                    readonly property real redline: 6500
                    readonly property real cx: width / 2
                    readonly property real cy: height / 2
                    readonly property real r: 38
                    readonly property real tickOuter: r * 0.76
                    readonly property color litColor: rpm >= redline ? Theme.gaugeDanger
                                                    : rpm >= 4500 ? Theme.gaugeWarn
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
                            strokeColor: Qt.alpha(Theme.gaugeTick, 0.12)
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
                            strokeColor: Qt.alpha(Theme.gaugeDanger, logo.rpm >= logo.redline ? 0.6 : 0.22)
                            strokeWidth: 6
                        }
                        GaugeArc {
                            centerX: logo.cx; centerY: logo.cy; radius: logo.r * 0.88
                            startAngle: logo.angleOf(0); sweepAngle: 270
                            strokeColor: Qt.alpha(Theme.gaugeTick, 0.08)
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
                            rotation: logo.angleOf(v) + 90   // 0 degrees rotation = 12 o'clock

                            Rectangle {
                                antialiasing: true
                                width: tick.major ? 2 : 1
                                height: tick.major ? 6 : 3.5
                                x: logo.cx - width / 2
                                y: logo.cy - logo.tickOuter
                                color: tick.v >= logo.redline ? Theme.gaugeDanger
                                     : tick.major ? Theme.gaugeTick : Qt.alpha(Theme.gaugeTick, 0.5)
                            }
                        }
                    }

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: logo.cy + logo.r * 0.42
                        text: qsTr("RPM")
                        font.pixelSize: 8
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
                            fillColor: Theme.gaugeDanger
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

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: qsTr("Havne")
                        font.pixelSize: Theme.fontHero
                        font.bold: true
                        color: Theme.text
                    }
                    Label {
                        objectName: "homeCoreVersion"
                        text: qsTr("core %1").arg(Bridge.coreVersion)
                        font.pixelSize: Theme.fontCaption
                        color: Theme.textDim
                    }
                }
            }

            ConnectList {
                objectName: "homeConnectList"
                visible: !Bridge.connected
                Layout.fillWidth: true
                recents: home.recents
                vehicleUri: home.vehicleUri
                adapters: home.adapters
                tapped: home.tapped
                onPicked: uri => home.tapped = uri
                onConnectTo: uri => home.connectToUri(uri)
                onOtherAddress: home.otherAddressRequested()
                onShowLog: home.logRequested()
                onShown: home.listShown()
            }

            // The session: its state, address, profile, vehicle and start
            Rectangle {
                objectName: "homeSession"
                visible: Bridge.connected
                Layout.fillWidth: true
                implicitHeight: sessionColumn.implicitHeight + 24
                color: Theme.surface
                radius: Theme.radiusCard
                border.width: 1
                border.color: Theme.hairline

                ColumnLayout {
                    id: sessionColumn
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
                    spacing: 6

                    RowLayout {
                        spacing: 8
                        Rectangle {
                            objectName: "linkDot"
                            implicitWidth: 10; implicitHeight: 10   // layout-managed
                            radius: 5
                            color: Theme.linkColor(Bridge.linkState)
                        }
                        Label {
                            objectName: "linkLabel"
                            text: Theme.linkLabel(Bridge.linkState)
                            font.pixelSize: Theme.fontEmphasis
                            font.weight: Font.DemiBold
                            color: Theme.text
                        }
                    }
                    Label {
                        objectName: "homeSessionAddress"
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        color: Theme.text
                        text: Bridge.uriLabel
                    }
                    Label {
                        objectName: "homeSessionDetail"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontCaption
                        color: Theme.textDim
                        text: [qsTr("Profile %1").arg(home.profileOf(Bridge.currentUri))]
                              .concat(Bridge.vehicleLabel !== "" ? [Bridge.vehicleLabel] : [])
                              .concat(home.connectedAt !== null
                                      ? [qsTr("since %1").arg(Qt.formatTime(home.connectedAt,
                                                                             Locale.ShortFormat))]
                                      : [])
                              .join(" · ")
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignRight
                        Layout.topMargin: 6
                        spacing: 8
                        ThemedButton {
                            objectName: "homeDashboard"
                            text: qsTr("Dashboard")
                            onClicked: home.dashboardRequested()
                        }
                        ThemedButton {
                            objectName: "homeDisconnect"
                            flat: true
                            enabled: !Bridge.disconnecting
                            text: Bridge.disconnecting ? qsTr("Disconnecting…") : qsTr("Disconnect")
                            onClicked: Bridge.disconnectSession()
                        }
                    }
                }
            }

            Label {
                objectName: "homeHint"
                visible: !Bridge.connected
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontCaption
                color: Theme.textDim
                text: qsTr("Connect from here, or from Settings > Connection to choose a profile")
            }
        }
    }
}
