// File/layer: app/qml/DashboardPage.qml | Qt6 Quick, UI layer
// Testing: manual smoke — mock://demo drives every gauge; resize across
//          760 px content width (4 ↔ 2 grid columns) and 600 px (compact
//          gaps); gauges sweep once per session

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

Page {
    id: root
    background: Rectangle { color: Theme.bg }

    readonly property bool compact: width < 600
    readonly property real margin: Theme.pageMargin
    readonly property real gap: compact ? 10 : 16

    SignalHub { id: hub }

    // One gauge input: the first of `names` the active profile provides.
    // The mock and obd2 sets share display names; a profile that names a
    // signal differently adds its name to the list. Unit and decimals are
    // core's display strings. `note` explains an empty gauge instead of
    // showing a fake zero.
    component Tap: QtObject {
        required property var names
        readonly property SignalHub.Entry entry: hub.lookup(names)
        readonly property bool live: Bridge.connected && entry !== null && entry.hasValue
        readonly property real value: entry ? entry.value : 0
        // Same 3 s staleness rule as LiveView
        readonly property bool stale: live && entry.ageMs > 3000
        readonly property string unit: entry ? entry.unit : ""
        readonly property int decimals: entry ? entry.decimals : 0
        readonly property string note: !Bridge.connected ? qsTr("no session")
                                     : entry === null ? qsTr("not in profile")
                                     : !entry.hasValue ? qsTr("waiting for data")
                                     : ""
    }

    Tap { id: rpm;      names: ["RPM"] }
    Tap { id: speed;    names: ["Vehicle Speed"] }
    Tap { id: coolant;  names: ["Coolant Temp"] }
    Tap { id: oil;      names: ["Engine Oil Temp"] }
    Tap { id: iat;      names: ["Intake Air Temp"] }
    Tap { id: maf;      names: ["MAF"] }
    Tap { id: throttle; names: ["Throttle Position"] }
    Tap { id: load;     names: ["Engine Load"] }

    // Ignition-style sweep, once per session, the first time the page
    // is on screen while connected
    property bool sweptThisSession: false
    function maybeSweep() {
        if (!Bridge.connected) {
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
        target: Bridge
        function onConnectedChanged() {
            if (Bridge.connected) root.resetPeaks()
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
            PageHeader {
                title: qsTr("Dashboard")

                Rectangle {
                    implicitWidth: 8; implicitHeight: 8   // layout-managed
                    radius: 4
                    color: Theme.linkColor(Bridge.linkState)
                }
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    // Short statuses come from Theme.linkLabel; "ok" keeps
                    // the richer URI text. Long-form detail (episode
                    // counts, error codes) stays in LiveView's banner.
                    text: Bridge.linkState === "ok"
                          ? qsTr("Live · %1").arg(Bridge.currentUri)
                          : Theme.linkLabel(Bridge.linkState)
                    font.pixelSize: 13
                    color: Theme.linkColor(Bridge.linkState)
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
                        decimals: rpm.decimals
                        value: rpm.value; hasValue: rpm.live; stale: rpm.stale; note: rpm.note
                        from: 0; to: 8000
                        majorStep: 1000; minorStep: 250; labelDivisor: 1000
                        // Danger from the shift point (6800), so the red
                        // zone is entered before the 7000 limiter cuts in
                        dangerFrom: 6800
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
                        decimals: speed.decimals
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
                        decimals: coolant.decimals
                        value: coolant.value; hasValue: coolant.live; stale: coolant.stale
                        note: coolant.note
                        // From 0 °C, so a cold engine at ambient reads on
                        // the scale instead of pinning at its start
                        from: 0; to: 130
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
                        decimals: oil.decimals
                        value: oil.value; hasValue: oil.live; stale: oil.stale
                        note: oil.note
                        from: 0; to: 160   // from 0 °C, like coolant
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
                        decimals: iat.decimals
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
                        decimals: maf.decimals
                        value: maf.value; hasValue: maf.live; stale: maf.stale; note: maf.note
                        from: 0; to: 300
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
                        decimals: throttle.decimals
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
                        decimals: load.decimals
                        value: load.value; hasValue: load.live; stale: load.stale; note: load.note
                    }
                }
            }
        }
    }
}
