// File/layer: tests/qml/tst_log.qml | Qt Quick Test, QML tier
// Testing: the Log page shows the activity log: "Ready.", then each line
//          the bridge logs, stamped HH:MM:SS.mmm, kept to its newest
//          lines. With developer mode off it shows nothing else; on, the
//          Activity | Frames switch shows the raw frame table, and turning
//          developer mode off goes back to the activity log. Settings'
//          developer box sets Theme.developerMode and the mock:// preset
//          follows it. How the page looks is manual smoke.

import QtQuick
import QtTest
import QtTune
import QtTuneTest

TestCase {
    id: testCase
    name: "Log"
    when: windowShown

    Component {
        id: logComponent
        LogPage { width: 1000; height: 700 }
    }
    Component {
        id: settingsComponent
        SettingsPage { width: 1000; height: 800 }
    }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/QtTune\//)
        Theme.developerMode = false
    }

    function cleanup() {
        Theme.developerMode = false
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    function create(component) {
        const page = createTemporaryObject(component, testCase.parent)
        verify(page, "the page loads")
        return page
    }

    function test_theActivityLogTakesTheBridgesLines() {
        const page = create(logComponent)
        const log = find(page, "activityLog")
        compare(log.text, "Ready.")
        compare(Bridge.connectSession("mock://demo"), "")
        verify(/\d\d:\d\d:\d\d\.\d\d\d  Connected: mock:\/\/demo/.test(log.text),
               "stamped like the surrogate's log: " + log.text)
        verify(log.text.includes("Session: mock transport; profile mock"))
    }

    function test_theActivityLogKeepsItsNewestLines() {
        const page = create(logComponent)
        const log = find(page, "activityLog")
        for (let i = 0; i < page.maxLogLines + 50; ++i)
            page.appendLog("line " + i)
        verify(log.lineCount <= page.maxLogLines, "bounded: " + log.lineCount)
        verify(log.text.includes("line " + (page.maxLogLines + 49)), "the newest is kept")
        verify(!log.text.includes("Ready."), "the oldest went first")
    }

    function test_framesOnlyInDeveloperMode() {
        const page = create(logComponent)
        const activity = find(page, "activityView")
        const frames = find(page, "frameView")
        const toFrames = find(page, "view_frames")
        verify(activity.visible, "the activity log, alone")
        verify(!frames.visible)
        verify(!toFrames.visible, "no switch without developer mode")

        Theme.developerMode = true
        verify(toFrames.visible, "the switch comes with developer mode")
        verify(activity.visible, "still the activity log until switched")
        toFrames.clicked()
        verify(frames.visible, "the raw frames")
        verify(!activity.visible)

        Theme.developerMode = false
        verify(activity.visible, "back to the activity log")
        verify(!frames.visible)
        verify(!toFrames.visible)
    }

    function test_settingsDeveloperBoxSetsTheModeAndTheMockPreset() {
        const page = create(settingsComponent)
        const box = find(page, "developerBox")
        const mock = find(page, "preset_mock")
        verify(!box.checked)
        verify(!mock.visible, "only real devices are offered")
        verify(find(page, "preset_j2534").visible)
        verify(find(page, "preset_elm327").visible)

        mouseClick(box)
        verify(Theme.developerMode, "the box sets the mode")
        verify(mock.visible, "the mock:// preset in developer mode")

        Theme.developerMode = false   // as a restore at launch would
        verify(!box.checked, "the box follows the mode")
        verify(!mock.visible)
    }
}
