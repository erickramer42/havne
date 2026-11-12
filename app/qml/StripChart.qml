// File/layer: app/qml/StripChart.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.chartlayout, QmlTest.liveview, QmlTest.phone,
//          QmlTest.recordings (test_qml); Manual smoke

import QtQuick
import QtQuick.Shapes
import App

// A strip chart: what ChartLayout.layout returns for the series its
// fetch() gives. Live and the Recordings page each make one. The Canvas
// draws the background, the grid, the captions, the panes' axes and seams
// and the labels; the traces
// are a Shape over it, which the scene graph draws on the GPU (rasterized
// on the Canvas, a phone's full-resolution strokes and bands held the GUI
// thread about 150 ms a paint), and a page's marks a Canvas over that.
// Traces take Theme.seriesColor and Theme.seriesDash by row, so
// a page's list rows can be its legend (a series' own dash, when it has
// one, in place of its row's). A Canvas paints an image at its
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
    // A page's marks, drawn over the traces: decorate(ctx, layout)
    property var decorate: null
    // What drawing has cost since the page last zeroed them: paints, and
    // their total and longest time in ms (onPaint's own, to the ms)
    property int paints: 0
    property real paintMs: 0
    property real paintMaxMs: 0
    // Set while a page moves the view: the Canvases draw without
    // antialiasing, then once more with it when the view rests (the
    // traces, on the GPU, keep theirs). On the CPU it was most of a crowded
    // chart's cost (a dense hour's pan on the test phone, 21 charts a
    // second with it, 62 without; ADR-0049's appendix "the viewer")
    property bool moving: false
    antialiasing: !moving
    onMovingChanged: if (!moving) requestPaint()

    readonly property real zoom: Theme.zoom
    property real shownW: 0
    property real shownH: 0
    width: shownW * zoom
    height: shownH * zoom
    scale: 1 / zoom
    transformOrigin: Item.TopLeft
    // Rasterized on the GUI thread, so each frame shows the chart its paint
    // drew. Threaded falls behind: the window presents an earlier paint, and
    // once the view stops a chart it has left stays up until later frames
    renderStrategy: Canvas.Immediate

    // The traces: per trace a ShapePath of its lines, one of its bands'
    // fill and one of their edges, made as traces first need them and kept
    property var tracePaths: []
    Shape {
        id: traceShape
        objectName: "chartTraces"
        width: chart.shownW
        height: chart.shownH
        scale: chart.zoom
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer
    }
    // A page's marks over the traces: a Canvas of its own above the Shape,
    // which holds only what decorate draws and is painted in the same
    // frame as the chart, from the layout the chart drew
    Canvas {
        id: marks
        objectName: "chartMarks"
        visible: chart.decorate !== null
        width: chart.width
        height: chart.height
        antialiasing: chart.antialiasing
        renderStrategy: Canvas.Immediate
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.clearRect(0, 0, width, height)
            ctx.scale(chart.zoom, chart.zoom)
            ctx.font = ChartLayout.labelPx + "px " + (Theme.fontFamily || "sans-serif")
            if (chart.drawn && chart.decorate)
                chart.decorate(ctx, chart.drawn)
        }
    }
    Component {
        id: tracePath
        ShapePath {
            fillColor: "transparent"
            strokeColor: "transparent"
            strokeWidth: 0
            capStyle: ShapePath.FlatCap
            joinStyle: ShapePath.MiterJoin
            PathMultiline { paths: [] }
        }
    }

    // Only a page's marks again, for a mark that moves while the traces rest
    function repaintMarks() {
        marks.requestPaint()
    }

    function pathsFor(i) {
        while (tracePaths.length <= i) {
            const set = { line: tracePath.createObject(traceShape),
                          fill: tracePath.createObject(traceShape),
                          edge: tracePath.createObject(traceShape) }
            traceShape.data.push(set.fill)
            traceShape.data.push(set.line)
            traceShape.data.push(set.edge)
            tracePaths.push(set)
        }
        return tracePaths[i]
    }

    // A Canvas dash (px) as a pen's, in units of its width
    function stroke(path, color, width, dash, lines) {
        path.strokeColor = lines.length > 0 ? color : "transparent"
        path.strokeWidth = width
        path.strokeStyle = dash.length > 0 ? ShapePath.DashLine : ShapePath.SolidLine
        if (dash.length > 0)
            path.dashPattern = dash.map(d => d / width)
        path.pathElements[0].paths = lines
    }

    function setTraces(traces) {
        const pts = list => list.map(p => Qt.point(p.x, p.y))
        for (let i = 0; i < traces.length; ++i) {
            const tr = traces[i]
            const set = pathsFor(i)
            const color = Theme.seriesColor(tr.row)
            const dash = tr.dash !== undefined ? tr.dash : Theme.seriesDash(tr.row)
            const lines = [], fills = [], edges = []
            for (const piece of tr.pieces) {
                if (!piece.band) {
                    lines.push(pts(piece.points))
                    continue
                }
                fills.push(pts(piece.top.concat(piece.bottom.slice().reverse())))
                edges.push(pts(piece.top))
                edges.push(pts(piece.bottom))
            }
            stroke(set.line, color, ChartLayout.lineW, dash, lines)
            stroke(set.edge, color, ChartLayout.bandEdgeW, dash, edges)
            const c = Qt.color(color)
            set.fill.fillColor = fills.length > 0 ? Qt.rgba(c.r, c.g, c.b, chart.bandAlpha)
                                                  : "transparent"
            set.fill.pathElements[0].paths = fills
        }
        for (let i = traces.length; i < tracePaths.length; ++i) {
            for (const path of [tracePaths[i].line, tracePaths[i].fill, tracePaths[i].edge]) {
                path.strokeColor = "transparent"
                path.fillColor = "transparent"
                path.pathElements[0].paths = []
            }
        }
    }

    onPaint: {
        const begun = Date.now()
        draw()
        if (marks.visible)
            marks.requestPaint()
        const took = Date.now() - begun
        paints += 1
        paintMs += took
        paintMaxMs = Math.max(paintMaxMs, took)
    }

    // Canvas text takes no font features, so tabular figures are drawn by
    // hand: each digit in a cell as wide as the face's widest digit, any
    // other run of text whole (its kerning, shaping and combining marks
    // intact). The layout measures the same way. digitW is set once per
    // draw(), for the font that draw has set.
    property real digitW: 0
    property var digitWs: []
    readonly property var runPattern: /[0-9]+|[^0-9]+/g

    function setDigitW(ctx) {
        const ws = []
        for (let d = 0; d < 10; ++d)
            ws.push(ctx.measureText(String(d)).width)
        digitWs = ws
        digitW = Math.max.apply(null, ws)
    }

    function runsOf(text) {
        return text.match(runPattern) || []
    }

    function isDigits(run) {
        return run.charCodeAt(0) >= 48 && run.charCodeAt(0) <= 57
    }

    function tabularWidth(ctx, text) {
        let w = 0
        for (const run of runsOf(text))
            w += isDigits(run) ? run.length * digitW : ctx.measureText(run).width
        return w
    }

    // fillText with tabular digits, at ctx.textAlign's anchor
    function tabularText(ctx, text, x, y) {
        const align = ctx.textAlign
        let at = x
        if (align === "right")
            at -= tabularWidth(ctx, text)
        else if (align === "center")
            at -= tabularWidth(ctx, text) / 2
        ctx.textAlign = "left"
        for (const run of runsOf(text)) {
            if (!isDigits(run)) {
                ctx.fillText(run, at, y)
                at += ctx.measureText(run).width
                continue
            }
            for (const c of run) {
                ctx.fillText(c, at + (digitW - digitWs[c.charCodeAt(0) - 48]) / 2, y)
                at += digitW
            }
        }
        ctx.textAlign = align
    }

    function draw() {
        const ctx = getContext("2d")
        ctx.reset()
        ctx.scale(zoom, zoom)
        ctx.fillStyle = Theme.surface
        ctx.fillRect(0, 0, shownW, shownH)

        const series = chart.fetch(Math.ceil(shownW))
        if (!series || series.length === 0) {
            chart.drawn = null
            setTraces([])
            ctx.fillStyle = Theme.textDim
            ctx.font = Theme.fontCaption + "px " + (Theme.fontFamily || "sans-serif")
            ctx.textAlign = "center"
            ctx.fillText(chart.emptyText, shownW / 2, shownH / 2)
            return
        }

        ctx.font = ChartLayout.labelPx + "px " + (Theme.fontFamily || "sans-serif")   // measured and drawn in
        setDigitW(ctx)
        const opts = { measure: text => tabularWidth(ctx, text) }
        for (const k in chart.layoutOpts)
            opts[k] = chart.layoutOpts[k]
        const l = ChartLayout.layout(series, shownW, shownH, opts)
        chart.drawn = l
        if (!l) {
            setTraces([])
            return
        }

        // Grid: a pane's value ticks (quarter lines without an axis), and
        // one vertical per time tick through every pane
        ctx.strokeStyle = Theme.bg
        ctx.lineWidth = 1
        ctx.beginPath()
        for (const gy of l.hGrid) {
            ctx.moveTo(l.left, gy)
            ctx.lineTo(l.left + l.plotW, gy)
        }
        for (const tick of l.ticks) {
            ctx.moveTo(tick.x, l.top)
            ctx.lineTo(tick.x, l.top + l.plotH)
        }
        ctx.stroke()

        // The seams between panes, a hairline across the plot
        if (l.seams.length > 0) {
            ctx.strokeStyle = Theme.outline
            ctx.beginPath()
            for (const sy of l.seams) {
                ctx.moveTo(0, Math.round(sy) + 0.5)
                ctx.lineTo(l.left + l.plotW, Math.round(sy) + 0.5)
            }
            ctx.stroke()
        }

        // Captions under the time ticks; each pane's name at its axis's
        // top and its value captions right-aligned against the plot
        ctx.fillStyle = Theme.textDim
        ctx.textBaseline = "top"
        for (const tick of l.ticks) {
            if (!tick.captioned) continue
            ctx.textAlign = tick.align
            tabularText(ctx, tick.caption, tick.x, l.top + l.plotH + 3)
        }
        for (const pane of l.panes) {
            if (pane.label !== "") {
                ctx.textAlign = "left"
                ctx.textBaseline = "top"
                tabularText(ctx, pane.label, 0, pane.top)
            }
            ctx.textAlign = "right"
            ctx.textBaseline = "middle"
            for (const tick of pane.ticks) {
                if (tick.captioned)
                    tabularText(ctx, tick.caption, l.left - ChartLayout.axisGap, tick.y)
            }
        }

        // Traces, each piece its own polyline (a gap is a break); a band
        // filled faintly, then its two edges: the Shape over the Canvas
        setTraces(l.traces)

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
            ctx.setLineDash(tr.dash !== undefined ? tr.dash : Theme.seriesDash(tr.row))
            ctx.beginPath()
            ctx.moveTo(l.labelX, tr.labelY)
            ctx.lineTo(l.labelX + ChartLayout.swatchW, tr.labelY)
            ctx.stroke()
            ctx.fillStyle = c
            tabularText(ctx, tr.label, l.labelX + ChartLayout.swatchW + 4, tr.labelY)
        }
        ctx.setLineDash([])
    }

    // The traces and the chrome are read from Theme as they are painted:
    // a new palette paints them again
    Connections {
        target: Theme
        function onActiveChanged() { chart.requestPaint() }
    }
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    Component.onCompleted: requestPaint()
}
