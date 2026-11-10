// File/layer: tests/qml/tst_buttons.qml | Qt Quick Test, QML tier
// Testing: ThemedButton's four ranks (primary, secondary, quiet, selected):
//          their fills and text colors from Theme's roles, a selection
//          that stays the page's state through a click, and the touch
//          size and compact bounds on every rank. No session.

import QtQuick
import QtTest
import App

TestCase {
    id: testCase
    name: "Buttons"
    when: windowShown
    width: 400
    height: 300

    // Four of one rank each in a host that asks for a phone's touch size
    Component {
        id: hostMaker
        Item {
            id: host
            property real touchMin: 0
            property bool state: false
            property alias primary: primary
            property alias secondary: secondary
            property alias quiet: quiet
            property alias chosen: chosen
            property alias compactQuiet: compactQuiet
            property alias shortQuiet: shortQuiet
            width: 400
            height: 300
            Column {
                spacing: 8
                ThemedButton { id: secondary; text: "Refresh" }
                ThemedButton { id: primary; text: "Connect"; highlighted: true }
                ThemedButton { id: quiet; text: "Reset peaks"; flat: true }
                ThemedButton {
                    id: chosen
                    text: "Metric"
                    checkable: true
                    checked: host.state
                    onClicked: host.state = true
                }
                ThemedButton { id: shortQuiet; text: "OK"; flat: true }
                ThemedButton { id: compactQuiet; text: "Reset peaks"; flat: true; compact: true }
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

    function test_theRanksDrawTheirFills() {
        const h = host(0)
        sameColor(h.secondary.background.color, Theme.elevated, "secondary fill")
        sameColor(h.secondary.contentItem.color, Theme.text, "secondary text")
        sameColor(h.primary.background.color, Theme.primary, "primary fill")
        sameColor(h.primary.contentItem.color, Theme.textOnPrimary, "primary text")
        compare(h.quiet.background.color.a, 0, "a quiet button has no fill")
        sameColor(h.quiet.contentItem.color, Theme.text, "quiet text is the text color, not dim")
        sameColor(h.chosen.background.color, Theme.elevated, "an unchosen choice is secondary")
        h.state = true
        sameColor(h.chosen.background.color, Theme.elevatedLight, "a chosen choice's fill")
        sameColor(h.chosen.contentItem.color, Theme.text, "a chosen choice's text")
    }

    // The ranks' fills follow the palette: Dark's light primary, Daylight's
    // dark one and the night palettes' dim ones
    function test_theRanksDrawTheirFillsInEveryPalette_data() {
        return Theme.paletteNames.map(n => ({ tag: n, name: n }))
    }
    function test_theRanksDrawTheirFillsInEveryPalette(data) {
        const night = Theme.nightPalettes.indexOf(data.name) >= 0
        Theme.night = night ? "on" : "off"
        if (night)
            Theme.nightStyle = data.name === "nightRed" ? "red" : "amber"
        else
            Theme.look = data.name
        const h = host(0)
        sameColor(h.secondary.background.color, Theme.elevated, "secondary fill")
        sameColor(h.primary.background.color, Theme.primary, "primary fill")
        sameColor(h.primary.contentItem.color, Theme.textOnPrimary, "primary text")
        sameColor(h.quiet.contentItem.color, Theme.text, "quiet text")
        compare(h.quiet.background.color.a, 0, "quiet has no fill at rest")
        h.state = true
        sameColor(h.chosen.background.color, Theme.elevatedLight, "a chosen choice's fill")
        // a hover moves the primary fill away from its rest color, toward
        // darker on a light primary (a lighter one clamps) and lighter on
        // a dark one
        verify(!Qt.colorEqual(h.primary.hoverColor, Theme.primary), "primary hover differs")
        const lighter = h.primary.hoverColor.hslLightness > Theme.primary.hslLightness
        compare(lighter, Theme.primary.hslLightness <= 0.5, "hover direction")
    }

    function cleanup() {
        Theme.night = "off"
        Theme.nightStyle = "amber"
        Theme.look = "dark"
    }

    function test_aQuietButtonFillsOnHover() {
        const h = host(0)
        h.quiet.hoverEnabled = true
        mouseMove(h.quiet, 5, 5)
        tryVerify(() => h.quiet.hovered)
        sameColor(h.quiet.background.color, Theme.elevated, "hovered")
    }

    function test_aSelectionWinsOverTheOtherRanks() {
        const h = host(0)
        h.primary.checkable = true
        h.primary.checked = true
        sameColor(h.primary.background.color, Theme.elevatedLight, "checked, though highlighted")
        h.quiet.checkable = true
        h.quiet.checked = true
        sameColor(h.quiet.background.color, Theme.elevatedLight, "checked, though flat")
        sameColor(h.quiet.contentItem.color, Theme.text, "its text")
    }

    // A click shows the page's state, it doesn't change it: the button
    // is checked while the page's state is, however it is pressed
    function test_aClickOnTheChosenOneLeavesItChosen() {
        const h = host(0)
        h.state = true
        mouseClick(h.chosen)
        compare(h.chosen.checked, true)
        mouseClick(h.chosen)
        compare(h.chosen.checked, true, "still chosen after a second click")
        h.state = false
        compare(h.chosen.checked, false, "the binding still drives it")
    }

    function test_aClickOnAnotherChoiceChecksItThroughThePage() {
        const h = host(0)
        compare(h.chosen.checked, false)
        mouseClick(h.chosen)
        compare(h.state, true, "the page's onClicked ran")
        compare(h.chosen.checked, true)
    }

    function test_everyRankKeepsTheTouchSize() {
        const h = host(48)
        for (const b of [h.secondary, h.primary, h.quiet, h.chosen, h.compactQuiet]) {
            verify(b.height >= 48, b.text + " is " + b.height + " tall")
        }
        verify(h.shortQuiet.width >= 48, "a quiet button is 48 wide at the least")
    }

    function test_aQuietButtonIsAsWideAsItsWordsPlusTwelve() {
        const h = host(0)
        verify(h.quiet.width <= h.quiet.implicitContentWidth + 12 + 0.5,
               "quiet is " + h.quiet.width + " for " + h.quiet.implicitContentWidth)
        verify(h.secondary.width >= 120, "secondary keeps its 120 px")
        verify(h.primary.width >= 120, "primary keeps its 120 px")
        verify(h.compactQuiet.width <= Math.max(40, h.compactQuiet.implicitContentWidth + 12) + 0.5,
               "compact keeps its bound")
    }
}
