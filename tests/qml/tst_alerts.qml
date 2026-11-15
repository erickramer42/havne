// File/layer: tests/qml/tst_alerts.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.alerts (test_qml)

import QtQuick
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "Alerts"
    when: windowShown

    Component {
        id: settingsComponent
        SettingsPage { width: 1000; height: 1400 }
    }
    Component {
        id: dashComponent
        DashboardPage { width: 1280; height: 800 }
    }
    Component { id: mainComponent; Main {} }

    readonly property string none: '{"havne_conditions":1,"conditions":[]}\n'

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        Bridge.unitSystem = 0
        Bridge.recording = false
        verify(Bridge.saveConditions(none).ok, "the case starts with no alert")
    }
    function cleanup() {
        Bridge.unitSystem = 0
        tryVerify(() => !Bridge.connecting, 10000, "a connect in flight ends")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        // Refused while connected; the page, still here, stores it off
        Bridge.recording = false
        Bridge.saveConditions(none)
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }
    // Settings under the window's root, where visible is the page's own
    function settingsOf() {
        const page = createTemporaryObject(settingsComponent, testCase.parent)
        verify(page, "Settings loads")
        page.open("alerts")
        return page
    }
    function save(conditions, extra) {
        const doc = Object.assign({ havne_conditions: 1, conditions: conditions }, extra ?? {})
        const r = Bridge.saveConditions(JSON.stringify(doc))
        verify(r.ok, "saved: " + r.words)
    }
    function stored() {
        return JSON.parse(Bridge.conditionsText())
    }
    function rowOf(page, i) {
        return find(find(page, "settingsAlertsPart"), "alertRow_" + i)
    }
    function wordsOf(row, name) {
        return find(row, name).text
    }
    function rpm(key, name, when, rest) {
        return Object.assign({ key: key, name: name, when: when,
                               alert: { level: "warning", banner: true } }, rest ?? {})
    }
    function editorOf(page) {
        return find(page, "settingsAlertEditor")
    }
    function termOf(editor, i) {
        return find(editor, "alertTerm_" + i)
    }
    // A field's typed text, as typing leaves it
    function type(field, text) {
        field.text = text
        field.textEdited()
    }

    // ---- The list

    // Each row names its alert and what it watches; its state is the
    // session's: nothing offline, ON once judged so, why where it can't be
    // judged, off while switched off
    function test_theListSaysEachAlertsState() {
        save([rpm("always", "Always on", { signal: ["id:1001"], unit: "RPM", below: 100000 }),
              rpm("off", "Switched off", { signal: ["id:1001"], unit: "RPM", above: 9000 },
                  { enabled: false }),
              rpm("missing", "Not in the mock", { signal: ["id:7005"], unit: "kPa", above: 300 })])
        const page = settingsOf()
        page.uriField.text = "mock://demo"
        const alerts = find(page, "settingsAlertsPart")
        alerts.refreshRows()
        compare(wordsOf(rowOf(page, 0), "alertName"), "Always on")
        verify(wordsOf(rowOf(page, 0), "alertSummary").indexOf("RPM below 100") === 0,
               wordsOf(rowOf(page, 0), "alertSummary"))
        compare(wordsOf(rowOf(page, 0), "alertLevel"), "Warning")
        compare(wordsOf(rowOf(page, 0), "alertState"), "", "offline, nothing judges it")
        compare(wordsOf(rowOf(page, 1), "alertState"), "off")
        verify(!find(rowOf(page, 1), "alertEnabled_1").checked, "its box reads off")
        compare(wordsOf(rowOf(page, 2), "alertState"), "")

        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        tryCompare(find(rowOf(page, 0), "alertState"), "text", "ON", 5000)
        compare(wordsOf(rowOf(page, 1), "alertState"), "off")
        tryVerify(() => wordsOf(rowOf(page, 2), "alertState").indexOf("not available: ") === 0, 5000,
                  "a term on a signal the session lacks says why")
        compare(alerts.stateWords({ key: "a" }, { live: true, stale: true, unavailable: false,
                                                  state: "on", why: "" }),
                "stale", "a stale term says so before its state")
    }

    // A term on a signal the listed profile lacks says so in words, in the
    // list and the editor; its reference is the description a screen
    // reader gives, and a signal the profile has keeps its name alone
    function test_aSignalNotInTheProfileIsSaidInWords() {
        save([rpm("missing", "Not in the mock", { signal: ["key:not_here"], unit: "kPa", above: 300 }),
              rpm("high", "High revs", { signal: ["id:1001"], unit: "RPM", above: 6000 })])
        const page = settingsOf()
        page.uriField.text = "mock://demo"
        find(page, "settingsAlertsPart").refreshRows()
        const row = rowOf(page, 0)
        compare(wordsOf(row, "alertSummary"), "a signal not in this profile above 300 kPa")
        compare(row.Accessible.description, "key:not_here")
        compare(wordsOf(rowOf(page, 1), "alertSummary"), "RPM above 6000 RPM")
        compare(rowOf(page, 1).Accessible.description, "")
        row.clicked()
        const term = find(termOf(editorOf(page), 0), "alertTermSignal")
        compare(term.text, "a signal not in this profile")
        compare(term.Accessible.description, "key:not_here")
    }

    // Counts read one or many, never "(s)": Settings' Alerts row counts
    // conditions, a record-only one too, and Import's question and note
    // count the alerts they replace and read
    function test_countsReadOneOrMany() {
        save([rpm("high", "High revs", { signal: ["id:1001"], unit: "RPM", above: 6000 })])
        const page = settingsOf()
        const row = () => page.pageList.find(r => r.key === "alerts")
        compare(row().summary, "1 condition")
        save([rpm("high", "High revs", { signal: ["id:1001"], unit: "RPM", above: 6000 }),
              { key: "rec", name: "Record only", when: { signal: ["id:1001"], unit: "RPM", above: 5000 },
                record: { after: 2 } }])
        compare(row().summary, "2 conditions", "a record-only condition counts")
        const alerts = find(page, "settingsAlertsPart")
        alerts.confirming = "import"
        compare(find(alerts, "alertConfirmText").text, "Import replaces the 2 saved alerts with the file's.")
        alerts.confirming = ""
        const file = Harness.writeScratchFile("alerts-two.json", JSON.stringify(
            { havne_conditions: 1, conditions: [rpm("a", "A", { link: "not_ok" }), rpm("b", "B", { link: "timeout" })] }))
        verify(alerts.importFrom(file), find(alerts, "alertProblem").text)
        compare(find(alerts, "alertSaveNote").text, "Imported 2 alerts.")
    }

    // In imperial the list says a level in its signal's own decimals, and
    // the editor keeps three, so a metric level left as it showed is saved
    // back as it was
    function test_anImperialLevelReadsInItsSignalsDecimals() {
        save([rpm("fast", "Fast", { signal: ["id:3001"], unit: "km/h", above: 20 })])
        Bridge.unitSystem = 1
        const page = settingsOf()
        page.uriField.text = "mock://demo"
        const alerts = find(page, "settingsAlertsPart")
        alerts.refreshRows()
        const speed = alerts.rows.find(r => r.ref === "id:3001")
        verify(speed !== undefined, "the mock lists its speed")
        compare(speed.decimals, 0, "the speed's own decimals")
        const summary = wordsOf(rowOf(page, 0), "alertSummary")
        verify(summary.endsWith(" above 12 mph"), summary)
        rowOf(page, 0).clicked()
        const term = termOf(editorOf(page), 0)
        compare(term.levelField.text, "12" + Qt.locale().decimalPoint + "427", "the field keeps three")
        verify(!editorOf(page).changed(), "untouched, it reads unchanged")
        find(editorOf(page), "alertLevel_danger").clicked()
        find(editorOf(page), "alertSave").clicked()
        const c = stored().conditions[0]
        verify(c.when.above === 20, String(c.when.above))
        compare(c.when.unit, "km/h")
    }

    // ---- The editor

    // A row opens its alert; a change saved goes into the file through
    // core, every key the page doesn't know kept, and Back returns to the
    // list, which reads the file again; the row's box writes enabled alone
    function test_anEditRoundTripsThroughTheStore() {
        save([rpm("high", "High revs", { signal: ["id:1001"], unit: "RPM", above: 6000, clear: 5800 },
                  { notes: "kept", "for": 0.5 })], { "x-made-by": "a test" })
        const page = settingsOf()
        rowOf(page, 0).clicked()
        compare(page.pageKey, "alertedit", "the row opens its editor")
        const editor = editorOf(page)
        compare(editor.nameField.text, "High revs")
        const term = termOf(editor, 0)
        compare(term.levelField.text, "6000")
        compare(term.clearField.text, "5800")
        compare(editor.forField.text, "0" + Qt.locale().decimalPoint + "5")
        editor.nameField.text = "Over 6500"
        type(term.levelField, "6500")
        find(editor, "alertLevel_danger").clicked()
        editor.soundBox.checked = true
        find(editor, "alertSave").clicked()
        compare(page.pageKey, "alerts", "a save goes back to the list")
        const c = stored().conditions[0]
        compare(c.key, "high", "the key stays")
        compare(c.name, "Over 6500")
        compare(c.when.above, 6500)
        compare(c.when.clear, 5800, "an untouched clear keeps its value")
        compare(c["for"], 0.5)
        compare(c.alert.level, "danger")
        compare(c.alert.sound, true)
        compare(c.alert.banner, true)
        compare(c.notes, "kept", "a key the page doesn't know stays")
        compare(stored()["x-made-by"], "a test")
        compare(wordsOf(rowOf(page, 0), "alertName"), "Over 6500")

        const box = find(rowOf(page, 0), "alertEnabled_0")
        box.toggle()
        box.toggled()
        compare(stored().conditions[0].enabled, false, "off writes false")
        box.toggle()
        box.toggled()
        verify(!("enabled" in stored().conditions[0]), "on is the default: the key goes")
    }

    // A number of seconds past the three decimals shown keeps its value
    // while its field reads as it opened: the editor reads unchanged, a
    // Save of something else leaves it, and a field typed in takes the text
    function test_anUntouchedSecondsFieldKeepsItsStoredValue() {
        save([rpm("high", "High revs", { signal: ["id:1001"], unit: "RPM", above: 6000 },
                  { "for": 0.1234, clear_for: 2.00005, record: { before: 1.0001, after: 3.33333 } })])
        const page = settingsOf()
        rowOf(page, 0).clicked()
        const editor = editorOf(page)
        const point = Qt.locale().decimalPoint
        compare(editor.forField.text, "0" + point + "123", "three decimals shown")
        compare(editor.afterField.text, "3" + point + "333")
        verify(!editor.changed(), "an editor left as it opened reads unchanged")
        find(editor, "alertLevel_danger").clicked()
        find(editor, "alertSave").clicked()
        compare(page.pageKey, "alerts", find(editor, "alertEditorProblem").text)
        let c = stored().conditions[0]
        compare(c.alert.level, "danger")
        verify(c["for"] === 0.1234, String(c["for"]))
        verify(c.clear_for === 2.00005, String(c.clear_for))
        verify(c.record.before === 1.0001, String(c.record.before))
        verify(c.record.after === 3.33333, String(c.record.after))

        rowOf(page, 0).clicked()
        type(editor.forField, "0" + point + "5")
        verify(editor.changed(), "a field typed in is a change")
        find(editor, "alertSave").clicked()
        c = stored().conditions[0]
        verify(c["for"] === 0.5, String(c["for"]))
        verify(c.record.after === 3.33333, String(c.record.after))
    }

    // A group's own keys beside its terms (notes, x-) stay through an
    // edit: the editor reads unchanged as it opened, and a Save with
    // another join keeps each in its place
    function test_aGroupsOwnKeysStayThroughAnEdit() {
        save([rpm("both", "Both high", { notes: "why both", all: [
                  { signal: ["id:1001"], unit: "RPM", above: 6000 },
                  { signal: ["id:2001"], unit: "°C", above: 110 }], "x-tool": { v: 1 } })])
        const page = settingsOf()
        rowOf(page, 0).clicked()
        const editor = editorOf(page)
        verify(!editor.changed(), "an editor left as it opened reads unchanged")
        find(editor, "alertJoin_any").clicked()
        find(editor, "alertSave").clicked()
        compare(page.pageKey, "alerts", find(editor, "alertEditorProblem").text)
        const w = stored().conditions[0].when
        compare(Object.keys(w), ["notes", "any", "x-tool"], "each key in its place")
        compare(w.notes, "why both")
        compare(w["x-tool"].v, 1)
        compare(w.any.length, 2)
    }

    // The sound switch is live and says where it plays
    function test_theSoundSaysItPlaysOnAndroid() {
        save([rpm("high", "High revs", { signal: ["id:1001"], unit: "RPM", above: 6000 })])
        const page = settingsOf()
        rowOf(page, 0).clicked()
        const editor = editorOf(page)
        verify(editor.soundBox.enabled, "the switch is live")
        compare(find(editor, "alertSoundNote").text, "Sound plays on Android.")
    }

    // A level typed in the display unit is stored in the def's: psi in,
    // kPa in the file to six decimals, as the Dashboard's zones, psi
    // shown again
    function test_aLevelTypedInPsiIsStoredInKpaAndShownBack() {
        save([rpm("boost", "Boost", { signal: ["id:11001"], unit: "kPa", above: 200, clear: 190 })])
        Bridge.unitSystem = 1
        const page = settingsOf()
        rowOf(page, 0).clicked()
        const editor = editorOf(page)
        const term = termOf(editor, 0)
        const point = Qt.locale().decimalPoint
        compare(term.levelField.text, "29" + point + "008", "200 kPa in psi")
        compare(find(term, "alertLevelUnit").text, "psi")
        type(term.levelField, "30")
        find(editor, "alertSave").clicked()
        const c = stored().conditions[0]
        compare(c.when.unit, "kPa", "the file keeps the def's unit")
        // Exactly: compare() passes numbers within 1e-5
        verify(c.when.above === 206.842719, "30 psi in kPa, six decimals: " + c.when.above)
        compare(c.when.above, DashboardBook.roundStored(Bridge.decodedValue("kPa", 30)),
                "the Dashboard's rounding")
        compare(c.when.clear, 190, "the clear left as it was")
        rowOf(page, 0).clicked()
        compare(termOf(editorOf(page), 0).levelField.text, "30", "shown back in psi")
        verify(wordsOf(rowOf(page, 0), "alertSummary").indexOf("psi") > 0,
               wordsOf(rowOf(page, 0), "alertSummary"))
    }

    // A field that isn't a number is the page's to say; a condition core
    // refuses is core's, without the line and column of text the page
    // made; neither changes the saved file
    function test_aRefusedEditSaysWhyAndSavesNothing() {
        save([rpm("high", "High revs", { signal: ["id:1001"], unit: "RPM", above: 6000 })])
        const before = Bridge.conditionsText()
        const page = settingsOf()
        rowOf(page, 0).clicked()
        const editor = editorOf(page)
        type(termOf(editor, 0).levelField, "fast")
        find(editor, "alertSave").clicked()
        compare(find(editor, "alertEditorProblem").text, "The level of term 1 isn't a number.")
        compare(page.pageKey, "alertedit", "the editor stays")
        type(termOf(editor, 0).levelField, "6000")
        editor.nameField.text = ""
        find(editor, "alertSave").clicked()
        const words = find(editor, "alertEditorProblem").text
        verify(words !== "", "core says why")
        verify(!/^line \d+, column \d+/.test(words), words)
        compare(page.pageKey, "alertedit")
        compare(Bridge.conditionsText(), before, "the saved file stays")
    }

    // Remove asks first; Keep it keeps it
    function test_removeAsksFirst() {
        save([rpm("a", "First", { signal: ["id:1001"], unit: "RPM", above: 6000 }),
              rpm("b", "Second", { signal: ["id:1001"], unit: "RPM", above: 7000 })])
        const page = settingsOf()
        rowOf(page, 0).clicked()
        const editor = editorOf(page)
        find(editor, "alertRemove").clicked()
        compare(find(editor, "alertRemoveText").text, "Remove First?")
        find(editor, "alertRemoveNo").clicked()
        compare(stored().conditions.length, 2)
        find(editor, "alertRemove").clicked()
        find(editor, "alertRemoveYes").clicked()
        compare(stored().conditions.map(c => c.key), ["b"])
        compare(page.pageKey, "alerts")
    }

    // A link term is offered beside a signal's, and Add a term joins them
    function test_aLinkTermJoinsASignalsUnderAny() {
        const page = settingsOf()
        find(page, "alertAdd").clicked()
        const editor = editorOf(page)
        compare(editor.nameField.text, "Alert 1")
        verify(editor.isNew)
        const signalTerm = termOf(editor, 0)
        page.uriField.text = "mock://demo"
        find(signalTerm, "alertTermSignal").clicked()
        find(signalTerm, "alertPick_id:1001").clicked()
        type(signalTerm.levelField, "6000")
        find(editor, "alertTermAdd").clicked()
        const linkTerm = termOf(editor, 1)
        find(linkTerm, "alertTermKind_link").clicked()
        find(linkTerm, "alertLink_timeout").clicked()
        compare(find(linkTerm, "alertTermRemove").text, "Remove term 2", "it names its term")
        compare(signalTerm.levelField.Accessible.name, "Level of term 1, RPM")
        compare(signalTerm.clearField.Accessible.name, "Clear level of term 1, RPM")
        find(editor, "alertJoin_any").clicked()
        find(editor, "alertSave").clicked()
        const c = stored().conditions[0]
        compare(c.key, "alert_1")
        compare(c.when.any.length, 2)
        compare(c.when.any[0].signal[0], "id:1001")
        compare(c.when.any[0].above, 6000)
        compare(c.when.any[1], { link: "timeout" })
    }

    // ---- Import and export

    // Import asks before it replaces saved alerts; a file core refuses says
    // so and leaves the saved one; one it takes replaces it
    function test_importAsksAndARefusedFileLeavesTheSavedOne() {
        save([rpm("high", "High revs", { signal: ["id:1001"], unit: "RPM", above: 6000 })])
        const before = Bridge.conditionsText()
        const page = settingsOf()
        const alerts = find(page, "settingsAlertsPart")
        find(alerts, "alertImport").clicked()
        verify(find(alerts, "alertConfirm").visible, "Import asks first")
        verify(find(alerts, "alertConfirmText").text.indexOf("replaces the 1 saved alert") >= 0,
               find(alerts, "alertConfirmText").text)
        find(alerts, "alertConfirmNo").clicked()
        verify(!find(alerts, "alertConfirm").visible)

        const refused = Harness.writeScratchFile("alerts-refused.json",
            '{"havne_conditions":1,"conditions":[{"key":"a","name":"A","when":'
            + '{"signal":["id:1001"],"unit":"RPM","abov":3}}]}')
        verify(!alerts.importFrom(refused))
        verify(find(alerts, "alertProblem").text.indexOf("abov") >= 0, find(alerts, "alertProblem").text)
        compare(Bridge.conditionsText(), before, "the saved file stays")
        compare(wordsOf(rowOf(page, 0), "alertName"), "High revs")

        const taken = Harness.writeScratchFile("alerts-taken.json", JSON.stringify(
            { havne_conditions: 1, conditions: [
                rpm("one", "One", { signal: ["id:1001"], unit: "RPM", above: 1 }),
                rpm("two", "Two", { link: "not_ok" })] }))
        verify(alerts.importFrom(taken))
        compare(find(alerts, "alertProblem").text, "")
        verify(find(alerts, "alertSaveNote").text.indexOf("Imported 2") === 0,
               find(alerts, "alertSaveNote").text)
        compare(stored().conditions.map(c => c.key), ["one", "two"])
        compare(wordsOf(rowOf(page, 1), "alertSummary"), "the link isn't OK")
    }

    // A saved file core refuses at the start reads as none, JSON or not: no
    // row, no Add, no gauge's draft; Import asks, Export writes the file as
    // it is, Start again asks and replaces it
    function test_aSavedFileCoreRefusedOffersStartAgainAndExportsAsItIs_data() {
        return [{ tag: "valid JSON",
                  text: '{"havne_conditions":1,"conditions":[{"key":"a","name":"A","when":'
                        + '{"signal":["id:1001"],"unit":"RPM","abov":3}}]}' },
                { tag: "not JSON", text: "not json" }]
    }
    function test_aSavedFileCoreRefusedOffersStartAgainAndExportsAsItIs(data) {
        verify(Harness.writeSavedConditions(data.text), "the saved file written")
        verify(Bridge.conditionsProblem.indexOf("The saved conditions weren't read: ") === 0,
               Bridge.conditionsProblem)
        const page = settingsOf()
        const alerts = find(page, "settingsAlertsPart")
        verify(alerts.unreadable)
        verify(!findChild(alerts, "alertRow_0"), "no row")
        verify(find(alerts, "alertsSavedProblem").visible)
        verify(!find(alerts, "alertsNone").visible)
        verify(!find(alerts, "alertAdd").enabled)
        verify(!find(alerts, "alertAddFromSignal").enabled)
        const why = page.openAlertDraft(ConditionBook.alertFromLevels("Coolant", ["id:2001"], "°C",
                                                                      { dangerFrom: 115 }, 0, 150, []))
        verify(why.indexOf("don't load") >= 0, why)
        compare(page.pageKey, "alerts", "no editor")

        verify(find(alerts, "alertExport").enabled, "Export writes the refused file")
        const url = Harness.writeScratchFile("alerts-export-refused.json", "")
        verify(alerts.exportTo(url), find(alerts, "alertProblem").text)
        compare(Harness.readScratchFile(url), data.text)

        find(alerts, "alertImport").clicked()
        compare(find(alerts, "alertConfirmText").text,
                "Import replaces the saved file, which doesn't load, with the file's. A file that couldn't be read is kept beside it.")
        find(alerts, "alertConfirmNo").clicked()
        find(alerts, "alertRestart").clicked()
        verify(find(alerts, "alertConfirm").visible)
        find(alerts, "alertConfirmYes").clicked()
        verify(!alerts.unreadable, "started again")
        compare(Bridge.conditionsProblem, "")
        compare(stored().conditions, [])
        verify(find(alerts, "alertAdd").enabled)
    }

    // Add names its alert past the names saved, as core takes a name once
    function test_addNamesPastTheNamesTaken() {
        save([rpm("alert_2", "Alert 2", { signal: ["id:1001"], unit: "RPM", above: 6000 })])
        const page = settingsOf()
        find(page, "alertAdd").clicked()
        const editor = editorOf(page)
        compare(editor.nameField.text, "Alert 1", "the first free number")
        page.uriField.text = "mock://demo"
        find(termOf(editor, 0), "alertTermSignal").clicked()
        find(termOf(editor, 0), "alertPick_id:1001").clicked()
        type(termOf(editor, 0).levelField, "7000")
        find(editor, "alertSave").clicked()
        compare(page.pageKey, "alerts", find(editor, "alertEditorProblem").text)
        find(page, "alertAdd").clicked()
        compare(editorOf(page).nameField.text, "Alert 3")
        find(editorOf(page), "alertCancel").clicked()
        compare(stored().conditions.map(c => c.name), ["Alert 2", "Alert 1"])
    }

    // A name is counted in UTF-8 bytes, as core counts it: a gauge's long
    // Norwegian label is cut to whole letters that fit and saves; a name
    // typed past the bytes is the editor's to refuse
    function test_aNameIsCutAndCheckedInBytes() {
        compare(ConditionBook.nameBytes("Kjøl"), 5)
        const label = "ø".repeat(30) + " temp"
        const c = ConditionBook.alertFromLevels(label, ["id:2001"], "°C", { dangerFrom: 115 }, 0, 150, [])
        compare(c.name, "ø".repeat(20), "40 bytes of whole letters")
        compare(ConditionBook.newName(label, [c.name]), "ø".repeat(19) + " 2")
        const page = settingsOf()
        compare(page.openAlertDraft(c), "")
        find(editorOf(page), "alertSave").clicked()
        compare(page.pageKey, "alerts", find(editorOf(page), "alertEditorProblem").text)
        compare(stored().conditions[0].name, "ø".repeat(20))

        rowOf(page, 0).clicked()
        const editor = editorOf(page)
        editor.nameField.text = "æ".repeat(21)
        find(editor, "alertSave").clicked()
        compare(find(editor, "alertEditorProblem").text,
                "The name is too long: it takes 42 bytes, 40 fit, and a letter outside A to Z takes two or more.")
        compare(stored().conditions[0].name, "ø".repeat(20), "the saved file stays")
    }

    // A key is the name in core's alphabet, numbered past the keys taken
    function test_aNewKeyIsTheNameNumberedPastTheTaken() {
        compare(ConditionBook.newKey("High revs!", []), "high_revs")
        compare(ConditionBook.newKey("High revs", ["high_revs"]), "high_revs_2")
        compare(ConditionBook.newKey("High revs", ["high_revs", "high_revs_2"]), "high_revs_3")
        compare(ConditionBook.newKey("Kjølevæske", []), "kj_lev_ske")
        compare(ConditionBook.newKey("", []), "alert")
        const long = "x".repeat(40)
        compare(ConditionBook.newKey(long, []), "x".repeat(31))
        compare(ConditionBook.newKey(long, ["x".repeat(31)]), "x".repeat(29) + "_2")
    }

    // ---- From a gauge's levels

    // Add from a signal's levels takes a gauge key's zone table: coolant's
    // danger level as above, at danger, clearing inside it
    function test_addFromAGaugeKeysLevels() {
        const page = settingsOf()
        page.uriField.text = "mock://demo"
        const alerts = find(page, "settingsAlertsPart")
        find(alerts, "alertAddFromSignal").clicked()
        verify(find(alerts, "alertSignalPanel").visible)
        const coolant = Bridge.gaugeSignals.coolant[0]
        find(alerts, "alertPick_" + coolant).clicked()
        compare(page.pageKey, "alertedit")
        const editor = editorOf(page)
        const draft = alerts.draft
        compare(draft.when.signal[0], coolant)
        compare(draft.when.above, 115, "coolant's danger level")
        fuzzyCompare(draft.when.clear, 112, 1e-9, "it clears 2 % of 0 to 150 inside")
        compare(draft.alert.level, "danger")
        compare(editor.nameField.text, "Coolant Temp")
        find(editor, "alertSave").clicked()
        compare(stored().conditions[0].when.above, 115)
    }

    // The alert a gauge's levels give: its danger above first, then its
    // warning above, then the below ones, danger first; none without one
    function test_theLevelsGiveTheirAlert_data() {
        return [
            { tag: "danger above", levels: { warnFrom: 105, dangerFrom: 115, warnBelow: 20 },
              comparison: "above", at: 115, level: "danger", clear: 112 },
            { tag: "warning above", levels: { warnFrom: 90, dangerBelow: 5 },
              comparison: "above", at: 90, level: "warning", clear: 87 },
            { tag: "danger below", levels: { warnBelow: 12.4, dangerBelow: 11.8 },
              comparison: "below", at: 11.8, level: "danger", clear: 14.8 },
            { tag: "warning below", levels: { warnBelow: 20 },
              comparison: "below", at: 20, level: "warning", clear: 23 },
            { tag: "none", levels: { coldBelow: 60 }, comparison: "" }]
    }
    function test_theLevelsGiveTheirAlert(data) {
        const c = ConditionBook.alertFromLevels("Gauge", ["id:2001"], "°C", data.levels, 0, 150, ["gauge"])
        if (data.comparison === "") {
            compare(c, null)
            return
        }
        compare(c.key, "gauge_2", "a key the set doesn't hold")
        compare(c.name, "Gauge")
        compare(ConditionBook.comparisonOf(c.when), data.comparison)
        compare(c.when[data.comparison], data.at)
        fuzzyCompare(c.when.clear, data.clear, 1e-9)
        compare(c.alert.level, data.level)
        compare(c.alert.banner, true)
        verify(!("for" in c), "nothing is held")
    }

    // A cell's Alert reads its own levels, refs and unit; a cell with no
    // level, and a blank, has none
    function test_aCellsAlertIsItsOwnLevels_data() {
        return [
            { tag: "danger above", options: { from: 0, to: 150, warnFrom: 105, dangerFrom: 115 },
              comparison: "above", at: 115, level: "danger" },
            { tag: "warning above", options: { from: 0, to: 150, warnFrom: 105 },
              comparison: "above", at: 105, level: "warning" },
            { tag: "danger below", options: { from: 0, to: 150, warnBelow: 30, dangerBelow: 10 },
              comparison: "below", at: 10, level: "danger" },
            { tag: "warning below", options: { from: 0, to: 150, warnBelow: 30 },
              comparison: "below", at: 30, level: "warning" },
            { tag: "no level", options: { from: 0, to: 150, coldBelow: 60 }, comparison: "" },
            { tag: "blank", kind: "blank", options: {}, comparison: "" },
            { tag: "shift lights", kind: "shift-light", options: { from: 0, to: 150, dangerFrom: 115 },
              comparison: "" }]
    }
    function test_aCellsAlertIsItsOwnLevels(data) {
        const doc = DashboardBook.builtIn("desktop")
        const kind = data.kind ?? "digital"
        doc.pages = [{ name: "Test", cells: [{ kind: kind, w: 2, h: 4, label: "Coolant",
                                               refs: kind === "blank" ? [] : ["id:2001"],
                                               unit: kind === "blank" ? "" : "°C",
                                               options: data.options }] }]
        const page = createTemporaryObject(dashComponent, testCase.Window.window.contentItem)
        page.doc = doc
        const asked = []
        page.alertRequested.connect(c => asked.push(c))
        page.startEditing()
        page.select(0)
        const button = find(page, "panelAlert")
        if (data.comparison === "") {
            verify(!button.visible, "no Alert")
            return
        }
        verify(button.visible)
        button.clicked()
        compare(asked.length, 1)
        const c = asked[0]
        compare(c.name, "Coolant")
        compare(c.when.signal, ["id:2001"])
        compare(c.when.unit, "°C")
        compare(c.when[data.comparison], data.at)
        compare(c.alert.level, data.level)
    }

    // The draft a gauge opens is named and keyed among the saved alerts, a
    // copy the gauge no longer reaches, and saved only by the editor's
    // Save: a second Alert from the same gauge saves too
    function test_aGaugesDraftIsKeyedAmongTheSavedAndSavedOnlyBySave() {
        save([rpm("coolant", "Coolant", { signal: ["id:2001"], unit: "°C", above: 100 })])
        const page = settingsOf()
        const draft = ConditionBook.alertFromLevels("Coolant", ["id:2001"], "°C",
                                                    { dangerFrom: 115 }, 0, 150, [])
        compare(page.openAlertDraft(draft), "")
        compare(page.pageKey, "alertedit")
        compare([draft.key, draft.name], ["coolant", "Coolant"], "the caller's object is left as it was")
        const alerts = find(page, "settingsAlertsPart")
        compare(alerts.draft.key, "coolant_2")
        compare(alerts.draft.name, "Coolant 2")
        draft.when.above = 1
        compare(alerts.draft.when.above, 115, "the draft is a copy")
        compare(termOf(editorOf(page), 0).levelField.text, "115")
        compare(stored().conditions.length, 1, "nothing saved yet")
        find(editorOf(page), "alertCancel").clicked()
        compare(page.pageKey, "alerts")
        compare(stored().conditions.length, 1)

        for (const name of ["Coolant 2", "Coolant 3"]) {
            compare(page.openAlertDraft(ConditionBook.alertFromLevels("Coolant", ["id:2001"], "°C",
                                                                      { dangerFrom: 115 }, 0, 150, [])), "")
            compare(editorOf(page).nameField.text, name)
            find(editorOf(page), "alertSave").clicked()
            compare(page.pageKey, "alerts", find(editorOf(page), "alertEditorProblem").text)
        }
        compare(stored().conditions.map(c => c.name), ["Coolant", "Coolant 2", "Coolant 3"])
    }

    // A gauge's Alert leaves an edit waiting in the editor and says why; an
    // editor with nothing changed gives way; a full file says so
    function test_aGaugesDraftNeverReplacesAWaitingEdit() {
        // The comparison before the signal: an editor left as it opened
        // builds the same text
        save([{ key: "high", name: "High revs", when: { above: 6000, signal: ["id:1001"], unit: "RPM" },
                alert: { level: "warning", banner: true } }])
        const page = settingsOf()
        const gauge = () => ConditionBook.alertFromLevels("Coolant", ["id:2001"], "°C",
                                                          { dangerFrom: 115 }, 0, 150, [])
        rowOf(page, 0).clicked()
        compare(page.openAlertDraft(gauge()), "", "nothing changed: the draft takes its place")
        compare(editorOf(page).nameField.text, "Coolant")
        find(editorOf(page), "alertCancel").clicked()

        rowOf(page, 0).clicked()
        type(termOf(editorOf(page), 0).levelField, "6500")
        const why = page.openAlertDraft(gauge())
        compare(why, "An alert's edit waits in Settings > Alerts: save or cancel it first.")
        compare(editorOf(page).nameField.text, "High revs", "the edit stays")
        compare(termOf(editorOf(page), 0).levelField.text, "6500")
        find(editorOf(page), "alertCancel").clicked()

        const full = []
        for (let i = 0; i < ConditionBook.maxConditions; ++i)
            full.push(rpm("a" + i, "A" + i, { signal: ["id:1001"], unit: "RPM", above: 6000 }))
        save(full)
        verify(page.openAlertDraft(gauge()).indexOf("holds 64 alerts") >= 0)
        compare(page.pageKey, "alerts")
    }

    // A refusal in the editor stays the editor's, and the list's own goes
    // when an editor opens: Cancel goes back to a list with no refusal on it
    function test_anEditorsRefusalIsNotLeftOnTheList() {
        save([rpm("high", "High revs", { signal: ["id:1001"], unit: "RPM", above: 6000 })])
        const page = settingsOf()
        const list = find(page, "settingsAlertsPart")
        verify(!list.importFrom(Harness.writeScratchFile("alerts-not-json.json", "not json")))
        verify(find(list, "alertProblem").text !== "", "the list's refusal")
        rowOf(page, 0).clicked()
        find(editorOf(page), "alertCancel").clicked()
        compare(find(list, "alertProblem").text, "", "gone when the editor opened")
        rowOf(page, 0).clicked()
        editorOf(page).nameField.text = ""
        find(editorOf(page), "alertSave").clicked()
        verify(find(editorOf(page), "alertEditorProblem").text !== "", "core says why")
        find(editorOf(page), "alertCancel").clicked()
        compare(page.pageKey, "alerts")
        compare(find(find(page, "settingsAlertsPart"), "alertProblem").text, "")
        verify(!find(find(page, "settingsAlertsPart"), "alertProblem").visible)
    }

    // ---- With a session

    // A save and an import under a session are judged at once: the new
    // set's rows read their state
    function test_aSaveAndAnImportUnderASessionAreJudgedAtOnce() {
        save([rpm("high", "High revs", { signal: ["id:1001"], unit: "RPM", above: 100000 })])
        const page = settingsOf()
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        tryVerify(() => Bridge.connected, 5000)
        rowOf(page, 0).clicked()
        const term = termOf(editorOf(page), 0)
        find(term, "alertComparison_below").clicked()
        type(term.levelField, "100000")
        type(term.clearField, "")
        find(editorOf(page), "alertSave").clicked()
        compare(page.pageKey, "alerts", find(editorOf(page), "alertEditorProblem").text)
        tryCompare(find(rowOf(page, 0), "alertState"), "text", "ON", 5000)
        compare(rowOf(page, 0).Accessible.name, "Edit High revs, ON", "the row is read with its state")
        compare(Bridge.conditionsPending, "")

        const alerts = find(page, "settingsAlertsPart")
        const file = Harness.writeScratchFile("alerts-session.json", JSON.stringify(
            { havne_conditions: 1, conditions: [
                rpm("other", "Other", { signal: ["id:1001"], unit: "RPM", below: 100000 })] }))
        verify(alerts.importFrom(file), find(alerts, "alertProblem").text)
        compare(find(alerts, "alertSaveNote").text, "Imported 1 alert.")
        tryCompare(find(rowOf(page, 0), "alertState"), "text", "ON", 5000)
        compare(wordsOf(rowOf(page, 0), "alertName"), "Other")
    }

    // Under a recording the set a session judges is held: a save and an
    // import say the change waits for its end
    function test_aSaveAndAnImportUnderARecordingWaitForItsEnd() {
        save([rpm("high", "High revs", { signal: ["id:1001"], unit: "RPM", above: 6000 })])
        const page = settingsOf()
        Bridge.recording = true
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        tryVerify(() => Bridge.recordingActive, 5000, "the session is recorded")
        const alerts = find(page, "settingsAlertsPart")
        rowOf(page, 0).clicked()
        type(termOf(editorOf(page), 0).levelField, "6500")
        find(editorOf(page), "alertSave").clicked()
        compare(page.pageKey, "alerts")
        compare(find(alerts, "alertSaveNote").text, "Saved: the change applies when this recording ends.")
        verify(find(alerts, "alertsPending").visible)
        compare(find(alerts, "alertsPending").text, "Your last change applies when this recording ends.")

        const file = Harness.writeScratchFile("alerts-recording.json", JSON.stringify(
            { havne_conditions: 1, conditions: [rpm("one", "One", { link: "not_ok" })] }))
        verify(alerts.importFrom(file), find(alerts, "alertProblem").text)
        compare(find(alerts, "alertSaveNote").text,
                "Imported 1 alert: the change applies when this recording ends.")
        compare(stored().conditions.map(c => c.key), ["one"], "the file is replaced at once")
    }

    // A gauge with no ends of its own clears inside its unit's default
    // range, as the cell draws it
    function test_aGaugesAlertWithNoRangeClearsInsideItsUnitsDefault() {
        const doc = DashboardBook.builtIn("desktop")
        doc.pages = [{ name: "Test", cells: [{ kind: "digital", w: 2, h: 4, label: "Coolant",
                                               refs: ["id:2001"], unit: "°C",
                                               options: { dangerFrom: 115 } }] }]
        const page = createTemporaryObject(dashComponent, testCase.Window.window.contentItem)
        page.doc = doc
        const asked = []
        page.alertRequested.connect(c => asked.push(c))
        page.startEditing()
        page.select(0)
        find(page, "panelAlert").clicked()
        const range = DashboardBook.defaultRange("°C")
        fuzzyCompare(asked[0].when.clear, 115 - ConditionBook.clearShare * (range.to - range.from), 1e-9)
        verify(asked[0].when.clear < 115, "it clears inside the level")
    }

    // A cell whose row's unit isn't the one its levels are in has no Alert;
    // the note of a refused Alert goes with the cell
    function test_aMismatchedCellHasNoAlertAndANoteGoesWithItsCell() {
        const doc = DashboardBook.builtIn("desktop")
        doc.pages = [{ name: "Test", cells: [
            { kind: "digital", w: 2, h: 4, label: "Revs", refs: ["id:1001"], unit: "RPM",
              options: { from: 0, to: 8000, dangerFrom: 6500 } },
            { kind: "digital", w: 2, h: 4, label: "Revs in deg C", refs: ["id:1001"], unit: "°C",
              options: { from: 0, to: 150, dangerFrom: 115 } }] }]
        const page = createTemporaryObject(dashComponent, testCase.Window.window.contentItem)
        page.doc = doc
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        page.startEditing()
        page.select(1)
        tryVerify(() => !find(page, "panelAlert").visible, 5000, "a mismatch has no Alert")
        page.select(0)
        tryVerify(() => find(page, "panelAlert").visible, 5000, "its unit's row has one")
        page.alertNote = "why"
        verify(find(page, "dashAlertNote").visible)
        page.select(1)
        compare(page.alertNote, "", "another cell clears it")
    }

    // A refused Alert's note goes once a later Alert from the same cell
    // opens its draft, so the cell's panel shows no stale refusal
    function test_aRefusalsNoteGoesWhenALaterAlertOpens() {
        const full = []
        for (let i = 0; i < ConditionBook.maxConditions; ++i)
            full.push(rpm("a" + i, "A" + i, { signal: ["id:1001"], unit: "RPM", above: 6000 }))
        save(full)
        const main = createTemporaryObject(mainComponent, testCase)
        verify(main, "Main loads")
        const dash = find(main.contentItem, "dashboardPage")
        const gauge = () => ConditionBook.alertFromLevels("Coolant", ["id:2001"], "°C",
                                                          { dangerFrom: 115 }, 0, 150, [])
        main.gotoPage(main.dashboardPage)
        main.openAlertDraft(gauge())
        verify(dash.alertNote.indexOf("holds 64 alerts") >= 0, dash.alertNote)
        compare(main.currentPage, main.dashboardPage, "a refusal stays on the Dashboard")
        save([])
        main.openAlertDraft(gauge())
        compare(main.currentPage, main.settingsPage, "the draft opens in Settings")
        compare(dash.alertNote, "", "the refusal's note is gone")
    }
}
