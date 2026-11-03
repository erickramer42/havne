// File/layer: app/qml/StripChart.qml | Qt6 Quick, UI layer
// Testing: the layout it strokes is ChartLayout's (tst_chartlayout.qml);
//          Live's chart through it (tst_liveview.qml, tst_phone.qml); the
//          drawing itself is manual smoke (docs/testing.md)

import QtQuick
import App

// A strip chart: what ChartLayout.layout returns for the series its
// fetch() gives, stroked on a Canvas. Live and the Recordings page each
// make one. Traces take Theme.seriesColor and Theme.seriesDash by row, so
// a page's list rows can be its legend. A Canvas paints an image at its
// own size, which Main.qml's zoom would stretch: this one is Theme.zoom
// times the size it shows at, scaled back by as much, so its pixels meet
// the screen's one to one, and onPaint draws in the shown size.
Canvas {
    id: chart

    // fetch(widthPx) -> series, as ChartLayout.layout takes them
    property var fetch: function(widthPx) { return [] }
    // ChartLayout's opts but measure, which the chart supplies
    property var layoutOpts: ({})
    // What an empty chart says
    property string emptyText: ""
    // A crowded trace's band fill, faint so the traces under it show
    property real bandAlpha: 0.3
    // The last layout drawn, null for none: a page places its own marks
    // (a cursor, markers) by its plotW, top and plotH
    property var drawn: null
    // Called after the traces, before the labels: decorate(ctx, layout)
    property var decorate: null

    readonly property real zoom: Theme.zoom
    property real shownW: 0
    property real shownH: 0
    width: shownW * zoom
    height: shownH * zoom
    scale: 1 / zoom
    transformOrigin: Item.TopLeft

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        ctx.scale(zoom, zoom)
        ctx.fillStyle = Theme.surface
        ctx.fillRect(0, 0, shownW, shownH)

        const series = chart.fetch(Math.ceil(shownW))
        if (!series || series.length === 0) {
            chart.drawn = null
            ctx.fillStyle = Theme.textDim
            ctx.font = "12px sans-serif"
            ctx.textAlign = "center"
            ctx.fillText(chart.emptyText, shownW / 2, shownH / 2)
            return
        }

        ctx.font = ChartLayout.labelPx + "px " + Theme.monoFamily   // measured and drawn in
        const opts = { measure: text => ctx.measureText(text).width }
        for (const k in chart.layoutOpts)
            opts[k] = chart.layoutOpts[k]
        const l = ChartLayout.layout(series, shownW, shownH, opts)
        chart.drawn = l
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

        // Captions under the ticks
        ctx.fillStyle = Theme.textDim
        ctx.textBaseline = "top"
        for (const tick of l.ticks) {
            if (!tick.captioned) continue
            ctx.textAlign = tick.align
            ctx.fillText(tick.caption, tick.x, l.top + l.plotH + 3)
        }

        // Traces, each piece its own stroke (a gap is a break);
        // a band filled faintly, then its two edges
        const polyline = pts => {
            ctx.moveTo(pts[0].x, pts[0].y)
            for (let i = 1; i < pts.length; ++i)
                ctx.lineTo(pts[i].x, pts[i].y)
        }
        for (const tr of l.traces) {
            const color = Theme.seriesColor(tr.row)
            ctx.strokeStyle = color
            ctx.fillStyle = color
            ctx.setLineDash(Theme.seriesDash(tr.row))
            for (const piece of tr.pieces) {
                if (!piece.band) {
                    ctx.lineWidth = ChartLayout.lineW
                    ctx.beginPath()
                    polyline(piece.points)
                    ctx.stroke()
                    continue
                }
                ctx.globalAlpha = chart.bandAlpha
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
        ctx.setLineDash([])

        if (chart.decorate)
            chart.decorate(ctx, l)

        // Value labels, each starting with a sample of its
        // trace's stroke, so dashed rows read apart from solid;
        // the layout leaves some out when the plot is short
        ctx.lineWidth = 1
        ctx.textAlign = "left"
        ctx.textBaseline = "middle"
        for (const tr of l.traces) {
            if (!tr.labeled) continue   // no room: its row shows the value
            const c = Theme.seriesColor(tr.row)
            ctx.strokeStyle = c
            ctx.setLineDash(Theme.seriesDash(tr.row))
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
