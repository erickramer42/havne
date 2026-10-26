// File/layer: app/qml/DashboardPage.qml | Qt6 Quick, UI layer
// Testing: manual smoke — mock://demo drives every gauge; resize across
//          760 px content width (4 ↔ 2 grid columns) and 600 px (compact
//          gaps), and to 1280 × 650 (the wide arrangement fits without
//          scrolling); gauges sweep once per session. test_qml
//          (tests/qml/tst_dashboard.qml) covers each Tap's lookup and
//          note, the gauges' imperial units, scales and zones, the
//          peaks cleared on a change of units, and a peak held from
//          inside a window (the replayed fixture); tst_linkhealth.qml the
//          header's link state, found by objectName

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
    // core's display strings. `high` and `low` are the last window's,
    // for the gauges' holds. `note` explains an empty gauge instead of
    // showing a fake zero.
    component Tap: QtObject {
        required property var names
        readonly property SignalHub.Entry entry: hub.lookup(names)
        readonly property bool live: Bridge.connected && entry !== null && entry.hasValue
        readonly property real value: entry ? entry.value : 0
        readonly property real high: entry ? entry.windowHigh : 0
        readonly property real low: entry ? entry.windowLow : 0
        readonly property bool stale: live && entry.ageMs > Theme.staleMs
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

    // Sizes for the gauge arrangements below
    QtObject {
        id: fit
        readonly property real w: column.width
        readonly property real g: root.gap
        readonly property bool narrow: w < 760
        readonly property real shiftHeight: root.compact ? 40 : 48
        readonly property real barHeight: 96
        // Everything but the dials and tiles: the page margins, the
        // header, the shift lights, the bars and the column's three gaps
        readonly property real fixedHeight: 2 * root.margin + header.implicitHeight
                                            + 3 * g + shiftHeight + barHeight
        // The stacked arrangement at full size decides whether the page
        // needs the wide one
        readonly property real stackedHeight: fixedHeight
                                              + Math.min((w - g) / 2 * 0.88, 380) + g
                                              + Math.min((w - 3 * g) / 4 * 1.05, 220)
        readonly property bool wide: w >= 900 && stackedHeight > flick.height

        readonly property int tileColumns: wide || narrow ? 2 : 4
        readonly property real tileCell: ((wide ? (w - 2 * g) / 3 : w) - (tileColumns - 1) * g)
                                         / tileColumns
        // Wide: the two tile rows share what the page has left, down to a
        // floor below which the page scrolls instead
        readonly property real tileHeight: wide
            ? Math.max(120, Math.min(tileCell * 1.05, 220, (flick.height - fixedHeight - g) / 2))
            : Math.min(tileCell * 1.05, 220)
        readonly property real dialWidth: wide ? (w - 2 * g) / 3 : narrow ? w : (w - g) / 2
        readonly property real dialHeight: wide ? 2 * tileHeight + g
                                                : Math.min(dialWidth * 0.88, 380)
    }

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
        // A peak held in the old units would read wrong in the new ones
        function onUnitSystemChanged() { root.resetPeaks() }
    }

    // Scale ends and zones are written once, in the decoded units, and
    // shown in the current unit system through core (ADR-0022). Reading
    // Bridge.unitSystem here makes each binding follow a change.
    readonly property string degC: "°C"
    function shown(unit, v) {
        return Bridge.unitSystem === 0 ? v : Bridge.displayValue(unit, v)
    }
    function shownUnit(unit) {
        return Bridge.unitSystem === 0 ? unit : Bridge.displayUnit(unit)
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight + 2 * root.margin
        clip: true
        // Shown whenever there's more below, not only while scrolling, so
        // a short window says it scrolls
        ScrollBar.vertical: ScrollBar {
            policy: flick.contentHeight > flick.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
        }

        ColumnLayout {
            id: column
            x: (flick.width - width) / 2
            y: root.margin
            width: Math.min(flick.width - 2 * root.margin, 1400)
            spacing: root.gap

            // ---- Header: title, link state, peak reset
            PageHeader {
                id: header
                title: qsTr("Dashboard")

                Rectangle {
                    objectName: "linkDot"
                    implicitWidth: 8; implicitHeight: 8   // layout-managed
                    radius: 4
                    color: Theme.linkColor(Bridge.linkState)
                }
                Label {
                    objectName: "linkLabel"
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    // Short statuses come from Theme.linkLabel; "ok" keeps
                    // the richer URI text. Long-form detail (episode
                    // counts, error codes) stays in LiveView's banner.
                    text: Bridge.linkState === "ok"
                          ? qsTr("Live · %1").arg(Bridge.uriLabel)
                          : Theme.linkLabel(Bridge.linkState)
                    font.pixelSize: 13
                    color: Theme.linkColor(Bridge.linkState)
                }
                ThemedButton {
                    text: qsTr("Reset peaks")
                    onClicked: root.resetPeaks()
                }
            }

            // ---- Gauges, in one of three arrangements:
            // - stacked: the two dials side by side, the four tiles in a
            //   row below them, then the two bars;
            // - narrow (a phone): one dial per row, the tiles 2 × 2, one
            //   bar per row;
            // - wide, when the stacked arrangement would overflow a short,
            //   wide page (a 1080p laptop at 150 % scaling): the two dials
            //   and the tiles, 2 × 2, side by side in one row sized to the
            //   page's height.
            // Each grid holds only single-cell items, so uniformCellWidths
            // keeps its columns equal.
            GaugeCard {
                Layout.fillWidth: true
                Layout.preferredHeight: fit.shiftHeight
                padding: 8
                ShiftLightBar {
                    id: shiftLights
                    anchors.fill: parent
                    count: root.compact ? 11 : 15
                    ledSpacing: root.compact ? 6 : 10
                    label: qsTr("Shift lights")
                    value: rpm.value; windowHigh: rpm.high; windowLow: rpm.low
                    hasValue: rpm.live; stale: rpm.stale
                    from: 4500; to: 6500
                }
            }

            GridLayout {
                id: dials
                Layout.fillWidth: true
                columns: fit.wide ? 3 : fit.narrow ? 1 : 2
                columnSpacing: root.gap
                rowSpacing: root.gap
                uniformCellWidths: true

                GaugeCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: fit.dialHeight
                    AnalogGauge {
                        id: tach
                        anchors.fill: parent
                        label: qsTr("Engine speed")
                        caption: qsTr("×1000 r/min")
                        unit: rpm.unit || qsTr("RPM")
                        decimals: rpm.decimals
                        value: rpm.value; windowHigh: rpm.high; windowLow: rpm.low
                        hasValue: rpm.live; stale: rpm.stale; note: rpm.note
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
                    Layout.fillWidth: true
                    Layout.preferredHeight: fit.dialHeight
                    AnalogGauge {
                        id: speedo
                        anchors.fill: parent
                        label: qsTr("Vehicle speed")
                        caption: speed.unit || root.shownUnit("km/h")
                        unit: speed.unit || root.shownUnit("km/h")
                        decimals: speed.decimals
                        value: speed.value; windowHigh: speed.high; windowLow: speed.low
                        hasValue: speed.live; stale: speed.stale; note: speed.note
                        // Round ends in either system: 260 km/h, 160 mph
                        from: 0; to: Bridge.unitSystem === 0 ? 260 : 160
                        majorStep: 20; minorStep: 10
                        showPeak: true
                        peakLabel: qsTr("TOP")
                    }
                }

                // The tiles: four in a row below the dials, or 2 × 2
                GridLayout {
                    Layout.columnSpan: fit.wide ? 1 : dials.columns
                    Layout.fillWidth: true
                    columns: fit.tileColumns
                    columnSpacing: root.gap
                    rowSpacing: root.gap
                    uniformCellWidths: true

                    GaugeCard {
                        Layout.fillWidth: true
                        Layout.preferredHeight: fit.tileHeight
                        RingGauge {
                            id: coolantRing
                            anchors.fill: parent
                            label: qsTr("Coolant")
                            unit: coolant.unit || root.shownUnit(root.degC)
                            decimals: coolant.decimals
                            value: coolant.value; windowHigh: coolant.high; windowLow: coolant.low
                            hasValue: coolant.live; stale: coolant.stale
                            note: coolant.note
                            // From 0 °C, so a cold engine at ambient reads on
                            // the scale instead of pinning at its start
                            from: root.shown(root.degC, 0); to: root.shown(root.degC, 130)
                            coldBelow: root.shown(root.degC, 60)
                            warnFrom: root.shown(root.degC, 105)
                            dangerFrom: root.shown(root.degC, 115)
                        }
                    }
                    GaugeCard {
                        Layout.fillWidth: true
                        Layout.preferredHeight: fit.tileHeight
                        RingGauge {
                            id: oilRing
                            anchors.fill: parent
                            label: qsTr("Oil temp")
                            unit: oil.unit || root.shownUnit(root.degC)
                            decimals: oil.decimals
                            value: oil.value; windowHigh: oil.high; windowLow: oil.low
                            hasValue: oil.live; stale: oil.stale
                            note: oil.note
                            // From 0 °C, like coolant
                            from: root.shown(root.degC, 0); to: root.shown(root.degC, 160)
                            coldBelow: root.shown(root.degC, 70)
                            warnFrom: root.shown(root.degC, 125)
                            dangerFrom: root.shown(root.degC, 140)
                        }
                    }
                    GaugeCard {
                        Layout.fillWidth: true
                        Layout.preferredHeight: fit.tileHeight
                        DigitalGauge {
                            id: iatTile
                            anchors.fill: parent
                            label: qsTr("IAT")
                            description: qsTr("Intake air temperature")
                            unit: iat.unit || root.shownUnit(root.degC)
                            decimals: iat.decimals
                            value: iat.value; windowHigh: iat.high; windowLow: iat.low
                            hasValue: iat.live; stale: iat.stale; note: iat.note
                            from: root.shown(root.degC, 0); to: root.shown(root.degC, 120)
                            warnFrom: root.shown(root.degC, 55)
                            dangerFrom: root.shown(root.degC, 70)
                        }
                    }
                    GaugeCard {
                        Layout.fillWidth: true
                        Layout.preferredHeight: fit.tileHeight
                        DigitalGauge {
                            id: mafTile
                            anchors.fill: parent
                            label: qsTr("MAF")
                            description: qsTr("Mass air flow")
                            unit: maf.unit || root.shownUnit("g/s")
                            decimals: maf.decimals
                            value: maf.value; windowHigh: maf.high; windowLow: maf.low
                            hasValue: maf.live; stale: maf.stale; note: maf.note
                            from: 0; to: root.shown("g/s", 300)
                            accent: Theme.info
                        }
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: fit.narrow ? 1 : 2
                columnSpacing: root.gap
                rowSpacing: root.gap
                uniformCellWidths: true

                GaugeCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: fit.barHeight
                    padding: 14
                    BarGauge {
                        id: throttleBar
                        anchors.fill: parent
                        label: qsTr("Throttle")
                        unit: throttle.unit || "%"
                        decimals: throttle.decimals
                        value: throttle.value; windowHigh: throttle.high; windowLow: throttle.low
                        hasValue: throttle.live; stale: throttle.stale
                        note: throttle.note
                        accent: Theme.success
                    }
                }
                GaugeCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: fit.barHeight
                    padding: 14
                    BarGauge {
                        id: loadBar
                        anchors.fill: parent
                        label: qsTr("Engine load")
                        unit: load.unit || "%"
                        decimals: load.decimals
                        value: load.value; windowHigh: load.high; windowLow: load.low
                        hasValue: load.live; stale: load.stale; note: load.note
                    }
                }
            }
        }
    }
}
