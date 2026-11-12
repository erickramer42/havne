// File/layer: app/qml/LiveViewPage.qml | Qt6 Quick, UI layer
// Testing: BRIDGE: test_bridge; QML: QmlTest.liveview, QmlTest.linkhealth,
//          QmlTest.chartlayout, QmlTest.phone (test_qml); Manual smoke

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Lexical id scoping: lets the delegate reference signalList (outer id)
// statically and is required for the required-property delegate pattern.
pragma ComponentBehavior: Bound

Page {
    id: root
    background: Rectangle { color: Theme.bg }

    // Settings owns connecting; Main opens it
    signal connectRequested()

    // The strip chart's time window, in seconds: one of chartWindows,
    // kept between runs. The bridge keeps 120 s of history per signal
    readonly property var chartWindows: [10, 30, 60, 120]
    readonly property int chartWindowS: chartWindows.includes(liveSettings.chartWindowS)
                                        ? liveSettings.chartWindowS : 30
    onChartWindowSChanged: {
        if (paused)
            viewEndMs = clampedEnd(viewEndMs)
        chart.requestPaint()
    }
    // One window along: direction > 0 a longer one, < 0 a shorter one,
    // stopping at either end. A pinch and the wheel step through them,
    // since Live's chart always ends at now
    function stepWindow(direction) {
        const i = chartWindows.indexOf(chartWindowS)
        const next = Math.max(0, Math.min(chartWindows.length - 1, i + (direction > 0 ? 1 : -1)))
        liveSettings.chartWindowS = chartWindows[next]
    }
    Settings {
        id: liveSettings
        category: "live"
        property int chartWindowS: 30
        // The splitter's place: the list's share of the page's height,
        // or -1 for the list sized to its rows (the default)
        property real listShare: -1
        // The first pause's hint has shown
        property bool pauseHintSeen: false
    }

    // The chart maximized: it takes the page but for the link banner and
    // the window row. Kept apart for a short layout (a phone on its side,
    // where it starts maximized) and a tall one, so turning the phone
    // never undoes a choice made the other way up. Not kept between runs
    // The splitter's share, as the page's own
    property alias listShare: liveSettings.listShare
    property bool maximizedTall: false
    property bool maximizedShort: true
    readonly property bool chartMaximized: Theme.shortLayout ? maximizedShort : maximizedTall
    function toggleMaximized() {
        if (Theme.shortLayout)
            maximizedShort = !maximizedShort
        else
            maximizedTall = !maximizedTall
    }
    // F on a keyboard; a text field takes its own letters first
    Shortcut {
        sequence: "F"
        enabled: root.visible && !Theme.mobile
        onActivated: root.toggleMaximized()
    }

    // The pause (L2): the plotted signals' history as it stood, kept in the
    // page, so the chart holds still while the rows and the session go on.
    // Its times are ms before the pause's newest sample, which is 0
    property var snapshot: null
    readonly property bool paused: snapshot !== null
    property double snapshotEndMs: 0     // the newest sample's session time
    property double snapshotStartMs: 0   // the oldest sample's time (<= 0)
    property double viewEndMs: 0         // the paused view's right edge (<= 0)
    property double cursorMs: NaN        // the tapped time, NaN for none
    property double pausedAt: 0          // Date.now() at the pause, for its age
    property double clockNow: 0
    // This pause shows the hint: the first pause of all, once
    property bool hintShown: false

    function pause() {
        if (paused || !Bridge.connected)
            return
        const series = Bridge.signalModel.chartData(0, 0)
        let start = Infinity, end = -Infinity
        for (const s of series) {
            if (s.samples.length < 2)
                continue
            start = Math.min(start, s.samples[0])
            end = Math.max(end, s.samples[s.samples.length - 2])
        }
        if (!isFinite(end))
            return
        const kept = []
        for (const s of series) {
            const samples = []
            for (let i = 0; i + 1 < s.samples.length; i += 2)
                samples.push(s.samples[i] - end, s.samples[i + 1])
            kept.push({ row: s.row, unit: s.unit, decimals: s.decimals, samples: samples })
        }
        snapshotEndMs = end
        snapshotStartMs = start - end
        viewEndMs = 0
        cursorMs = NaN
        pausedAt = Date.now()
        clockNow = pausedAt
        hintShown = !liveSettings.pauseHintSeen
        if (hintShown) {
            // Stored at once, not on the store's timer: a page made next reads it
            liveSettings.pauseHintSeen = true
            liveSettings.setValue("pauseHintSeen", true)
        }
        snapshot = kept
        chart.requestPaint()
    }
    // Follows now again, the snapshot dropped
    function resume() {
        if (!paused)
            return
        snapshot = null
        cursorMs = NaN
        hintShown = false
        chart.requestPaint()
    }
    // The view's end, kept so the window stays inside the snapshot (or
    // starts at its start, when it's shorter) and never past the pause
    function clampedEnd(end) {
        const least = Math.min(0, snapshotStartMs + chartWindowS * 1000)
        return Math.max(least, Math.min(0, end))
    }
    function setViewEnd(end) {
        viewEndMs = clampedEnd(end)
        chart.requestPaint()
    }
    function plotWidth() {
        return chart.drawn ? chart.drawn.plotW : Math.max(1, chart.shownW)
    }
    // The paused view's time at x in chartFrame's coordinates
    function timeAt(x) {
        const w = chartWindowS * 1000
        const t = viewEndMs - w + (x - chart.x) / Math.max(1, plotWidth() - 1) * w
        return Math.max(viewEndMs - w, Math.min(viewEndMs, t))
    }
    function setCursor(t) {
        cursorMs = t
        chart.requestPaint()
    }
    // The sample nearest the cursor, as an index into samples; -1 without a
    // cursor or a sample within staleMs of it
    function cursorSample(samples) {
        if (isNaN(cursorMs))
            return -1
        let best = -1
        for (let i = 0; i + 1 < samples.length; i += 2) {
            if (best < 0 || Math.abs(samples[i] - cursorMs) < Math.abs(samples[best] - cursorMs))
                best = i
        }
        return best >= 0 && Math.abs(samples[best] - cursorMs) <= staleMs ? best : -1
    }
    // The snapshot's samples in view, thinned to each column's lowest,
    // highest and newest, as chartData() thins the live window
    function pausedSeries(widthPx) {
        const w = chartWindowS * 1000
        const t1 = viewEndMs, t0 = t1 - w
        const columns = Math.max(1, widthPx)
        const out = []
        for (const s of snapshot) {
            const sp = s.samples
            const picked = []
            let col = -1, lo = -1, hi = -1, last = -1
            const flush = () => {
                const keep = [lo, hi, last].filter((v, k, all) => all.indexOf(v) === k)
                for (const i of keep.sort((a, b) => a - b))
                    picked.push(sp[i], sp[i + 1])
            }
            for (let i = 0; i + 1 < sp.length; i += 2) {
                if (sp[i] < t0 || sp[i] > t1)
                    continue
                const c = Math.min(columns - 1, Math.floor((sp[i] - t0) / w * columns))
                if (c !== col) {
                    if (col >= 0)
                        flush()
                    col = c
                    lo = hi = last = i
                    continue
                }
                if (sp[i + 1] < sp[lo + 1]) lo = i
                if (sp[i + 1] > sp[hi + 1]) hi = i
                last = i
            }
            if (col >= 0)
                flush()
            const series = { row: s.row, unit: s.unit, decimals: s.decimals, samples: picked }
            const at = cursorSample(sp)
            if (at >= 0) {
                series.labelValue = sp[at + 1]
                series.labelMs = sp[at]
            }
            out.push(series)
        }
        return out
    }
    // The cursor over the paused chart: a line, and a dot on each trace at
    // the sample its label reads
    function cursorMarks(ctx, l) {
        const w = chartWindowS * 1000
        const xOf = t => (t - (viewEndMs - w)) / w * (l.plotW - 1)
        const x = xOf(cursorMs)
        if (x < 0 || x > l.plotW)
            return
        ctx.strokeStyle = Theme.text
        ctx.lineWidth = 1
        ctx.beginPath()
        ctx.moveTo(x, l.top)
        ctx.lineTo(x, l.top + l.plotH)
        ctx.stroke()
        for (const tr of l.traces) {
            if (tr.labelMs === undefined)
                continue
            const at = xOf(tr.labelMs)
            let near = null
            for (const seg of tr.segments) {
                for (const p of seg) {
                    if (!near || Math.abs(p.x - at) < Math.abs(near.x - at))
                        near = p
                }
            }
            if (!near)
                continue
            ctx.fillStyle = Theme.seriesColor(tr.row)
            ctx.beginPath()
            ctx.arc(near.x, near.y, 3.5, 0, 2 * Math.PI)
            ctx.fill()
        }
    }
    function pausedText() {
        const line = qsTr("Paused · %1 s ago").arg(Math.max(0, Math.round((clockNow - pausedAt) / 1000)))
        if (isNaN(cursorMs))
            return line
        return line + " · " + qsTr("cursor at −%1 s")
                                  .arg(Number(-cursorMs / 1000).toLocaleString(Qt.locale(), 'f', 1))
    }
    Timer {
        interval: 1000
        repeat: true
        running: root.paused && root.visible
        onTriggered: root.clockNow = Date.now()
    }
    // Space on a keyboard pauses and resumes; a text field takes it first
    Shortcut {
        sequence: "Space"
        enabled: root.visible && !Theme.mobile
        onActivated: root.paused ? root.resume() : root.pause()
    }
    // The snapshot goes with what it was taken of: the session, its signal
    // set, the units it was converted to, and the page
    onVisibleChanged: if (!visible) resume()
    Connections {
        target: Bridge
        function onConnectedChanged() { root.resume() }
        function onUnitSystemChanged() { root.resume() }
    }
    Connections {
        target: Bridge.signalModel
        function onModelReset() { root.resume() }
    }

    readonly property int rowHeight: Theme.rowHeight
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
    // unsubscribed (Theme.seriesColor and Theme.seriesDash)
    function seriesColor(row) { return Theme.seriesColor(row) }
    function seriesDash(row) { return Theme.seriesDash(row) }

    // Value plus unit at the signal's decimal places (core's
    // decimal_places), formatted the way the Dashboard's gauges are
    function valueText(v, decimals, unit) {
        const n = Number(v).toLocaleString(Qt.locale(), 'f', decimals)
        return unit !== "" ? n + " " + unit : n
    }

    function statusText() {
        if (root.paused) return root.pausedText()
        if (Bridge.connecting) return qsTr("Connecting…")
        if (Bridge.disconnecting) return qsTr("Disconnecting…")
        if (!Bridge.connected) return qsTr("Connect a session to read live values")
        if (Bridge.liveSignalModel.shownCount === 0)
            return qsTr("This profile decodes no signals; its frames are on the Log page")
        // Refusals past an ECU's report list leave their rows blank: say
        // how many, since the rows can't
        const unnamed = Bridge.refusedUnnamed !== "" ? " · " + Bridge.refusedUnnamed : ""
        // The plotted signals are what a log records: said while it runs,
        // and where Start log is offered
        if (Bridge.logActive)
            return qsTr("Logging the %1 plotted signals").arg(Bridge.signalModel.subscribedCount) + unnamed
        const logNote = Bridge.recordingActive || Bridge.currentUri.toLowerCase().startsWith("replay://")
            ? "" : (Theme.compact ? qsTr(" · Start log records them")
                                  : qsTr(" · Start log records the plotted ones"))
        if (Bridge.liveSignalModel.filter.active)
            return (Theme.compact ? qsTr("%1 of %2 shown · %3 plotted")
                                  : qsTr("Showing %1 of %2 signals · plotting %3"))
                       .arg(signalList.count).arg(Bridge.liveSignalModel.shownCount)
                       .arg(Bridge.signalModel.subscribedCount) + unnamed
                       + (Theme.compact ? "" : logNote)   // the phone's line is one line: the count first
        // The 10 Hz clause is a desktop fact; a phone's line keeps the count
        if (Theme.compact)
            return qsTr("%1 of %2 plotted")
                       .arg(Bridge.signalModel.subscribedCount).arg(Bridge.liveSignalModel.shownCount) + unnamed + logNote
        return qsTr("Plotting %1 of %2 signals · values update at 10 Hz")
                   .arg(Bridge.signalModel.subscribedCount).arg(Bridge.liveSignalModel.shownCount) + unnamed + logNote
    }

    // The link-health banner's message. Unknown states read as degraded,
    // as Theme.linkColor and Theme.linkLabel treat them.
    function bannerText(state, episodes, errorCode) {
        switch (state) {
        // Ignition off reads as quiet on a bus that still acknowledges
        // (the first vehicle's did), so the hint goes here too
        case "quiet":    return qsTr("Vehicle not responding: quiet bus; check the ignition")
        case "requests": return qsTr("Requests failing: check the vehicle connection and ignition")
        case "fault":    return qsTr("Adapter fault: %1 episode(s), retrying with backoff").arg(episodes)
        // A replay's file changed under it (ADR-0023): nothing more will come
        case "stopped":  return qsTr("Replay stopped: the recording changed after it was opened")
        default:         return qsTr("Link degraded (error %1)").arg(errorCode)
        }
    }

    function chartEmptyText() {
        if (!Bridge.connected) return qsTr("No session")
        if (Bridge.liveSignalModel.shownCount === 0) return qsTr("No decoded signals")
        if (Bridge.signalModel.subscribedCount === 0)
            return qsTr("No signals plotted; tick a row to plot it")
        return qsTr("Waiting for data…")
    }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: 12

        PageHeader {
            visible: !root.chartMaximized
            title: qsTr("Live signals")
            // On a phone the title gives way first, so the field keeps its words
            titleShrinks: Theme.compact

            // Finding a signal among many (ADR-0050): the list's own
            // filter, in the title's row so a short page loses no height
            SignalFilterBar {
                objectName: "liveFilter"
                Layout.fillWidth: true
                visible: Bridge.connected && Bridge.liveSignalModel.shownCount > 0
                filter: Bridge.liveSignalModel.filter
                codes: Bridge.liveSignalModel.groups
            }
        }

        // Link-health banner: shown for every state but ok (quiet,
        // requests, fault, degraded) from Bridge.linkState, colored by
        // Theme.linkColor. Transitions are mirrored in the Settings
        // activity log.
        Rectangle {
            objectName: "linkBanner"
            visible: Bridge.connected && Bridge.linkState !== "ok"
            Layout.fillWidth: true
            implicitHeight: Theme.controlHeight + 4
            radius: Theme.radius
            color: Qt.alpha(Theme.linkColor(Bridge.linkState), 0.15)

            Label {
                objectName: "linkBannerText"
                anchors.centerIn: parent
                text: root.bannerText(Bridge.linkState, Bridge.errorEpisodes, Bridge.linkErrorCode)
                color: Theme.linkColor(Bridge.linkState)
                font.pixelSize: Theme.fontCaption
            }
        }

        // Status line; disconnected, the empty state with a way to connect.
        // Paused, it shows over a maximized chart too
        RowLayout {
            visible: !root.chartMaximized || root.paused
            Layout.fillWidth: true
            spacing: Theme.spacing

            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: root.statusText()
                color: Theme.textDim
            }
            // Secondary: the header's Connect... is the window's primary
            ThemedButton {
                objectName: "liveConnect"
                visible: !Bridge.connected && !Bridge.connecting && !Bridge.disconnecting
                text: qsTr("Connect…")
                onClicked: root.connectRequested()
            }
        }
        // The first pause's hint, once: a paused chart isn't a recording
        Label {
            objectName: "pauseHint"
            visible: root.paused && root.hintShown
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: qsTr("Start log records the session for the viewer")
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
            visible: !root.chartMaximized
            readonly property bool overflows: contentHeight > height
            readonly property real oneRow: Math.min(count, 1) * root.rowHeight
            // Below the list: the hidden note when shown, the window row,
            // then the chart at its minimum
            readonly property real roomAbove: column.height - y - column.spacing
                                              - (hiddenNote.visible ? hiddenNote.height + column.spacing : 0)
                                              - windowRow.height - column.spacing
                                              - root.chartMinHeight
            // The splitter's height, once dragged: within one row and
            // what the chart leaves, and never past the rows themselves
            readonly property real dragged: liveSettings.listShare < 0 ? -1
                : Math.max(oneRow, Math.min(liveSettings.listShare * column.height, roomAbove,
                                            count * root.rowHeight))

            Layout.fillWidth: true
            Layout.preferredHeight: dragged >= 0 ? dragged : count * root.rowHeight
            Layout.maximumHeight: dragged >= 0 ? dragged
                : Math.max(oneRow, Math.min(Math.max(3 * root.rowHeight, column.height * 0.55),
                                            roomAbove))
            Layout.minimumHeight: oneRow
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            // No current row: its default highlight animates to a row's old
            // width, past a phone's edge
            currentIndex: -1
            highlightFollowsCurrentItem: false
            // The rows whose PID no ECU marked unsupported (ADR-0039 section 6)
            model: Bridge.liveSignalModel

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
                // The signal model's row: its plot box and its trace
                // color, which chartData's `row` names
                required property int row
                required property string name
                required property string unit
                required property double value
                required property int decimals
                required property string text          // a text row's value
                required property string label         // an enum value's word
                required property string refused       // its ECU's refusal in words, or ""
                required property string refusedCode   // that reply's bytes ("7F 22 31"), or ""
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
                    id: rowLayout
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12

                    // Plots the row on the chart; the value keeps
                    // updating either way, here and on the Dashboard
                    ThemedCheckBox {
                        id: plotBox
                        objectName: "plotCheck"
                        checked: signalDelegate.subscribed
                        enabled: !signalDelegate.isText   // nothing to plot
                        onToggled: Bridge.signalModel.toggleSubscribed(signalDelegate.row)
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
                                                       ? root.seriesColor(signalDelegate.row)
                                                       : "transparent"
                        implicitWidth: 10; implicitHeight: 10   // layout-managed
                        radius: 2
                        color: signalDelegate.subscribed ? swatch.trace : "transparent"
                        border.width: 1
                        border.color: swatch.trace
                        opacity: signalDelegate.subscribed ? 1.0 : 0.5
                    }
                    Label {
                        id: nameLabel
                        objectName: "nameLabel"
                        text: signalDelegate.name
                        color: Theme.text
                        Layout.fillWidth: true
                        // A long value (a refusal's words, a text) leaves
                        // the name a third of the row
                        Layout.minimumWidth: Math.min(implicitWidth,
                                                      Math.round(signalDelegate.width / 3))
                        elide: Text.ElideRight
                    }
                    // The refusal's words at the value's font: on a row
                    // too narrow for them beside the name (a phone), the
                    // value is "Refused (7F 22 31)" instead
                    TextMetrics {
                        id: refusedWords
                        font: valueLabel.font
                        text: signalDelegate.refused
                    }
                    Label {
                        id: valueLabel
                        objectName: "valueLabel"
                        // Room past the whole name (its x is the checkbox's and
                        // the swatch's), within the value's cap below
                        readonly property bool refusedFits:
                            refusedWords.advanceWidth
                            <= Math.min(Math.round(signalDelegate.width * 0.6),
                                        signalDelegate.width - 24 - nameLabel.x
                                        - nameLabel.implicitWidth - 12)
                        // A signal its ECU refused says so, with the code
                        // by name (ADR-0051 section 3); one not yet
                        // answered renders "—", not a fabricated 0.
                        text: !Bridge.connected ? qsTr("—")
                              : signalDelegate.refused !== ""
                                ? (refusedFits || signalDelegate.refusedCode === ""
                                   ? signalDelegate.refused
                                   : qsTr("Refused (%1)").arg(signalDelegate.refusedCode))
                              : !signalDelegate.hasValue ? qsTr("—")
                              : signalDelegate.isText ? signalDelegate.text
                              : signalDelegate.label !== "" ? signalDelegate.label
                              : root.valueText(signalDelegate.value, signalDelegate.decimals,
                                               signalDelegate.unit)
                        // A text value can run to kilobytes: the name
                        // keeps its room
                        // and, on a row too narrow for its cap beside the
                        // box, the swatch and the name's least, what is left
                        Layout.maximumWidth: Math.max(0, Math.min(
                            Math.round(signalDelegate.width * 0.6),
                            signalDelegate.width - 24 - plotBox.width - swatch.width
                            - nameLabel.Layout.minimumWidth - 3 * rowLayout.spacing))
                        elide: Text.ElideRight
                        color: signalDelegate.refused !== "" ? Theme.textDim : Theme.text
                        font.pixelSize: Theme.fontEmphasis
                        // The value face, its digits tabular, so a reading
                        // doesn't jitter as its digits change
                        font.family: Theme.valueFamily
                        font.features: Theme.valueFeatures
                        opacity: signalDelegate.hasValue
                                 && signalDelegate.ageMs > root.staleMs
                                     ? 0.45 : 1.0
                    }
                }
            }
        }

        // What the list leaves out, and why: rows an ECU's supported-PID
        // bitmaps mark unsupported, and fields its replies come without
        // (core's absent signals). The Dashboard keeps them.
        Label {
            id: hiddenNote
            objectName: "hiddenNote"
            visible: Bridge.signalModel.hiddenCount > 0 && !root.chartMaximized
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: Bridge.signalModel.hiddenCount === 1
                  ? qsTr("1 signal hidden: the ECU doesn't send it")
                  : qsTr("%1 signals hidden: the ECU doesn't send them")
                        .arg(Bridge.signalModel.hiddenCount)
        }

        // The chart's time window: the last N seconds of every trace. The
        // buttons share the row, so on a phone's width they narrow
        // instead of laying the whole page out past its edge
        RowLayout {
            id: windowRow
            objectName: "chartWindowRow"
            // Every button a touch's height on a phone (ThemedButton's touchMin)
            readonly property real touchMin: Theme.touchTarget
            // Paused on a phone, Live takes the row's end: the label gives way
            // and the window buttons show their numbers alone
            readonly property bool tight: root.paused && Theme.compact
            Layout.fillWidth: true
            spacing: tight ? 4 : 6
            // The caption gives way under 360 px: with it the row's least
            // width is past the narrowest phone's
            Label {
                visible: !windowRow.tight && root.width >= 360
                text: qsTr("Window")
                color: Theme.textDim
            }
            Repeater {
                model: root.chartWindows
                ThemedButton {
                    required property int modelData
                    Layout.fillWidth: true
                    Layout.minimumWidth: 44
                    Layout.maximumWidth: 120
                    // A phone's quarter of the row holds "120 s" on one
                    // line only with the sides kept narrow
                    horizontalPadding: Theme.dense ? 4 : 8
                    objectName: "chartWindow_" + modelData
                    text: windowRow.tight ? String(modelData) : qsTr("%1 s").arg(modelData)
                    checkable: true
                    checked: root.chartWindowS === modelData
                    Accessible.name: qsTr("Show the last %1 seconds").arg(modelData)
                    onClicked: liveSettings.chartWindowS = modelData
                }
            }
            Item { Layout.fillWidth: true; visible: !windowRow.tight }
            ThemedButton {
                objectName: "maximizeChart"
                text: root.chartMaximized ? qsTr("List") : qsTr("Chart")
                compact: windowRow.tight
                Layout.minimumWidth: windowRow.tight ? 48 : 52
                Layout.maximumWidth: 72
                Accessible.name: root.chartMaximized ? qsTr("Show the list again")
                                                     : qsTr("Give the chart the page")
                onClicked: root.toggleMaximized()
            }
            // Paused only: the chart follows now again
            ThemedButton {
                objectName: "liveButton"
                visible: root.paused
                highlighted: true
                compact: windowRow.tight
                text: qsTr("Live")
                Layout.minimumWidth: Math.max(48, Theme.touchTarget)
                Layout.maximumWidth: 96
                Accessible.name: qsTr("Follow the live values again")
                onClicked: root.resume()
            }
        }

        // ---- Strip chart: per-signal rolling history, one repaint per
        // coalesced batch. Each trace takes its list row's color
        // (root.seriesColor), so the list is the legend. ChartLayout
        // works out where everything goes; StripChart strokes it.
        Rectangle {
            objectName: "chartFrame"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: root.chartMinHeight
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
                emptyText: root.chartEmptyText()
                moving: pan.active
                fetch: function(widthPx) {
                    return root.paused ? root.pausedSeries(widthPx)
                                       : Bridge.signalModel.chartData(root.chartWindowS * 1000, widthPx)
                }
                // Paused, the axis is the view's span, its ticks seconds before the pause
                layoutOpts: root.paused
                    ? ({ gapMs: root.staleMs, minSpanUnits: root.minSpanUnits,
                         format: root.valueText,
                         span: { t0: root.viewEndMs - root.chartWindowS * 1000, t1: root.viewEndMs },
                         absolute: true,
                         caption: s => s === 0 ? qsTr("paused") : qsTr("−%1 s").arg(-s) })
                    : ({ gapMs: root.staleMs, minSpanUnits: root.minSpanUnits,
                         format: root.valueText,
                         windowMs: root.chartWindowS * 1000,
                         caption: s => s === 0 ? qsTr("now") : qsTr("−%1 s").arg(s) })
                decorate: root.paused && !isNaN(root.cursorMs) ? root.cursorMarks : null

                Connections {
                    target: Bridge.signalModel
                    function onValuesUpdated() {
                        if (!root.paused)
                            chart.requestPaint()
                    }
                    function onSubscribedCountChanged() { chart.requestPaint() }
                }
                Connections {
                    target: Bridge
                    function onConnectedChanged() { chart.requestPaint() }
                }
            }
            // A double-click or double-tap maximizes it, or gives the list
            // back; the two taps of a thumb may land a little apart. Paused,
            // a tap places the cursor
            DoubleTapHandler {
                objectName: "chartDoubleTap"
                onTapped: eventPoint => {
                    if (root.paused)
                        root.setCursor(root.timeAt(eventPoint.position.x))
                }
                onDoubleTap: root.toggleMaximized()
            }
            // A drag pauses the chart and pans it back through the snapshot;
            // pushed past the snapshot's right edge, it follows now again.
            // Sideways only; a pinch's last finger moves nothing
            DragHandler {
                id: pan
                objectName: "chartPan"
                target: null
                yAxis.enabled: false
                property real startEnd: 0
                property bool fromPinch: false
                onActiveChanged: {
                    if (!active)
                        return
                    fromPinch = windowPinch.active || Date.now() - windowPinch.endedAt < 300
                    if (fromPinch)
                        return
                    root.pause()
                    startEnd = root.viewEndMs
                }
                onActiveTranslationChanged: {
                    if (!active || fromPinch || !root.paused)
                        return
                    const end = startEnd - activeTranslation.x / Math.max(1, root.plotWidth() - 1)
                                * root.chartWindowS * 1000
                    if (end > 0)
                        root.resume()
                    else
                        root.setViewEnd(end)
                }
            }
            // Fingers spreading show a shorter window, closing a longer
            // one: a step each time the scale moves by kPinchStep since the
            // last step, so one pinch can cross several windows
            PinchHandler {
                id: windowPinch
                objectName: "chartPinch"
                target: null
                readonly property real kPinchStep: 1.35
                property real fromScale: 1
                property double endedAt: 0
                onActiveChanged: {
                    fromScale = 1
                    if (!active)
                        endedAt = Date.now()
                }
                onActiveScaleChanged: {
                    if (!active || activeScale <= 0)
                        return
                    if (activeScale / fromScale >= kPinchStep) {
                        root.stepWindow(-1)
                        fromScale = activeScale
                    } else if (fromScale / activeScale >= kPinchStep) {
                        root.stepWindow(1)
                        fromScale = activeScale
                    }
                }
            }
            // The wheel the same way, a step per notch: up a shorter
            // window, down a longer one. A high-resolution wheel or a
            // touchpad sends a notch's 120 in parts, so they add up here,
            // the remainder kept for the next event; turning the other way
            // starts afresh, and a sideways scroll steps nothing
            WheelHandler {
                target: null
                property int pending: 0
                onWheel: event => {
                    const dy = event.angleDelta.y
                    if (dy === 0)
                        return
                    if ((pending > 0) !== (dy > 0))
                        pending = 0
                    pending += dy
                    while (Math.abs(pending) >= 120) {
                        const up = pending > 0
                        root.stepWindow(up ? -1 : 1)
                        pending -= up ? 120 : -120
                    }
                }
            }
        }
    }

    // The splitter: drag it to give the list or the chart more of the
    // page, kept between runs; a double-tap sizes the list to its rows
    // again. It sits in the column's gap below the list, outside the
    // layout, so a short page loses no height to it
    Item {
        id: splitter
        objectName: "listSplitter"
        visible: signalList.visible && signalList.count > 0
        x: column.x + signalList.x
        y: column.y + signalList.y + signalList.height + (column.spacing - height) / 2
        width: signalList.width
        height: column.spacing
        Rectangle {
            anchors.centerIn: parent
            width: 40
            height: 4
            radius: 2
            color: splitDrag.active || splitHover.hovered ? Theme.textDim : Theme.outline
        }
        HoverHandler { id: splitHover; cursorShape: Qt.SizeVerCursor }
        DragHandler {
            id: splitDrag
            target: null
            xAxis.enabled: false
            property real startHeight: 0
            onActiveChanged: if (active) startHeight = signalList.height
            onActiveTranslationChanged: {
                if (active && column.height > 0)
                    liveSettings.listShare = Math.max(0, (startHeight + activeTranslation.y)
                                                         / column.height)
            }
        }
        DoubleTapHandler { onDoubleTap: liveSettings.listShare = -1 }
    }
}
