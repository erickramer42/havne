// File/layer: app/qml/DashCell.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit, QmlTest.phone,
//          QmlTest.linkhealth, QmlTest.driveview (test_qml); BENCH, Manual smoke

import QtQuick
import QtQuick.Controls

pragma ComponentBehavior: Bound

// ---- A cell: its gauge's inputs, from the cell and its entry

GaugeCard {
    id: card
    required property DashboardPage host
    required property var cell
    // On a page two or more away from the one shown, the gauge loads
    // between frames, so a full document doesn't hold back the first
    // one; on the page shown and its neighbors (a swipe drags one in)
    // it finishes at once
    property bool later: false
    // Whether this card's page is the one on screen and the app is
    // active: the sparkline samples only then (a SwipeView keeps its
    // neighbors visible)
    property bool shown: true
    property int pageNo: -1
    property int cellIndex: -1
    readonly property bool editable: card.host.editing && pageNo === card.host.pageIndex
    readonly property bool lifted: card.host.held === cellIndex && pageNo === card.host.pageIndex
    // The panel names this cell, in edit mode or out of it
    readonly property bool chosen: pageNo === card.host.pageIndex && card.host.selected === cellIndex
                                   && (card.host.editing || card.host.panel !== "")

    // What the panel's Digits and Lights show with the option unset: what
    // the cell draws, in or out of edit mode
    Binding {
        when: card.chosen
        target: card.host
        property: "selectedDigits"
        value: card.digits
    }
    Binding {
        when: card.chosen
        target: card.host
        property: "selectedCount"
        value: card.count
    }

    // A press held still lifts the cell; these go to its page's grid
    signal holdStarted(point scene)
    signal holdMoved(point scene)
    signal holdEnded(bool canceled)

    // A blank cell draws its own outline in edit mode and nothing else.
    // On a joined panel no cell paints, except the one held up: a card
    // again, which covers what it passes over
    plain: (card.host.joined && !lifted) || kind === "blank"
    scale: lifted ? 1.03 : 1
    z: lifted ? 5 : 0

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
    readonly property SignalHub.Entry entry: card.host.signalHub.lookup(card.host.lookupRefs(cell && cell.refs ? cell.refs : []))

    // A signal the session isn't asking has no value to show: its
    // last one would read as no new data (the note says not asked)
    readonly property bool hasValue: Bridge.connected && entry !== null && entry.hasValue
                                     && !entry.unasked
    readonly property real value: entry ? entry.value : 0
    readonly property real windowHigh: entry ? entry.windowHigh : 0
    readonly property real windowLow: entry ? entry.windowLow : 0
    // A trace reads the model's history of the def, kept whatever page shows
    readonly property var traceSource: Bridge.signalModel
    readonly property int signalId: entry ? entry.id : -1
    // The highest alert ON that reads the signal; a preview cell shows none
    readonly property int alertLevel: signalId >= 0 && pageNo >= 0
                                      ? (card.host.alertLevels[String(signalId)] ?? 0) : 0
    readonly property bool stale: hasValue && entry.ageMs > Theme.staleMs
    // An enumeration's label, which only `digital` is offered
    readonly property string valueLabel: entry && entry.isEnum ? entry.label : ""

    // The row's unit against the one the cell was made for (section 6)
    readonly property string expectedUnit: card.host.shownUnit(cellUnit)
    readonly property bool mismatch: entry !== null && entry.unit !== ""
                                     && expectedUnit !== "" && entry.unit !== expectedUnit
    readonly property string unit: entry && entry.unit !== "" ? entry.unit : expectedUnit
    readonly property int decimals: options.decimals !== undefined ? options.decimals
                                    : entry ? entry.decimals : 0

    // Section 3: the first state that applies
    readonly property string note: {
        let words = ""
        if (!Bridge.connected) words = qsTr("no session")
        else if (entry === null) words = card.host.missingNote(card.host.lookupRefs(cell && cell.refs ? cell.refs : []))
        // Live's rule (hiddenCount): the ECU reported it unsupported, or
        // never answered it
        else if (entry.hidden) words = qsTr("not sent by this ECU")
        else if (entry.refusedCode !== "") words = qsTr("refused (%1)").arg(entry.refusedCode)
        else if (entry.unasked) words = qsTr("not asked")
        // A replay that has played its last frame will send nothing more
        else if (!entry.hasValue) words = Bridge.replayFinished ? qsTr("not in this recording")
                                                                : qsTr("waiting for data")
        return words !== "" && mismatch ? qsTr("%1 (expected %2)").arg(words).arg(expectedUnit)
                                        : words
    }

    function opt(name, fallback) {
        return options[name] !== undefined ? options[name] : fallback
    }
    // A zone in the unit shown; none under another unit's row
    function zone(name) {
        return mismatch || options[name] === undefined ? NaN
                                                       : card.host.shown(cellUnit, options[name])
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
            a = card.host.shown(cellUnit, f)
            b = card.host.shown(cellUnit, t)
            snap = kind === "analog" && Bridge.unitSystem !== 0
        }
        if (!(a < b)) b = a + 1
        let major = opt("majorStep", NaN)
        if (!(major > 0) || (b - a) / major > 40)
            major = card.host.niceStep((b - a) / 8)
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
    readonly property real warnBelow: zone("warnBelow")
    readonly property real dangerBelow: zone("dangerBelow")
    // A picked color (options.color) takes the normal zone's place; the
    // zones, and Night, still win
    readonly property string picked: Theme.gaugeColor(opt("color", ""))
    // A stored "info" draws as the value's own color: blue means cold alone
    readonly property color accent: {
        if (picked !== "")
            return picked
        const a = opt("accent", "accent")
        return a === "success" ? Theme.success : Theme.gaugeValue
    }
    readonly property color readoutColor: picked !== "" ? picked : Theme.gaugeReadout
    readonly property color needleColor: picked !== "" ? picked : Theme.gaugeNeedle

    // Each kind's own
    readonly property real labelDivisor: opt("labelDivisor", 1)
    readonly property string caption: opt("caption", label)
    readonly property string peakLabel: opt("peakLabel", qsTr("Peak"))
    readonly property bool showPeak: opt("peak", false)
    // The new kinds' own options (ADR-0057 section 1): the cells' count
    // and curve, the digits, the sparkline's window, the bar standing up
    // and its ticks
    readonly property int cells: opt("cells", 24)
    readonly property real rise: opt("rise", 1.6)
    readonly property int digits: opt("digits", DashboardBook.digitsFor(from, to, decimals))
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
    readonly property int count: opt("count", card.host.compact ? 11 : 15)
    readonly property real ledSpacing: card.host.compact ? 6 : 10

    readonly property bool drawn: gauge.sourceComponent !== null
    padding: kind === "shift-light" ? 8 : kind === "bar" ? 14 : 12

    readonly property var shared: ["value", "windowHigh", "windowLow", "hasValue", "stale",
                                   "from", "to", "label", "unit", "note", "decimals",
                                   "coldBelow", "warnFrom", "dangerFrom", "warnBelow", "dangerBelow", "accent", "readoutColor",
                                   "showPeak", "picked", "alertLevel"]
    readonly property var own: ({
        "analog": ["majorStep", "minorStep", "labelDivisor", "caption", "peakLabel",
                   "showProgress", "needleColor"],
        "bar": ["scaleMarks", "vertical", "marks"],
        "segments": ["cells", "rise"],
        "seven-segment": ["digits"],
        "sparkline": ["windowS", "shown", "traceSource", "signalId"],
        "digital": ["description", "valueLabel", "shown", "traceSource", "signalId"],
        "shift-light": ["count", "ledSpacing"]
    })

    // A held peak, low and trace belong to the signal that made them:
    // a cell that shows another signal starts them again
    onEntryChanged: if (gauge.item) (gauge.item as GaugeBase).resetPeaks()

    Loader {
        id: gauge
        objectName: "gauge"
        anchors.fill: parent
        // Editable, the gauge leaves Remove's corner free, so the
        // button covers no title
        anchors.rightMargin: card.editable ? Theme.touchTarget - card.padding + 2 : 0
        asynchronous: card.later
        sourceComponent: card.host.kindComponent(card.kind)
        onLoaded: {
            const g = item
            for (const name of card.shared.concat(card.own[card.kind] ?? []))
                g[name] = Qt.binding(() => card[name])
            if (card.host.sweepIsOn()) card.sweep()
        }
        Connections {
            target: card.host
            function onSweepRequested() { card.sweep() }
            function onPeaksResetRequested() {
                if (gauge.item) (gauge.item as GaugeBase).resetPeaks()
            }
        }
    }

    // A tap on a dial that shows its peak resets the peak, and in edit
    // mode a tap opens the cell's panel. A hold, in or out of edit mode,
    // lifts the cell. A blank cell has nothing to hold outside edit mode
    MouseArea {
        objectName: "cellTap"
        z: 1
        anchors.fill: parent
        anchors.margins: -card.padding
        enabled: card.pageNo === card.host.pageIndex && (card.kind !== "blank" || card.host.editing)
        // Moved past the drag distance before the hold completed: a
        // swipe or a scroll, even where nothing has taken the finger
        property bool strayed: false
        property point pressedAt: Qt.point(0, 0)
        onPressed: (mouse) => {
            strayed = false
            pressedAt = Qt.point(mouse.x, mouse.y)
        }
        onClicked: {
            if (strayed)
                return
            if (card.host.driving) {
                card.host.drivePart.driveTap()
                return
            }
            if (card.kind === "analog" && card.showPeak && gauge.item)
                (gauge.item as GaugeBase).resetPeaks()
            if (card.editable)
                card.host.select(card.cellIndex)
        }
        onPressAndHold: (mouse) => {
            if (card.host.driving)
                return
            if (strayed)
                mouse.accepted = false
            else
                card.holdStarted(mapToItem(null, mouse.x, mouse.y))
        }
        onPositionChanged: (mouse) => {
            if (card.lifted)
                card.holdMoved(mapToItem(null, mouse.x, mouse.y))
            else if (Math.hypot(mouse.x - pressedAt.x, mouse.y - pressedAt.y)
                     > Application.styleHints.startDragDistance)
                strayed = true
        }
        onReleased: if (card.lifted) card.holdEnded(false)
        onCanceled: if (card.lifted) card.holdEnded(true)
    }
    // The outline and Remove exist only while the cell is editable
    Loader {
        z: 2
        active: card.chosen || card.lifted
        anchors.fill: parent
        anchors.margins: -card.padding + 2
        sourceComponent: Rectangle {
            objectName: "cellSelected"
            color: "transparent"
            border.color: Theme.primary
            border.width: 2
            radius: Theme.radius
        }
    }
    Loader {
        z: 3
        active: card.editable
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: -card.padding + 2
        width: Theme.touchTarget
        height: Theme.touchTarget
        sourceComponent: ThemedButton {
            id: removeBox
            objectName: "cellRemove"
            compact: true
            text: "\u00D7"
            Accessible.name: card.kind === "blank" ? qsTr("Remove blank cell")
                                                 : qsTr("Remove %1").arg(card.host.cellName(card.cell))
            onClicked: card.host.editsPart.removeCell(card.cellIndex)
            // A touch's size drawn as a smaller raised square with an edge:
            // a button, not an empty tile, and inside a short cell
            background: Item {
                Rectangle {
                    objectName: "cellRemoveFace"
                    anchors.centerIn: parent
                    width: 32
                    height: 32
                    radius: Theme.radius
                    color: removeBox.down ? removeBox.pressedColor
                         : removeBox.hovered ? removeBox.hoverColor
                         : removeBox.baseColor
                    border.width: removeBox.visualFocus ? 2 : 1
                    border.color: removeBox.visualFocus ? Theme.text : Theme.outline
                }
            }
            contentItem: Text {
                text: removeBox.text
                font.pixelSize: Theme.fontBody + 6
                font.weight: Font.DemiBold
                color: Theme.text
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
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
        font.pixelSize: Theme.fontCaption
    }
}
