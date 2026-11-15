// File/layer: app/qml/DocumentSaveDialog.qml | Qt Quick, app layer
// Testing: QML: QmlTest.documentpicker (test_qml); MANUAL SMOKE: the Android
//          run sheet's picker checks
//
// A save-mode file dialog in FileDialog's shape (title, nameFilters,
// defaultSuffix, currentFolder, currentFile, selectedFile,
// selectedNameFilter.index, open(), accepted, rejected), so a page changes
// only the type and adds onFailed. On Android it asks DocumentPicker's save
// picker, offering currentFile's name (else that of a selectedFile the page
// set, never the last answer's) and the MIME type of defaultSuffix (none when
// the filters name more than one suffix); the answer notes whether the pick
// made its document, so a
// failed save removes only that one (healthreport.h). Elsewhere it is Qt
// Quick's FileDialog. selectedNameFilter.index is the filter the picked
// name's suffix matches, else 0 (Android's picker shows no filters).
// failed(words) says why nothing could be picked. A cancel says nothing.

import QtQuick
import QtQuick.Dialogs
import App

QtObject {
    id: root

    property string title
    property var nameFilters: []
    property string defaultSuffix
    property url currentFolder
    property url currentFile
    property url selectedFile
    property string selectedName
    component NameFilterChoice: QtObject {
        property int index: 0
    }
    readonly property NameFilterChoice selectedNameFilter: NameFilterChoice {}
    // The system's picker: Android's; a test sets it on the desktop
    property bool usePicker: DocumentPicker.available
    // This dialog's pick under way, 0 for none: another dialog's answer
    // isn't this one's
    property int request: 0

    signal accepted()
    signal rejected()
    signal failed(string words)

    // The last answer's URL: a provider's names the document by an id ("89"),
    // so the next open never offers it as a name
    property url answeredFile

    function open() {
        // The page's own name: currentFile, else a selectedFile it set since
        // the last answer
        const initial = currentFile.toString() !== "" ? currentFile
                      : selectedFile.toString() !== answeredFile.toString() ? selectedFile
                      : ""
        if (!usePicker) {
            if (initial.toString() !== "")
                fileDialog.selectedFile = initial
            fileDialog.open()
            return
        }
        request = DocumentPicker.pickSave(nameOf(initial), pickSuffix())
    }

    // The suffixes the filters name ("json" in "JSON (*.json)"), lower case
    function suffixesOf(filters) {
        const out = []
        for (const f of filters) {
            for (const p of f.match(/\*\.[^\s;)]+/g) || []) {
                const s = p.substring(2).toLowerCase()
                if (out.indexOf(s) < 0)
                    out.push(s)
            }
        }
        return out
    }

    // The type's suffix the picker asks for: with filters of more than one
    // suffix none (application/octet-stream), since a provider appends its
    // type's extension to a name ending in another ("report.json.txt")
    function pickSuffix() {
        return suffixesOf(nameFilters).length > 1 ? "" : defaultSuffix
    }

    // A URL's last part, as a name to show or offer
    function nameOf(url) {
        const text = decodeURIComponent(url.toString())
        return text.substring(text.lastIndexOf("/") + 1)
    }

    // The filter whose suffixes end the name, else the first
    function filterOf(name) {
        const lower = name.toLowerCase()
        for (let i = 0; i < nameFilters.length; ++i) {
            for (const s of suffixesOf([nameFilters[i]])) {
                if (lower.endsWith("." + s))
                    return i
            }
        }
        return 0
    }

    readonly property FileDialog fileDialog: FileDialog {
        title: root.title
        nameFilters: root.nameFilters
        defaultSuffix: root.defaultSuffix
        fileMode: FileDialog.SaveFile
        onAccepted: {
            root.selectedFile = selectedFile
            root.answeredFile = selectedFile
            root.selectedName = root.nameOf(selectedFile)
            root.selectedNameFilter.index = selectedNameFilter.index
            root.accepted()
        }
        onRejected: root.rejected()
    }

    // Set only when given, so a dialog that names no folder opens as before
    Component.onCompleted: {
        if (currentFolder.toString() !== "")
            fileDialog.currentFolder = currentFolder
    }

    readonly property Connections answers: Connections {
        target: DocumentPicker
        function onPicked(request: int, url: url, name: string) {
            if (request !== root.request)
                return
            root.request = 0
            root.selectedFile = url
            root.answeredFile = url
            root.selectedName = name !== "" ? name : root.nameOf(url)
            root.selectedNameFilter.index = root.filterOf(root.selectedName)
            root.accepted()
        }
        function onCanceled(request: int) {
            if (request !== root.request)
                return
            root.request = 0
            root.rejected()
        }
        function onFailed(request: int, words: string) {
            if (request !== root.request)
                return
            root.request = 0
            root.failed(words)
        }
    }
}
