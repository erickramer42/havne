// File/layer: app/qml/RecordingChart.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)
// The viewer's chart: the strip chart, its pan, pinch, wheel and tap, the
// cursor handles and strip, the pane axes and seams; one part of
// RecordingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

Rectangle {
    id: root
    objectName: "chartFrame"
    required property RecordingsPage host
    property alias chart: chart
    property alias pan: pan
    property alias pinch: pinch
    // Maximized with Frames chosen, the table has the page instead;
    // a phone's Stretches panel stands in the chart's place
    visible: root.host.recording.state === RecordingView.Open && !root.host.exportShown
             && !(Theme.compact && (root.host.stretchesOpen || root.host.conditionsOpen))
             && !(root.host.chartMaximized && root.host.framesShown)
    Layout.fillWidth: true
    // A share of the page, so the rows show under it; the splitter
    // below sets it once dragged. Maximized, it has the page
    readonly property real restingHeight: root.host.viewerSettings.chartHeight > 0
                                          ? Math.max(200, root.host.viewerSettings.chartHeight)
                                          : root.host.defaultChartHeight
    Layout.minimumHeight: root.host.chartMaximized || root.host.pinnedViewer ? 200 : restingHeight
    Layout.preferredHeight: root.host.chartMaximized ? -1 : restingHeight
    // Pinned, it fills up to its height, so a short page shrinks it
    Layout.maximumHeight: root.host.pinnedViewer ? restingHeight : Number.POSITIVE_INFINITY
    Layout.fillHeight: root.host.chartMaximized || root.host.pinnedViewer
    color: Theme.surface
    radius: 4
    border.color: Theme.hairline
    border.width: 1

    // Maximized, the cursor's values in a strip beside the chart (below
    // it on a tall frame), as the signal rows show them otherwise; only
    // while the cursor is in view, so until a tap the chart has the page.
    // Its button folds it to a tab and opens it again
    readonly property bool cursorInView: root.host.hasCursor && root.host.cursorMs >= root.host.viewT0
                                         && root.host.cursorMs <= root.host.viewT1
    readonly property bool stripShown: root.host.chartMaximized && cursorInView
    // Taller than wide (a phone upright) the values go below the
    // chart, a band of cells, since height is what's to spare;
    // wider than tall, beside it in a column
    readonly property bool stripBelow: height > width
    readonly property var stripModel: stripShown
        ? root.host.recording.signalList.filter(s => root.host.plotted[s.index] === true) : []
    readonly property int stripColumns:
        Math.max(1, Math.floor((width - stripFold.width - 16) / 140))
    readonly property real stripWidth: !stripShown || stripBelow ? 0
        : root.host.viewerSettings.stripFolded ? stripFold.width + 8
        : Math.max(stripFold.width + 8, Math.min(170, Math.round(width * 0.25)))
    readonly property real stripHeight: !stripShown || !stripBelow ? 0
        : root.host.viewerSettings.stripFolded ? stripFold.height + 8
        : Math.max(stripFold.height + 8,
                   Math.min(Math.ceil(stripModel.length / stripColumns) * stripCellH + 8,
                            Math.round(height * 0.4)))
    // A cell's height: a third line with a span
    readonly property real stripCellH: root.host.hasSpan ? 56 : 40

    StripChart {
        id: chart
        objectName: "chart"
        x: 4
        y: 4
        shownW: parent.width - 8 - root.stripWidth
        shownH: parent.height - 8 - root.stripHeight
        emptyText: root.host.plottedNoValues.length > 0 ? root.host.reportPart.noValuesText(root.host.plottedNoValues)
                                                   : qsTr("No signals plotted; tick a row to plot it")
        fetch: function(widthPx) { return root.host.seriesPart.series(widthPx) }
        layoutOpts: ({
            gapMs: Theme.staleMs, minSpanUnits: 20, format: root.host.valueText,
            span: { t0: root.host.viewT0, t1: root.host.viewT1 }, absolute: true,
            caption: root.host.captionOf,
            tags: root.host.cursorsPart.cursorTags(),
            axisH: root.host.phoneShape && root.host.hasCursor ? ChartLayout.axisHCursor : ChartLayout.axisH,
            panes: { max: root.host.paneLimit, minH: ChartLayout.paneMinH,
                     shares: root.host.paneShares, ranges: root.host.paneRanges,
                     assign: root.host.panesPart.assignRows(),
                     mixedLabel: qsTr("mixed") } })
        onDrawnChanged: root.host.panesPart.notePanes(drawn)
        // Findings as bars along the plot's foot and the conditions in two
        // rows over them, the picked condition tinted, markers as dashed
        // lines with their labels, the cursors as solid lines over
        // their span's tint, the extremes' dots on the traces
        decorateBelow: function(ctx, l) {
            const span = root.host.viewT1 - root.host.viewT0
            const xOf = t => l.left + (t - root.host.viewT0) / span * (l.plotW - 1)
            const plotEnd = l.left + l.plotW
            // The condition last shown from the list, tinted the
            // plot's height so the eye finds it after the move
            const picked = root.host.conditionList[root.host.conditionPick]
            if (picked !== undefined) {
                const x0 = Math.max(l.left, xOf(picked.fromMs))
                const x1 = Math.min(plotEnd, xOf(picked.toMs))
                if (x1 > x0) {
                    ctx.fillStyle = Qt.alpha(root.host.conditionsPanel.colorOf(picked), 0.15)
                    ctx.fillRect(x0, l.top, x1 - x0, l.plotH)
                }
            }
            if (!root.host.bandsShown || root.host.stretchList === null)
                return
            const chosen = root.host.jumpIndex >= 0 ? root.host.stretchesOf(root.host.jumpKind)[root.host.jumpIndex]
                                               : undefined
            for (const s of root.host.stretchList) {
                const x0 = Math.max(l.left, xOf(s.fromMs))
                const x1 = Math.min(plotEnd, xOf(s.toMs))
                if (x1 <= x0)
                    continue
                const role = s.kind === "pull" ? Theme.stretchPull : Theme.stretchCruise
                const here = chosen !== undefined && s.kind === root.host.jumpKind
                             && s.fromMs === chosen.fromMs
                ctx.fillStyle = Qt.alpha(role, here ? 0.22 : 0.15)
                ctx.fillRect(x0, l.top, x1 - x0, l.plotH)
                // The edge along the plot's top, solid
                ctx.fillStyle = role
                ctx.fillRect(x0, l.top, x1 - x0, 2)
            }
        }
        decorate: function(ctx, l) {
            const span = root.host.viewT1 - root.host.viewT0
            const xOf = t => l.left + (t - root.host.viewT0) / span * (l.plotW - 1)
            const plotEnd = l.left + l.plotW
            for (const f of root.host.chartFindings) {
                const x0 = xOf(f.atMs), x1 = xOf(f.atMs + f.lengthMs)
                if (x1 < l.left || x0 > plotEnd) continue
                ctx.fillStyle = root.host.findingColor(f.kind)
                const left = Math.max(l.left, x0)
                ctx.fillRect(left, l.top + l.plotH - 4, Math.max(2, Math.min(plotEnd, x1) - left), 4)
            }
            root.host.drawConditions(ctx, l.top + l.plotH - 10, l.left, plotEnd, xOf, root.host.conditionsByLevel)
            ctx.strokeStyle = Theme.textDim
            ctx.fillStyle = Theme.textDim
            ctx.lineWidth = 1
            ctx.setLineDash([4, 3])
            ctx.textAlign = "left"
            ctx.textBaseline = "top"
            for (const m of root.host.chartMarkers) {
                const x = xOf(m.timeMs)
                if (x < l.left || x > plotEnd) continue
                ctx.beginPath()
                ctx.moveTo(x, l.top)
                ctx.lineTo(x, l.top + l.plotH)
                ctx.stroke()
                ctx.fillText(m.label, x + 3, l.top + 1)
            }
            ctx.setLineDash([])
            const tint = (from, to, alpha) => {
                const x0 = Math.max(l.left, xOf(from))
                const x1 = Math.min(plotEnd, xOf(to))
                if (x1 <= x0)
                    return
                const c = Qt.color(Theme.text)
                ctx.fillStyle = Qt.rgba(c.r, c.g, c.b, alpha)
                ctx.fillRect(x0, l.top, x1 - x0, l.plotH)
            }
            if (root.host.hasSpan)
                tint(root.host.spanFromMs, root.host.spanToMs, 0.06)
            if (!isNaN(root.host.boxFromMs))
                tint(Math.min(root.host.boxFromMs, root.host.boxToMs),
                     Math.max(root.host.boxFromMs, root.host.boxToMs), 0.14)
            ctx.strokeStyle = Theme.text
            for (const t of [root.host.cursorMs, root.host.cursorBMs]) {
                if (!(t >= root.host.viewT0 && t <= root.host.viewT1))
                    continue
                ctx.beginPath()
                ctx.moveTo(xOf(t), l.top)
                ctx.lineTo(xOf(t), l.top + l.plotH)
                ctx.stroke()
            }
            // The span's lowest hollow, its highest filled
            root.host.spanDots = root.host.hasSpan
                ? ChartLayout.extremeDots(l, root.host.spanStats, root.host.viewT0, root.host.viewT1) : []
            for (const d of root.host.spanDots) {
                ctx.beginPath()
                ctx.arc(d.x, d.y, root.host.cursorDotR, 0, 2 * Math.PI)
                if (d.filled) {
                    ctx.fillStyle = Theme.seriesColor(d.slot)
                    ctx.fill()
                } else {
                    ctx.strokeStyle = Theme.seriesColor(d.slot)
                    ctx.stroke()
                }
            }
            // A dot on each trace at the sample its value comes from
            for (const tr of l.traces) {
                if (!isFinite(tr.labelMs))
                    continue
                const x = xOf(tr.labelMs)
                if (x < l.left || x > plotEnd)
                    continue
                ctx.fillStyle = Theme.seriesColor(tr.slot)
                ctx.beginPath()
                ctx.arc(x, tr.endY, root.host.cursorDotR, 0, 2 * Math.PI)
                ctx.fill()
            }
        }
    }

    // Drag pans, the wheel and a pinch zoom about the pointer, a
    // tap places the cursor. A drag is sideways only, so the page
    // scrolls up and down; one that starts while a pinch is on or
    // just after is the pinch's last finger, and moves nothing.
    // Under Shift or after a long press it draws the span's box
    DragHandler {
        id: pan
        objectName: "chartPan"
        target: null
        yAxis.enabled: false
        property real startT0: 0
        property bool fromPinch: false
        property bool boxing: false
        onActiveChanged: {
            if (!active) {
                if (boxing)
                    root.host.cursorsPart.finishBox()
                boxing = false
                return
            }
            startT0 = root.host.viewT0
            fromPinch = pinch.active || Date.now() - pinch.endedAt < 300
            boxing = !fromPinch && (chartTap.held || (centroid.modifiers & Qt.ShiftModifier) !== 0)
            if (boxing) {
                root.host.boxFromMs = root.host.cursorsPart.timeAt(centroid.pressPosition.x)
                root.host.boxToMs = root.host.boxFromMs
            }
        }
        onActiveTranslationChanged: {
            if (!active || fromPinch) return
            if (boxing) {
                root.host.boxToMs = Math.min(root.host.viewT1, Math.max(root.host.viewT0, root.host.cursorsPart.timeAt(centroid.position.x)))
                chart.repaintMarks()
                return
            }
            const span = root.host.viewT1 - root.host.viewT0
            const t0 = startT0 - activeTranslation.x / Math.max(1, root.host.cursorsPart.plotWidth() - 1) * span
            root.host.cursorsPart.setView(t0, t0 + span)
        }
    }
    // A pinch zooms the time about its middle; one whose fingers
    // lie more up and down than across as it starts, over a pane
    // with an axis, zooms that pane's range about its middle instead
    PinchHandler {
        id: pinch
        objectName: "chartPinch"
        target: null
        property real lastScale: 1
        property double endedAt: 0
        // The pane a vertical pinch zooms, -1 for a time zoom
        property int pane: -1
        property var startPane: null
        property real startY: 0
        onActiveChanged: {
            lastScale = 1
            pane = -1
            if (!active) {
                endedAt = Date.now()
                return
            }
            const l = chart.drawn
            const a = fingerA.point.position, b = fingerB.point.position
            if (!l || !fingerA.active || !fingerB.active
                    || Math.abs(b.y - a.y) <= Math.abs(b.x - a.x))
                return
            startY = centroid.position.y - chart.y
            const i = ChartLayout.paneAt(l, startY)
            if (i < 0 || !l.panes[i].axis)
                return
            pane = i
            startPane = Object.assign({}, l.panes[i])
        }
        onActiveScaleChanged: {
            if (!active || activeScale <= 0) return
            if (pane >= 0) {
                root.host.panesPart.setPaneRange(startPane.key,
                                  ChartLayout.scaledRange(startPane, startY, 1 / activeScale))
                return
            }
            root.host.cursorsPart.zoomAt(root.host.cursorsPart.timeAt(centroid.position.x), lastScale / activeScale)
            lastScale = activeScale
        }
    }
    // The pinch's two fingers, for which way it runs as it starts
    PointHandler {
        id: fingerA
        acceptedDevices: PointerDevice.TouchScreen
    }
    PointHandler {
        id: fingerB
        acceptedDevices: PointerDevice.TouchScreen
    }
    // A notch (120) in shows 0.8 of the span, out 1.25; a
    // high-resolution wheel's or a touchpad's parts of a notch zoom
    // by their share of it, and a sideways scroll doesn't zoom.
    // With Ctrl, over a pane with an axis, it zooms the pane's range
    // about the pointer instead
    WheelHandler {
        target: null
        onWheel: event => {
            const dy = event.angleDelta.y
            if (dy === 0)
                return
            const factor = Math.pow(0.8, dy / 120)
            if (event.modifiers & Qt.ControlModifier) {
                const l = chart.drawn
                const i = l ? ChartLayout.paneAt(l, event.y - chart.y) : -1
                if (i >= 0 && l.panes[i].axis)
                    root.host.panesPart.setPaneRange(l.panes[i].key,
                                      ChartLayout.scaledRange(l.panes[i], event.y - chart.y, factor))
                return
            }
            root.host.cursorsPart.zoomAt(root.host.cursorsPart.timeAt(event.x), factor)
        }
    }
    DoubleTapHandler {
        id: chartTap
        objectName: "chartTap"
        property point lastAt: Qt.point(0, 0)
        // A long press fired in this press, so its drag draws a box
        property bool held: false
        onPressedChanged: if (pressed) {
            held = false
            // A box a mouse pick focused gives the page's keys back (the
            // chart's own item: the page's focus would leave the box's)
            root.forceActiveFocus()
        }
        onLongPressed: {
            const at = chartTap.point.position
            // A press on a value label offers its signal's pane, on any form
            const labeled = root.host.panesPart.labelRowAt(at.x - chart.x, at.y - chart.y)
            if (labeled >= 0) {
                held = true
                root.host.panesPart.openPaneMenu(labeled, chart)
                return
            }
            if (!root.host.phoneShape || at.x - chart.x < root.host.cursorsPart.plotLeft())
                return
            held = true
            root.host.cursorsPart.placeSecond(root.host.cursorsPart.snapped(root.host.cursorsPart.timeAt(at.x)))
        }
        onTapped: (eventPoint, button) => {
            lastAt = eventPoint.position
            // A tap on the axes' column places no cursor
            if (eventPoint.position.x - chart.x < root.host.cursorsPart.plotLeft())
                return
            const t = root.host.cursorsPart.snapped(root.host.cursorsPart.timeAt(eventPoint.position.x))
            if ((chartTap.point.modifiers & Qt.ShiftModifier) !== 0)
                root.host.cursorsPart.placeSecond(t)
            else
                root.host.cursorsPart.setCursor(t)
        }
        // A double-click or double-tap maximizes it, or gives the
        // signals back (on an axis or a seam, root.doubleTapAt);
        // the two taps of a thumb may land a little apart
        onDoubleTap: root.host.panesPart.doubleTapAt(lastAt.x - chart.x, lastAt.y - chart.y)
    }

    // On the desktop a drag on a pane's axis pans its range (a
    // double click there, the handler above, makes it automatic again)
    Repeater {
        model: Theme.mobile ? 0 : root.host.paneBoxes.length
        delegate: Item {
            id: paneAxis
            required property int index
            readonly property var box: root.host.paneBoxes[index]
            objectName: "paneAxis_" + index
            visible: box !== undefined && box.axis
            x: chart.x
            y: chart.y + (box ? box.top : 0)
            width: box ? box.left : 0
            height: box ? box.h : 0
            HoverHandler { cursorShape: Qt.SizeVerCursor }
            DragHandler {
                target: null
                xAxis.enabled: false
                property var startRange: null
                onActiveChanged: {
                    const p = active && chart.drawn ? chart.drawn.panes[paneAxis.index] : null
                    startRange = p ? { vMin: p.vMin, vMax: p.vMax, key: p.key, h: p.h } : null
                }
                onActiveTranslationChanged: {
                    if (active && startRange)
                        root.host.panesPart.setPaneRange(startRange.key, ChartLayout.pannedRange(
                            startRange, startRange.h, activeTranslation.y))
                }
            }
        }
    }
    // The seams between panes: a drag trades two neighbors' heights,
    // kept by the count of panes; a double tap evens them (above).
    // A count for a model, so a drag's own resize keeps its handler
    Repeater {
        model: Math.max(0, root.host.paneBoxes.length - 1)
        delegate: Item {
            id: seam
            required property int index
            readonly property var box: root.host.paneBoxes[index]
            objectName: "paneSeam_" + index
            x: chart.x + (box ? box.left : 0)
            y: chart.y + (box ? box.top + box.h + ChartLayout.paneGap / 2 : 0) - height / 2
            width: box ? box.plotW : 0
            height: 24
            Rectangle {
                anchors.centerIn: parent
                width: 40
                height: 4
                radius: 2
                color: seamDrag.active || seamHover.hovered ? Theme.textDim : Theme.outline
            }
            HoverHandler { id: seamHover; cursorShape: Qt.SizeVerCursor }
            DragHandler {
                id: seamDrag
                objectName: "seamDrag"
                target: null
                // A touch's height on a phone, drawn as it is
                margin: root.host.phoneShape ? Math.max(0, (Theme.touchTarget - seam.height) / 2) : 0
                xAxis.enabled: false
                property var startHeights: []
                onActiveChanged: if (active) startHeights = root.host.paneBoxes.map(b => b.h)
                onActiveTranslationChanged: {
                    if (active && startHeights.length > seam.index + 1)
                        root.host.panesPart.setPaneShares(startHeights.length, ChartLayout.seamShares(
                            startHeights, seam.index, activeTranslation.y, ChartLayout.paneMinH))
                }
            }
        }
    }

    // A cursor's handle at the axis's foot, its time on it; a drag
    // moves its cursor. It reaches up over the plot to a touch's
    // height, and takes the press from the chart's own gestures
    component CursorHandle: Item {
        id: handle
        required property bool isB
        readonly property real at: isB ? root.host.cursorBMs : root.host.cursorMs
        readonly property real minWidth: root.host.phoneShape ? Theme.touchTarget : 24
        readonly property real tabH: chart.drawn ? chart.drawn.axisH : ChartLayout.axisH
        visible: chart.drawn !== null && at >= root.host.viewT0 && at <= root.host.viewT1
        width: Math.max(minWidth, handleText.implicitWidth + 8)
        height: root.host.phoneShape ? Theme.touchTarget : 24
        x: Math.min(Math.max(0, root.host.cursorsPart.xOfTime(at) - width / 2), root.width - width)
        y: chart.y + (chart.drawn ? chart.drawn.top + chart.drawn.plotH + tabH : 0) - height
        Accessible.role: Accessible.Slider
        Accessible.name: qsTr("Cursor at %1").arg(handleText.text)
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: handle.tabH
            radius: 2
            readonly property bool moved: handle.isB === root.host.arrowsMoveB
            color: moved ? Theme.text : Theme.elevated
            border.width: 1
            border.color: Theme.text
            Label {
                id: handleText
                objectName: "handleTime"
                anchors.centerIn: parent
                text: root.host.timeText(handle.at, true)
                color: parent.moved ? Theme.bg : Theme.text
                font.pixelSize: ChartLayout.labelPx
                font.family: Theme.valueFamily
                font.features: Theme.valueFeatures
            }
        }
        MouseArea {
            objectName: "handleDrag"
            anchors.fill: parent
            cursorShape: Qt.SizeHorCursor
            preventStealing: true
            property real pressX: 0
            property bool dragging: false
            function frameX(x) {
                return mapToItem(root, x, 0).x
            }
            onPressed: mouse => {
                pressX = frameX(mouse.x)
                dragging = false
            }
            onPositionChanged: mouse => {
                const x = frameX(mouse.x)
                if (!dragging && Math.abs(x - pressX) < Application.styleHints.startDragDistance)
                    return
                dragging = true
                // Past the plot's edge the handle would leave the view,
                // hide, and lose the grab before the drop
                const t = Math.min(Math.max(root.host.cursorsPart.timeAt(x), root.host.viewT0), root.host.viewT1)
                if (handle.isB)
                    root.host.cursorsPart.setCursorB(t)
                else
                    root.host.cursorsPart.setCursor(t)
            }
            onReleased: if (dragging) root.host.cursorsPart.dropHandle(handle.isB)
            onCanceled: if (dragging) root.host.cursorsPart.dropHandle(handle.isB)
        }
    }
    CursorHandle { id: handleA; objectName: "cursorHandleA"; isB: false }
    CursorHandle { id: handleB; objectName: "cursorHandleB"; isB: true }

    ThemedButton {
        id: stripFold
        objectName: "stripFold"
        visible: root.stripShown
        compact: true
        // Placed, not anchored: an anchor switched off keeps the
        // size it gave, so the strip's two shapes set x and y
        x: parent.width - 4 - width
        y: root.stripBelow ? parent.height - 4 - height : 4
        text: root.stripBelow ? (root.host.viewerSettings.stripFolded ? "^" : "v")
                                    : (root.host.viewerSettings.stripFolded ? "<" : ">")
        Accessible.name: root.host.viewerSettings.stripFolded ? qsTr("Show the values at the cursor")
                                                    : qsTr("Hide the values at the cursor")
        onClicked: root.host.viewerSettings.stripFolded = !root.host.viewerSettings.stripFolded
    }

    GridView {
        objectName: "cursorStrip"
        visible: root.stripShown && !root.host.viewerSettings.stripFolded
        x: root.stripBelow ? 4 : parent.width - root.stripWidth
        y: root.stripBelow ? parent.height - root.stripHeight + 4
                                 : stripFold.y + stripFold.height + 4
        width: root.stripBelow ? stripFold.x - 8 : root.stripWidth - 4
        height: root.stripBelow ? root.stripHeight - 8 : parent.height - 4 - y
        cellWidth: root.stripBelow ? Math.floor(width / root.stripColumns) : width
        cellHeight: root.stripCellH
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: visible ? root.stripModel : []
        delegate: Item {
            id: stripRow
            required property var modelData
            readonly property var at: root.host.readoutFrom[modelData.index]
            width: GridView.view.cellWidth
            height: root.stripCellH
            Rectangle {
                x: 0
                y: 6
                width: 8
                height: 8
                radius: 2
                color: Theme.seriesColor(root.host.panesPart.slotOf(stripRow.modelData.index))
            }
            Label {
                objectName: "stripName"
                x: 12
                width: parent.width - 12
                text: stripRow.modelData.name
                color: Theme.textDim
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
            Label {
                objectName: "stripValue"
                x: 12
                y: 16
                width: parent.width - 12
                text: !stripRow.at || stripRow.at.atMs < 0 ? qsTr("—")
                      : stripRow.modelData.isText ? stripRow.at.text
                      : stripRow.at.label ? stripRow.at.label
                      : root.host.valueText(Number(stripRow.at.text), stripRow.modelData.decimals,
                                       stripRow.at.unit)
                color: Theme.text
                font.family: Theme.valueFamily
                font.features: Theme.valueFeatures
                elide: Text.ElideRight
            }
            Label {
                objectName: "stripSpan"
                visible: root.host.hasSpan
                x: 12
                y: 34
                width: parent.width - 12
                text: visible ? root.host.spanCaption(stripRow.modelData) : ""
                color: Theme.textDim
                font.pixelSize: Theme.fontCaption
                font.family: Theme.valueFamily
                font.features: Theme.valueFeatures
                elide: Text.ElideRight
            }
        }
    }
}
