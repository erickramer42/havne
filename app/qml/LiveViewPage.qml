// File/layer: app/qml/LiveViewPage.qml | Qt6 Quick, UI layer
// Testing: model contract covered by test_bridge; swatch colours, dashes
//          and banner text by test_qml (tests/qml/tst_liveview.qml, and
//          tst_linkhealth.qml for the banner in each link state), which
//          finds items by objectName; the strip chart's drawing is
//          verified manually (docs/testing.md).

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

    // Trace styles, keyed by model row (chartData's `row`), so a row's
    // swatch matches its trace and styles hold when another signal is
    // unsubscribed. Colours come from Theme.seriesColors; rows past the
    // palette reuse its colours with a dashed trace, so the chart still
    // tells them apart (the row swatch shows the colour only).
    readonly property var traceDashes: [[], [8, 4], [2, 3]]
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
        case "quiet":    return qsTr("Vehicle not responding — quiet bus")
        case "requests": return qsTr("Requests failing — check the vehicle connection and ignition")
        case "fault":    return qsTr("Adapter fault — %1 episode(s), retrying with backoff").arg(episodes)
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

        // Sized to its rows, capped so the chart keeps the rest of the
        // page; past the cap it scrolls, with the bar pinned on so the
        // hidden rows are visible as such.
        ListView {
            id: signalList
            objectName: "signalList"
            readonly property bool overflows: contentHeight > height

            Layout.fillWidth: true
            Layout.preferredHeight: count * root.rowHeight
            Layout.maximumHeight: Math.max(3 * root.rowHeight, column.height * 0.55)
            Layout.minimumHeight: Math.min(count, 3) * root.rowHeight
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
                        checked: signalDelegate.subscribed
                        onToggled: Bridge.signalModel.toggleSubscribed(signalDelegate.index)
                        Accessible.name: qsTr("Plot %1").arg(signalDelegate.name)
                    }
                    // The chart's legend: this row's trace colour, hollow
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
                        // Never-arrived signals render "—", not a
                        // fabricated 0 (unsupported PIDs under obd2).
                        text: (Bridge.connected && signalDelegate.hasValue)
                              ? root.valueText(signalDelegate.value, signalDelegate.decimals,
                                               signalDelegate.unit)
                              : qsTr("—")
                        color: Theme.text
                        font.pixelSize: 16
                        font.family: Theme.monoFamily   // fixed-width digits, no jitter
                        // Staleness dimming: 3 s is generous against the
                        // scheduler's full PID rotation (100 ms per PID, so
                        // about 1 s for the obd2 set).
                        opacity: signalDelegate.hasValue
                                 && signalDelegate.ageMs > 3000
                                     ? 0.45 : 1.0
                    }
                }
            }
        }

        // ---- Strip chart: per-signal rolling history, one repaint per
        // coalesced batch. Each trace takes its list row's colour
        // (root.seriesColor), so the list is the legend.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 120
            color: Theme.surface
            radius: 4
            border.color: Theme.textDim
            border.width: 1

            Canvas {
                id: chart
                anchors.fill: parent
                anchors.margins: 4

                Connections {
                    target: Bridge.signalModel
                    function onValuesUpdated() { chart.requestPaint() }
                    function onSubscribedCountChanged() { chart.requestPaint() }
                }
                Connections {
                    target: Bridge
                    function onConnectedChanged() { chart.requestPaint() }
                }

                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.fillStyle = Theme.surface
                    ctx.fillRect(0, 0, width, height)

                    const series = Bridge.signalModel.chartData()
                    if (!series || series.length === 0) {
                        ctx.fillStyle = Theme.textDim
                        ctx.font = "12px sans-serif"
                        ctx.textAlign = "center"
                        ctx.fillText(root.chartEmptyText(), width / 2, height / 2)
                        return
                    }

                    // Shared TIME axis across series (actual timestamps)
                    let tMin = Infinity, tMax = -Infinity
                    for (let s = 0; s < series.length; ++s) {
                        const sp = series[s].samples
                        if (sp.length >= 2) {
                            if (sp[0] < tMin) tMin = sp[0]
                            // flattened pairs end with a VALUE, not a
                            // timestamp — the last timestamp is at length-2
                            if (sp[sp.length - 2] > tMax) tMax = sp[sp.length - 2]
                        }
                    }
                    if (!isFinite(tMin) || tMax <= tMin) return

                    // Per-series VALUE scaling (kept independent
                    // deliberately), so the numbers come from the labels
                    // in the right gutter: one per trace, at its newest
                    // sample's height. A trace that doesn't change by a
                    // displayed digit is flat.
                    ctx.font = "11px " + Theme.monoFamily
                    const lineH = 14, swatchW = 12
                    const traces = []
                    let labelW = 0
                    for (let s = 0; s < series.length; ++s) {
                        const sp = series[s].samples
                        if (sp.length < 2) continue
                        let vMin = sp[1], vMax = sp[1]
                        for (let i = 1; i < sp.length; i += 2) {
                            if (sp[i] < vMin) vMin = sp[i]
                            if (sp[i] > vMax) vMax = sp[i]
                        }
                        const label = root.valueText(sp[sp.length - 1], series[s].decimals,
                                                     series[s].unit)
                        labelW = Math.max(labelW, ctx.measureText(label).width)
                        traces.push({ sp: sp, row: series[s].row, vMin: vMin, vMax: vMax,
                                      flat: vMax - vMin < 0.5 * Math.pow(10, -series[s].decimals),
                                      label: label })
                    }

                    const axisH = 16, top = 6
                    const plotH = Math.max(1, height - axisH - top - 4)
                    const plotW = Math.max(1, width - Math.min(Math.ceil(labelW) + swatchW + 14,
                                                               width * 0.4))
                    const xOf = t => (t - tMin) / (tMax - tMin) * (plotW - 1)
                    const yOf = norm => top + (1 - norm) * plotH

                    // Flat traces each get a lane of their own instead of
                    // stacking on one line, top down in list order; the
                    // label carries the value
                    const flatCount = traces.filter(tr => tr.flat).length
                    let lane = 0
                    for (const tr of traces) {
                        if (tr.flat) tr.lane = 1 - (++lane) / (flatCount + 1)
                    }
                    function traceY(tr, v) {
                        return tr.flat ? yOf(tr.lane) : yOf((v - tr.vMin) / (tr.vMax - tr.vMin))
                    }

                    // Grid: quarter lines, and one vertical per time tick
                    // (seconds before the newest sample)
                    const spanS = (tMax - tMin) / 1000
                    const stepS = spanS > 12 ? 5 : spanS > 6 ? 2 : 1
                    ctx.strokeStyle = Theme.bg
                    ctx.lineWidth = 1
                    ctx.beginPath()
                    for (let g = 1; g < 4; ++g) {
                        const gy = top + plotH * g / 4
                        ctx.moveTo(0, gy)
                        ctx.lineTo(plotW, gy)
                    }
                    for (let k = 0; k * stepS <= spanS; ++k) {
                        const gx = xOf(tMax - k * stepS * 1000)
                        ctx.moveTo(gx, top)
                        ctx.lineTo(gx, top + plotH)
                    }
                    ctx.stroke()

                    ctx.fillStyle = Theme.textDim
                    ctx.textBaseline = "top"
                    for (let k = 0; k * stepS <= spanS; ++k) {
                        const gx = xOf(tMax - k * stepS * 1000)
                        if (k > 0 && gx < 24) break   // no room left of the tick
                        ctx.textAlign = k === 0 ? "right" : "center"
                        ctx.fillText(k === 0 ? qsTr("now") : qsTr("−%1 s").arg(k * stepS),
                                     gx, top + plotH + 3)
                    }

                    for (const tr of traces) {
                        const sp = tr.sp
                        ctx.strokeStyle = root.seriesColor(tr.row)
                        ctx.setLineDash(root.seriesDash(tr.row))
                        ctx.lineWidth = 2
                        ctx.beginPath()
                        for (let i = 0; i < sp.length; i += 2) {
                            const x = xOf(sp[i])
                            const y = traceY(tr, sp[i + 1])
                            if (i === 0) ctx.moveTo(x, y)
                            else ctx.lineTo(x, y)
                        }
                        ctx.stroke()
                        tr.y = traceY(tr, sp[sp.length - 1])
                    }

                    // Value labels, nudged apart where traces end close
                    // together and kept inside the plot's height. Each
                    // starts with a sample of its trace's stroke, so
                    // dashed rows read apart from solid ones.
                    const byY = traces.slice().sort((a, b) => a.y - b.y)
                    for (let i = 0; i < byY.length; ++i) {
                        byY[i].ly = Math.max(byY[i].y,
                                             i > 0 ? byY[i - 1].ly + lineH : top + lineH / 2)
                    }
                    for (let i = byY.length - 1; i >= 0; --i) {
                        byY[i].ly = Math.min(byY[i].ly, i < byY.length - 1
                                                        ? byY[i + 1].ly - lineH
                                                        : top + plotH - lineH / 2)
                    }
                    const lx = plotW + 6
                    ctx.textAlign = "left"
                    ctx.textBaseline = "middle"
                    for (const tr of byY) {
                        const c = root.seriesColor(tr.row)
                        ctx.strokeStyle = c
                        ctx.setLineDash(root.seriesDash(tr.row))
                        ctx.beginPath()
                        ctx.moveTo(lx, tr.ly)
                        ctx.lineTo(lx + swatchW, tr.ly)
                        ctx.stroke()
                        ctx.fillStyle = c
                        ctx.fillText(tr.label, lx + swatchW + 4, tr.ly)
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
