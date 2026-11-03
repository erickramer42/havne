// File/layer: app/qml/RecordingsPage.qml | Qt6 Quick, UI layer
// Testing: test_qml (tests/qml/tst_recordings.qml): the list from
//          Bridge.recentRecordings, a recording that ends joining it, Replay
//          handing its URI on, Open reaching the viewer (signals, the
//          default plotted, the chart's series over the view), the cursor's
//          readout, pan and zoom kept inside the recording, a finding's row
//          centering the view, a refused folder's error, back to the list;
//          every item inside a phone's width (tst_phone.qml). RecordingView
//          itself: test_bridge (RecordingViewTest); the chart's layout:
//          tst_chartlayout.qml; the drawing is manual smoke (docs/testing.md).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

pragma ComponentBehavior: Bound

// The recordings a session saved (ADR-0049), newest first: Open shows one
// in the viewer, Replay puts its replay:// URI in Settings' field. The
// viewer: the recording's signals (ticked ones plotted, the cursor's
// values beside each), the chart over the part in view (drag to pan, the
// wheel or a pinch to zoom, a tap to place the cursor), markers and the
// frames check's findings on it, and the findings listed.
Page {
    id: root
    background: Rectangle { color: Theme.bg }

    // Main puts the URI in Settings' field and shows it
    signal replayRequested(string uri)

    readonly property int recordingsListed: 20
    // [{ title, uri, folder, source, profile }] (Bridge.recentRecordings)
    property var recordings: []
    property string openedTitle: ""
    readonly property bool viewing: recording.state !== RecordingView.Closed

    // The part of the recording in view, in ms since it started, and the
    // cursor's time (-1 for none)
    property real viewT0: 0
    property real viewT1: 1000
    property real cursorMs: -1
    // Signal index -> plotted, from the recording's default at the open
    property var plotted: ({})
    // Signal index -> { text, unit } at the cursor
    property var readout: ({})
    readonly property real minSpanMs: 200
    readonly property int rowHeight: 40

    function refresh() {
        recordings = Bridge.recentRecordings(recordingsListed)
    }

    function open(row) {
        openedTitle = row.title
        recording.open(row.folder)
    }

    function backToList() {
        recording.close()
        openedTitle = ""
        refresh()
    }

    function durationMs() {
        return recording.durationMs > 0 ? recording.durationMs : 1000
    }

    // The view, kept inside the recording and no narrower than minSpanMs
    function setView(t0, t1) {
        const whole = durationMs()
        const span = Math.min(whole, Math.max(minSpanMs, t1 - t0))
        const start = Math.min(Math.max(0, t0), whole - span)
        viewT0 = start
        viewT1 = start + span
        chart.requestPaint()
    }
    function zoomAt(t, factor) {
        setView(t - (t - viewT0) * factor, t + (viewT1 - t) * factor)
    }
    function showAll() {
        setView(0, durationMs())
    }
    // A time placed in the middle of the view, the view's span kept
    function centerOn(t) {
        const half = (viewT1 - viewT0) / 2
        setView(t - half, t + half)
    }

    function setCursor(t) {
        cursorMs = Math.min(Math.max(0, t), durationMs())
        const values = {}
        const indexes = recording.signalList.map(s => s.index)
        for (const v of recording.valuesAt(indexes, cursorMs))
            values[v.index] = v
        readout = values
        chart.requestPaint()
    }

    function togglePlotted(index) {
        const next = Object.assign({}, plotted)
        next[index] = !next[index]
        plotted = next
        chart.requestPaint()
    }

    // The plot's width as last drawn, or the chart's before its first
    function plotWidth() {
        return chart.drawn ? chart.drawn.plotW : chart.shownW
    }
    function timeAt(x) {
        return viewT0 + (x - chart.x) / Math.max(1, plotWidth() - 1) * (viewT1 - viewT0)
    }

    // The chart's series: each plotted number's columns over the view, two
    // pixels a column, its label the cursor's value while that's in view
    function series(widthPx) {
        if (recording.state !== RecordingView.Open)
            return []
        const sigs = recording.signalList
        const indexes = sigs.filter(s => !s.isText && root.plotted[s.index]).map(s => s.index)
        if (indexes.length === 0)
            return []
        const n = Math.max(1, Math.floor((chart.drawn ? chart.drawn.plotW : widthPx * 0.8) / 2))
        const inView = cursorMs >= viewT0 && cursorMs <= viewT1
        return recording.columns(indexes, viewT0, viewT1, n).map(c => {
            const s = sigs[c.index]
            const at = root.readout[c.index]
            const value = inView && at && at.text !== "" ? Number(at.text) : NaN
            return { columns: { lows: c.lows, highs: c.highs }, row: c.index,
                     decimals: s.decimals, unit: s.unit, labelValue: value }
        })
    }

    function valueText(v, decimals, unit) {
        const n = Number(v).toLocaleString(Qt.locale(), 'f', decimals)
        return unit !== "" ? n + " " + unit : n
    }

    // 12.3 s, 1:02.3, 1:02:03
    function timeText(ms, tenths) {
        const s = ms / 1000
        if (s < 60)
            return tenths ? qsTr("%1 s").arg(s.toFixed(1)) : qsTr("%1 s").arg(Math.round(s))
        const whole = Math.floor(s)
        const sec = tenths ? (s % 60).toFixed(1).padStart(4, "0") : String(whole % 60).padStart(2, "0")
        if (s < 3600)
            return Math.floor(s / 60) + ":" + sec
        return Math.floor(s / 3600) + ":" + String(Math.floor(s / 60) % 60).padStart(2, "0") + ":" + sec
    }
    function captionOf(seconds) {
        return timeText(seconds * 1000, false)
    }
    function lengthText(ms) {
        const s = Math.round(ms / 1000)
        if (s < 60) return ms < 10000 ? qsTr("%1 s").arg((ms / 1000).toFixed(1)) : qsTr("%1 s").arg(s)
        if (s < 3600) return qsTr("%1 min %2 s").arg(Math.floor(s / 60)).arg(s % 60)
        return qsTr("%1 h %2 min").arg(Math.floor(s / 3600)).arg(Math.floor(s / 60) % 60)
    }

    function idText(f) {
        return "0x" + f.id.toString(16).toUpperCase().padStart(f.extended ? 8 : 3, "0")
    }
    // A finding in a few words (ECUIO_CHECK_* kinds)
    function findingText(f) {
        switch (f.kind) {
        case 1: return qsTr("No frames for %1").arg(lengthText(f.lengthMs))
        case 2: return qsTr("%1 slowed for %2").arg(idText(f)).arg(lengthText(f.lengthMs))
        case 3: return qsTr("%1 first heard late").arg(idText(f))
        case 4: return qsTr("Adapter overflow: frames lost")
        case 5: return qsTr("%1: a message broken off").arg(idText(f))
        case 6: return qsTr("Link: %1").arg(Bridge.statusString(f.code))
        case 7: return qsTr("Asleep for %1").arg(lengthText(f.lengthMs))
        default: return qsTr("Finding of kind %1").arg(f.kind)
        }
    }
    function findingColor(kind) {
        return kind === 7 ? Theme.info : kind === 1 || kind === 4 || kind === 5 ? Theme.danger : Theme.warning
    }

    function infoText() {
        const info = recording.info
        let text = qsTr("%1 long").arg(lengthText(recording.durationMs))
        if (info.utcStartMs > 0)
            text += qsTr(" · started %1").arg(new Date(info.utcStartMs).toLocaleString(Qt.locale(), Locale.ShortFormat))
        if (info.recordsDropped > 0)
            text += qsTr(" · %1 records dropped while recording").arg(info.recordsDropped)
        if (info.noSummary)
            text += qsTr(" · it ended without its summary (the app or the power stopped)")
        return text
    }
    function checkText() {
        const check = recording.check
        if (!check.ran)
            return qsTr("Frames check: didn't run (%1)").arg(check.error)
        const n = recording.findings.length
        return n === 0 ? qsTr("Frames check: nothing found in %1 frames").arg(check.frames)
                       : n === 1 ? qsTr("Frames check: 1 finding")
                                 : qsTr("Frames check: %1 findings").arg(n)
    }

    RecordingView {
        id: recording
        unitSystem: Bridge.unitSystem
        onStateChanged: {
            if (recording.state !== RecordingView.Open)
                return
            const defaults = {}
            for (const s of recording.signalList)
                defaults[s.index] = s.plotted
            root.plotted = defaults
            root.readout = ({})
            root.cursorMs = -1
            root.showAll()
        }
        onSignalListChanged: {
            if (root.cursorMs >= 0)
                root.setCursor(root.cursorMs)
            chart.requestPaint()
        }
    }

    Connections {
        target: Bridge
        // A recording that ended joins the list
        function onRecordingStateChanged() {
            if (!Bridge.recordingActive)
                root.refresh()
        }
    }
    onVisibleChanged: if (visible && !viewing) refresh()
    Component.onCompleted: refresh()

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: 10

        PageHeader { title: qsTr("Recordings") }

        // ---- The list ----
        Label {
            objectName: "recordingsHint"
            visible: !root.viewing
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: root.recordings.length > 0
                  ? qsTr("Newest first. Open one to look through it, or replay it as a session.")
                  : qsTr("No recordings yet: turn Record on in Settings, then connect.")
        }

        ListView {
            id: recordingList
            objectName: "recordingList"
            visible: !root.viewing
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            currentIndex: -1
            highlightFollowsCurrentItem: false
            boundsBehavior: Flickable.StopAtBounds
            model: root.recordings
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: recordingRow
                required property var modelData
                width: recordingList.width
                height: Math.max(56, rowLayout.implicitHeight + 2 * Theme.spacing)
                radius: Theme.radius
                color: rowArea.hovered ? Theme.elevatedLight : Theme.elevated

                // The row opens the recording: a button under the labels, so
                // UI Automation and a screen reader can press it
                AbstractButton {
                    id: rowArea
                    objectName: "recordingRowArea"
                    anchors.fill: parent
                    hoverEnabled: true
                    Accessible.name: qsTr("Open %1").arg(recordingRow.modelData.title)
                    onClicked: root.open(recordingRow.modelData)
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                }

                RowLayout {
                    id: rowLayout
                    anchors.fill: parent
                    anchors.margins: Theme.spacing
                    spacing: Theme.spacing

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            objectName: "recordingTitle"
                            text: recordingRow.modelData.title
                            color: Theme.text
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            objectName: "recordingDetail"
                            text: recordingRow.modelData.source !== ""
                                  ? qsTr("%1 · replays as %2").arg(recordingRow.modelData.source)
                                                              .arg(recordingRow.modelData.profile)
                                  : qsTr("replays as %1").arg(recordingRow.modelData.profile)
                            color: Theme.textDim
                            font.pointSize: 8
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                    }
                    ThemedButton {
                        objectName: "replayButton"
                        text: qsTr("Replay")
                        // Darker than its row, so it reads as a button there
                        baseColor: Theme.surface
                        enabled: !Bridge.connected && !Bridge.connecting
                        Layout.maximumWidth: 96
                        Accessible.name: qsTr("Replay %1").arg(recordingRow.modelData.title)
                        onClicked: root.replayRequested(recordingRow.modelData.uri)
                    }
                }
            }
        }

        // ---- The viewer ----
        RowLayout {
            visible: root.viewing
            Layout.fillWidth: true
            spacing: Theme.spacing
            ThemedButton {
                objectName: "backToList"
                text: qsTr("All recordings")
                onClicked: root.backToList()
            }
            Label {
                objectName: "openedTitle"
                text: root.openedTitle
                color: Theme.text
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        RowLayout {
            visible: recording.state === RecordingView.Opening
            Layout.fillWidth: true
            ProgressBar {
                objectName: "openProgress"
                value: recording.progress
                Layout.fillWidth: true
            }
            ThemedButton {
                objectName: "cancelOpen"
                text: qsTr("Cancel")
                onClicked: root.backToList()
            }
        }

        Label {
            objectName: "openError"
            visible: recording.state === RecordingView.Failed
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.danger
            text: qsTr("This recording can't be opened: %1").arg(recording.error)
        }

        Label {
            objectName: "recordingInfo"
            visible: recording.state === RecordingView.Open
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: recording.state === RecordingView.Open ? root.infoText() : ""
        }

        // The recording's signals: a tick plots one, and beside each its
        // value at the cursor
        ListView {
            id: signalRows
            objectName: "signalRows"
            visible: recording.state === RecordingView.Open
            Layout.fillWidth: true
            Layout.preferredHeight: count * root.rowHeight
            Layout.maximumHeight: Math.max(root.rowHeight, column.height * 0.3)
            clip: true
            currentIndex: -1
            highlightFollowsCurrentItem: false
            boundsBehavior: Flickable.StopAtBounds
            model: recording.signalList
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: signalRow
                required property var modelData
                required property int index
                readonly property int signalIndex: modelData.index
                readonly property var at: root.readout[signalIndex]
                width: signalRows.width
                height: root.rowHeight
                color: index % 2 === 0 ? Theme.surface : Theme.bg

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 12
                    CheckBox {
                        objectName: "plotCheck"
                        checked: root.plotted[signalRow.signalIndex] === true
                        enabled: !signalRow.modelData.isText
                        onToggled: root.togglePlotted(signalRow.signalIndex)
                        Accessible.name: qsTr("Plot %1").arg(signalRow.modelData.name)
                    }
                    Rectangle {
                        objectName: "swatch"
                        readonly property bool shown: root.plotted[signalRow.signalIndex] === true
                        implicitWidth: 10; implicitHeight: 10
                        radius: 2
                        color: shown ? Theme.seriesColor(signalRow.signalIndex) : "transparent"
                        border.width: 1
                        border.color: Theme.seriesColor(signalRow.signalIndex)
                        opacity: shown ? 1.0 : 0.5
                    }
                    Label {
                        text: signalRow.modelData.name
                        color: Theme.text
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        objectName: "cursorValue"
                        text: !signalRow.at || signalRow.at.atMs < 0 ? qsTr("—")
                              : signalRow.modelData.isText ? signalRow.at.text
                              : root.valueText(Number(signalRow.at.text), signalRow.modelData.decimals,
                                               signalRow.at.unit)
                        Layout.maximumWidth: Math.round(signalRow.width * 0.5)
                        elide: Text.ElideRight
                        color: Theme.text
                        font.family: Theme.monoFamily
                    }
                }
            }
        }

        // Zoom, and where the cursor is
        RowLayout {
            visible: recording.state === RecordingView.Open
            Layout.fillWidth: true
            spacing: 6
            ThemedButton {
                objectName: "zoomAll"
                text: qsTr("All")
                Layout.maximumWidth: 72
                onClicked: root.showAll()
            }
            ThemedButton {
                objectName: "zoomOut"
                text: "−"
                Layout.maximumWidth: 48
                Accessible.name: qsTr("Zoom out")
                onClicked: root.zoomAt((root.viewT0 + root.viewT1) / 2, 2)
            }
            ThemedButton {
                objectName: "zoomIn"
                text: "+"
                Layout.maximumWidth: 48
                Accessible.name: qsTr("Zoom in")
                onClicked: root.zoomAt((root.viewT0 + root.viewT1) / 2, 0.5)
            }
            Label {
                objectName: "cursorTime"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                color: Theme.textDim
                text: root.cursorMs < 0 ? qsTr("Tap the chart to read the values at a time")
                                        : qsTr("At %1").arg(root.timeText(root.cursorMs, true))
            }
        }

        Rectangle {
            id: chartFrame
            objectName: "chartFrame"
            visible: recording.state === RecordingView.Open
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 140
            color: Theme.surface
            radius: 4
            border.color: Theme.textDim
            border.width: 1

            StripChart {
                id: chart
                objectName: "chart"
                x: 4
                y: 4
                shownW: parent.width - 8
                shownH: parent.height - 8
                emptyText: qsTr("No signals plotted; tick a row to plot it")
                fetch: function(widthPx) { return root.series(widthPx) }
                layoutOpts: ({
                    gapMs: Theme.staleMs, minSpanUnits: 20, format: root.valueText,
                    span: { t0: root.viewT0, t1: root.viewT1 }, absolute: true,
                    caption: root.captionOf })
                // Findings as bars along the plot's foot, markers as dashed
                // lines with their labels, the cursor as a solid line
                decorate: function(ctx, l) {
                    const span = root.viewT1 - root.viewT0
                    const xOf = t => (t - root.viewT0) / span * (l.plotW - 1)
                    for (const f of recording.findings) {
                        const x0 = xOf(f.atMs), x1 = xOf(f.atMs + f.lengthMs)
                        if (x1 < 0 || x0 > l.plotW) continue
                        ctx.fillStyle = root.findingColor(f.kind)
                        const left = Math.max(0, x0)
                        ctx.fillRect(left, l.top + l.plotH - 4, Math.max(2, Math.min(l.plotW, x1) - left), 4)
                    }
                    ctx.strokeStyle = Theme.info
                    ctx.fillStyle = Theme.info
                    ctx.lineWidth = 1
                    ctx.setLineDash([4, 3])
                    ctx.textAlign = "left"
                    ctx.textBaseline = "top"
                    for (const m of recording.markers) {
                        const x = xOf(m.timeMs)
                        if (x < 0 || x > l.plotW) continue
                        ctx.beginPath()
                        ctx.moveTo(x, l.top)
                        ctx.lineTo(x, l.top + l.plotH)
                        ctx.stroke()
                        ctx.fillText(m.label, x + 3, l.top + 1)
                    }
                    ctx.setLineDash([])
                    if (root.cursorMs >= root.viewT0 && root.cursorMs <= root.viewT1) {
                        ctx.strokeStyle = Theme.text
                        ctx.beginPath()
                        ctx.moveTo(xOf(root.cursorMs), l.top)
                        ctx.lineTo(xOf(root.cursorMs), l.top + l.plotH)
                        ctx.stroke()
                    }
                }
            }

            // Drag pans, the wheel and a pinch zoom about the pointer, a
            // tap places the cursor
            DragHandler {
                id: pan
                target: null
                property real startT0: 0
                onActiveChanged: if (active) startT0 = root.viewT0
                onActiveTranslationChanged: {
                    if (!active) return
                    const span = root.viewT1 - root.viewT0
                    const t0 = startT0 - activeTranslation.x / Math.max(1, root.plotWidth() - 1) * span
                    root.setView(t0, t0 + span)
                }
            }
            PinchHandler {
                target: null
                property real lastScale: 1
                onActiveChanged: lastScale = 1
                onActiveScaleChanged: {
                    if (!active || activeScale <= 0) return
                    root.zoomAt(root.timeAt(centroid.position.x), lastScale / activeScale)
                    lastScale = activeScale
                }
            }
            WheelHandler {
                target: null
                onWheel: event => root.zoomAt(root.timeAt(event.x), event.angleDelta.y > 0 ? 0.8 : 1.25)
            }
            TapHandler {
                onTapped: (eventPoint, button) => root.setCursor(root.timeAt(eventPoint.position.x))
            }
        }

        // The frames check: what it found, each row centering the view on
        // its finding
        Label {
            objectName: "checkSummary"
            visible: recording.state === RecordingView.Open
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: recording.state === RecordingView.Open ? root.checkText() : ""
        }
        ListView {
            id: findingList
            objectName: "findingList"
            visible: recording.state === RecordingView.Open && count > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(count, 3) * 32
            clip: true
            currentIndex: -1
            highlightFollowsCurrentItem: false
            boundsBehavior: Flickable.StopAtBounds
            model: recording.findings
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: findingRow
                required property var modelData
                width: findingList.width
                height: 32
                color: findingArea.hovered ? Theme.elevatedLight : "transparent"
                AbstractButton {
                    id: findingArea
                    objectName: "findingRowArea"
                    anchors.fill: parent
                    hoverEnabled: true
                    Accessible.name: qsTr("Show %1").arg(root.findingText(findingRow.modelData))
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    onClicked: {
                        const f = findingRow.modelData
                        root.centerOn(f.atMs + f.lengthMs / 2)
                        root.setCursor(f.atMs)
                    }
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    Rectangle {
                        implicitWidth: 10; implicitHeight: 4
                        color: root.findingColor(findingRow.modelData.kind)
                    }
                    Label {
                        text: root.timeText(findingRow.modelData.atMs, true)
                        color: Theme.textDim
                        font.family: Theme.monoFamily
                    }
                    Label {
                        objectName: "findingText"
                        text: root.findingText(findingRow.modelData)
                        color: Theme.text
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }
}
