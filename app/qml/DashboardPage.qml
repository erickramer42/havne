// File/layer: app/qml/DashboardPage.qml | Qt6 Quick, UI layer
// Testing: manual smoke — mock://demo drives every gauge; resize across
//          760 px content width (4 ↔ 2 grid columns) and 600 px (phone
//          margins); gauges sweep once per session

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

Page {
    id: root
    background: Rectangle { color: Theme.bg }

    readonly property bool compact: width < 600
    readonly property real margin: compact ? 12 : 24
    readonly property real gap: compact ? 10 : 16

    SignalHub { id: hub }

    // One gauge input: the first alias the active profile provides
    // (mock and obd2 name signals differently). `note` explains an
    // empty gauge instead of showing a fake zero.
    component Tap: QtObject {
        required property var names
        readonly property SignalHub.Entry entry: hub.lookup(names)
        readonly property bool live: QtTune.connected && entry !== null && entry.hasValue
        readonly property real value: entry ? entry.value : 0
        // Same 3 s staleness rule as LiveView
        readonly property bool stale: live && entry.ageMs > 3000
        readonly property string unit: entry ? hub.prettyUnit(entry.unit) : ""
        readonly property string note: !QtTune.connected ? qsTr("no session")
                                     : entry === null ? qsTr("not in profile")
                                     : !entry.hasValue ? qsTr("waiting for data")
                                     : ""
    }

    Tap { id: rpm;      names: ["Engine_RPM", "RPM"] }
    Tap { id: speed;    names: ["Vehicle_Speed", "Vehicle Speed"] }
    Tap { id: coolant;  names: ["Coolant_Temp", "Coolant Temp"] }
    Tap { id: oil;      names: ["Oil_Temp", "Engine Oil Temp"] }
    Tap { id: iat;      names: ["Intake_Air_Temp", "Intake Air Temp"] }
    Tap { id: maf;      names: ["MAF", "Mass_Air_Flow", "MAF Air Flow Rate"] }
    Tap { id: throttle; names: ["Throttle_Pos", "Throttle Position"] }
    Tap { id: load;     names: ["Engine_Load", "Engine Load"] }

    // Ignition-style sweep, once per session, the first time the page
    // is on screen while connected
    property bool sweptThisSession: false
    function maybeSweep() {
        if (!QtTune.connected) {
            sweptThisSession = false
            return
        }
        if (!visible || sweptThisSession) return
        sweptThisSession = true
        for (const g of [shiftLights, tach, speedo, coolantRing, oilRing]) g.sweep()
    }
    function resetPeaks() {
        for (const g of [tach, speedo, coolantRing, oilRing, iatTile, mafTile,
                         throttleBar, loadBar])
            g.resetPeaks()
    }
    onVisibleChanged: maybeSweep()
    Component.onCompleted: maybeSweep()
    Connections {
        target: QtTune
        function onConnectedChanged() {
            if (QtTune.connected) root.resetPeaks()
            root.maybeSweep()
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight + 2 * root.margin
        clip: true
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            x: (flick.width - width) / 2
            y: root.margin
            width: Math.min(flick.width - 2 * root.margin, 1400)
            spacing: root.gap

            // ---- Header: title, link state, peak reset
            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Label {
                    text: qsTr("Dashboard")
                    font.pixelSize: 20
                    color: Theme.text
                }
                Rectangle {
                    implicitWidth: 8; implicitHeight: 8   // layout-managed
                    radius: 4
                    // Same mapping as the Home status card
                    color: !QtTune.connected ? Theme.danger
                         : QtTune.linkState === "ok" ? Theme.success
                         : QtTune.linkState === "fault" ? Theme.danger
                         : Theme.warning
                }
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    text: !QtTune.connected ? qsTr("No session")
                         : QtTune.linkState === "ok" ? qsTr("Live · %1").arg(QtTune.currentUri)
                         : QtTune.linkState === "fault" ? qsTr("Adapter fault")
                         : QtTune.linkState === "quiet" ? qsTr("Link quiet")
                         : qsTr("Link degraded")
                    font.pixelSize: 13
                    color: Theme.textDim
                }
                ThemedButton {
                    text: qsTr("Reset peaks")
                    onClicked: root.resetPeaks()
                }
            }

            // ---- Gauges. Four columns when there's room, else two:
            // dials span two columns (so stack on a phone), rings and
            // tiles take one, bars two.
            GridLayout {
                id: grid
                Layout.fillWidth: true
                columns: column.width >= 760 ? 4 : 2
                columnSpacing: root.gap
                rowSpacing: root.gap
                uniformCellWidths: true

                readonly property real cellWidth: (column.width - (columns - 1) * columnSpacing) / columns
                readonly property real dialHeight: Math.min((2 * cellWidth + columnSpacing) * 0.88, 380)
                readonly property real tileHeight: Math.min(cellWidth * 1.05, 220)

                GaugeCard {
                    Layout.columnSpan: grid.columns
                    Layout.fillWidth: true
                    Layout.preferredHeight: root.compact ? 40 : 48
                    padding: 8
                    ShiftLightBar {
                        id: shiftLights
                        anchors.fill: parent
                        count: root.compact ? 11 : 15
                        ledSpacing: root.compact ? 6 : 10
                        label: qsTr("Shift lights")
                        value: rpm.value; hasValue: rpm.live; stale: rpm.stale
                        from: 4500; to: 6500
                    }
                }

                GaugeCard {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    Layout.preferredHeight: grid.dialHeight
                    AnalogGauge {
                        id: tach
                        anchors.fill: parent
                        label: qsTr("Engine speed")
                        caption: qsTr("×1000 r/min")
                        unit: rpm.unit || qsTr("RPM")
                        value: rpm.value; hasValue: rpm.live; stale: rpm.stale; note: rpm.note
                        from: 0; to: 8000
                        majorStep: 1000; minorStep: 250; labelDivisor: 1000
                        dangerFrom: 7000
                        showProgress: true
                        showPeak: true
                    }
                }
                GaugeCard {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    Layout.preferredHeight: grid.dialHeight
                    AnalogGauge {
                        id: speedo
                        anchors.fill: parent
                        label: qsTr("Vehicle speed")
                        caption: speed.unit || qsTr("km/h")
                        unit: speed.unit || qsTr("km/h")
                        value: speed.value; hasValue: speed.live; stale: speed.stale; note: speed.note
                        from: 0; to: 260
                        majorStep: 20; minorStep: 10
                        showPeak: true
                        peakLabel: qsTr("TOP")
                    }
                }

                GaugeCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: grid.tileHeight
                    RingGauge {
                        id: coolantRing
                        anchors.fill: parent
                        label: qsTr("Coolant")
                        unit: coolant.unit || "°C"
                        value: coolant.value; hasValue: coolant.live; stale: coolant.stale
                        note: coolant.note
                        from: 40; to: 130
                        coldBelow: 60; warnFrom: 105; dangerFrom: 115
                    }
                }
                GaugeCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: grid.tileHeight
                    RingGauge {
                        id: oilRing
                        anchors.fill: parent
                        label: qsTr("Oil temp")
                        unit: oil.unit || "°C"
                        value: oil.value; hasValue: oil.live; stale: oil.stale
                        note: oil.note
                        from: 50; to: 160
                        coldBelow: 70; warnFrom: 125; dangerFrom: 140
                    }
                }
                GaugeCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: grid.tileHeight
                    DigitalGauge {
                        id: iatTile
                        anchors.fill: parent
                        label: qsTr("IAT")
                        description: qsTr("Intake air temperature")
                        unit: iat.unit || "°C"
                        value: iat.value; hasValue: iat.live; stale: iat.stale; note: iat.note
                        from: 0; to: 120
                        warnFrom: 55; dangerFrom: 70
                    }
                }
                GaugeCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: grid.tileHeight
                    DigitalGauge {
                        id: mafTile
                        anchors.fill: parent
                        label: qsTr("MAF")
                        description: qsTr("Mass air flow")
                        unit: maf.unit || "g/s"
                        value: maf.value; hasValue: maf.live; stale: maf.stale; note: maf.note
                        from: 0; to: 300
                        decimals: 1
                        accent: Theme.info
                    }
                }

                GaugeCard {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    Layout.preferredHeight: 96
                    padding: 14
                    BarGauge {
                        id: throttleBar
                        anchors.fill: parent
                        label: qsTr("Throttle")
                        unit: throttle.unit || "%"
                        value: throttle.value; hasValue: throttle.live; stale: throttle.stale
                        note: throttle.note
                        accent: Theme.success
                    }
                }
                GaugeCard {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    Layout.preferredHeight: 96
                    padding: 14
                    BarGauge {
                        id: loadBar
                        anchors.fill: parent
                        label: qsTr("Engine load")
                        unit: load.unit || "%"
                        value: load.value; hasValue: load.live; stale: load.stale; note: load.note
                    }
                }
            }
        }
    }
}
