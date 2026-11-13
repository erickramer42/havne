// File/layer: tests/qml/tst_documentpicker.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.documentpicker (test_qml)

import QtQuick
import QtTest
import App

// The open dialogs' own picker (DocumentPicker, DocumentOpenDialog.qml):
// each answer reaches the dialog that asked, as Android's would. The desktop
// stands in for the phone through setPickerForTesting and deliver(), the
// call Android's activity result makes
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
    SignalSpy { id: pickedSpy; target: DocumentPicker; signalName: "picked" }

    function init() {
        DocumentPicker.setPickerForTesting(true)
        pickedSpy.clear()
    }
    function cleanup() {
        DocumentPicker.setPickerForTesting(false)
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
}
