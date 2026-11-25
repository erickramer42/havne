// File/layer: tests/qml/tst_unlock.qml | Qt Quick Test, QML tier
// Testing: QML: the unlock's question (UnlockDialog) and the Diagnostics
//          page's Unlock... and the module's unlock words. A stub list
//          stands for Bridge.consent.conversations; the unlock itself is
//          BridgeTest's.

import QtQuick
import QtTest
import App

TestCase {
    id: testCase
    name: "Unlock"
    when: windowShown
    width: 900
    height: 1600

    // A module the plugin talks to, as the bridge's entry holds it
    function entry(over) {
        return Object.assign({
            channel: 0, ecu: "ecm", rx: 0x7E8, state: "talking", unlock: "none",
            level: 0, nrc: 0, failed: 0, delayLeftMs: 0, delayEndsMs: 0, kept: true,
            levels: [{ level: 1, delayMs: 10000 }]
        }, over)
    }
    readonly property var coreLines: [
        { key: "engine-state", heading: "The engine", text: "Keep the engine off and the ignition on." },
        { key: "lockout", heading: "A lockout", text: "Wrong keys lock the module out for a while." }
    ]

    Component {
        id: dialogMaker
        UnlockDialog { width: 600 }
    }
    Component {
        id: pageMaker
        DiagnosticsPage { width: 900; height: 1600 }
    }
    SignalSpy { id: unlockSpy; signalName: "unlockChosen" }
    SignalSpy { id: cancelSpy; signalName: "cancelChosen" }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
    }
    function cleanup() {
        unlockSpy.target = null
        cancelSpy.target = null
        unlockSpy.clear()
        cancelSpy.clear()
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
    }
    function find(page, name) {
        const item = findChild(page, name)
        verify(item, name + " exists")
        return item
    }
    function dialog(conversation) {
        const d = createTemporaryObject(dialogMaker, testCase.parent, { conversation: conversation, lines: coreLines })
        verify(d)
        unlockSpy.target = d
        cancelSpy.target = d
        return d
    }

    function test_theQuestionShowsCoresTwoLinesInFull() {
        const d = dialog(entry({}))
        compare(find(d, "unlockTitle").text, "Unlock ECU 0x7E8 (ecm)?")
        // Each line's text is the second child of its column
        compare(find(d, "unlockLine_engine-state").children[1].text,
                "Keep the engine off and the ignition on.")
        compare(find(d, "unlockLine_lockout").children[1].text,
                "Wrong keys lock the module out for a while.")
        compare(find(d, "unlockConfirm").text, "Unlock this module")
    }

    // The level is a choice only where the module declares more than one
    function test_aLevelIsAChoiceOnlyWithMoreThanOne() {
        const one = dialog(entry({}))
        verify(!find(one, "unlockLevels").visible)
        mouseClick(find(one, "unlockConfirm"))
        compare(unlockSpy.count, 1)
        compare(unlockSpy.signalArguments[0][0], 1)

        unlockSpy.clear()
        const two = dialog(entry({ levels: [{ level: 1, delayMs: 0 }, { level: 3, delayMs: 0 },
                                            { level: 1, delayMs: 0 }] }))
        verify(find(two, "unlockLevels").visible)
        verify(find(two, "unlockLevel_1").checked, "the first is chosen until another is")
        compare(find(two, "unlockLevel_1").text, "Level 0x01")
        mouseClick(find(two, "unlockLevel_3"))
        verify(find(two, "unlockLevel_3").checked)
        mouseClick(find(two, "unlockConfirm"))
        compare(unlockSpy.count, 1)
        compare(unlockSpy.signalArguments[0][0], 3)
    }

    function test_whileAskingTheWordCannotBeSentAgain() {
        const d = dialog(entry({}))
        d.asking = true
        verify(!find(d, "unlockConfirm").enabled)
        compare(find(d, "unlockNote").text, "Asking the module...")
        d.asking = false
        d.note = "Unlock refused: the module is busy"
        verify(find(d, "unlockConfirm").enabled)
        compare(find(d, "unlockNote").text, "Unlock refused: the module is busy")
        mouseClick(find(d, "unlockCancel"))
        compare(cancelSpy.count, 1)
    }

    function test_unlockIsOfferedOnlyToATalkingModuleNotYetUnlocked() {
        const page = createTemporaryObject(pageMaker, testCase.parent)
        verify(page)
        verify(page.canUnlock(entry({})))
        verify(page.canUnlock(entry({ unlock: "failed", nrc: 0x35 })))
        verify(!page.canUnlock(entry({ unlock: "unlocked" })))
        verify(!page.canUnlock(entry({ unlock: "seed" })), "an unlock under way")
        verify(!page.canUnlock(entry({ state: "declined" })))
        verify(!page.canUnlock(entry({ state: "ended" })))
        verify(!page.canUnlock(entry({ levels: [] })), "no level declared")
        verify(!page.canUnlock(null))
    }

    function test_theRowFollowsTheUnlock() {
        const page = createTemporaryObject(pageMaker, testCase.parent)
        verify(page)
        compare(page.unlockText(entry({})), "")
        compare(page.unlockText(entry({ unlock: "seed" })), "Unlocking...")
        compare(page.unlockText(entry({ unlock: "unlocked", level: 1 })), "Unlocked, level 0x01")
        page.nowMs = 100000
        compare(page.unlockText(entry({ unlock: "failed", nrc: 0x35, delayEndsMs: 107500 })),
                "The unlock failed (module code 0x35). Try again in 8 s")
        compare(page.unlockText(entry({ unlock: "failed", nrc: 0x35, delayEndsMs: 90000 })),
                "The unlock failed (module code 0x35)")
        compare(page.unlockText(entry({ unlock: "delayed", delayEndsMs: 130000 })),
                "The module asks for a wait: 30 s")
    }

    // A card of the mock's one ECU, given a conversation by the stub list
    function test_aTalkingModulesCardOpensTheQuestionAndShowsARefusal() {
        const page = createTemporaryObject(pageMaker, testCase.parent)
        verify(page)
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => Bridge.diagnostics.length === 1, 2000)
        const card = find(page, "ecu_0x7E8")
        verify(!find(card, "unlockOpen").visible, "no plugin talks to the mock")
        verify(!find(page, "unlockDialog").visible)

        page.conversations = [entry({ rx: 0x7E8 })]
        tryVerify(() => find(page, "ecu_0x7E8").conversation !== null)
        const open = find(find(page, "ecu_0x7E8"), "unlockOpen")
        verify(open.visible)
        mouseClick(open)
        verify(find(page, "unlockDialog").visible)
        compare(page.unlocking, 0)
        verify(!find(find(page, "ecu_0x7E8"), "unlockOpen").visible, "the question is open")
        // The bridge's session talks to no module here: its words, in the question
        mouseClick(find(page, "unlockConfirm"))
        compare(find(page, "unlockNote").text, "The plugin isn't talking to this module")
        verify(!page.unlockAsking)
        mouseClick(find(page, "unlockCancel"))
        verify(!find(page, "unlockDialog").visible)
    }

    // The question closes by itself when its module stops talking
    function test_theQuestionClosesWhenTheModuleStopsTalking() {
        const page = createTemporaryObject(pageMaker, testCase.parent)
        verify(page)
        page.conversations = [entry({})]
        page.unlockOpen(page.conversations[0])
        compare(page.unlocking, 0)
        page.conversations = [entry({ state: "ended" })]
        compare(page.unlocking, -1)
    }
}
