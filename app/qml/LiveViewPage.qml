// File/layer: app/qml/LiveViewPage.qml | Qt6 Quick, UI layer
// Testing: model contract covered by test_bridge; render verified manually

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Lexical id scoping — lets the delegate reference signalList (outer id)
// statically and is required for the required-property delegate pattern.
pragma ComponentBehavior: Bound

Page {
    id: root
    background: Rectangle { color: Theme.bg }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            text: qsTr("Live Signals")
            font.pixelSize: 20
            color: Theme.text
        }

        Label {
            text: QtTune.connected
                ? qsTr("Subscribed: %1 signals · batched @ 10 Hz").arg(QtTune.signalModel.subscribedCount)
                : qsTr("Connect a session to stream decoded values")
            color: QtTune.connected ? Theme.textDim : Theme.danger
        }

        ListView {
            id: signalList
            Layout.fillWidth: true
            Layout.preferredHeight: 240
            Layout.minimumHeight: 140
            clip: true
            model: QtTune.signalModel

            // Roles consumed via required properties: static typing, no
            // implicit model-chain lookups, updates driven by the model's
            // dataChanged role filtering (ValueRole re-binds these).
            delegate: Rectangle {
                id: signalDelegate
                required property int index
                required property string name
                required property string unit
                required property double value
                required property bool subscribed

                width: signalList.width
                height: 48
                color: signalDelegate.index % 2 === 0 ? Theme.surface : Theme.bg

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12

                    CheckBox {
                        checked: signalDelegate.subscribed
                        onToggled: QtTune.signalModel.toggleSubscribed(signalDelegate.index)
                    }
                    Label {
                        text: signalDelegate.name
                        color: Theme.text
                        Layout.fillWidth: true
                    }
                    Label {
                        text: QtTune.connected
                            ? "%1 %2".arg(signalDelegate.value.toFixed(1)).arg(signalDelegate.unit)
                            : qsTr("—")
                        color: signalDelegate.subscribed ? Theme.primary : Theme.textDim
                        font.family: "Consolas"   // tabular-ish digits, no jitter
                    }
                }
            }
        }

        // ---- Strip chart: per-signal rolling history, one repaint per
        // coalesced batch. Trace palette is hardcoded: Theme exposes the
        // few chromatic roles used above only, not a full series palette.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.surface
            radius: 4
            border.color: Theme.textDim
            border.width: 1

            Canvas {
                id: chart
                anchors.fill: parent
                anchors.margins: 4

                // Indexed by position in chartData(), which skips
                // unsubscribed signals, so unsubscribing one shifts the
                // colours of the signals after it
                readonly property var seriesColors: [
                    "#ff5555", "#55ff88", "#55aaff",
                    "#ffd166", "#c792ea", "#f78c6c"
                ]

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
                    let colorIdx = 0
                    for (let s = 0; s < series.length; ++s) {
                        const sp = series[s].samples
                        if (sp.length < 2) continue

                        let vMin = sp[1], vMax = sp[1]
                        for (let i = 1; i < sp.length; i += 2) {
                            if (sp[i] < vMin) vMin = sp[i]
                            if (sp[i] > vMax) vMax = sp[i]
                        }
                        if (vMax <= vMin) { vMin -= 1; vMax += 1 }   // flat line guard

                        const color = seriesColors[colorIdx % seriesColors.length]
                        ++colorIdx

                        ctx.strokeStyle = color
                        ctx.lineWidth = 2
                        ctx.beginPath()
                        for (let i = 0; i < sp.length; i += 2) {
                            const x = (sp[i] - tMin) / (tMax - tMin) * (width - 1)
                            const yNorm = (sp[i + 1] - vMin) / (vMax - vMin)
                            const y = height - botPad - yNorm * (height - topPad - botPad)
                            if (i === 0) ctx.moveTo(x, y)
                            else ctx.lineTo(x, y)
                        }
                        ctx.stroke()

                        // Legend: current value, stacked top-left
                        ctx.fillStyle = color
                        ctx.font = "11px Consolas"
                        ctx.textAlign = "left"
                        ctx.fillText(
                            series[s].name + "  " +
                                series[s].value.toFixed(1) + " " + series[s].unit,
                            8, 16 + colorIdx * 14)
                    }
                }

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                Component.onCompleted: requestPaint()
            }
        }
    }
}
