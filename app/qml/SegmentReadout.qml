// File/layer: app/qml/SegmentReadout.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.gauges, QmlTest.dashboard (test_qml)

import QtQuick

pragma ComponentBehavior: Bound

// Digits of seven segments (ADR-0057 section 1), the unlit segments faint,
// the unit beside. It has no scale to color, so it colors its digits by the
// zones instead: lit in the theme's value color, warm past warnFrom, danger
// past dangerFrom (with the mark beside the unit). A value too long for
// `digits` shows dashes in every digit, never a wrong number; no value
// shows blanks. A digit is seven static segments whose opacity follows the
// character.
GaugeBase {
    id: gauge

    // How many digits (1 to 8)
    property int digits: 4

    readonly property int count: Math.max(1, Math.min(8, Math.round(digits)))

    // What the digits show, one entry per digit, right aligned: the
    // character ("0" to "9", "-" or " ") and whether its point is lit
    readonly property var shown: {
        const blank = []
        for (let i = 0; i < count; ++i)
            blank.push({ ch: " ", dp: false })
        if (!hasValue || isNaN(value))
            return blank
        const text = Number(value).toFixed(decimals)
        let chars = []
        for (const c of text) {
            if (c === ".")
                chars[chars.length - 1].dp = true
            else
                chars.push({ ch: c, dp: false })
        }
        if (chars.length > count) {
            const dashes = []
            for (let i = 0; i < count; ++i)
                dashes.push({ ch: "-", dp: false })
            return dashes
        }
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
    readonly property color digitColor: hasValue ? zoneColor(value, Theme.gaugeValue) : Theme.textDim

    implicitHeight: 64

    Text {
        id: title
        anchors { left: parent.left; top: parent.top }
        text: gauge.label
        font.pixelSize: 12
        font.weight: Font.DemiBold
        font.letterSpacing: 1.2
        font.capitalization: Font.AllUppercase
        color: Theme.textDim
    }
    Text {
        anchors { left: title.right; leftMargin: 10; baseline: title.baseline }
        visible: !gauge.hasValue
        text: gauge.note
        font.pixelSize: 11
        color: Qt.alpha(Theme.textDim, 0.7)
    }

    Row {
        id: row
        objectName: "digits"
        anchors { left: parent.left; top: title.bottom; topMargin: 6 }
        readonly property real digitH: Math.max(24, Math.min(gauge.height - title.height - 14,
            (gauge.width - unitBox.width - 12) / (gauge.count * 0.62)))
        height: digitH
        spacing: digitH * 0.14
        opacity: gauge.stale ? 0.45 : 1.0

        Repeater {
            model: gauge.count
            delegate: Item {
                id: digit
                required property int index
                objectName: "digit"
                readonly property var entry: gauge.shown[index]
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
        anchors { left: row.right; leftMargin: 10; bottom: row.bottom }
        spacing: 4
        opacity: gauge.stale ? 0.45 : 1.0
        Text {
            objectName: "unit"
            visible: gauge.hasValue
            text: gauge.unit
            font.pixelSize: Math.max(11, row.digitH * 0.28)
            color: Theme.textDim
        }
        // Past dangerFrom, beside the unit; its slot stays
        DangerMark {
            visible: !isNaN(gauge.dangerFrom)
            opacity: gauge.inDanger ? 1 : 0
            size: Math.max(12, row.digitH * 0.3)
        }
    }
}
