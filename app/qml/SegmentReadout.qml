// File/layer: app/qml/SegmentReadout.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.kinds, QmlTest.accessibility, QmlTest.dashboard

import QtQuick

pragma ComponentBehavior: Bound

// Digits of seven segments (ADR-0057 section 1), the unlit segments faint,
// the unit beside, or under them and centered on a card where stacking draws
// them larger. It has no scale to color, so it colors its digits by the
// zones instead: lit in the gauge's accent, warm past warnFrom, danger past
// dangerFrom (with the mark beside the unit). A value too long for `digits`
// (or past 1e15, or infinite) shows dashes in every digit, never a wrong
// number; no value shows blanks; a minus zero shows 0. The unit's box is
// always laid out and sized from the card, never from the digits, so the
// first value never resizes them. A digit is seven static segments whose
// opacity follows the character.
GaugeBase {
    id: gauge

    // How many digits (1 to 8)
    property int digits: 4

    readonly property int count: Math.max(1, Math.min(8, Math.round(digits)))

    function dashes() {
        const out = []
        for (let i = 0; i < count; ++i)
            out.push({ ch: "-", dp: false })
        return out
    }

    // What the digits show, one entry per digit, right aligned: the
    // character ("0" to "9", "-" or " ") and whether its point is lit
    readonly property var shown: {
        const blank = []
        for (let i = 0; i < count; ++i)
            blank.push({ ch: " ", dp: false })
        if (!hasValue || isNaN(value))
            return blank
        if (Math.abs(value) >= 1e15)   // also Infinity: more than any digits show
            return dashes()
        let text = Number(value).toFixed(decimals)
        if (/^-0(\.0+)?$/.test(text))
            text = text.slice(1)   // a minus zero is a zero
        const chars = []
        for (const c of text) {
            if (c === ".")
                chars[chars.length - 1].dp = true
            else
                chars.push({ ch: c, dp: false })
        }
        if (chars.length > count)
            return dashes()
        while (chars.length < count)
            chars.unshift({ ch: " ", dp: false })
        return chars
    }

    // The segments each character lights, a to g clockwise from the top,
    // g the middle bar
    readonly property var lit: ({
        "0": "abcdef", "1": "bc", "2": "abdeg", "3": "abcdg", "4": "bcfg",
        "5": "acdfg", "6": "acdefg", "7": "abc", "8": "abcdefg", "9": "abcdfg",
        "-": "g", " ": ""
    })
    readonly property color digitColor: hasValue ? zoneColor(value, accent) : Theme.textDim

    // The unit's box: always there, its size from the card
    readonly property real unitPx: Math.max(11, Math.min(16, height * 0.16))
    readonly property real markPx: Math.max(12, Math.min(18, height * 0.2))

    // Stacked, the digits have the card's width, less the unit's line in
    // height; worth moving the unit when that draws them a fifth larger
    readonly property real unitGap: 4
    readonly property real roomBelow: height - title.height - 14
    readonly property real besideH: Math.min(roomBelow, (width - unitBox.width - 14) / (count * 0.62))
    readonly property real stackedH: Math.min(roomBelow - unitBox.height - unitGap,
                                              (width - 14) / (count * 0.62))
    readonly property bool tall: stackedH > Math.max(besideH, 1) * 1.2

    implicitHeight: 64

    Text {
        id: title
        objectName: "readoutTitle"
        Accessible.ignored: true
        anchors { left: parent.left; top: parent.top }
        text: gauge.label
        font.pixelSize: 12
        font.weight: Font.DemiBold
        color: Theme.textDim
    }
    Text {
        objectName: "gaugeNote"
        Accessible.ignored: true
        anchors { left: title.right; leftMargin: 10; right: parent.right; baseline: title.baseline }
        visible: !gauge.hasValue && width > 0
        elide: Text.ElideRight
        text: gauge.note
        font.pixelSize: 11
        color: Qt.alpha(Theme.textDim, 0.7)
    }

    Row {
        id: row
        objectName: "digits"
        // The room divided among the digits (each a little over half its
        // height wide), at most the card's own height
        readonly property real digitH: Math.max(8, gauge.tall ? gauge.stackedH : gauge.besideH)
        x: gauge.tall ? (gauge.width - width) / 2 : 0
        y: gauge.tall ? title.height + (gauge.height - title.height - digitH - gauge.unitGap
                                        - unitBox.height) / 2
                      : title.height + 6
        height: digitH
        spacing: digitH * 0.14
        opacity: gauge.stale ? 0.45 : 1.0

        Repeater {
            model: gauge.count
            delegate: Item {
                id: digit
                required property int index
                objectName: "digit"
                // The model can outrun `shown` by a binding when `digits` changes
                readonly property var entry: gauge.shown[index] ?? ({ ch: " ", dp: false })
                readonly property string on: gauge.lit[entry.ch] ?? ""
                readonly property real t: Math.max(2, row.digitH * 0.12)
                readonly property real w: row.digitH * 0.5
                width: w
                height: row.digitH

                // One segment: a rounded bar whose opacity is its state
                component Seg: Rectangle {
                    required property string name
                    radius: digit.t / 2
                    color: gauge.digitColor
                    opacity: digit.on.indexOf(name) >= 0 ? 1.0 : 0.08
                }
                readonly property real half: (height - 3 * t) / 2

                Seg { name: "a"; x: digit.t; y: 0; width: digit.w - 2 * digit.t; height: digit.t }
                Seg { name: "b"; x: digit.w - digit.t; y: digit.t; width: digit.t; height: digit.half + digit.t / 2 }
                Seg { name: "c"; x: digit.w - digit.t; y: digit.t + digit.half + digit.t / 2; width: digit.t; height: digit.half + digit.t / 2 }
                Seg { name: "d"; x: digit.t; y: digit.height - digit.t; width: digit.w - 2 * digit.t; height: digit.t }
                Seg { name: "e"; x: 0; y: digit.t + digit.half + digit.t / 2; width: digit.t; height: digit.half + digit.t / 2 }
                Seg { name: "f"; x: 0; y: digit.t; width: digit.t; height: digit.half + digit.t / 2 }
                Seg { name: "g"; x: digit.t; y: digit.t + digit.half; width: digit.w - 2 * digit.t; height: digit.t }
                Rectangle {   // the decimal point
                    objectName: "point"
                    width: digit.t * 1.1; height: width; radius: width / 2
                    x: digit.w + row.spacing / 2 - width / 2
                    y: digit.height - height
                    color: gauge.digitColor
                    opacity: digit.entry.dp ? 1.0 : 0.0
                }
            }
        }
    }

    Row {
        id: unitBox
        objectName: "unitBox"
        x: gauge.tall ? (gauge.width - width) / 2 : gauge.width - width
        y: gauge.tall ? row.y + row.height + gauge.unitGap : row.y + row.height - height
        spacing: 4
        opacity: gauge.stale ? 0.45 : 1.0
        Text {
            objectName: "unit"
            Accessible.ignored: true
            // Kept laid out without a value, so the digits keep their size
            opacity: gauge.hasValue ? 1 : 0
            text: gauge.unit
            font.pixelSize: gauge.unitPx
            color: Theme.textDim
        }
        // Past dangerFrom, beside the unit; its slot stays
        DangerMark {
            visible: !isNaN(gauge.dangerFrom)
            opacity: gauge.inDanger ? 1 : 0
            size: gauge.markPx
        }
    }
}
