// File/layer: tests/qml/tst_fields.qml | Qt Quick Test, QML tier
// Testing: ThemedTextField and ThemedCheckBox: the fills, borders and text
//          colors from Theme's roles in every palette, the ring that
//          follows the focus, the touch size on a phone, and a check box
//          whose mark and fill follow its state. No session.

import QtQuick
import QtTest
import App

TestCase {
    id: testCase
    name: "Fields"
    when: windowShown
    width: 400
    height: 300

    Component {
        id: hostMaker
        Item {
            id: host
            property real touchMin: 0
            property alias field: field
            property alias other: other
            property alias box: box
            property alias longBox: longBox
            property alias bare: bare
            width: 400
            height: 300
            Column {
                spacing: 8
                ThemedTextField { id: field; width: 200; placeholderText: "Find" }
                ThemedTextField { id: other; width: 200; text: "abc" }
                ThemedCheckBox { id: box; text: "Plotted" }
                ThemedCheckBox { id: bare }
                ThemedCheckBox { id: longBox; width: 120; text: "A label long enough to need a second line" }
            }
        }
    }

    function host(touchMin) {
        const h = createTemporaryObject(hostMaker, testCase.parent, { touchMin: touchMin })
        verify(h)
        waitForPolish(h)
        return h
    }
    function sameColor(actual, expected, what) {
        verify(Qt.colorEqual(actual, expected), what + ": " + actual + ", expected " + expected)
    }
    function usePalette(name) {
        Theme.night = name === "dark" ? "off" : "on"
        if (name !== "dark")
            Theme.nightStyle = name === "nightRed" ? "red" : "amber"
    }
    function cleanup() {
        Theme.night = "off"
        Theme.nightStyle = "amber"
    }

    function test_aFieldDrawsTheCardsControlsInEveryPalette_data() {
        return Theme.paletteNames.map(n => ({ tag: n, name: n }))
    }
    function test_aFieldDrawsTheCardsControlsInEveryPalette(data) {
        usePalette(data.name)
        const h = host(0)
        sameColor(h.other.background.color, Theme.elevated, "fill")
        sameColor(h.other.background.border.color, Theme.outline, "border at rest")
        compare(h.other.background.border.width, 1)
        compare(h.other.background.radius, Theme.radius)
        sameColor(h.other.color, Theme.text, "text")
        sameColor(h.other.placeholderTextColor, Theme.textDim, "placeholder")
        // the ring reads against the fill in the palette: text, not primary
        h.other.forceActiveFocus()
        verify(h.other.activeFocus)
        sameColor(h.other.background.border.color, Theme.text, "the focus ring")
        compare(h.other.background.border.width, 2)
        verify(Math.abs(Theme.text.hslLightness - Theme.elevated.hslLightness) > 0.3,
               "the ring is told from the fill")
    }

    function test_theRingMovesWithTheFocus() {
        const h = host(0)
        h.field.forceActiveFocus()
        compare(h.field.background.border.width, 2)
        compare(h.other.background.border.width, 1)
        h.other.forceActiveFocus()
        compare(h.field.background.border.width, 1)
        compare(h.other.background.border.width, 2)
    }

    function test_aFieldIsAPhonesTouchSizeWhereTheTargetsAre() {
        const small = host(0)
        verify(small.field.height >= Theme.touchTarget, "at the theme's own target: " + small.field.height)
        const phone = host(48)
        verify(phone.field.height >= 48, "a page that asks for 48: " + phone.field.height)
    }

    function test_aDisabledFieldIsDimmed() {
        const h = host(0)
        h.field.enabled = false
        compare(h.field.opacity, 0.45)
    }

    function test_aCheckBoxIndicatorIsDrawnFromItsState() {
        const h = host(0)
        const ind = h.box.indicator
        compare(ind.width, 20)
        compare(ind.radius, Theme.radius)
        compare(ind.color.a, 0, "unchecked has no fill")
        sameColor(ind.border.color, Theme.outline, "unchecked border")
        compare(ind.children[0].visible, false, "no mark unchecked")
        h.box.checked = true
        sameColor(ind.color, Theme.primary, "checked fill")
        compare(ind.children[0].visible, true, "the mark")
        sameColor(ind.children[0].data[0].strokeColor, Theme.textOnPrimary, "the mark's color")
    }

    // The ring follows the keyboard focus, as a button's does; a pointer
    // click leaves it off
    function test_aCheckBoxShowsAFocusRingForTheKeyboardOnly() {
        const h = host(0)
        compare(h.box.indicator.border.width, 1)
        sameColor(h.box.indicator.border.color, Theme.outline, "at rest")
        h.box.forceActiveFocus(Qt.TabFocusReason)
        verify(h.box.visualFocus, "tab focus")
        compare(h.box.indicator.border.width, 2)
        sameColor(h.box.indicator.border.color, Theme.text, "the ring")
        h.field.forceActiveFocus(Qt.MouseFocusReason)
        compare(h.box.indicator.border.width, 1)
    }

    function test_aCheckBoxWithNoTextIsATouchsWidth() {
        const h = host(0)
        verify(h.bare.width >= Theme.touchTarget, "bare width " + h.bare.width)
        verify(h.box.width > 0 && h.box.contentItem.text !== "", "a box with text keeps its own")
    }

    function test_aClickTogglesTheBoxAndItsRowOfTextIsTheTargetToo() {
        const h = host(0)
        mouseClick(h.box, h.box.width - 4, h.box.height / 2)
        compare(h.box.checked, true, "a click on the text")
        mouseClick(h.box, 10, h.box.height / 2)
        compare(h.box.checked, false, "a click on the indicator")
    }

    function test_aCheckBoxIsAPhonesTouchSizeWhereAskedAndWrapsItsText() {
        const h = host(48)
        verify(h.box.height >= 48, "height " + h.box.height)
        verify(h.longBox.height > h.box.height, "a long label wraps: " + h.longBox.height)
        verify(h.longBox.contentItem.contentWidth <= h.longBox.width, "inside its width")
    }
}
