// File/layer: tests/qml/DashboardBookKit.qml | Qt Quick Test, QML tier
// Testing: QML: the base of the tst_dashboardbook*.qml files (test_qml); not a test itself

import QtQuick
import QtTest
import App
import AppTest

// The shared setup and helpers of tst_dashboardbook*.qml: a file derives from it and sets its `name`;
// `init` and `cleanup` are the kit's.
TestCase {

    readonly property string degC: "\u00B0C"

    // One character outside the BMP: two UTF-16 units, one code point
    readonly property string emoji: "\uD83D\uDE97"

    function cell(label, extra) {
        const c = { kind: "digital", w: 1, h: 4, label: label, refs: ["id:1005", "id:7005"],
                    unit: degC, options: { from: 0, to: 120 } }
        for (const k in extra)
            c[k] = extra[k]
        return c
    }

    // Two pages, the first with two cells
    function book() {
        return {
            format: DashboardBook.format,
            pages: [
                { name: "Engine", cells: [cell("IAT"), cell("Oil")] },
                { name: "Track", cells: [] }
            ]
        }
    }

    // A page of n cells labeled c0, c1, ...
    function pageOf(n) {
        const cells = []
        for (let i = 0; i < n; ++i)
            cells.push(cell("c" + i))
        return { format: DashboardBook.format, pages: [{ name: "P", cells: cells }] }
    }

    function read(d) {
        return DashboardBook.parse(JSON.stringify(d), "desktop")
    }

    function labels(d, page) {
        return d.pages[page].cells.map(c => c.label)
    }

    // The text's size in UTF-8 bytes, counted apart from DashboardBook's
    function bytes(s) {
        const enc = encodeURIComponent(s)
        return enc.length - 2 * (enc.split("%").length - 1)
    }

    // A document whose text is exactly n bytes, padded in an unknown key
    // with characters of one to four bytes
    function docOfBytes(n, format) {
        const d = { format: format === undefined ? DashboardBook.format : format,
                    pages: [{ name: "P", cells: [] }], pad: "\u20AC" + emoji }
        let pad = n - bytes(JSON.stringify(d))
        d.pad += "\u00E9".repeat(Math.floor(pad / 2)) + (pad % 2 ? "a" : "")
        return d
    }

    // ---- Placement

    function spots(list) {
        return list.map(s => [s.x, s.y, s.w, s.h])
    }
}
