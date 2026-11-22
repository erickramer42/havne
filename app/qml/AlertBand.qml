// File/layer: app/qml/AlertBand.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.alertband, QmlTest.phone_* (test_qml)

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

// The highest alert ON, as a band in the page (ADR-0063 section 9): danger
// above warning, then the newest firing; the others counted as "+N more".
// An alert stays while ON and for at least floorMs from its firing, timed
// on Theme.now. Only an alert that asks for a banner shows, and only while
// a session judges it. The rows are read through the model's roles, never
// a bridge map: each read of one makes a JS wrapper (the Qt 6.12 sweep).
Rectangle {
    id: band

    // The conditions, one row each (Bridge.conditionModel's roles); a test
    // hands in a ListModel of the same roles
    property var conditions: Bridge.conditionModel
    readonly property int floorMs: 3000

    // The band's rows, the shown one first: {key, name, level, value, unit,
    // decimals, stale, staleSinceMs}
    property var rows: []
    readonly property var shownRow: rows.length > 0 ? rows[0] : null
    readonly property bool shown: shownRow !== null
    readonly property int more: Math.max(0, rows.length - 1)
    readonly property bool danger: shown && shownRow.level === "danger"

    // When each key last fired, in Theme.now's ms
    property var firedAt: ({})

    function noteFiring(key) {
        firedAt[key] = Theme.now().getTime()
        later()
    }
    function later() { Qt.callLater(band.recompute) }

    // The value the condition was settled at (its last event), not the live one
    function valueText(r) {
        if (isNaN(r.value))
            return ""
        const v = Number(r.value).toLocaleString(Qt.locale(), "f", r.decimals)
        return qsTr("at %1").arg(r.unit !== "" ? v + " " + r.unit : v)
    }
    function staleText(r) {
        return r.staleSinceMs >= 0
            ? qsTr("nothing new read since %1").arg(Qt.formatTime(new Date(r.staleSinceMs), "hh:mm:ss"))
            : qsTr("nothing new read")
    }
    function levelWord(level) { return level === "danger" ? qsTr("DANGER") : qsTr("WARNING") }

    function recompute() {
        const now = Theme.now().getTime()
        const list = []
        let next = Infinity
        for (let i = 0; i < rowsOf.count; ++i) {
            const r = rowsOf.objectAt(i) as BandRow
            if (!r || !r.enabled || !r.live || !r.banner || r.level === "" || r.unavailable)
                continue
            const at = band.firedAt[r.key] ?? -Infinity
            const on = r.state === "on"
            if (!on && now >= at + band.floorMs)
                continue
            if (!on)
                next = Math.min(next, at + band.floorMs)
            list.push({ row: i, at: at, key: r.key, name: r.name, level: r.level,
                        value: r.value, unit: r.unit, decimals: r.decimals,
                        stale: r.stale, staleSinceMs: Number(r.staleSinceMs) })
        }
        list.sort((a, b) => ((b.level === "danger") - (a.level === "danger"))
                            || (b.at - a.at) || (a.row - b.row))
        rows = list
        if (next < Infinity) {
            floorTimer.interval = Math.max(1, next - now)
            floorTimer.restart()
        } else {
            floorTimer.stop()
        }
    }

    component BandRow: QtObject {
        required property string key
        required property string name
        required property bool enabled
        required property string level
        required property bool banner
        required property bool live
        required property string state
        required property bool stale
        required property bool unavailable
        required property double fired
        required property double value
        required property string unit
        required property int decimals
        required property double staleSinceMs
        // A firing, or a set attached while ON: one change can bring both
        // live and state, in either order, so each looks at the other
        onFiredChanged: if (live && fired > 0) band.noteFiring(key)
        onStateChanged: if (live && state === "on") band.noteFiring(key)
        onLiveChanged: if (live && state === "on") band.noteFiring(key)
    }

    Instantiator {
        id: rowsOf
        model: band.conditions
        delegate: BandRow {}
        onObjectAdded: band.later()
        onObjectRemoved: band.later()
    }
    Connections {
        target: band.conditions
        ignoreUnknownSignals: true
        function onDataChanged() { band.later() }
        function onModelReset() { band.firedAt = ({}); band.later() }
    }
    Timer {
        id: floorTimer
        objectName: "alertFloorTimer"
        repeat: false
        onTriggered: band.recompute()
    }

    readonly property color ink: danger ? Theme.textOnAlertDanger : Theme.textOnAlertWarn
    color: danger ? Theme.alertDangerFill : Theme.alertWarnFill
    border.width: 1
    border.color: Theme.hairline
    radius: Theme.radius
    implicitHeight: content.implicitHeight + 16
    visible: shown

    Accessible.role: Accessible.AlertMessage
    Accessible.name: !shown ? ""
        : [(danger ? qsTr("Danger: %1") : qsTr("Warning: %1")).arg(shownRow.name)]
          .concat(valueText(shownRow) !== "" ? [valueText(shownRow)] : [])
          .concat(shownRow.stale ? [staleText(shownRow)] : [])
          .join(", ")
          + (more > 0 ? qsTr(", and %1 more").arg(more) : "")

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.topMargin: 8
        anchors.bottomMargin: 8
        spacing: 2

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            DangerMark {
                objectName: "alertBandMark"
                visible: band.danger
                size: 16
                color: band.ink
                cutout: Theme.alertDangerFill
            }
            Label {
                textFormat: Text.PlainText
                objectName: "alertBandLevel"
                Accessible.ignored: true
                text: band.shown ? band.levelWord(band.shownRow.level) : ""
                color: band.ink
                font.pixelSize: Theme.fontCaption
                font.weight: Font.DemiBold
            }
            Label {
                textFormat: Text.PlainText
                objectName: "alertBandName"
                Accessible.ignored: true
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                text: band.shown ? band.shownRow.name : ""
                color: band.ink
                font.pixelSize: Theme.fontBody
            }
            Label {
                textFormat: Text.PlainText
                objectName: "alertBandValue"
                Accessible.ignored: true
                visible: text !== ""
                text: band.shown ? band.valueText(band.shownRow) : ""
                color: band.ink
                font.pixelSize: Theme.fontBody
                font.family: Theme.valueFamily
                font.features: Theme.valueFeatures
            }
            Label {
                textFormat: Text.PlainText
                objectName: "alertBandMore"
                Accessible.ignored: true
                visible: band.more > 0
                text: qsTr("+%1 more").arg(band.more)
                color: band.ink
                font.pixelSize: Theme.fontCaption
            }
        }
        Label {
            textFormat: Text.PlainText
            objectName: "alertBandStale"
            Accessible.ignored: true
            Layout.fillWidth: true
            visible: band.shown && band.shownRow.stale
            wrapMode: Text.WordWrap
            text: band.shown ? band.staleText(band.shownRow) : ""
            color: band.ink
            font.pixelSize: Theme.fontCaption
        }
    }
}
