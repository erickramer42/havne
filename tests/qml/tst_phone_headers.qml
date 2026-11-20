// File/layer: tests/qml/tst_phone_headers.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.phone_headers (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

PhoneKit {
    id: testCase
    name: "Phone"

    function test_theHeadersLogStartsStopsAndOpensItsFolder() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 800
        main.font.pixelSize = 16
        const start = find(main.contentItem, "startLogButton")
        const stop = find(main.contentItem, "stopLogButton")
        const readout = find(main.contentItem, "logReadout")
        const mark = find(main.contentItem, "markButton")
        const disconnect = find(main.contentItem, "disconnectButton")
        const title = find(main.contentItem, "headerTitle")
        const row = find(main.contentItem, "headerRow")
        verify(!start.visible, "nothing to log before a connect")
        compare(Bridge.connectSession("mock://logged"), "", "mock://logged connects")
        tryVerify(() => start.visible && start.enabled, 5000, "Start log once connected")
        verify(disconnect.visible)
        waitForRendering(main.contentItem)
        verify(!title.visible, "the name gives way to Start log on a phone")
        const pillText = find(main.contentItem, "linkPillText")
        tryCompare(pillText, "text", "Connected", 5000, "the state is a word, not 'Ses...'")
        verify(!pillText.truncated, "and it fits")
        compare(find(main.contentItem, "linkPillAddress").text, "· mock://logged")
        let out = pastTheEdge(row, main.contentItem, [])
        compare(out.length, 0, "before the log, past " + main.contentItem.width + " px: "
                               + out.join(", "))
        start.clicked()
        verify(Bridge.logActive, "the log runs")
        verify(stop.visible && readout.visible, "Stop log and the readout")
        verify(!start.visible && !mark.visible && !disconnect.visible,
               "Start log, Mark and Disconnect step aside")
        tryVerify(() => Bridge.logBytes > 0 && Bridge.logElapsedMs >= 1000, 10000,
                  "the readout's second")
        // A second's log is well under the 0.1 MB the size shows from
        tryVerify(() => /^\d+:\d\d$/.test(readout.text), 2000, readout.text)
        verify(!find(main.contentItem, "linkPillRecording").visible, "a log is no recording")
        waitForRendering(main.contentItem)
        verify(!title.visible, "the name gives way on a phone")
        out = pastTheEdge(row, main.contentItem, [])
        compare(out.length, 0, "past " + main.contentItem.width + " px: " + out.join(", "))

        // A ten-hour log's readout: the name stays away at 420 px of
        // window and comes back at 420 px of header row, and both fit
        const gutter = main.width - row.width
        readout.text = "10:05:07\n123.4 MB"
        for (const [width, named] of [[420, false], [420 + gutter, true]]) {
            main.width = width
            waitForRendering(main.contentItem)
            compare(title.visible, named, "the name at " + width + " px")
            out = pastTheEdge(row, main.contentItem, [])
            compare(out.length, 0, "past " + main.contentItem.width + " px: " + out.join(", "))
        }
        main.width = 384

        const folder = Bridge.recordingFolder
        stop.clicked()
        verify(!Bridge.logActive, "the log stopped")
        verify(!stop.visible && !readout.visible)
        verify(start.visible && disconnect.visible, "Start log and Disconnect again")
        const recordings = find(main.contentItem, "recordingsPage")
        compare(main.currentPage, main.recordingsPage, "on Recordings")
        tryVerify(() => recordings.viewing, 5000, "the log's folder opens")
        compare(recordings.openedTitle, Bridge.recordingRow(folder).title)

        // A log the session's end closes leaves the driver where they are
        const livePage = 2
        main.gotoPage(livePage)
        start.clicked()
        verify(Bridge.logActive, "a second log runs")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting && !Bridge.logActive, 5000,
                  "the disconnect ends the log")
        compare(main.currentPage, livePage, "still on Live")
    }

    // The log readout's text: the time alone under 0.1 MB, then the size
    // beside it where the row has room, else under it
    function test_theLogReadoutShowsASizeOnceThereIsOne_data() {
        return [
            { tag: "aSecond", ms: 2000, bytes: 1000, oneLine: false, text: "0:02" },
            { tag: "aSecondWide", ms: 2000, bytes: 1000, oneLine: true, text: "0:02" },
            { tag: "justUnder", ms: 12000, bytes: 104857, oneLine: true, text: "0:12" },
            { tag: "twoLines", ms: 12000, bytes: 104858, oneLine: false, text: "0:12\n0.1 MB" },
            { tag: "oneLine", ms: 12000, bytes: 104858, oneLine: true, text: "0:12 · 0.1 MB" },
            { tag: "tenHours", ms: 36307000, bytes: 129395916, oneLine: false,
              text: "10:05:07\n123.4 MB" },
        ]
    }
    function test_theLogReadoutShowsASizeOnceThereIsOne(data) {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        compare(main.logReadoutText(data.ms, data.bytes, data.oneLine), data.text)
    }

    // A running log's readout takes one line where the header row has
    // room (a tablet, a desktop window) and two on a phone
    function test_theLogReadoutTakesOneLineWhereTheRowHasRoom() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 800
        const readout = find(main.contentItem, "logReadout")
        const row = find(main.contentItem, "headerRow")
        compare(Bridge.connectSession("mock://readout"), "", "mock://readout connects")
        const start = find(main.contentItem, "startLogButton")
        tryVerify(() => start.visible && start.enabled, 5000, "Start log once connected")
        start.clicked()
        verify(Bridge.logActive, "the log runs")
        // The log's own size is a second's worth: what the row would show
        // of a 0.1 MB one is read from the function with the row's choice
        tryVerify(() => row.width < 560, 5000, "a phone's row is under 560 px")
        verify(!main.logReadoutOneLine, "two lines on a phone")
        compare(main.logReadoutText(1000, 104858, main.logReadoutOneLine), "0:01\n0.1 MB")
        main.width = 700
        tryVerify(() => row.width >= 560, 5000, "a wide row")
        verify(main.logReadoutOneLine, "one line where the row has room")
        compare(main.logReadoutText(1000, 104858, main.logReadoutOneLine), "0:01 · 0.1 MB")
        compare(readout.font.pixelSize, Theme.fontBody, "the readout is body text")
        main.width = 559 + (main.width - row.width)
        tryVerify(() => !main.logReadoutOneLine, 5000, "two lines again")
    }

    // The header ends in the one divider the look has, a hairline
    function test_theHeaderEndsInAHairline() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        const rule = find(main.contentItem, "headerRule")
        compare(rule.height, 1)
        compare(rule.color, Theme.hairline)
        verify(rule.visible)
    }

    // The tab bar's labels at a narrow bar (a phone zoomed far in): each
    // stays inside its tab, smaller and elided before it runs into the
    // next
    function test_theTabLabelsStayInsideTheirTabs() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 320
        main.height = 700
        const tabs = find(main.contentItem, "tabBar")
        tryVerify(() => tabs.visible && tabs.height > 0, 2000, "the tab bar shows")
        const pages = [main.dashboardPage, 2, 3, main.recordingsPage, main.settingsPage]
        for (const page of pages)
            compare(find(main.contentItem, "tabLabel_" + page).font.pixelSize, Theme.fontBody,
                    "body text at 64 px a tab")
        Theme.zoom = 2
        waitForRendering(main.contentItem)
        for (const page of pages) {
            const tab = find(main.contentItem, "tab_" + page)
            const label = find(main.contentItem, "tabLabel_" + page)
            verify(tab.width < 64, "a tab at zoom 2: " + tab.width)
            compare(label.font.pixelSize, Theme.fontCaption, "caption text in a narrow tab")
            verify(label.width <= tab.width, label.text + " is " + label.width + " in " + tab.width)
            verify(label.contentWidth <= label.width + 0.5,
                   label.text + " paints " + label.contentWidth + " in " + label.width)
        }
        const settings = find(main.contentItem, "tabLabel_" + main.settingsPage)
        verify(settings.truncated, "Settings elides in a tab of " + settings.width)
    }

    // Other address... then a connect that works returns to the page that
    // sent the user, and the page's arrow goes where Back goes
    function test_aConnectFromOtherAddressReturnsToDash() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        main.gotoPage(main.dashboardPage)
        main.openConnectFromDash()
        compare(main.currentPage, main.settingsPage)
        settings.connectPart.uriField.text = "mock://demo"
        settings.connectFromPart.requestConnect("mock://demo")
        tryCompare(main, "currentPage", main.dashboardPage, 10000, "back on Dash with the gauges")
        verify(!settings.backLeaves)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        // The arrow, with nothing connected
        main.gotoPage(main.dashboardPage)
        main.openConnectFromDash()
        find(find(settings, "settingsConnectionPage"), "settingsBack").clicked()
        compare(main.currentPage, main.dashboardPage, "the arrow does what Back does")
    }

    // The status says Connected (Replaying for a replay) and ends in a
    // chevron where it opens the Log; the Profile row is three segments
    function test_thePhonesStatusAndProfileRow() {
        const main = phoneMain()
        const word = find(main.contentItem, "linkPillText")
        const chevron = find(main.contentItem, "linkPillChevron")
        verify(chevron.visible, "a button, marked")
        compare(Bridge.connectSession("mock://demo"), "")
        tryCompare(word, "text", "Connected", 5000)
        verify(!chevron.visible, "not where it would squeeze a connected phone's address")
        main.width = 600
        waitForRendering(main.contentItem)
        verify(chevron.visible, "with 420 px of header it shows beside the address")
        main.width = 384
        waitForRendering(main.contentItem)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        const row = Bridge.recordingRow(Harness.recordingFixtures + "/findings")
        compare(Bridge.connectSession(row.uri), "")
        tryCompare(word, "text", "Replaying", 5000)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)

        main.openConnect()
        const settings = find(main.contentItem, "settingsPage")
        const segments = find(settings, "profileSegments")
        verify(segments.visible, "the three profiles are one segmented row")
        compare(find(settings, "profile_default").text, "Default")
        compare(find(settings, "profile_raw").text, "Raw")
        compare(find(settings, "profile_uds-generic").text, "UDS DIDs")
        verify(find(settings, "profile_default").selected)
        find(settings, "profile_raw").clicked()
        verify(find(settings, "profile_raw").selected && !find(settings, "profile_default").selected)
        compare(settings.uriPart.profileChoice, "raw")
    }

    // On the tab-bar form the Diagnostics page's title is its tab's word,
    // and Settings' Recording row reads the switch (off, on)
    function test_theCodesPageAndRecordingRowUseTheirShortWords() {
        const main = phoneMain()
        compare(find(main.contentItem, "diagnosticsHeader").title, "Codes", "the tab's word")
        const settings = find(main.contentItem, "settingsPage")
        const row = () => settings.pageList.find(r => r.key === "recording")
        try {
            compare(row().summary, "off")
            Bridge.recording = true
            compare(row().summary, "on")
        } finally {
            Bridge.recording = false
        }
    }

    // A recording's Replay connects it in one tap and lands on Live, the
    // address in Settings' field as a typed one would be
    function test_replayConnectsInOneTapAndLandsOnLive() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 800
        main.gotoPage(main.recordingsPage)
        const row = Bridge.recordingRow(Harness.recordingFixtures + "/findings")
        verify(row.uri.startsWith("replay://"), "a replay address: " + row.uri)
        main.replayRecording(row.uri)
        tryVerify(() => Bridge.connected, 10000, "the replay connects")
        compare(Bridge.currentUri, row.uri)
        tryCompare(main, "currentPage", main.livePage, 2000, "on Live once it connects")
        compare(find(main.contentItem, "settingsPage").connectPart.uriField.text, row.uri, "Settings shows it")
    }

    // A Replay the core refuses (its folder gone) leaves the user on
    // Settings' Connection page with the field's error, never on Live
    function test_aFailedReplayShowsSettingsWithItsReason() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 800
        main.gotoPage(main.recordingsPage)
        const settings = find(main.contentItem, "settingsPage")
        const uri = "replay://" + Harness.recordingFixtures + "/no-such-recording?profile=obd2"
        main.replayRecording(uri)
        tryVerify(() => settings.connectPart.errorLabel.text !== "", 10000, "the refusal arrives")
        verify(!Bridge.connected, "nothing connected")
        compare(main.currentPage, main.settingsPage, "on Settings, not Live")
        verify(settings.connectPart.errorLabel.visible, "the refusal shows")
        compare(settings.shownKey, "connection", "the Connection page")
        compare(settings.connectPart.uriField.text, uri, "the address in the field")
        verify(settings.connectPart.errorLabel.text !== "", "with its reason")
        compare(main.replayPending, "", "nothing left waiting")
        // Back returns to Recordings, as Other address returns to Dash
        verify(main.goBack(), "Back is the app's")
        compare(main.currentPage, main.recordingsPage, "back to Recordings")
    }

    // One primary on Dash: while its connect list shows, the header's
    // Connect... is secondary and the list's Connect the primary; on
    // another page the header's is primary again
    function test_theHeadersConnectGivesWayToDashsList() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 384
        main.height = 832
        const dash = find(main.contentItem, "dashboardPage")
        dash.form = "phone"
        main.gotoPage(main.dashboardPage)
        const header = find(main.contentItem, "disconnectButton")
        tryVerify(() => dash.connectShown, 3000, "Dash shows its connect list")
        verify(header.visible, "the header's Connect... shows")
        verify(!header.highlighted, "secondary on Dash")
        verify(!header.flat, "a secondary, not a quiet one")
        main.gotoPage(main.livePage)
        verify(header.highlighted, "primary elsewhere")
    }

    // Show log beside a refusal and the Log's back arrow return to Connection
    // with Back still going to the page that sent it
    function test_aLogRoundTripKeepsBackGoingToTheSender() {
        const main = phoneMain()
        const settings = find(main.contentItem, "settingsPage")
        main.gotoPage(main.dashboardPage)
        main.openConnectFromDash()
        compare(main.currentPage, main.settingsPage)
        verify(settings.backLeaves, "Back leaves for Dash")
        settings.connectFromPart.requestConnect("mock://demo?profile=no-such-profile")
        tryVerify(() => find(settings, "errorShowLog").visible, 5000, "the refusal")
        settings.logRequested()
        compare(main.currentPage, main.logPage)
        main.logBack()
        compare(main.currentPage, main.settingsPage, "the Log's back arrow")
        verify(settings.backLeaves, "still leaving for Dash")
        verify(main.goBack(), "Back is taken")
        compare(main.currentPage, main.dashboardPage, "where it came from, not the Log")
        verify(!settings.backLeaves, "and the flag is spent")
    }

    // The zoom badge is a small rounded rectangle, not a capsule
    function test_theZoomBadgeIsNotACapsule() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        const badge = find(main.contentItem, "zoomBadge")
        verify(badge.radius <= Theme.radiusCard, "radius " + badge.radius)
    }

    // The desktop's Home is a connect list too, so the header's Connect...
    // is secondary there while no session runs: one primary a page
    function test_theHeadersConnectGivesWayToHomesList() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 1280
        main.height = 720
        const settings = find(main.contentItem, "settingsPage")
        settings.connectFromPart.recentUris = ["mock://demo"]
        main.gotoPage(main.homePage)
        const header = find(main.contentItem, "disconnectButton")
        const list = find(main.contentItem, "homeConnectList")
        tryCompare(list, "chosen", "mock://demo", 2000, "a recent row is chosen")
        verify(header.visible, "the header's Connect... shows")
        verify(!header.highlighted, "secondary on Home's list")
        verify(find(list, "connectGo").highlighted, "the list's Connect is the primary")
        main.gotoPage(main.livePage)
        verify(header.highlighted, "primary elsewhere")
    }

    // The header at the narrowest phone and the top zoom: the pill's word
    // elides inside the pill, and the pill stays inside the page. The
    // header's Connect... passes the edge there (160 layout units hold no
    // button of its words beside a pill), which is the review's open item
    function test_thePillsWordElidesAtTheTopZoom() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 320
        main.height = 800
        main.font.pixelSize = 16
        const pill = find(main.contentItem, "linkPill")
        const word = find(main.contentItem, "linkPillText")
        const rate = find(main.contentItem, "frameRate")
        const was = Theme.developerMode
        Theme.developerMode = true
        try {
            verify(waitForPolish(main), "laid out at 100 %")
            verify(!word.truncated, "at 100 % the word is whole")
            verify(!rate.visible, "the frame rate gives way in a row this narrow")
            main.width = 384
            // The row follows the window a resize event later
            tryVerify(() => rate.visible, 2000, "and shows at 384")
            verify(waitForPolish(main), "laid out at 384")
            verify(rate.x >= word.mapToItem(rate.parent, word.width, 0).x + 8 - 0.5,
                   "apart from the word")
            main.width = 320
        } finally {
            Theme.developerMode = was
        }
        Theme.zoom = 2
        verify(waitForPolish(main), "laid out")
        waitForRendering(main.contentItem)
        verify(word.width <= pill.width + 0.5, "the word inside the pill")
        verify(word.contentWidth <= word.width + 0.5 || word.truncated, "painted inside its box or elided")
        verify(pill.mapToItem(main.contentItem, pill.width, 0).x <= main.contentItem.width + 0.5,
               "the pill inside the page")
        compare(find(main.contentItem, "disconnectButton").visible, true, "Connect... still on the row")
    }

    // The display line says the form and the orientation and no size, scale
    // or inset: Copy and a saved report would carry it to others
    function test_theDisplayLineHoldsNoPixelSize() {
        const main = phoneMain()
        const line = main.displayLine
        verify(line.startsWith("Display: phone form, "), line)
        verify(!/[0-9]/.test(line), "no digit in: " + line)
        compare(line.indexOf("screen"), -1)
        compare(line.indexOf("window"), -1)
        compare(line.indexOf("safe area"), -1)
    }

    function checkedTabs(bar) {
        const on = []
        for (let i = 0; i < bar.count; ++i) {
            if (bar.itemAt(i).checked)
                on.push(bar.itemAt(i).text)
        }
        return on.join()
    }

    // The bar lists five pages and each tab marks its own; the phone opens
    // on Dash. The Log, reached from the status, marks none, and a tap on
    // the tab that was marked still goes there
    function test_theTabBarHasFivePagesAndThePhoneOpensOnDash() {
        const main = phoneMain()
        const bar = find(main.contentItem, "tabBar")
        tryVerify(() => bar.visible, 2000, "the tab bar shows")
        compare(main.currentPage, main.dashboardPage, "a phone opens on Dash")
        const pages = [main.dashboardPage, 2, 3, main.recordingsPage, main.settingsPage]
        const names = ["Dash", "Live", "Codes", "Recs", "Settings"]
        compare(bar.count, names.length)
        for (let i = names.length - 1; i >= 0; --i) {
            const tab = bar.itemAt(i)
            compare(tab.text, names[i])
            mouseClick(tab)
            tryCompare(main, "currentPage", pages[i], 2000, names[i] + "'s page")
            compare(checkedTabs(bar), names[i], "only its own tab is marked")
            verify(find(tab, "tabMark_" + pages[i]).visible, names[i] + "'s mark")
        }
        const pill = find(main.contentItem, "linkPill")
        mouseClick(pill)
        tryCompare(main, "currentPage", main.logPage, 2000, "the status opens the Log")
        compare(checkedTabs(bar), "", "no tab is the Log's")
        mouseClick(bar.itemAt(0))
        tryCompare(main, "currentPage", main.dashboardPage, 2000, "the last tab still answers")
        compare(checkedTabs(bar), "Dash")
    }

    // The mark is the primary fill in Dark and the text at night, where the
    // primary is a dim fill
    function test_theTabMarkIsTheTextAtNight() {
        const main = phoneMain()
        const mark = find(find(main.contentItem, "tabBar"), "tabMark_" + main.dashboardPage)
        verify(Qt.colorEqual(mark.color, Theme.primary), "Dark")
        Theme.night = "on"
        verify(Theme.nightActive)
        verify(Qt.colorEqual(mark.color, Theme.text), "night")
        verify(!Qt.colorEqual(mark.color, Theme.primary), "not the dim fill")
    }

    // Every header button is a touch's size on the glass and draws inside
    // the bar, in each state the header can be in, the address keeping its
    // width beside them at 100 %
    function checkHeaderButtons(main, names) {
        for (const name of names) {
            const button = find(main.contentItem, name)
            verify(button.visible, name + " shows")
            const r = onGlass(button, main)
            verify(r.height >= 48 - 0.5, name + " is " + r.height + " tall on the glass")
            verify(r.width >= 48 - 0.5, name + " is " + r.width + " wide on the glass")
            verify(button.background.height < button.height, name + " draws inside its target")
        }
    }

    function test_theHeadersButtonsAreATouchsSize_data() {
        return [{ tag: "100 %", zoom: 1.0 }, { tag: "67 %", zoom: 0.67 }]
    }

    function test_theHeadersButtonsAreATouchsSize(data) {
        const main = phoneMain()
        Theme.zoom = data.zoom
        verify(waitForPolish(main), "laid out")
        checkHeaderButtons(main, ["disconnectButton"])    // Connect...

        compare(Bridge.connectSession("mock://demo"), "", "connects")
        const start = find(main.contentItem, "startLogButton")
        tryVerify(() => start.visible && start.enabled, 5000, "Start log")
        waitForRendering(main.contentItem)
        checkHeaderButtons(main, ["startLogButton", "disconnectButton"])
        if (data.zoom === 1.0)
            verify(!find(main.contentItem, "linkPillText").truncated, "the word keeps its width; the address gives way first")

        start.clicked()
        verify(Bridge.logActive, "the log runs")
        waitForRendering(main.contentItem)
        checkHeaderButtons(main, ["stopLogButton"])
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")

        Bridge.recording = true
        compare(Bridge.connectSession("mock://marked"), "", "a recorded session connects")
        const mark = find(main.contentItem, "markButton")
        tryVerify(() => mark.visible, 5000, "Mark")
        waitForRendering(main.contentItem)
        checkHeaderButtons(main, ["markButton", "disconnectButton"])
    }

    // On the phone Settings' list is where the core's version is shown
    function test_thePhonesSettingsShowTheCoresVersion() {
        const main = phoneMain()
        main.gotoPage(main.settingsPage)
        const label = find(find(main.contentItem, "settingsPage"), "settingsCoreVersion")
        tryVerify(() => label.visible, 2000, "on Settings' list")
        compare(label.text, "core " + Bridge.coreVersion)
        verify(Bridge.coreVersion !== "", "a version to show")
    }

    // A window narrowed past the sidebar's width leaves Home, which the bar
    // doesn't list; the sidebar's own pages stay
    function test_aNarrowedWindowLeavesHomeForDash() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 1400
        main.height = 800
        verify(waitForPolish(main), "laid out")
        compare(main.currentPage, 0, "the desktop opens on Home")
        main.width = 500
        tryCompare(main, "currentPage", main.dashboardPage, 2000)
        main.width = 1400
        tryVerify(() => !find(main.contentItem, "tabBar").visible, 2000, "the sidebar again")
        compare(main.currentPage, main.dashboardPage, "and the page stays")
    }

    // The status reads the state and the address, the address eliding from
    // its end and the state word never; with the sidebar it is no button
    function test_theStatusReadsTheStateAndTheAddress_data() {
        return [{ tag: "100 %", zoom: 1.0 }, { tag: "67 %", zoom: 0.67 }]
    }

    function test_theStatusReadsTheStateAndTheAddress(data) {
        const main = phoneMain()
        Theme.zoom = data.zoom
        verify(waitForPolish(main), "laid out")
        const state = find(main.contentItem, "linkPillText")
        const address = find(main.contentItem, "linkPillAddress")
        const pill = find(main.contentItem, "linkPill")
        const row = find(main.contentItem, "headerRow")
        compare(state.text, "No session")
        verify(!address.visible, "no address without a session")
        verify(onGlass(pill, main).height >= 48 - 0.5,
               "a touch's size: " + onGlass(pill, main).height)

        compare(Bridge.connectSession("mock://demo"), "", "connects")
        tryCompare(state, "text", "Connected", 5000)
        compare(address.text, "· mock://demo")
        waitForRendering(main.contentItem)
        verify(address.visible && !find(main.contentItem, "linkPillText").truncated,
               "the address shows and the word keeps its width")

        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        const long = "mock://a-long-name-for-an-adapter-that-cannot-fit-beside-the-buttons"
        compare(Bridge.connectSession(long), "", "the long one connects")
        tryCompare(state, "text", "Connected", 5000)
        waitForRendering(main.contentItem)
        verify(address.truncated, "the address elides")
        verify(!state.truncated && state.width >= state.implicitWidth - 0.5, "the word doesn't")
        compare(pastTheEdge(row, main.contentItem, []).length, 0, "the row fits")
    }

    function test_theStatusIsNoButtonBesideTheSidebar() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 1400
        main.height = 800
        verify(waitForPolish(main), "laid out")
        const pill = find(main.contentItem, "linkPill")
        tryVerify(() => !find(main.contentItem, "tabBar").visible, 2000, "the sidebar shows")
        verify(!pill.enabled, "the sidebar lists the Log")
        const before = main.currentPage
        mouseClick(pill)
        wait(0)
        compare(main.currentPage, before, "a tap stays put")
    }

    // The sidebar's page that shows the session's activity is called
    // Activity: "Log" names Start log and a recording
    function test_theSidebarNamesTheActivityPage() {
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        main.width = 1400
        main.height = 800
        verify(waitForPolish(main), "laid out")
        tryVerify(() => !find(main.contentItem, "tabBar").visible, 2000, "the sidebar shows")
        const labels = []
        function walk(item) {
            if (item.label !== undefined && item.active !== undefined)
                labels.push(item.label)
            for (const c of item.children)
                walk(c)
        }
        walk(main.contentItem)
        verify(labels.indexOf("Activity") >= 0, "the sidebar lists Activity: " + labels)
        verify(labels.indexOf("Log") < 0, "no Log entry: " + labels)
    }
}
