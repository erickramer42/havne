// File/layer: app/qml/LiveViewPage.qml | Qt6 Quick, UI layer
// Testing: model contract covered by test_bridge; render verified manually.
// Banner states need a j2534 fake with a knob armed before launch:
// QTTUNE_FAKE_READ_STATUS (0x09 quiet, 0x07 fault) or
// QTTUNE_FAKE_WRITE_STATUS=7 (requests) — see docs/testing.md smoke recipe.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Lexical id scoping — lets the delegate reference signalList (outer id)
// statically and is required for the required-property delegate pattern.
pragma ComponentBehavior: Bound

Page {
    id: root
    background: Rectangle { color: Theme.bg }

    readonly property int rowHeight: 44

    // Trace colours, keyed by model row (chartData's `row`), so a row's
    // swatch matches its trace and colours hold when another signal is
    // unsubscribed. Twelve hues spaced for a dark surface, ordered so
    // the first rows differ most; a larger set wraps. Red comes late, so
    // an ordinary signal near the top doesn't read as an alarm.
    readonly property var seriesColors: [
        "#60a5fa", "#fbbf24", "#4ade80", "#f472b6",
        "#22d3ee", "#fb923c", "#a78bfa", "#a3e635",
        "#f87171", "#2dd4bf", "#e879f9", "#818cf8"
    ]
    function seriesColor(row) { return root.seriesColors[row % root.seriesColors.length] }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            text: qsTr("Live Signals")
            font.pixelSize: 20
            color: Theme.text
        }

        // Link-health banner: shown for every state but ok (quiet,
        // requests, fault, degraded) from QtTune.linkState, colored by
        // Theme.linkColor. Transitions are mirrored in the Settings
        // activity log.
        Rectangle {
            visible: QtTune.connected && QtTune.linkState !== "ok"
            Layout.fillWidth: true
            implicitHeight: 40
            radius: Theme.radius
            color: Qt.alpha(Theme.linkColor(QtTune.linkState), 0.15)

            Label {
                anchors.centerIn: parent
                text: QtTune.linkState === "fault"
                      ? qsTr("Adapter fault — %1 episode(s), retrying with backoff")
                            .arg(QtTune.errorEpisodes)
                      : QtTune.linkState === "requests"
                        ? qsTr("Requests failing — check the vehicle connection and ignition")
                      : QtTune.linkState === "degraded"
                        ? qsTr("Link degraded (error %1)").arg(QtTune.errorErrorCode)
                      : qsTr("Vehicle not responding — quiet bus")
                color: Theme.linkColor(QtTune.linkState)
                font.pixelSize: 13
            }
        }

        Label {
            text: QtTune.connected
                ? qsTr("Subscribed: %1 signals · batched @ 10 Hz").arg(QtTune.signalModel.subscribedCount)
                : qsTr("Connect a session to stream decoded values")
            color: QtTune.connected ? Theme.textDim : Theme.danger
        }

        // Sized to its rows, capped so the chart keeps the rest of the
        // page; past the cap it scrolls, with the bar pinned on so the
        // hidden rows are visible as such.
        ListView {
            id: signalList
            readonly property bool overflows: contentHeight > height

            Layout.fillWidth: true
<<<<<<< Updated upstream
            Layout.preferredHeight: count * root.rowHeight
            Layout.maximumHeight: Math.max(3 * root.rowHeight, column.height * 0.55)
            Layout.minimumHeight: Math.min(count, 3) * root.rowHeight
=======
            // Room for ~6 rows of 48 px; the cap keeps the chart from
            // collapsing when the whole obd2 set (10 signals) is listed.
            Layout.preferredHeight: 290
            Layout.minimumHeight: 140
            Layout.maximumHeight: 340
>>>>>>> Stashed changes
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: QtTune.signalModel
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

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

                    CheckBox {
                        checked: signalDelegate.subscribed
                        onToggled: QtTune.signalModel.toggleSubscribed(signalDelegate.index)
                    }
                    // The chart's legend: this row's trace colour, hollow
                    // while the signal isn't plotted
                    Rectangle {
                        id: swatch
                        readonly property color trace: root.seriesColor(signalDelegate.index)
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
                    }
                    Label {
                        // Never-arrived signals render "—", not a
                        // fabricated 0.0 (unsupported PIDs under obd2).
                        text: (QtTune.connected && signalDelegate.hasValue)
                              ? "%1 %2".arg(signalDelegate.value.toFixed(1)).arg(signalDelegate.unit)
                              : qsTr("—")
                        color: signalDelegate.subscribed ? Theme.text : Theme.textDim
                        font.pixelSize: 16
                        font.family: "Consolas"   // tabular-ish digits, no jitter
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
<<<<<<< Updated upstream
        // coalesced batch. Each trace takes its list row's colour
        // (root.seriesColor), so the list is the legend.
=======
        // coalesced batch. Trace palette is generated by hue rotation
        // (Theme exposes the few chromatic roles used above only, not a
        // series palette) so any number of series stays distinguishable.
>>>>>>> Stashed changes
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

<<<<<<< Updated upstream
=======
                // Series palette lives in Theme.qml — same swatches
                // across the app, no per-paint allocations.
                function seriesColor(i) {
                    return Theme.seriesColors[i % Theme.seriesColors.length]
                }

>>>>>>> Stashed changes
                Connections {
                    target: QtTune.signalModel
                    function onValuesUpdated() { chart.requestPaint() }
                    function onSubscribedCountChanged() { chart.requestPaint() }
                }

                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.fillStyle = Theme.surface
                    ctx.fillRect(0, 0, width, height)

                    const series = QtTune.signalModel.chartData()
                    if (!series || series.length === 0) {
                        ctx.fillStyle = Theme.textDim
                        ctx.font = "12px sans-serif"
                        ctx.textAlign = "center"
                        ctx.fillText(qsTr("no subscribed signals yet"), width / 2, height / 2)
                        return
                    }

                    // Shared TIME axis across series (actual timestamps),
                    // per-series VALUE scaling (kept independent deliberately).
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

                    // Horizontal grid: quarter lines
                    ctx.strokeStyle = Theme.bg
                    ctx.lineWidth = 1
                    ctx.beginPath()
                    for (let g = 1; g < 4; ++g) {
                        const gy = height * g / 4
                        ctx.moveTo(0, gy)
                        ctx.lineTo(width, gy)
                    }
                    ctx.stroke()

                    const topPad = 6, botPad = 6
                    for (let s = 0; s < series.length; ++s) {
                        const sp = series[s].samples
                        if (sp.length < 2) continue

                        let vMin = sp[1], vMax = sp[1]
                        for (let i = 1; i < sp.length; i += 2) {
                            if (sp[i] < vMin) vMin = sp[i]
                            if (sp[i] > vMax) vMax = sp[i]
                        }
                        if (vMax <= vMin) { vMin -= 1; vMax += 1 }   // flat line guard

<<<<<<< Updated upstream
                        ctx.strokeStyle = root.seriesColor(series[s].row)
=======
                        const color = Theme.seriesColors[colorIdx % Theme.seriesColors.length]
                        // Past the palette, hue space is spent: distinguish
                        // by dash tier as well. Tier = floor(idx / palette).
                        const tier = Math.floor(colorIdx / Theme.seriesColors.length)
                        ++colorIdx
                        ctx.strokeStyle = color
>>>>>>> Stashed changes
                        ctx.lineWidth = 2
                        if (tier === 1) {
                            ctx.setLineDash([8, 4])
                        } else if (tier === 2) {
                            ctx.setLineDash([2, 3])
                        } else {
                            ctx.setLineDash([])   // solid; tier 0 and beyond 2
                        }
                        ctx.beginPath()
                        for (let i = 0; i < sp.length; i += 2) {
                            const x = (sp[i] - tMin) / (tMax - tMin) * (width - 1)
                            const yNorm = (sp[i + 1] - vMin) / (vMax - vMin)
                            const y = height - botPad - yNorm * (height - topPad - botPad)
                            if (i === 0) ctx.moveTo(x, y)
                            else ctx.lineTo(x, y)
                        }
                        ctx.stroke()
<<<<<<< Updated upstream
=======

                        // Legend: current value, stacked top-left
                        ctx.fillStyle = color
                        ctx.font = "11px Consolas"
                        ctx.textAlign = "left"
                        ctx.fillText(
                            series[s].name + "  " +
                                series[s].value.toFixed(1) + " " + series[s].unit,
                            8, 16 + (colorIdx - 1) * 14)
>>>>>>> Stashed changes
                    }
                }

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                Component.onCompleted: requestPaint()
            }
        }
    }
}
