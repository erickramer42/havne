// File/layer: tests/qml/tst_consent.qml | Qt Quick Test, QML tier
// Testing: QML: the consent screen, wide (ConsentDialog) and on a phone
//          (ConsentPage): the title by class, a version change's lines,
//          the terms whole at 64 KiB, Decline as the default and Escape,
//          the fingerprint. Stub maps stand for Bridge.consent.ask; the
//          waiting connect is BridgeTest's.

import QtQuick
import QtTest
import App

TestCase {
    id: testCase
    name: "Consent"
    when: windowShown
    width: 1000
    height: 800

    // A first consent, as the bridge's ask map holds it
    function screen(over) {
        const base = {
            id: "talker3", name: "Talker 3", version: "1.0.0", set: 1,
            classes: ["conversation"],
            lines: [
                { id: 1, key: "asks", heading: "", text: "Talker 3 1.0.0 asks to send requests to your car's modules.", action: false },
                { id: 3, key: "lock", heading: "Some requests can lock a module", text: "Havne can't tell whether this plugin's content is correct.", action: true }
            ],
            publisher: { name: "Example Tuning", contact: "support@example.invalid", license: "Proprietary" },
            terms: "These are the publisher's terms.\nA second line.",
            fingerprint: "9c1e4a0752d8b6f3" + "0123456789abcdef".repeat(3),
            fingerprintShort: "9c1e 4a07 52d8 b6f3",
            changed: {},
            limitMs: 120000
        }
        return Object.assign({}, base, over)
    }
    // What a record of an earlier version holds
    function earlier(over) {
        return Object.assign({ id: "talker3", version: "0.9.0", publisher: "Example Tuning",
                               fingerprint: "ab".repeat(32), givenMs: 1700000000000,
                               publisherSame: true, termsSame: true }, over)
    }

    Component {
        id: wideMaker
        ConsentDialog {
            width: 1000
            height: 800
            shown: true
        }
    }
    Component {
        id: phoneMaker
        ConsentPage {
            width: 384
            height: 800
            shown: true
        }
    }
    SignalSpy { id: allowSpy; signalName: "allowed" }
    SignalSpy { id: declineSpy; signalName: "declined" }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        Theme.compact = false
        Theme.shortLayout = false
    }
    function cleanup() {
        Theme.compact = false
        Theme.shortLayout = false
        allowSpy.target = null
        declineSpy.target = null
        allowSpy.clear()
        declineSpy.clear()
    }

    function find(page, name) {
        const item = findChild(page, name)
        verify(item, name + " exists")
        return item
    }
    function wide(s) {
        const d = createTemporaryObject(wideMaker, testCase.parent, { screen: s })
        verify(d)
        allowSpy.target = d
        declineSpy.target = d
        tryVerify(() => findChild(d, "consentTitle") !== null)
        return d
    }
    function phone(s) {
        Theme.compact = true
        const p = createTemporaryObject(phoneMaker, testCase.parent, { screen: s })
        verify(p)
        allowSpy.target = p
        declineSpy.target = p
        tryVerify(() => findChild(p, "consentBar") !== null)
        return p
    }

    // The title says what the plugin's operations are
    function test_aFirstConsentIsTitledByTheClass_data() {
        return [
            { tag: "conversation", classes: ["conversation"],
              title: "Allow this plugin to talk to your car?" },
            { tag: "read", classes: ["conversation", "read"],
              title: "Allow this plugin to unlock and read your car's modules?" },
            { tag: "write", classes: ["conversation", "read", "write"],
              title: "Allow this plugin to unlock and write to your car's modules?" }
        ]
    }
    function test_aFirstConsentIsTitledByTheClass(data) {
        const d = wide(screen({ classes: data.classes }))
        compare(find(d, "consentTitle").text, data.title)
        compare(find(d, "consentAllow").text, "Allow")
        compare(findChild(d, "consentChanged_0"), null, "no changes block on a first consent")
    }

    function test_aFirstConsentShowsCoresLinesThePublisherAndTheTerms() {
        const d = wide(screen({}))
        compare(find(d, "consentLine_asks").visible, true)
        compare(find(d, "consentLine_lock").visible, true)
        compare(find(d, "consentPublisherName").text, "Example Tuning")
        compare(find(d, "consentContact").text, "support@example.invalid")
        compare(find(d, "consentLicense").text, "Proprietary")
        compare(find(d, "consentPlugin").text, "talker3 1.0.0")
        const terms = find(d, "pluginTerms")
        compare(terms.text, "These are the publisher's terms.\nA second line.")
        verify(terms.readOnly)
        compare(terms.Accessible.name, "Plugin terms")
        verify(terms.activeFocusOnTab, "the box takes the focus")
        verify(!find(d, "consentTermsSize").visible, "a short text has no size")
        compare(find(d, "consentAllowLine").text, "Allow Talker 3 1.0.0 to talk to your car's modules.")
        compare(find(d, "consentDeclineLine").text,
                "If you decline, Havne still reads with this plugin's signals. It sends nothing else.")
        compare(find(d, "consentWithdrawLine").text, "You can withdraw this in Settings, under Plugins.")
    }

    // A text with markup is shown as the characters it holds
    function test_theTermsArePlainText() {
        const markup = "<b>bold</b> <img src=\"file://host/a.png\">"
        const d = wide(screen({ terms: markup }))
        compare(find(d, "pluginTerms").text, markup)
        compare(find(d, "pluginTerms").textFormat, TextEdit.PlainText)
    }

    function test_theFingerprintIsSixteenDigitsInFourGroupsAndWholeOnRequest() {
        const s = screen({})
        const d = wide(s)
        compare(find(d, "consentFingerprint").text, "Fingerprint 9C1E 4A07 52D8 B6F3")
        const digits = find(d, "consentFingerprint").text.replace("Fingerprint ", "").replace(/ /g, "")
        compare(digits.length, 16)
        compare(digits.toLowerCase(), s.fingerprint.substring(0, 16), "the prefix of the whole")
        const full = find(d, "consentFingerprintFull")
        verify(!full.visible)
        mouseClick(find(d, "consentShowFull"))
        verify(full.visible)
        compare(full.text, s.fingerprint)
        compare(full.text.length, 64)
        compare(find(d, "consentShowFull").text, "Hide full fingerprint")
        mouseClick(find(d, "consentShowFull"))
        verify(!full.visible)
    }

    function test_theContactCopies() {
        const d = wide(screen({}))
        mouseClick(find(d, "consentCopyContact"))
        const probe = createTemporaryObject(probeMaker, testCase.parent)
        probe.paste()
        compare(probe.text, "support@example.invalid")
    }
    Component {
        id: probeMaker
        TextEdit { }
    }

    // Terms of 64 KiB: the box holds them whole, its size is said, and
    // Allow needs no scroll to its end
    function test_sixtyFourKiBOfTermsShowWholeAndAllowNeedsNoScroll() {
        const line = "Term clause number 0123456789, in plain words.\n"
        let text = ""
        while (text.length + line.length <= 65536)
            text += line
        text += "x".repeat(65536 - text.length)
        compare(text.length, 65536)
        const d = wide(screen({ terms: text }))
        const terms = find(d, "pluginTerms")
        compare(terms.text.length, 65536)
        verify(find(d, "consentTermsSize").visible)
        compare(find(d, "consentTermsSize").text, "64 KB")
        verify(find(d, "consentAllow").enabled, "Allow waits for no scroll")
        verify(find(d, "consentAllow").visible)
        // The card stays inside the window with the buttons in it
        const card = find(d, "consentCard")
        verify(card.height <= d.height)
        verify(find(d, "consentAllow").mapToItem(d, 0, 0).y + find(d, "consentAllow").height <= d.height)
        mouseClick(find(d, "consentAllow"))
        compare(allowSpy.count, 1)
    }

    // Decline holds the focus, Escape is Decline, Allow is the other button
    function test_declineIsTheDefaultAndEscapeDeclines() {
        const d = wide(screen({}))
        const decline = find(d, "consentDecline")
        tryVerify(() => decline.activeFocus, 2000, "Decline has the focus")
        keyClick(Qt.Key_Escape)
        compare(declineSpy.count, 1)
        compare(allowSpy.count, 0)
        keyClick(Qt.Key_Space)
        compare(declineSpy.count, 2, "Space presses the button with the focus: Decline")
        compare(allowSpy.count, 0)
        mouseClick(find(d, "consentAllow"))
        compare(allowSpy.count, 1)
    }

    // Declining leaves nothing behind in the screen: it is hidden by its
    // owner, and a hidden screen builds none of its text
    function test_aHiddenScreenHoldsNoTerms() {
        const d = wide(screen({}))
        verify(findChild(d, "pluginTerms") !== null)
        d.shown = false
        tryVerify(() => findChild(d, "pluginTerms") === null)
        verify(!d.visible)
    }

    function test_aVersionChangeAsksAgainAndSaysWhatChanged_data() {
        return [
            { tag: "newVersion", changed: earlier({}),
              lines: [/^You allowed Talker 3 0\.9\.0 on .+\. This is 1\.0\.0\.$/,
                      "Same publisher: Example Tuning.",
                      "The terms are the same as before."] },
            { tag: "sameNumber", changed: earlier({ version: "1.0.0" }),
              lines: [/^You allowed Talker 3 1\.0\.0 on .+\. This file is not the same plugin: its contents differ, though it still says 1\.0\.0\.$/,
                      "Same publisher: Example Tuning.",
                      "The terms are the same as before."] },
            { tag: "publisher", changed: earlier({ publisher: "Old Tuning", publisherSame: false }),
              lines: [/^You allowed/,
                      "The publisher is different. Before: Old Tuning. Now: Example Tuning.",
                      "The terms are the same as before."] },
            { tag: "terms", changed: earlier({ termsSame: false }),
              lines: [/^You allowed/, "Same publisher: Example Tuning.",
                      "The terms have changed. Read them below."] }
        ]
    }
    function test_aVersionChangeAsksAgainAndSaysWhatChanged(data) {
        const d = wide(screen({ changed: data.changed }))
        compare(find(d, "consentTitle").text, "This plugin has changed. Allow it again?")
        compare(find(d, "consentAllow").text, "Allow this version")
        for (let i = 0; i < data.lines.length; ++i) {
            const text = find(d, "consentChanged_" + i).text
            if (data.lines[i] instanceof RegExp)
                verify(data.lines[i].test(text), "line " + i + ": " + text)
            else
                compare(text, data.lines[i])
        }
        compare(find(d, "consentChanged_3").text,
                "Havne can't tell you what else changed. The plugin may now send other requests.")
        // The new plugin is shown whole, as on a first consent
        compare(find(d, "pluginTerms").text, "These are the publisher's terms.\nA second line.")
        compare(find(d, "consentPublisherName").text, "Example Tuning")
    }

    // The phone's form: a page with the buttons pinned, labels over values
    function test_thePhonesPageHasTheBarPinnedAndNoInnerScrollBox() {
        const p = phone(screen({ terms: "Terms.\n".repeat(400) }))
        compare(findChild(p, "consentTitle"), null, "no wide title on a page")
        const sub = find(p, "consentPage")
        compare(sub.title, "Allow this plugin?")
        compare(sub.Accessible.name, "Allow this plugin to talk to your car?")
        verify(find(p, "settingsBack").visible)
        const block = find(p, "pluginTermsBlock")
        verify(block.visible)
        compare(findChild(p, "consentScroll"), null, "no inner scroll")
        compare(block.Accessible.name, "Plugin terms")
        const bar = find(p, "consentBar")
        const allow = find(p, "consentAllow")
        const decline = find(p, "consentDecline")
        verify(allow.height >= 48 && decline.height >= 48, "targets of 48")
        compare(allow.width, decline.width, "equal widths")
        verify(decline.mapToItem(p, 0, 0).x < allow.mapToItem(p, 0, 0).x, "Decline left, Allow right")
        verify(bar.mapToItem(p, 0, 0).y + bar.height <= p.height)
        compare(bar.mapToItem(p, 0, 0).y + bar.height, p.height, "pinned at the bottom")
        // The scroll moves under the bar, which stays
        sub.contentY = sub.contentHeight
        compare(bar.mapToItem(p, 0, 0).y + bar.height, p.height)
        mouseClick(allow)
        compare(allowSpy.count, 1)
    }

    function test_thePhonesBackArrowDeclines() {
        const p = phone(screen({}))
        mouseClick(find(p, "settingsBack"))
        compare(declineSpy.count, 1)
        compare(allowSpy.count, 0)
    }

    function test_thePhonesVersionChangeIsTheSamePageWithTheBlockFirst() {
        const p = phone(screen({ changed: earlier({}) }))
        compare(find(p, "consentPage").title, "This plugin has changed")
        verify(find(p, "consentChanged_0").visible)
        const first = find(p, "consentChanged_0").mapToItem(p, 0, 0).y
        const lines = find(p, "consentLines").mapToItem(p, 0, 0).y
        verify(first < lines, "what changed comes above Havne's lines")
    }

    // Labels sit above their values on a phone, beside them otherwise
    function test_theLabelsSitAboveTheirValuesOnAPhone() {
        const p = phone(screen({}))
        const grid = find(p, "consentPublisher")
        compare(grid.columns, 1)
        Theme.compact = false
        const w = wide(screen({}))
        compare(find(w, "consentPublisher").columns, 2)
    }
}
