// File/layer: app/qml/DashboardPage.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.phone, QmlTest.linkhealth
//          (test_qml); Manual smoke

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The Dashboard (ADR-0057): one form's pages of gauges, drawn from the
// form's document. DashboardBook holds the document's rules; this page
// reads it, places each page's cells on a grid and binds each cell's
// gauge to the first of its refs the session has.
Page {
    id: root
    background: Rectangle { color: Theme.bg }

    readonly property bool compact: width < 600
    readonly property real margin: Theme.pageMargin
    readonly property real gap: compact ? 10 : 16

    // The platform's form, never the window's width (section 4)
    property string form: Theme.mobile ? "phone" : "desktop"

    SignalHub { id: hub }

    // One document per form, and the page last shown on each
    Settings {
        id: store
        objectName: "dashStore"
        category: "dashboards"
        property string phone: ""
        property string desktop: ""
        property int phonePage: 0
        property int desktopPage: 0
    }

    // The stored text read: the document, or the built-in layout with
    // the reason when there is none or it can't be used
    readonly property var parsed: DashboardBook.parse(storedText, form)
    property var doc: parsed.doc
    readonly property string problem: parsed.problem
    readonly property var pages: doc && Array.isArray(doc.pages) ? doc.pages : []
    property bool problemDismissed: false
    // A different stored text is a different problem, so its line shows again
    readonly property string storedText: form === "phone" ? store.phone : store.desktop
    onStoredTextChanged: problemDismissed = false

    // ---- Pages

    property int pageIndex: 0
    property bool restored: false
    function goTo(i) {
        const n = Math.max(1, pages.length)
        pageIndex = Math.max(0, Math.min(n - 1, Math.round(i)))
    }
    onPagesChanged: goTo(pageIndex)
    onPageIndexChanged: {
        if (swipe.count > pageIndex && swipe.currentIndex !== pageIndex)
            swipe.setCurrentIndex(pageIndex)
        if (!restored) return
        if (form === "phone") store.phonePage = pageIndex
        else store.desktopPage = pageIndex
    }

    // Left and Right on the desktop form, never while a text field has
    // the focus (an editable one keeps them; a read-only one would pass
    // them on, though they move its cursor)
    readonly property bool typing: {
        const f = Window.activeFocusItem
        return f !== null && (f instanceof TextInput || f instanceof TextEdit)
    }
    readonly property bool pageKeys: visible && form === "desktop" && pages.length > 1 && !typing
    Shortcut {
        sequence: "Left"
        enabled: root.pageKeys
        onActivated: root.goTo(root.pageIndex - 1)
    }
    Shortcut {
        sequence: "Right"
        enabled: root.pageKeys
        onActivated: root.goTo(root.pageIndex + 1)
    }

    // ---- The sweep and the peaks, on every page

    signal sweepRequested()
    signal peaksResetRequested()

    // Ignition-style sweep, once per session, the first time the page
    // is on screen while connected
    property bool sweptThisSession: false
    // When the sweep was last requested (ms since the epoch) and how long
    // it plays. A gauge that finishes loading within that time of it
    // plays its own, so a page loaded between frames sweeps too.
    property double sweptAt: 0
    property int sweepMs: 1550
    function sweepIsOn() { return Date.now() - sweptAt < sweepMs }
    function maybeSweep() {
        if (!Bridge.connected) {
            sweptThisSession = false
            sweptAt = 0
            return
        }
        if (!visible || sweptThisSession) return
        sweptThisSession = true
        sweptAt = Date.now()
        sweepRequested()
    }
    function resetPeaks() { peaksResetRequested() }
    onVisibleChanged: maybeSweep()
    Component.onCompleted: {
        goTo(form === "phone" ? store.phonePage : store.desktopPage)
        restored = true
        maybeSweep()
    }
    Connections {
        target: Bridge
        function onConnectedChanged() {
            if (Bridge.connected) root.resetPeaks()
            root.maybeSweep()
        }
        // A peak held in the old units would read wrong in the new ones
        function onUnitSystemChanged() { root.resetPeaks() }
    }

    // ---- Units

    // Scale ends and zones are written in the unit the signal decodes
    // to and shown in the current unit system through core (ADR-0022).
    // Reading Bridge.unitSystem here makes each binding follow a change.
    function shown(unit, v) {
        return Bridge.unitSystem === 0 ? v : Bridge.displayValue(unit, v)
    }
    function shownUnit(unit) {
        return Bridge.unitSystem === 0 ? unit : Bridge.displayUnit(unit)
    }
    // A round tick spacing near `step`: 1, 2 or 5 times a power of ten
    function niceStep(step) {
        if (!(step > 0) || !isFinite(step)) return 1
        const p = Math.pow(10, Math.floor(Math.log(step) / Math.LN10))
        const m = step / p
        return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p
    }

    // ---- A cell: its gauge's inputs, from the cell and its entry

    component Cell: GaugeCard {
        id: card
        required property var cell
        // On a page two or more away from the one shown, the gauge loads
        // between frames, so a full document doesn't hold back the first
        // one; on the page shown and its neighbors (a swipe drags one in)
        // it finishes at once
        property bool later: false

        // The sweep plays on the dials, the cells and the shift lights only
        function sweep() {
            if (gauge.item && kind !== "bar" && kind !== "digital"
                    && kind !== "seven-segment" && kind !== "sparkline")
                (gauge.item as GaugeBase).sweep()
        }

        readonly property string kind: cell && typeof cell.kind === "string" ? cell.kind : ""
        readonly property var options: cell && cell.options ? cell.options : ({})
        readonly property string cellUnit: cell && typeof cell.unit === "string" ? cell.unit : ""
        readonly property string label: cell && typeof cell.label === "string" ? cell.label : ""
        readonly property SignalHub.Entry entry: hub.lookup(cell && cell.refs ? cell.refs : [])

        readonly property bool hasValue: Bridge.connected && entry !== null && entry.hasValue
        readonly property real value: entry ? entry.value : 0
        readonly property real windowHigh: entry ? entry.windowHigh : 0
        readonly property real windowLow: entry ? entry.windowLow : 0
        readonly property bool stale: hasValue && entry.ageMs > Theme.staleMs
        // An enumeration's label, which only `digital` is offered
        readonly property string valueLabel: entry && entry.isEnum ? entry.label : ""

        // The row's unit against the one the cell was made for (section 6)
        readonly property string expectedUnit: root.shownUnit(cellUnit)
        readonly property bool mismatch: entry !== null && entry.unit !== ""
                                         && expectedUnit !== "" && entry.unit !== expectedUnit
        readonly property string unit: entry && entry.unit !== "" ? entry.unit : expectedUnit
        readonly property int decimals: options.decimals !== undefined ? options.decimals
                                        : entry ? entry.decimals : 0

        // Section 3: the first state that applies
        readonly property string note: {
            let words = ""
            if (!Bridge.connected) words = qsTr("no session")
            else if (entry === null) words = qsTr("not in this profile: %1").arg(label)
            else if (entry.hidden) words = qsTr("not supported by this car")
            else if (entry.refusedCode !== "") words = qsTr("refused (%1)").arg(entry.refusedCode)
            else if (!entry.hasValue) words = qsTr("waiting for data")
            return words !== "" && mismatch ? qsTr("%1 (expected %2)").arg(words).arg(expectedUnit)
                                            : words
        }

        function opt(name, fallback) {
            return options[name] !== undefined ? options[name] : fallback
        }
        // A zone in the unit shown; none under another unit's row
        function zone(name) {
            return mismatch || options[name] === undefined ? NaN
                                                           : root.shown(cellUnit, options[name])
        }

        // The scale: the stored ends shown in the current units (a
        // dial's converted end snapped to its major step), or the row's
        // own range under another unit. A step that would draw more
        // ticks than a face holds gives a round one instead.
        readonly property var axis: {
            let a = 0, b = 100
            let snap = false
            const d = DashboardBook.defaultRange(cellUnit)
            const f = opt("from", d.from), t = opt("to", d.to)
            if (mismatch) {
                const lo = entry.rangeLow, hi = entry.rangeHigh
                if (isFinite(lo) && isFinite(hi) && lo < hi) {
                    a = lo
                    b = hi
                }
            } else {
                a = root.shown(cellUnit, f)
                b = root.shown(cellUnit, t)
                snap = kind === "analog" && Bridge.unitSystem !== 0
            }
            if (!(a < b)) b = a + 1
            let major = opt("majorStep", NaN)
            if (!(major > 0) || (b - a) / major > 40)
                major = root.niceStep((b - a) / 8)
            if (snap) {
                const sa = a !== f ? Math.round(a / major) * major : a
                const sb = b !== t ? Math.round(b / major) * major : b
                if (sa < sb) {
                    a = sa
                    b = sb
                }
            }
            let minor = opt("minorStep", major / 2)
            if (!(minor > 0) || (b - a) / minor > 200)
                minor = major / 2
            return { from: a, to: b, major: major, minor: minor }
        }
        readonly property real from: axis.from
        readonly property real to: axis.to
        readonly property real majorStep: axis.major
        readonly property real minorStep: axis.minor
        readonly property real coldBelow: zone("coldBelow")
        readonly property real warnFrom: zone("warnFrom")
        readonly property real dangerFrom: zone("dangerFrom")
        readonly property color accent: {
            const a = opt("accent", "accent")
            return a === "info" ? Theme.info : a === "success" ? Theme.success
                                             : Qt.lighter(Theme.primary, 1.3)
        }

        // Each kind's own
        readonly property real labelDivisor: opt("labelDivisor", 1)
        readonly property string caption: opt("caption", unit)
        readonly property string peakLabel: opt("peakLabel", qsTr("PEAK"))
        readonly property bool showPeak: opt("peak", false)
        // The new kinds' own options (ADR-0057 section 1): the cells' count
        // and curve, the digits, the sparkline's window, the bar standing up
        // and its ticks
        readonly property int cells: opt("cells", 24)
        readonly property real rise: opt("rise", 1.6)
        readonly property int digits: opt("digits", 4)
        readonly property int windowS: opt("window", 30)
        readonly property bool vertical: opt("vertical", false)
        readonly property bool marks: opt("marks", true)
        readonly property bool showProgress: opt("progress", false)
        readonly property string description: opt("description", "")
        readonly property var scaleMarks: {
            const marks = []
            for (let i = 0; i <= 4; ++i)
                marks.push(from + (to - from) * i / 4)
            return marks
        }
        readonly property int count: opt("count", root.compact ? 11 : 15)
        readonly property real ledSpacing: root.compact ? 6 : 10

        readonly property bool drawn: gauge.sourceComponent !== null
        padding: kind === "shift-light" ? 8 : kind === "bar" ? 14 : 12

        readonly property var shared: ["value", "windowHigh", "windowLow", "hasValue", "stale",
                                       "from", "to", "label", "unit", "note", "decimals",
                                       "coldBelow", "warnFrom", "dangerFrom", "accent", "showPeak"]
        readonly property var own: ({
            "analog": ["majorStep", "minorStep", "labelDivisor", "caption", "peakLabel",
                       "showProgress"],
            "bar": ["scaleMarks", "vertical", "marks"],
            "segments": ["cells", "rise"],
            "seven-segment": ["digits"],
            "sparkline": ["windowS"],
            "digital": ["description", "valueLabel"],
            "shift-light": ["count", "ledSpacing"]
        })

        Loader {
            id: gauge
            objectName: "gauge"
            anchors.fill: parent
            asynchronous: card.later
            sourceComponent: card.kind === "analog" ? analogKind
                           : card.kind === "ring" ? ringKind
                           : card.kind === "bar" ? barKind
                           : card.kind === "digital" ? digitalKind
                           : card.kind === "shift-light" ? shiftKind
                           : card.kind === "segments" ? segmentsKind
                           : card.kind === "seven-segment" ? readoutKind
                           : card.kind === "sparkline" ? sparkKind
                           : null
            onLoaded: {
                const g = item
                for (const name of card.shared.concat(card.own[card.kind] ?? []))
                    g[name] = Qt.binding(() => card[name])
                if (root.sweepIsOn()) card.sweep()
            }
            Connections {
                target: root
                function onSweepRequested() { card.sweep() }
                function onPeaksResetRequested() {
                    if (gauge.item) (gauge.item as GaugeBase).resetPeaks()
                }
            }
        }

        // A kind this version doesn't draw, kept so it can be removed
        Label {
            objectName: "unknownKind"
            visible: !card.drawn
            anchors.centerIn: parent
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("a gauge this version of Havne doesn't have")
            color: Theme.textDim
            font.pixelSize: 13
        }
    }

    Component { id: analogKind; AnalogGauge {} }
    Component { id: ringKind; RingGauge {} }
    Component { id: barKind; BarGauge {} }
    Component { id: digitalKind; DigitalGauge {} }
    Component { id: shiftKind; ShiftLightBar {} }
    Component { id: segmentsKind; SegmentGauge {} }
    Component { id: readoutKind; SegmentReadout {} }
    Component { id: sparkKind; SparkGauge {} }

    // ---- The header: title, link state, peak reset; the desktop's
    // page buttons; why the stored layout isn't shown

    ColumnLayout {
        id: topBar
        x: (root.width - width) / 2
        y: root.margin
        width: Math.max(0, root.width - 2 * root.margin)
        spacing: root.gap / 2

        PageHeader {
            id: header
            Layout.fillWidth: true
            title: qsTr("Dashboard")

            Rectangle {
                objectName: "linkDot"
                implicitWidth: 8; implicitHeight: 8   // layout-managed
                radius: 4
                color: Theme.linkColor(Bridge.linkState)
            }
            Label {
                objectName: "linkLabel"
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                // Short statuses come from Theme.linkLabel; "ok" keeps
                // the richer URI text. Long-form detail (episode
                // counts, error codes) stays in LiveView's banner.
                text: Bridge.linkState === "ok"
                      ? qsTr("Live · %1").arg(Bridge.uriLabel)
                      : Theme.linkLabel(Bridge.linkState)
                font.pixelSize: 13
                color: Theme.linkColor(Bridge.linkState)
            }
            ThemedButton {
                objectName: "resetPeaks"
                text: qsTr("Reset peaks")
                onClicked: root.resetPeaks()
            }
        }

        Flow {
            objectName: "dashPageButtons"
            Layout.fillWidth: true
            visible: root.form === "desktop" && root.pages.length > 1
            spacing: 6
            Repeater {
                model: root.pages.length
                ThemedButton {
                    required property int index
                    objectName: "dashPageButton_" + index
                    compact: true
                    highlighted: index === root.pageIndex
                    text: root.pages[index] && root.pages[index].name
                          ? root.pages[index].name : qsTr("Page %1").arg(index + 1)
                    onClicked: root.goTo(index)
                }
            }
        }

        RowLayout {
            objectName: "dashProblem"
            Layout.fillWidth: true
            visible: (root.problem === "damaged" || root.problem === "newer")
                     && !root.problemDismissed
            Label {
                objectName: "dashProblemText"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.warning
                font.pixelSize: 13
                text: root.problem === "newer"
                      ? qsTr("The saved dashboard was made by a newer version of Havne. "
                             + "This shows the built-in one; the saved one is kept.")
                      : qsTr("The saved dashboard couldn't be read. "
                             + "This shows the built-in one; the saved text is kept.")
            }
            ThemedButton {
                objectName: "dashProblemDismiss"
                compact: true
                text: qsTr("Dismiss")
                onClicked: root.problemDismissed = true
            }
        }
    }

    // ---- The pages: one SwipeView, swiped on the phone form, moved by
    // the buttons and keys on the desktop form

    SwipeView {
        id: swipe
        objectName: "dashPages"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: topBar.bottom
        anchors.bottom: indicator.visible ? indicator.top : parent.bottom
        clip: true
        interactive: root.form === "phone"
        // The page shown goes in once every page is in: the first page
        // added makes the index 0, which would override a stored one
        onCountChanged: {
            if (count === root.pages.length && currentIndex !== root.pageIndex)
                setCurrentIndex(root.pageIndex)
        }
        onCurrentIndexChanged: {
            if (count === root.pages.length && currentIndex >= 0)
                root.pageIndex = currentIndex
        }

        Repeater {
            model: root.pages.length

            Flickable {
                id: pageView
                required property int index
                readonly property var page: root.pages[index]
                readonly property var cells: page && Array.isArray(page.cells) ? page.cells : []
                objectName: "dashPage_" + index
                clip: true
                contentWidth: width
                contentHeight: grid.height + 2 * root.margin
                // Shown whenever there's more below, not only while
                // scrolling, so a short window says it scrolls
                ScrollBar.vertical: ScrollBar {
                    policy: pageView.contentHeight > pageView.height ? ScrollBar.AlwaysOn
                                                                     : ScrollBar.AsNeeded
                }

                // The grid (section 4): the phone's 2 columns upright and
                // 4 on its side, the desktop's one per 220 px from 2 to 12
                readonly property real gridWidth: Math.max(0, width - 2 * root.margin)
                readonly property int columns: root.form === "phone"
                    ? (root.width > root.height ? 4 : 2)
                    : Math.max(2, Math.min(12, Math.round(gridWidth / 220)))
                readonly property real columnWidth: Math.max(0, (gridWidth - (columns - 1)
                                                                 * root.gap) / columns)
                readonly property var boxes: DashboardBook.place(cells, columns)
                readonly property int tallest: cells.reduce((m, c) => Math.max(m, c.h), 1)
                // The row unit is a quarter of a column, so h = 4w is
                // square; never so tall that the tallest cell overflows
                // what the page shows, down to a floor of 160 px, below
                // which the page scrolls instead
                readonly property real pitch: Math.max(1, Math.min(
                    (columnWidth + root.gap) / 4,
                    (Math.max(160, height - 2 * root.margin) + root.gap) / tallest))
                readonly property int rows: boxes.reduce((m, b) => Math.max(m, b.y + b.h), 0)

                Item {
                    id: grid
                    x: (pageView.width - width) / 2
                    y: root.margin
                    width: pageView.gridWidth
                    height: Math.max(0, pageView.rows * pageView.pitch - root.gap)

                    Repeater {
                        model: pageView.cells.length

                        Cell {
                            required property int index
                            readonly property var box: pageView.boxes[index]
                            objectName: "dashCell_" + pageView.index + "_" + index
                            cell: pageView.cells[index]
                            later: Math.abs(pageView.index - root.pageIndex) > 1
                            x: box ? box.x * (pageView.columnWidth + root.gap) : 0
                            y: box ? box.y * pageView.pitch : 0
                            width: box ? box.w * pageView.columnWidth + (box.w - 1) * root.gap : 0
                            height: box ? Math.max(0, box.h * pageView.pitch - root.gap) : 0
                        }
                    }
                }
            }
        }
    }

    PageIndicator {
        id: indicator
        objectName: "dashIndicator"
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        visible: root.form === "phone" && root.pages.length > 1
        count: swipe.count
        currentIndex: swipe.currentIndex
    }
}
