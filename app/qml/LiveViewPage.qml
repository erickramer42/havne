// File/layer: app/qml/LiveViewPage.qml | Qt6 Quick, UI layer
// Testing: model contract covered by test_bridge; swatch colors, dashes
//          and banner text by test_qml (tests/qml/tst_liveview.qml, and
//          tst_linkhealth.qml for the banner in each link state), which
//          finds items by objectName, and the list giving up rows so the
//          chart stays whole on a short page; the strip chart's layout (ranges,
//          lanes, gaps, ticks, labels, a crowded trace's band) by tst_chartlayout.qml through
//          ChartLayout; the drawing itself, sharp under zoom, is verified
//          manually (docs/testing.md).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Lexical id scoping — lets the delegate reference signalList (outer id)
// statically and is required for the required-property delegate pattern.
pragma ComponentBehavior: Bound

Page {
    id: root
    background: Rectangle { color: Theme.bg }

    // Settings owns connecting; Main opens it
    signal connectRequested()

    readonly property int rowHeight: 44
    // The strip chart's least height; the list gives up rows before the
    // chart goes below it
    readonly property int chartMinHeight: 120

    // Theme.staleMs: a row that old dims, and the chart breaks its trace
    // across the gap instead of drawing a ramp
    readonly property int staleMs: Theme.staleMs

    // A trace's range is at least this many displayed digits, so a step
    // of one or two (fuel level between 71 % and 73 %, the charging
    // voltage within 0.4 V) doesn't fill the plot's height
    readonly property int minSpanUnits: 20

    // Trace styles, keyed by model row (chartData's `row`), so a row's
    // swatch matches its trace and styles hold when another signal is
    // unsubscribed. Colors come from Theme.seriesColors; rows past the
    // palette reuse its colors with a dashed trace, so the chart still
    // tells them apart (the row swatch shows the color only).
    readonly property var traceDashes: [[], [8, 4], [2, 3]]
    // A crowded trace's band fill, faint so the traces under it show
    readonly property real bandAlpha: 0.3
    function seriesColor(row) { return Theme.seriesColors[row % Theme.seriesColors.length] }
    function seriesDash(row) {
        const tier = Math.floor(row / Theme.seriesColors.length)
        return root.traceDashes[tier % root.traceDashes.length]
    }

    // Value plus unit at the signal's decimal places (core's
    // decimal_places), formatted the way the Dashboard's gauges are
    function valueText(v, decimals, unit) {
        const n = Number(v).toLocaleString(Qt.locale(), 'f', decimals)
        return unit !== "" ? n + " " + unit : n
    }

    function statusText() {
        if (Bridge.connecting) return qsTr("Connecting…")
        if (Bridge.disconnecting) return qsTr("Disconnecting…")
        if (!Bridge.connected) return qsTr("Connect a session to stream decoded values")
        if (signalList.count === 0)
            return qsTr("This profile decodes no signals; its frames are on the Log page")
        return qsTr("Plotting %1 of %2 signals · values update at 10 Hz")
                   .arg(Bridge.signalModel.subscribedCount).arg(signalList.count)
    }

    // The link-health banner's message. Unknown states read as degraded,
    // as Theme.linkColor and Theme.linkLabel treat them.
    function bannerText(state, episodes, errorCode) {
        switch (state) {
        // Ignition off reads as quiet on a bus that still acknowledges
        // (the first vehicle's did), so the hint goes here too
        case "quiet":    return qsTr("Vehicle not responding — quiet bus; check the ignition")
        case "requests": return qsTr("Requests failing — check the vehicle connection and ignition")
        case "fault":    return qsTr("Adapter fault — %1 episode(s), retrying with backoff").arg(episodes)
        // A replay's file changed under it (ADR-0023): nothing more will come
        case "stopped":  return qsTr("Replay stopped — the recording changed after it was opened")
        default:         return qsTr("Link degraded (error %1)").arg(errorCode)
        }
    }

    function chartEmptyText() {
        if (!Bridge.connected) return qsTr("No session")
        if (signalList.count === 0) return qsTr("No decoded signals")
        if (Bridge.signalModel.subscribedCount === 0)
            return qsTr("No signals plotted; tick a row to plot it")
        return qsTr("Waiting for data…")
    }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: 12

        PageHeader { title: qsTr("Live signals") }

        // Link-health banner: shown for every state but ok (quiet,
        // requests, fault, degraded) from Bridge.linkState, colored by
        // Theme.linkColor. Transitions are mirrored in the Settings
        // activity log.
        Rectangle {
            objectName: "linkBanner"
            visible: Bridge.connected && Bridge.linkState !== "ok"
            Layout.fillWidth: true
            implicitHeight: 40
            radius: Theme.radius
            color: Qt.alpha(Theme.linkColor(Bridge.linkState), 0.15)

            Label {
                objectName: "linkBannerText"
                anchors.centerIn: parent
                text: root.bannerText(Bridge.linkState, Bridge.errorEpisodes, Bridge.linkErrorCode)
                color: Theme.linkColor(Bridge.linkState)
                font.pixelSize: 13
            }
        }

        // Status line; disconnected, the empty state with a way to connect
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: root.statusText()
                color: Theme.textDim
            }
            ThemedButton {
                visible: !Bridge.connected && !Bridge.connecting && !Bridge.disconnecting
                highlighted: true
                text: qsTr("Connect…")
                onClicked: root.connectRequested()
            }
        }

        // Sized to its rows, capped at about half the page and at what
        // the chart's minimum leaves below it, so on a short page (a
        // small window, a large zoom) the list gives up rows, down to
        // one, and the chart stays whole on the page; past the cap it
        // scrolls, with the bar pinned on so the hidden rows are visible
        // as such. y is the list's top, which only the rows above set.
        ListView {
            id: signalList
            objectName: "signalList"
            readonly property bool overflows: contentHeight > height
            readonly property real oneRow: Math.min(count, 1) * root.rowHeight
            readonly property real roomAbove: column.height - y - column.spacing - root.chartMinHeight

            Layout.fillWidth: true
            Layout.preferredHeight: count * root.rowHeight
            Layout.maximumHeight: Math.max(oneRow, Math.min(Math.max(3 * root.rowHeight,
                                                                     column.height * 0.55),
                                                            roomAbove))
            Layout.minimumHeight: oneRow
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: Bridge.signalModel

            ScrollBar.vertical: ScrollBar {
                id: listBar
                policy: signalList.overflows ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
            }

            // Roles consumed via required properties: static typing, no
            // implicit model-chain lookups, updates driven by the model's
            // dataChanged role filtering (ValueRole, HasValueRole and
            // AgeMsRole re-bind these).
            delegate: Rectangle {
                id: signalDelegate
                required property int index
                required property string name
                required property string unit
                required property double value
                required property int decimals
                required property string text          // a text row's value
                required property bool isText
                required property bool subscribed
                required property bool hasValue
                // The model notifies AgeMsRole per batch and on every
                // coalesce tick (refreshAges), so a row that stops
                // updating still re-evaluates and dims. A delegate-side
                // clock stamped in onValueChanged would dim steady values
                // that are still arriving.
                required property double ageMs          // AgeMsRole; -1 = never arrived

                // Clear of the pinned scroll bar, so values stay readable
                width: signalList.width - (signalList.overflows ? listBar.width : 0)
                height: root.rowHeight
                color: signalDelegate.index % 2 === 0 ? Theme.surface : Theme.bg

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12

                    // Plots the row on the chart; the value keeps
                    // updating either way, here and on the Dashboard
                    CheckBox {
                        objectName: "plotCheck"
                        checked: signalDelegate.subscribed
                        enabled: !signalDelegate.isText   // nothing to plot
                        onToggled: Bridge.signalModel.toggleSubscribed(signalDelegate.index)
                        Accessible.name: qsTr("Plot %1").arg(signalDelegate.name)
                    }
                    // The chart's legend: this row's trace color, hollow
                    // while the signal isn't plotted
                    Rectangle {
                        id: swatch
                        objectName: "swatch"
                        // index is -1 while a model reset (a connect)
                        // removes the delegate
                        readonly property color trace: signalDelegate.index >= 0
                                                       ? root.seriesColor(signalDelegate.index)
                                                       : "transparent"
                        implicitWidth: 10; implicitHeight: 10   // layout-managed
                        radius: 2
                        color: signalDelegate.subscribed ? swatch.trace : "transparent"
                        border.width: 1
                        border.color: swatch.trace
                        opacity: signalDelegate.subscribed ? 1.0 : 0.5
                    }
                    Label {
                        text: signalDelegate.name
                        color: Theme.text
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Label {
                        objectName: "valueLabel"
                        // Never-arrived signals render "—", not a
                        // fabricated 0 (unsupported PIDs under obd2).
                        text: !(Bridge.connected && signalDelegate.hasValue) ? qsTr("—")
                              : signalDelegate.isText ? signalDelegate.text
                              : root.valueText(signalDelegate.value, signalDelegate.decimals,
                                               signalDelegate.unit)
                        // A text value can run to kilobytes: the name
                        // keeps its room
                        Layout.maximumWidth: Math.round(signalDelegate.width * 0.6)
                        elide: Text.ElideRight
                        color: Theme.text
                        font.pixelSize: 16
                        font.family: Theme.monoFamily   // fixed-width digits, no jitter
                        opacity: signalDelegate.hasValue
                                 && signalDelegate.ageMs > root.staleMs
                                     ? 0.45 : 1.0
                    }
                }
            }
        }

        // ---- Strip chart: per-signal rolling history, one repaint per
        // coalesced batch. Each trace takes its list row's color
        // (root.seriesColor), so the list is the legend. ChartLayout
        // works out where everything goes; the Canvas strokes it.
        Rectangle {
            objectName: "chartFrame"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: root.chartMinHeight
            color: Theme.surface
            radius: 4
            border.color: Theme.textDim
            border.width: 1

            // A Canvas paints an image at its own size, which Main.qml's
            // zoom would stretch: this one is Theme.zoom times the size it
            // shows at, scaled back by as much, so its pixels meet the
            // screen's one to one, and onPaint draws in the shown size
            Canvas {
                id: chart
                readonly property real zoom: Theme.zoom
                readonly property real shownW: parent.width - 8
                readonly property real shownH: parent.height - 8
                x: 4
                y: 4
                width: shownW * zoom
                height: shownH * zoom
                scale: 1 / zoom
                transformOrigin: Item.TopLeft

                Connections {
                    target: Bridge.signalModel
                    function onValuesUpdated() { chart.requestPaint() }
                    function onSubscribedCountChanged() { chart.requestPaint() }
                }
                Connections {
                    target: Bridge
                    function onConnectedChanged() { chart.requestPaint() }
                }

                // The layout is ChartLayout's (tested); this only strokes
                // what it returns
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.scale(zoom, zoom)
                    ctx.fillStyle = Theme.surface
                    ctx.fillRect(0, 0, shownW, shownH)

                    const series = Bridge.signalModel.chartData()
                    if (!series || series.length === 0) {
                        ctx.fillStyle = Theme.textDim
                        ctx.font = "12px sans-serif"
                        ctx.textAlign = "center"
                        ctx.fillText(root.chartEmptyText(), shownW / 2, shownH / 2)
                        return
                    }

                    ctx.font = ChartLayout.labelPx + "px " + Theme.monoFamily   // measured and drawn in
                    const l = ChartLayout.layout(series, shownW, shownH, {
                        gapMs: root.staleMs, minSpanUnits: root.minSpanUnits,
                        measure: text => ctx.measureText(text).width,
                        format: root.valueText })
                    if (!l) return

                    // Grid: quarter lines, and one vertical per time tick
                    ctx.strokeStyle = Theme.bg
                    ctx.lineWidth = 1
                    ctx.beginPath()
                    for (const gy of l.hGrid) {
                        ctx.moveTo(0, gy)
                        ctx.lineTo(l.plotW, gy)
                    }
                    for (const tick of l.ticks) {
                        ctx.moveTo(tick.x, l.top)
                        ctx.lineTo(tick.x, l.top + l.plotH)
                    }
                    ctx.stroke()

                    // Captions: seconds before the newest sample
                    ctx.fillStyle = Theme.textDim
                    ctx.textBaseline = "top"
                    for (const tick of l.ticks) {
                        if (!tick.captioned) continue
                        ctx.textAlign = tick.seconds === 0 ? "right" : "center"
                        ctx.fillText(tick.seconds === 0 ? qsTr("now") : qsTr("−%1 s").arg(tick.seconds),
                                     tick.x, l.top + l.plotH + 3)
                    }

                    // Traces, each piece its own stroke (a gap is a break);
                    // a band filled faintly, then its two edges
                    const polyline = pts => {
                        ctx.moveTo(pts[0].x, pts[0].y)
                        for (let i = 1; i < pts.length; ++i)
                            ctx.lineTo(pts[i].x, pts[i].y)
                    }
                    for (const tr of l.traces) {
                        const color = root.seriesColor(tr.row)
                        ctx.strokeStyle = color
                        ctx.fillStyle = color
                        ctx.setLineDash(root.seriesDash(tr.row))
                        for (const piece of tr.pieces) {
                            if (!piece.band) {
                                ctx.lineWidth = ChartLayout.lineW
                                ctx.beginPath()
                                polyline(piece.points)
                                ctx.stroke()
                                continue
                            }
                            ctx.globalAlpha = root.bandAlpha
                            ctx.beginPath()
                            polyline(piece.top.concat(piece.bottom.slice().reverse()))
                            ctx.closePath()
                            ctx.fill()
                            ctx.globalAlpha = 1
                            ctx.lineWidth = ChartLayout.bandEdgeW
                            ctx.beginPath()
                            polyline(piece.top)
                            polyline(piece.bottom)
                            ctx.stroke()
                        }
                    }

                    // Value labels, each starting with a sample of its
                    // trace's stroke, so dashed rows read apart from solid;
                    // the layout leaves some out when the plot is short
                    ctx.lineWidth = 1
                    ctx.textAlign = "left"
                    ctx.textBaseline = "middle"
                    for (const tr of l.traces) {
                        if (!tr.labeled) continue   // no room: its row shows the value
                        const c = root.seriesColor(tr.row)
                        ctx.strokeStyle = c
                        ctx.setLineDash(root.seriesDash(tr.row))
                        ctx.beginPath()
                        ctx.moveTo(l.labelX, tr.labelY)
                        ctx.lineTo(l.labelX + ChartLayout.swatchW, tr.labelY)
                        ctx.stroke()
                        ctx.fillStyle = c
                        ctx.fillText(tr.label, l.labelX + ChartLayout.swatchW + 4, tr.labelY)
                    }
                    ctx.setLineDash([])
                }

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                Component.onCompleted: requestPaint()
            }
        }
    }
}
