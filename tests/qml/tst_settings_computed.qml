// File/layer: tests/qml/tst_settings_computed.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.settings_computed (test_qml)
// The signal form's computed kind (ADR-0062's appendix of 2026-11-16): the
// expression checked as it is typed, the Insert list, the test values, Save
// and the notes it says. Core judges; these cases pin what the form shows.

import QtCore
import QtQuick
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "SettingsComputed"
    when: windowShown

    Component {
        id: pageComponent
        SettingsPage { width: 1000; height: 1400 }
    }

    Settings {
        id: stored
        category: "session"
    }
    Settings {
        id: storedVehicles
        category: "vehicles"
    }

    // A binding error in the app's QML fails the test; each case starts
    // with nothing stored
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        stored.setValue("uri", "")
        stored.setValue("recentUris", "[]")
        stored.setValue("recording", false)
        stored.sync()
        storedVehicles.setValue("book", "")
        storedVehicles.setValue("document", "")
        storedVehicles.sync()
        wait(0)
    }

    function cleanup() {
        if (Bridge.connected || Bridge.disconnecting) {
            Bridge.disconnectSession()
            tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000, "no session after a case")
        }
        Bridge.unitSystem = 0
        Theme.compact = false
        Theme.dense = false
        Bridge.vehicleLabel = ""
        for (const p of Bridge.plugins())     // the store starts empty
            Bridge.removePlugin(p.sha256)
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    function createPage() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        verify(page, "Settings loads")
        page.open("connection")
        return page
    }

    function press(page, name) {
        find(page, name).clicked()
    }

    // The visible items under item whose right edge lies past column's,
    // named by objectName, text or type; a Text's edge is its painted
    // text's when wider
    function pastTheEdge(item, column, out) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (!child.visible || child.width <= 0)
                continue
            const painted = child.elide !== undefined && !child.clip
                            ? Math.max(child.width, child.contentWidth) : child.width
            const right = child.mapToItem(column, painted, 0).x
            if (right > column.width + 0.5)
                out.push((child.objectName || child.text || String(child))
                         + " at " + Math.round(right))
            pastTheEdge(child, column, out)
        }
        return out
    }

    function typeSignal(page, name, number, bits) {
        find(page, "formName").text = name
        find(page, "formNumber").text = number
        if (bits !== undefined)
            find(page, "formBits").text = bits
    }

    function typeComputed(page, name, expr) {
        press(page, "formKind_computed")
        find(page, "formName").text = name
        find(page, "formExpr").text = expr
    }

    function ownFileOf(page) {
        const r = Bridge.pluginVariantText(page.ownPlace.plugin.sha256, page.ownPlace.variant)
        verify(r.ok, r.words)
        return JSON.parse(r.text)
    }

    function test_theComputedKindHidesWhatItHasNoneOf() {
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        for (const n of ["formRx", "formTx", "formReadBy", "formByte", "formScale", "formOffset",
                         "formReadOnce"])
            verify(find(page, n).visible, n + " shows for a number")
        press(page, "formKind_computed")
        for (const n of ["formRx", "formTx", "formReadBy", "formByte", "formBits", "formScale",
                         "formOffset", "formReadOnce", "formValues"])
            verify(!find(page, n).visible, n + " hides for a computed signal")
        for (const n of ["formName", "formExpr", "formInsert", "formUnit", "formDecimals", "formSave"])
            verify(find(page, n).visible, n + " stays")
    }

    function test_aComputedSignalIsCheckedAsItIsTyped() {
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        typeComputed(page, "Boost", "obd2.map - obd2.baro")
        compare(find(page, "formExprProblem").text, "")
        verify(!find(page, "formExprMark").visible)
        verify(find(page, "formInput_0").text.indexOf("Reads ") === 0)
        compare(page.ownFormPart.computedPart.leafRefs, ["obd2.map", "obd2.baro"])
        find(page, "formExpr").text = "obd2.map - bost"
        verify(find(page, "formExprProblem").text.indexOf("bost") >= 0, find(page, "formExprProblem").text)
        compare(page.ownFormPart.computedPart.checked.at, 12)
        verify(find(page, "formExprMark").visible, "the character is marked")
        const mark = find(page, "formExprMark")
        tryVerify(() => Math.abs(mark.x - find(page, "formExpr").positionToRectangle(11).x) < 0.5, 2000,
                  "the mark sits under the refused character, " + mark.x)
    }

    function test_aRefusedExpressionSavesNothingAndSaysCoresWords() {
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        typeComputed(page, "Boost", "obd2.map - bost")
        press(page, "formSave")
        verify(find(page, "formProblem").text.indexOf("bost") >= 0, find(page, "formProblem").text)
        compare(page.ownSignals.length, 0)
        compare(page.pickedVehicle.own, "", "no plugin was made")
        find(page, "formExpr").text = "x".repeat(257)
        press(page, "formSave")
        verify(find(page, "formProblem").text.indexOf("256") >= 0, find(page, "formProblem").text)
    }

    function test_theTestValuesGiveAPreview() {
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        typeComputed(page, "Boost", "obd2.map - obd2.baro")
        find(page, "formUnit").text = "kPa"
        verify(find(page, "formPreview").text.indexOf("No value yet") >= 0, find(page, "formPreview").text)
        const a = find(page, "formTest_0")
        a.text = "180"
        verify(find(page, "formPreview").text.indexOf("No value yet") >= 0, "one still missing")
        const b = find(page, "formTest_1")
        b.text = "100"
        compare(find(page, "formPreview").text, "= 80 kPa")
        b.text = "abc"
        compare(find(page, "formPreview").text, "abc is not a number", "the typed text is named")
        // A value typed with the locale's decimal point reads, whichever it is
        b.text = "100" + Qt.locale().decimalPoint + "5"
        verify(find(page, "formPreview").text.indexOf("= 79") === 0, find(page, "formPreview").text)
        verify(find(page, "formPreview").text.indexOf("not a number") < 0)
        b.text = "abc"
        find(page, "formExpr").text = "obd2.map / (obd2.baro - obd2.baro)"
        compare(find(page, "formTest_0").text, "180", "a value stays for a signal still read")
        find(page, "formTest_1").text = "100"
        verify(find(page, "formPreview").text.indexOf("no number") >= 0, find(page, "formPreview").text)
    }

    function test_insertingASignalPutsItsNameAtTheCursor() {
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        press(page, "formKind_computed")
        const field = find(page, "formExpr")
        field.text = "1 + 2"
        field.cursorPosition = 4
        press(page, "formInsert")
        verify(find(page, "formInsertPanel").visible)
        const filter = find(page, "formInsertFilter")
        filter.text = "baro"
        tryVerify(() => findChild(page, "formInsertRow_0") !== null
            && findChild(page, "formInsertRow_0").contentItem.text.toLowerCase().indexOf("baro") >= 0, 2000, "a row")
        const row = find(page, "formInsertRow_0")
        verify(row.enabled)
        row.clicked()
        compare(field.text, "1 + obd2.baro 2")
        verify(!find(page, "formInsertPanel").visible, "the list closes")
        verify(field.activeFocus, "the field keeps the focus")
        verify(row.Accessible.name.indexOf("(obd2.baro)") >= 0, "a row is named: " + row.Accessible.name)
        // Escape closes the list and gives the field the focus back
        press(page, "formInsert")
        tryVerify(() => filter.activeFocus, 2000, "the filter takes the focus")
        keyClick(Qt.Key_Escape)
        verify(!find(page, "formInsertPanel").visible, "Escape closes the list")
        tryVerify(() => field.activeFocus, 2000, "the field has the focus")
        // A text signal has no number to compute with: dimmed, with its reason
        page.ownPart.clearForm()
        press(page, "formKind_text")
        typeSignal(page, "Label", "D200")
        press(page, "formSave")
        page.ownPart.clearForm()
        press(page, "formKind_computed")
        press(page, "formInsert")
        filter.text = "label"
        tryVerify(() => findChild(page, "formInsertRow_0") !== null
            && findChild(page, "formInsertRow_0").contentItem.text.toLowerCase().indexOf("label") >= 0,
            2000, "the text signal's row")
        verify(!find(page, "formInsertRow_0").enabled, "a text signal can't be read")
        verify(find(page, "formInsertRow_0").contentItem.text.indexOf("no number") >= 0)
        // obd2's computed Boost is listed, dimmed, with core's reason: it has no name to read
        filter.text = "boost"
        tryVerify(() => findChild(page, "formInsertRow_0") !== null
            && findChild(page, "formInsertRow_0").contentItem.text.toLowerCase().indexOf("boost") >= 0,
            2000, "obd2's Boost is listed")
        verify(!find(page, "formInsertRow_0").enabled)
        verify(find(page, "formInsertRow_0").contentItem.text.indexOf("no name") >= 0)
        // A chip puts a call's start at the cursor
        const field2 = find(page, "formExpr")
        field2.text = ""
        press(page, "formChip_max")
        compare(field2.text, "max(")
    }

    function test_saveWritesTheComputedSignalIntoTheOwnVariant() {
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        typeComputed(page, "Boost", "obd2.map - obd2.baro")
        find(page, "formUnit").text = "kPa"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        const file = ownFileOf(page)
        const s = file.signals.find(x => x.key === "boost")
        compare(s.kind, "computed")
        compare(s.expr, "obd2.map - obd2.baro")
        compare(s.unit, "kPa")
        compare(s.read, undefined)
        compare(file.extends, "obd2")
        compare(Bridge.profileSignals("mock://?profile=" + page.pickedVehicle.own)
                    .filter(r => r.ref === "key:boost").length, 1, "the variant lists it")
    }

    function test_aSaveThatRaisesTheFormatSaysSo() {
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        typeComputed(page, "Hot", "max(obd2.coolant, obd2.iat)")
        press(page, "formSave")
        compare(ownFileOf(page).havne_signals, 2)
        verify(page.ownPart.formNote.indexOf("needs Havne 0.8.0 or newer") >= 0, page.ownPart.formNote)
        page.ownPart.clearForm()
        typeComputed(page, "Diff", "obd2.coolant - obd2.iat")
        press(page, "formSave")
        compare(ownFileOf(page).havne_signals, 2, "it stays where it went")
        verify(page.ownPart.formNote.indexOf("needs Havne") < 0, page.ownPart.formNote)
    }

    function test_aSaveThatNeedsObd2AddsItAndSaysSo() {
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        typeSignal(page, "Oil", "D100", "8")
        press(page, "formSave")
        compare(ownFileOf(page).extends, undefined)
        page.ownPart.clearForm()
        typeComputed(page, "Twice", "oil * 2")
        press(page, "formSave")
        compare(ownFileOf(page).extends, undefined, "its own keys alone need no obd2")
        verify(page.ownPart.formNote.indexOf("obd2") < 0, page.ownPart.formNote)
        page.ownPart.clearForm()
        typeComputed(page, "Rpm over oil", "obd2.rpm / oil")
        press(page, "formSave")
        compare(ownFileOf(page).extends, "obd2")
        verify(page.ownPart.formNote.indexOf("Your signals now read with obd2") >= 0, page.ownPart.formNote)
        verify(page.ownPart.formNote.indexOf("PIDs") >= 0, page.ownPart.formNote)
        page.ownPart.clearForm()
        typeComputed(page, "Again", "obd2.rpm / 2")
        press(page, "formSave")
        verify(page.ownPart.formNote.indexOf("Your signals now read") < 0, "said once: " + page.ownPart.formNote)
    }

    function test_aDigitFirstNameIsRefusedForAComputedSignal() {
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        typeComputed(page, "2nd gear", "obd2.rpm")
        press(page, "formSave")
        verify(find(page, "formProblem").text.indexOf("digit") >= 0, find(page, "formProblem").text)
        compare(page.ownSignals.length, 0)
        find(page, "formName").text = "Gear 2"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
    }

    function test_anEditKeepsTheKeyOfAComputedSignalAndWhatReadsIt() {
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        typeComputed(page, "Double", "obd2.rpm * 2")
        press(page, "formSave")
        page.ownPart.clearForm()
        typeComputed(page, "Quad", "double * 2")
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        press(page, "ownEdit_0")
        compare(page.ownPart.formKind, "computed")
        compare(find(page, "formExpr").text, "obd2.rpm * 2", "the expression comes back")
        find(page, "formName").text = "Twice"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        compare(page.ownSignals.map(s => s.key), ["double", "quad"], "renamed, the key kept")
        compare(page.ownSignals[0].name, "Twice")
        compare(ownFileOf(page).signals[1].expr, "double * 2", "its reader still loads")
    }

    function test_anEditCantReadItself() {
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        typeComputed(page, "Double", "obd2.rpm * 2")
        press(page, "formSave")
        press(page, "ownEdit_0")
        find(page, "formExpr").text = "double + 1"
        verify(find(page, "formExprProblem").text !== "", "a circle is named as typed")
        press(page, "formSave")
        verify(find(page, "formProblem").text !== "")
        compare(page.ownSignals[0].expr, "obd2.rpm * 2", "nothing was written")
        // And the edited signal isn't offered to itself: the list says none, and
        // another signal is there for a filter that matches it
        press(page, "formInsert")
        find(page, "formInsertFilter").text = "double"
        const list = find(page, "formInsertList")
        waitForPolish(list)
        tryVerify(() => find(page, "formInsertNone").visible, 2000, "no signal to read")
        compare(list.count, 0)
        find(page, "formInsertFilter").text = "rpm"
        tryVerify(() => findChild(page, "formInsertRow_0") !== null
            && findChild(page, "formInsertRow_0").contentItem.text.toLowerCase().indexOf("rpm") >= 0,
            2000, "another signal is listed")
        verify(!find(page, "formInsertNone").visible)
    }

    function test_theDemoHintShowsForAnObd2Reader() {
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        typeComputed(page, "Double", "2 * 3")
        verify(!find(page, "formDemoHint").visible, "a file that reads no obd2")
        find(page, "formExpr").text = "obd2.rpm * 2"
        verify(find(page, "formDemoHint").visible, "an expression reading obd2")
        verify(find(page, "formDemoHint").text.indexOf("mock://demo") >= 0
               && find(page, "formDemoHint").text.indexOf(page.pickedVehicle.label) >= 0,
               find(page, "formDemoHint").text)
        press(page, "formSave")
        page.openOwnForm()   // a save goes back to the list
        page.ownPart.clearForm()
        typeComputed(page, "Six", "2 * 3")
        verify(find(page, "formDemoHint").visible, "a file that extends obd2, whatever is typed")
    }

    // A name obd2 has is read beside obd2's, and the form says so first
    function test_aNameObd2HasSaysTheVehicleReadsBoth() {
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        typeComputed(page, "Boost", "obd2.map - obd2.baro")
        const note = find(page, "formReplaces")
        verify(note.visible, "Boost is obd2's")
        compare(note.text, "obd2 has a signal named Boost: the vehicle reads both. Put this one on a gauge to show it.")
        find(page, "formName").text = "Boost gauge"
        verify(!note.visible, "another name replaces nothing")
        find(page, "formName").text = "Boost"
        press(page, "formSave")
        compare(find(page, "formProblem").text, "")
        page.openOwnForm()   // a save goes back to the list
        page.ownPart.clearForm()
        typeComputed(page, "Boost", "obd2.map")
        verify(find(page, "formComputed").visible, "the form is open")
        verify(!find(page, "formReplaces").visible, "the vehicle's own Boost is not obd2's")
    }

    // The computed form at the narrowest phone: nothing past the column's
    // edge with three inputs, their test values, the Insert list open and
    // a refusal showing, and every control a touch's height
    function test_theComputedFormFitsAPhonesWidth() {
        Theme.compact = true
        const page = createPage()
        page.width = 320
        press(page, "vehicleNew")
        page.openOwnForm()
        const column = find(page, "settingsOwnFormPageColumn")
        tryCompare(column, "width", 320 - 2 * Theme.pageMargin)
        typeComputed(page, "A long computed signal name to wrap", "obd2.map - obd2.baro + obd2.iat")
        find(page, "formUnit").text = "kPa"
        press(page, "formInsert")
        find(page, "formInsertFilter").text = "pressure"
        const form = find(page, "signalForm")
        waitForPolish(column)
        verify(form.width <= column.width + 0.5, "the form is no wider than the column, " + form.width)
        compare(pastTheEdge(form, column, []).join("; "), "", "nothing past the column's edge, " + column.width)
        verify(Theme.touchTarget >= 48, "a touch's height is 48")
        for (const n of ["formInsert", "formChip_max", "formTest_0", "formTest_2", "formExpr", "formSave"])
            verify(find(page, n).height >= Theme.touchTarget - 0.5, n + " is a touch's height")
        tryVerify(() => findChild(page, "formInsertRow_0") !== null, 2000)
        verify(find(page, "formInsertRow_0").height >= Theme.touchTarget - 0.5, "a list row is too")
        find(page, "formExpr").text = "obd2.map - bost and a long refusal's words to wrap"
        waitForPolish(column)
        compare(pastTheEdge(form, column, []).join("; "), "", "nothing past the edge with a refusal")
    }

    // The session's values fill the test values: each signal's own value, in
    // the decoded unit though the model shows the chosen system's. The demo's
    // key-on phase holds its values still, so a row read beside the fill is
    // the fill's value
    Instantiator {
        id: sessionRows
        model: Bridge.signalModel
        delegate: QtObject {
            required property int id
            required property double value
            required property bool hasValue
        }
    }

    function sessionValue(id) {
        for (let i = 0; i < sessionRows.count; ++i) {
            const r = sessionRows.objectAt(i)
            if (r["id"] === id && r["hasValue"])
                return r["value"]
        }
        return undefined
    }

    function test_useTheSessionsValuesFillsTheTestValues() {
        Bridge.unitSystem = 1
        const page = createPage()
        press(page, "vehicleNew")
        page.openOwnForm()
        typeComputed(page, "Mixed", "obd2.coolant + obd2.baro")
        verify(!find(page, "formUseLive").visible, "no session")
        compare(Bridge.connectSession("mock://demo?profile=obd2"), "", "the demo connects")
        tryVerify(() => Bridge.connected, 5000, "connected")
        tryVerify(() => find(page, "formUseLive").visible, 5000, "the button shows")
        const leaves = page.ownFormPart.computedPart.checked.leaves
        compare(leaves.length, 2)
        tryVerify(() => leaves.every(l => sessionValue(l.id) !== undefined), 10000, "the demo has both values")
        find(page, "formUseLive").clicked()
        compare(find(page, "formUseLiveNote").text, "Filled 2 of 2 from the session")
        for (let i = 0; i < 2; ++i) {
            const shown = sessionValue(leaves[i].id)
            const decoded = Bridge.decodedValue(leaves[i].unit, shown)
            verify(Math.abs(Number(find(page, "formTest_" + i).text) - decoded) < 1,
                   leaves[i].ref + " is " + decoded + " decoded, field " + find(page, "formTest_" + i).text)
        }
        verify(find(page, "formPreview").text.indexOf("= ") === 0, find(page, "formPreview").text)
        // A signal the car doesn't send has no value: its field stays empty
        tryVerify(() => Bridge.signalModel.hiddenCount > 0, 10000, "the demo car lacks some PIDs")
        const hidden = Bridge.signalModel.hiddenSignals().map(h => h.name)
        const none = page.ownFormPart.computedPart.editor.names().find(r => r.usable && r.ref.startsWith("obd2.")
                                                           && hidden.indexOf(r.name) >= 0)
        verify(none !== undefined, "a PID the demo doesn't send is offered")
        find(page, "formExpr").text = "obd2.coolant + " + none.ref
        find(page, "formTest_0").text = ""
        find(page, "formTest_1").text = ""
        find(page, "formUseLive").clicked()
        compare(find(page, "formUseLiveNote").text, "Filled 1 of 2 from the session")
        compare(find(page, "formTest_1").text, "", none.name + " has no value")
    }
}
