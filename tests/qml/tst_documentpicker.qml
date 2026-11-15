// File/layer: tests/qml/tst_documentpicker.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.documentpicker (test_qml)

import QtQuick
import QtTest
import App
import AppTest

// The dialogs' own picker (DocumentPicker, DocumentOpenDialog.qml,
// DocumentSaveDialog.qml): each answer reaches the dialog that asked, as
// Android's would, and a save's notes whether the pick made its document. The
// desktop stands in for the phone through setPickerForTesting and deliver(),
// the call Android's activity result makes
TestCase {
    id: testCase
    name: "DocumentPicker"
    when: windowShown

    readonly property int ok: -1
    readonly property int canceled: 0
    readonly property string providerUri:
        "content://com.android.externalstorage.documents/document/primary%3ADownload%2Fname%20(1).json"

    Component { id: plainComponent; DocumentOpenDialog {} }
    Component { id: pickerComponent; DocumentOpenDialog { usePicker: true } }
    Component {
        id: saveComponent
        DocumentSaveDialog {
            usePicker: true
            defaultSuffix: "json"
            nameFilters: ["JSON (*.json)", "All files (*)"]
        }
    }
    Component {
        id: reportComponent
        DocumentSaveDialog {
            usePicker: true
            defaultSuffix: "txt"
            nameFilters: ["Text (*.txt)", "JSON (*.json)"]
        }
    }
    Component { id: plainSaveComponent; DocumentSaveDialog { nameFilters: ["Text (*.txt)", "JSON (*.json)"] } }
    readonly property string pageName: "file:///C:/Docs/dashboard-phone.json"
    readonly property string downloadsUri:
        "content://com.android.providers.downloads.documents/document/89"
    SignalSpy { id: pickedSpy; target: DocumentPicker; signalName: "picked" }

    function init() {
        DocumentPicker.setPickerForTesting(true)
        pickedSpy.clear()
    }
    function cleanup() {
        DocumentPicker.setPickerForTesting(false)
        DocumentPicker.setPlatformTypesForTesting({})
    }

    // A picker's dialog and a tally of its answers
    function spied() {
        const d = createTemporaryObject(pickerComponent, testCase)
        verify(d)
        const s = { d: d, accepted: 0, rejected: 0, failed: [] }
        d.accepted.connect(() => s.accepted++)
        d.rejected.connect(() => s.rejected++)
        d.failed.connect(words => s.failed.push(words))
        return s
    }

    function test_theDesktopUsesQtsDialog() {
        verify(!DocumentPicker.available, "the system's picker is Android's")
        const d = createTemporaryObject(plainComponent, testCase)
        verify(d)
        verify(!d.usePicker, "a desktop dialog opens Qt's FileDialog")
    }

    function test_aPickReachesTheDialogWithItsUrlAsItCame() {
        const s = spied()
        s.d.open()
        verify(s.d.request > 0, "a pick under way")
        DocumentPicker.deliver(s.d.request, ok, providerUri, "name (1).json")
        compare(s.accepted, 1)
        // A url's toString() decodes for display; the text it opens by is
        // BridgeTest.APickersUriOpensByTheTextItCameAs's
        compare(s.d.selectedFile, Qt.url(providerUri), "the URI as it came")
        compare(s.d.selectedName, "name (1).json", "the picker's name for it")
        compare(s.d.request, 0, "answered")
    }

    // Downloads names a document by an id: the provider's name is the one shown
    function test_theProvidersNameIsShownAndTheUrlsLastPartWithoutOne() {
        const s = spied()
        s.d.open()
        DocumentPicker.deliver(s.d.request, ok,
                               "content://com.android.providers.downloads.documents/document/msf%3A1000000034",
                               "report.csv")
        compare(s.d.selectedName, "report.csv")
        s.d.open()
        DocumentPicker.deliver(s.d.request, ok, providerUri, "")
        compare(s.d.selectedName, "name (1).json", "with no name from the provider, the URL's last part")
    }

    function test_aCancelReadsNothingAndSaysNothing() {
        const s = spied()
        s.d.open()
        DocumentPicker.deliver(s.d.request, ok, providerUri, "name (1).json")
        s.d.open()
        DocumentPicker.deliver(s.d.request, canceled, "", "")
        compare(s.rejected, 1)
        compare(s.accepted, 1, "no second accept")
        compare(s.failed, [])
        compare(s.d.selectedFile, Qt.url(providerUri), "the last pick stays as it was")
    }

    function test_aPickerThatReturnsNoDocumentSaysSo() {
        const s = spied()
        s.d.open()
        DocumentPicker.deliver(s.d.request, ok, "", "")
        compare(s.failed, ["The picker returned no document"])
        compare(s.accepted, 0)
    }

    // Two pages each hold an importDialog: an answer is the asking dialog's alone
    function test_anAnswerIsTheAskingDialogsAlone() {
        const a = spied()
        const b = spied()
        a.d.open()
        DocumentPicker.deliver(a.d.request, ok, providerUri, "a.json")
        compare(a.accepted, 1)
        compare(b.accepted, 0, "the other dialog didn't ask")
        compare(b.d.selectedFile.toString(), "")
    }

    // A pick whose answer never came holds nothing up: the newer one is answered
    function test_aNewerPickMakesAnOlderAnswerStale() {
        const a = spied()
        const b = spied()
        a.d.open()
        const older = a.d.request
        b.d.open()
        DocumentPicker.deliver(older, ok, providerUri, "late.json")
        compare(a.accepted, 0, "the older pick's answer is dropped")
        compare(pickedSpy.count, 0)
        DocumentPicker.deliver(b.d.request, ok, providerUri, "b.json")
        compare(b.accepted, 1)
        compare(b.d.selectedName, "b.json")
    }

    function test_anAnswerComesOnceAndAnUnknownOneNever() {
        const s = spied()
        s.d.open()
        const request = s.d.request
        DocumentPicker.deliver(request, ok, providerUri, "x.json")
        DocumentPicker.deliver(request, ok, providerUri, "x.json")
        compare(pickedSpy.count, 1, "a second answer to one pick is dropped")
        DocumentPicker.deliver(0, ok, providerUri, "x.json")
        DocumentPicker.deliver(request + 100, ok, providerUri, "x.json")
        compare(pickedSpy.count, 1)
    }

    // A second open of one dialog before the first answer: the first answer
    // is dropped and the second is taken once
    function test_aSecondOpenWhileAPickIsOutTakesOnlyTheSecond() {
        const s = spied()
        s.d.open()
        const first = s.d.request
        s.d.open()
        verify(s.d.request !== first, "a pick of its own")
        DocumentPicker.deliver(first, ok, providerUri, "first.json")
        compare(s.accepted, 0, "the first pick's answer is stale")
        DocumentPicker.deliver(s.d.request, ok, providerUri, "second.json")
        compare(s.accepted, 1)
        compare(s.d.selectedName, "second.json")
    }

    // An activity result finds its pick by its code: two picks in a row never
    // share one, and every code is inside Android's 16 bits
    function test_eachPickHasARequestCodeOfItsOwn() {
        let codes = []
        for (let request = 1; request <= 300; ++request) {
            const code = DocumentPicker.requestCodeOf(request)
            verify(code > 0 && code <= 0xFFFF, "a 16-bit code: " + code)
            codes.push(code)
        }
        for (let i = 1; i < codes.length; ++i)
            verify(codes[i] !== codes[i - 1], "a pick's code differs from the one before: " + (i + 1))
        compare(new Set(codes.slice(0, 256)).size, 256, "256 in turn before one comes back")
    }

    // Android's result carries only the code: it answers the pick under way
    // alone, never a stale pick, another code or a pick already answered
    function test_aResultCodeFindsOnlyThePickUnderWay() {
        const s = spied()
        s.d.open()
        const older = s.d.request
        const olderCode = DocumentPicker.requestCodeOf(older)
        compare(DocumentPicker.requestOfCode(olderCode), older)
        compare(DocumentPicker.requestOfCode(olderCode + 1), 0, "the next code is no pick's")
        compare(DocumentPicker.requestOfCode(olderCode - 1), 0, "nor the one before")
        s.d.open()
        const newer = s.d.request
        compare(DocumentPicker.requestOfCode(olderCode), 0, "a newer pick makes the older code stale")
        compare(DocumentPicker.requestOfCode(DocumentPicker.requestCodeOf(newer)), newer)
        DocumentPicker.deliver(newer, ok, providerUri, "x.json")
        compare(DocumentPicker.requestOfCode(DocumentPicker.requestCodeOf(newer)), 0, "answered: no pick under way")
    }

    // Elsewhere the dialog is Qt's FileDialog, and its answer is the dialog's
    function test_qtsDialogsAnswerIsTheDialogs() {
        const d = createTemporaryObject(plainComponent, testCase)
        verify(d)
        let accepted = 0
        let rejected = 0
        d.accepted.connect(() => accepted++)
        d.rejected.connect(() => rejected++)
        // A file that exists: Qt's dialog refuses one that doesn't
        const url = Harness.writeScratchFile("qt-dialog (1).json", "{}")
        d.fileDialog.selectedFile = url
        d.fileDialog.accepted()
        compare(accepted, 1)
        compare(d.selectedFile, url, "the dialog's pick")
        compare(d.selectedName, "qt-dialog (1).json")
        d.fileDialog.rejected()
        compare(rejected, 1)
        compare(accepted, 1, "a cancel accepts nothing")
    }

    // A failure that can't start is the newest pick's alone: an older
    // dialog's queued words are dropped
    function test_aStaleFailureIsDropped() {
        DocumentPicker.setPickerForTesting(false)
        const a = spied()
        const b = spied()
        a.d.open()
        b.d.open()
        tryVerify(() => b.failed.length === 1)
        wait(0)
        compare(a.failed, [], "the older pick's words are dropped")
    }

    // A dialog gone with its pick out: the next dialog's pick starts and is
    // answered, and the gone one's late answer reaches no one
    function test_aDialogGoneWithItsPickOutHoldsNothingUp() {
        const gone = pickerComponent.createObject(testCase)
        verify(gone)
        gone.open()
        const request = gone.request
        gone.destroy()
        wait(0)
        const s = spied()
        s.d.open()
        verify(s.d.request > 0 && s.d.request !== request, "a pick of its own")
        DocumentPicker.deliver(s.d.request, ok, providerUri, "next.json")
        compare(s.accepted, 1)
        compare(s.d.selectedName, "next.json")
        DocumentPicker.deliver(request, ok, providerUri, "late.json")
        compare(pickedSpy.count, 1, "the late answer is dropped")
        compare(s.accepted, 1)
    }

    // Off Android with no stand-in, a pick fails in words; they come after
    // open() has the request, so the dialog takes them
    function test_aPickThatCantStartSaysWhyToTheDialog() {
        DocumentPicker.setPickerForTesting(false)
        const s = spied()
        s.d.open()
        verify(s.d.request > 0)
        compare(s.failed, [], "not before the dialog knows its request")
        tryVerify(() => s.failed.length === 1)
        compare(s.failed[0], "This build has no document picker")
        compare(s.d.request, 0)
    }

    // A save's dialog and a tally of its answers
    function saving(props) {
        const d = createTemporaryObject(saveComponent, testCase, props || {})
        verify(d)
        const s = { d: d, accepted: 0, rejected: 0, failed: [] }
        d.accepted.connect(() => s.accepted++)
        d.rejected.connect(() => s.rejected++)
        d.failed.connect(words => s.failed.push(words))
        return s
    }

    // A save offers the page's name and its suffix's type; the answer reaches
    // the dialog as it came
    function test_aSaveOffersItsNameAndTypeAndAnswersItsDialog() {
        const s = saving({ currentFile: pageName })
        s.d.open()
        verify(s.d.request > 0, "a pick under way")
        compare(DocumentPicker.titleForTesting(), "dashboard-phone.json")
        compare(DocumentPicker.mimeTypeForTesting(), "application/json")
        DocumentPicker.deliver(s.d.request, ok, providerUri, "name (1).json",
                               DocumentPicker.launchedMsForTesting())
        compare(s.accepted, 1)
        compare(s.d.selectedFile, Qt.url(providerUri), "the URI as it came")
        compare(s.d.selectedName, "name (1).json")
        compare(s.d.selectedNameFilter.index, 0, "the one JSON filter")
    }

    // Filters of two suffixes ask no type, so a provider keeps the name typed
    // ("report.json", never "report.json.txt"); the filter follows the name
    function test_aSaveOfTwoSuffixesAsksNoTypeAndTakesItsFilterByName() {
        const d = createTemporaryObject(reportComponent, testCase)
        verify(d)
        d.open()
        compare(DocumentPicker.mimeTypeForTesting(), "application/octet-stream")
        DocumentPicker.deliver(d.request, ok, providerUri, "report.json", -1)
        compare(d.selectedNameFilter.index, 1, "the JSON filter")
        d.open()
        DocumentPicker.deliver(d.request, ok, downloadsUri, "report.txt", -1)
        compare(d.selectedNameFilter.index, 0, "the text filter")
        compare(d.suffixesOf(["Text (*.txt)", "JSON (*.json)", "All files (*)"]),
                ["txt", "json"])
    }

    // Downloads names its document by an id: a second save offers the page's
    // name again, never the last answer's "89"
    function test_aSecondSaveOffersThePagesNameAgain() {
        const s = saving({ currentFile: pageName })
        s.d.open()
        DocumentPicker.deliver(s.d.request, ok, downloadsUri, "", -1)
        compare(s.d.selectedName, "89", "the answer's last part, with no name")
        s.d.open()
        compare(DocumentPicker.titleForTesting(), "dashboard-phone.json")

        const plain = saving()
        plain.d.open()
        compare(DocumentPicker.titleForTesting(), "", "a page that names none")
        DocumentPicker.deliver(plain.d.request, ok, downloadsUri, "", -1)
        plain.d.open()
        compare(DocumentPicker.titleForTesting(), "", "never the last answer's id")
        plain.d.selectedFile = "file:///C:/Docs/20261105-220322.csv"
        plain.d.open()
        compare(DocumentPicker.titleForTesting(), "20261105-220322.csv",
                "a name the page set before the open")
    }

    function test_aSavesTypeFollowsItsSuffix() {
        compare(DocumentPicker.mimeTypeOf("json"), "application/json")
        compare(DocumentPicker.mimeTypeOf("JSON"), "application/json")
        compare(DocumentPicker.mimeTypeOf("csv"), "text/csv")
        compare(DocumentPicker.mimeTypeOf("txt"), "text/plain")
        compare(DocumentPicker.mimeTypeOf("pcapng"), "application/octet-stream")
        compare(DocumentPicker.mimeTypeOf(""), "application/octet-stream")
    }

    // A save asks the platform's own type for its suffix, so the picker
    // offers the documents of that suffix to replace; with none named, the
    // app's own
    function test_aSaveAsksThePlatformsTypeForItsSuffix() {
        DocumentPicker.setPlatformTypesForTesting({ csv: "text/comma-separated-values" })
        compare(DocumentPicker.saveTypeOf("csv"), "text/comma-separated-values")
        compare(DocumentPicker.saveTypeOf("CSV"), "text/comma-separated-values")
        compare(DocumentPicker.saveTypeOf("json"), "application/json")
        compare(DocumentPicker.saveTypeOf(""), "application/octet-stream")

        const d = createTemporaryObject(saveComponent, testCase,
                                        { defaultSuffix: "csv", nameFilters: ["CSV (*.csv)"] })
        verify(d)
        d.open()
        compare(DocumentPicker.mimeTypeForTesting(), "text/comma-separated-values",
                "the pick asks the platform's type")

        DocumentPicker.setPlatformTypesForTesting({})
        compare(DocumentPicker.saveTypeOf("csv"), "text/csv", "no platform type off a phone")
    }

    // A save's answer notes whether the pick made its document, by its
    // last_modified against the launch; an open's answer notes nothing
    function test_aSavesAnswerNotesItsVerdict() {
        const made = "content://test/document/verdict-made"
        const replaced = "content://test/document/verdict-replaced"
        const s = saving()
        s.d.open()
        const launched = DocumentPicker.launchedMsForTesting()
        verify(launched > 0, "the launch is kept")
        DocumentPicker.deliver(s.d.request, ok, made, "a.json", launched + 50)
        compare(DocumentPicker.verdictOfForTesting(made), 1, "made by the pick")
        s.d.open()
        DocumentPicker.deliver(s.d.request, ok, replaced, "b.json",
                               DocumentPicker.launchedMsForTesting() - 3600 * 1000)
        compare(DocumentPicker.verdictOfForTesting(replaced), 2, "chosen to replace")
        s.d.open()
        DocumentPicker.deliver(s.d.request, ok, made, "a.json", -1)
        compare(DocumentPicker.verdictOfForTesting(made), 0, "no time clears it")
        s.d.open()
        DocumentPicker.deliver(s.d.request, ok, made, "a.json", Date.now() + 60000)
        compare(DocumentPicker.verdictOfForTesting(made), 0,
                "a time past the answer says nothing")
        // A document made late in a long pick: past the launch's window, at
        // the answer
        s.d.open()
        wait(2500)
        DocumentPicker.deliver(s.d.request, ok, made, "a.json", Date.now())
        compare(DocumentPicker.verdictOfForTesting(made), 1, "made at the answer")
        compare(s.accepted, 5)

        const opened = "content://test/document/verdict-opened"
        const o = spied()
        o.d.open()
        DocumentPicker.deliver(o.d.request, ok, opened, "c.json", Date.now())
        compare(o.accepted, 1)
        compare(DocumentPicker.verdictOfForTesting(opened), 0, "an open notes nothing")
        compare(s.accepted, 5, "the open's answer isn't the save's")
        DocumentPicker.deliver(0, ok, replaced, "", -1)
        compare(DocumentPicker.verdictOfForTesting(replaced), 2, "a stale answer notes nothing")
    }

    // The verdict is noted under the name the save opens the document by: a
    // refused save on the dialog's own URL takes it
    function test_aRefusedSaveTakesItsPicksVerdict() {
        const uri = "content://com.android.externalstorage.documents/document/"
                    + "primary%3ADownload%2Fa%20(1).json"
        const s = saving()
        s.d.open()
        DocumentPicker.deliver(s.d.request, ok, uri, "a (1).json",
                               DocumentPicker.launchedMsForTesting() + 50)
        compare(DocumentPicker.verdictOfForTesting(uri), 1, "made by the pick")
        verify(!Bridge.saveLayoutFile("", s.d.selectedFile).ok)
        compare(DocumentPicker.verdictOfForTesting(uri), 0, "the save took it")
    }

    // A URI Qt's URL writes back another way (lower-case hex, an unreserved
    // letter encoded): the verdict is still the save's to take
    function test_aVerdictIsKeyedAsTheSaveNamesTheDocument() {
        const uri = "content://com.android.externalstorage.documents/document/"
                    + "primary%3aDownload%2F%41b.json"
        const s = saving()
        s.d.open()
        DocumentPicker.deliver(s.d.request, ok, uri, "Ab.json",
                               DocumentPicker.launchedMsForTesting() + 50)
        compare(DocumentPicker.verdictOfForTesting(uri), 1, "made by the pick")
        verify(!Bridge.saveLayoutFile("", s.d.selectedFile).ok)
        compare(DocumentPicker.verdictOfForTesting(uri), 0, "the save took it")
    }

    // A refused save pick writes nothing: no answer, no verdict
    function test_aCanceledSaveNotesNothingAndSaysNothing() {
        const uri = "content://test/document/verdict-canceled"
        const s = saving()
        s.d.open()
        DocumentPicker.deliver(s.d.request, canceled, uri, "", Date.now())
        compare(s.rejected, 1)
        compare(s.accepted, 0)
        compare(s.failed, [])
        compare(DocumentPicker.verdictOfForTesting(uri), 0)
        s.d.open()
        DocumentPicker.deliver(s.d.request, ok, "", "", -1)
        compare(s.failed, ["The picker returned no document"])
    }

    // A save pick and an open pick share the picker: the newer is answered
    function test_aSaveAfterAnOpenMakesTheOpenStale() {
        const o = spied()
        const s = saving()
        o.d.open()
        const older = o.d.request
        s.d.open()
        DocumentPicker.deliver(older, ok, providerUri, "late.json")
        compare(o.accepted, 0)
        DocumentPicker.deliver(s.d.request, ok, providerUri, "x.json", -1)
        compare(s.accepted, 1)
    }

    function test_aSavePickThatCantStartSaysWhy() {
        DocumentPicker.setPickerForTesting(false)
        const s = saving()
        s.d.open()
        tryVerify(() => s.failed.length === 1)
        compare(s.failed[0], "This build has no document picker")
    }

    // Elsewhere the save is Qt's FileDialog: its answer and filter are the
    // dialog's
    function test_qtsSaveDialogsAnswerIsTheDialogs() {
        const d = createTemporaryObject(plainSaveComponent, testCase)
        verify(d)
        verify(!d.usePicker)
        let accepted = 0
        d.accepted.connect(() => accepted++)
        const url = Harness.writeScratchFile("qt-save.json", "{}")
        d.fileDialog.selectedFile = url
        d.fileDialog.accepted()
        compare(accepted, 1)
        compare(d.selectedFile, url)
        compare(d.selectedName, "qt-save.json")
        compare(d.selectedNameFilter.index, d.fileDialog.selectedNameFilter.index)
    }
}
