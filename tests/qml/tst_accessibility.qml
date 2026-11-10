// File/layer: tests/qml/tst_accessibility.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.accessibility (test_qml)

import QtQuick
import QtTest
import App

// The gauges as an accessibility client meets them: a value never shows,
// hides, makes or destroys an item (on Android each is a node rebuild on the
// main thread), and a gauge is one named node whose parts are ignored
TestCase {
    id: testCase
    name: "Accessibility"
    when: windowShown

    Component { id: analogC;   AnalogGauge { width: 300; height: 300 } }
    Component { id: ringC;     RingGauge { width: 240; height: 220 } }
    Component { id: barC;      BarGauge { width: 300; height: 72 } }
    Component { id: segmentsC; SegmentGauge { width: 300; height: 110 } }
    Component { id: digitalC;  DigitalGauge { width: 180; height: 160 } }
    Component { id: readoutC;  SegmentReadout { width: 260; height: 90 } }
    Component { id: sparkC;    SparkGauge { width: 300; height: 110 } }
    Component { id: shiftC;    ShiftLightBar { width: 400; height: 40 } }

    // Every kind DashboardPage draws but the blank, which has no parts
    readonly property var kinds: [
        { tag: "analog", c: analogC }, { tag: "ring", c: ringC },
        { tag: "bar", c: barC }, { tag: "segments", c: segmentsC },
        { tag: "digital", c: digitalC }, { tag: "seven-segment", c: readoutC },
        { tag: "sparkline", c: sparkC }, { tag: "shift-light", c: shiftC }]

    function init() { failOnWarning(/^qrc:\/qt\/qml\/App\//) }

    function rowsOfKinds() {
        return kinds.map(k => ({ tag: k.tag, kind: k }))
    }

    // A gauge with zones and a peak shown, at its range's start
    function gaugeOf(kind) {
        const g = createTemporaryObject(kind.c, testCase.parent, {
            label: "Test", unit: "u", from: 0, to: 100,
            warnFrom: 60, dangerFrom: 80, showPeak: true })
        verify(g, kind.tag + " loads")
        g.hasValue = true
        g.value = 0
        return g
    }

    // Every item under the gauge, in tree order
    function itemsOf(item, out) {
        for (let i = 0; i < item.children.length; ++i) {
            out.push(item.children[i])
            itemsOf(item.children[i], out)
        }
        return out
    }

    function isText(item) { return item.textFormat !== undefined }

    // Up past the range, back below it and up again: the zones, the danger
    // mark, a new peak and a new low each cross at least once
    readonly property var sweep: {
        const out = []
        for (let v = 0; v <= 120; v += 4) out.push(v)
        for (let v = 120; v >= -20; v -= 4) out.push(v)
        for (let v = -20; v <= 100; v += 10) out.push(v)
        return out
    }

    function test_aValueSweepShowsHidesMakesAndDestroysNothing_data() { return rowsOfKinds() }
    function test_aValueSweepShowsHidesMakesAndDestroysNothing(data) {
        const g = gaugeOf(data.kind)
        wait(20)
        const items = itemsOf(g, [])
        const shown = items.map(it => it.visible)
        verify(items.length > 0, "the gauge has parts")
        for (const v of sweep) {
            g.value = v
            wait(10)
            const now = itemsOf(g, [])
            compare(now.length, items.length, "the items at " + v)
            for (let i = 0; i < items.length; ++i) {
                verify(now[i] === items[i], "item " + i + " kept at " + v + ": " + String(items[i]))
                compare(now[i].visible, shown[i], "item " + i + " (" + String(items[i])
                        + ") keeps its visibility at " + v)
            }
        }
    }

    function test_aGaugeIsOneNamedNode_data() { return rowsOfKinds() }
    function test_aGaugeIsOneNamedNode(data) {
        const g = gaugeOf(data.kind)
        g.value = 42
        compare(g.Accessible.name, "Test: " + g.valueText + " u")
        g.value = 90
        compare(g.Accessible.name, "Test: " + g.valueText + " u, danger")
        let texts = 0
        for (const it of itemsOf(g, [])) {
            if (isText(it)) {
                ++texts
                verify(it.Accessible.ignored, "a part is ignored: " + String(it) + " '" + it.text + "'")
            } else if (it.objectName === "dangerMark") {
                verify(it.Accessible.ignored, "the danger mark is ignored")
            }
        }
        if (data.tag !== "shift-light")
            verify(texts > 0, "the gauge's texts were walked")
    }
}
