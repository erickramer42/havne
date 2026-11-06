// File/layer: app/qml/LiveViewPage.qml | Qt6 Quick, UI layer
// Testing: model contract covered by test_bridge; swatch colors, dashes
//          and banner text by test_qml (tests/qml/tst_liveview.qml, and
//          tst_linkhealth.qml for the banner in each link state), which
//          finds items by objectName, the rows an ECU lacks left out with
//          the note saying how many (against the fake J2534 DLL), the
//          filter above the list (SignalFilterBar.qml, ADR-0050), and the
//          list giving up rows so the
//          chart stays whole on a short page; the strip chart's layout (ranges,
//          lanes, gaps, ticks, labels, a crowded trace's band) by tst_chartlayout.qml through
//          ChartLayout, drawn by StripChart.qml; every item inside 320 px alone and inside the
//          app's window at the test phone's size (tests/qml/tst_phone.qml),
//          the chart maximized by shape and on request, the splitter's
//          share and the window's steps (stepWindow, which a pinch and
//          the wheel call; the wheel a step per notch, a notch's parts
//          added up, a sideways scroll none) there too, and a double tap
//          on the chart or the splitter (DoubleTapHandler.qml: taps or
//          clicks a little apart within the interval) as well; F toggling
//          the chart, and typed into the filter staying a letter of it, by
//          tst_liveview.qml (the drag itself is manual smoke: run-havne at
//          384 x 832 and 832 x 384);
//          the drawing itself, sharp under zoom, is verified manually
//          (docs/testing.md).

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
    onChartWindowSChanged: chart.requestPaint()
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
        if (Bridge.connecting) return qsTr("Connecting…")
        if (Bridge.disconnecting) return qsTr("Disconnecting…")
        if (!Bridge.connected) return qsTr("Connect a session to stream decoded values")
        if (Bridge.liveSignalModel.shownCount === 0)
            return qsTr("This profile decodes no signals; its frames are on the Log page")
        if (Bridge.liveSignalModel.filter.active)
            return qsTr("Showing %1 of %2 signals · plotting %3")
                       .arg(signalList.count).arg(Bridge.liveSignalModel.shownCount)
                       .arg(Bridge.signalModel.subscribedCount)
        return qsTr("Plotting %1 of %2 signals · values update at 10 Hz")
                   .arg(Bridge.signalModel.subscribedCount).arg(Bridge.liveSignalModel.shownCount)
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
                font.pixelSize: 13
            }
        }

        // Status line; disconnected, the empty state with a way to connect
        RowLayout {
            visible: !root.chartMaximized
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
                        text: signalDelegate.name
                        color: Theme.text
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Label {
                        objectName: "valueLabel"
                        // Never-arrived signals render "—", not a
                        // fabricated 0 (a PID not yet answered, or refused
                        // with a negative reply rather than by its bitmap).
                        text: !(Bridge.connected && signalDelegate.hasValue) ? qsTr("—")
                              : signalDelegate.isText ? signalDelegate.text
                              : signalDelegate.label !== "" ? signalDelegate.label
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
            Layout.fillWidth: true
            spacing: 6
            Label {
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
                    text: qsTr("%1 s").arg(modelData)
                    highlighted: root.chartWindowS === modelData
                    Accessible.name: qsTr("Show the last %1 seconds").arg(modelData)
                    onClicked: liveSettings.chartWindowS = modelData
                }
            }
            Item { Layout.fillWidth: true }
            ThemedButton {
                objectName: "maximizeChart"
                text: root.chartMaximized ? qsTr("List") : qsTr("Chart")
                Layout.minimumWidth: 52
                Layout.maximumWidth: 72
                Accessible.name: root.chartMaximized ? qsTr("Show the list again")
                                                     : qsTr("Give the chart the page")
                onClicked: root.toggleMaximized()
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
                fetch: function(widthPx) {
                    return Bridge.signalModel.chartData(root.chartWindowS * 1000, widthPx)
                }
                layoutOpts: ({
                    gapMs: root.staleMs, minSpanUnits: root.minSpanUnits,
                    format: root.valueText,
                    windowMs: root.chartWindowS * 1000,
                    caption: s => s === 0 ? qsTr("now") : qsTr("−%1 s").arg(s) })

                Connections {
                    target: Bridge.signalModel
                    function onValuesUpdated() { chart.requestPaint() }
                    function onSubscribedCountChanged() { chart.requestPaint() }
                }
                Connections {
                    target: Bridge
                    function onConnectedChanged() { chart.requestPaint() }
                }
            }
            // A double-click or double-tap maximizes it, or gives the list
            // back; the two taps of a thumb may land a little apart
            DoubleTapHandler { objectName: "chartDoubleTap"; onDoubleTap: root.toggleMaximized() }
            // Fingers spreading show a shorter window, closing a longer
            // one: a step each time the scale moves by kPinchStep since the
            // last step, so one pinch can cross several windows
            PinchHandler {
                id: windowPinch
                objectName: "chartPinch"
                target: null
                readonly property real kPinchStep: 1.35
                property real fromScale: 1
                onActiveChanged: fromScale = 1
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
