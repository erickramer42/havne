// File/layer: app/qml/DocumentOpenDialog.qml | Qt Quick, app layer
// Testing: QML: QmlTest.documentpicker (test_qml); MANUAL SMOKE: the Android
//          run sheet's picker checks
//
// An open-mode file dialog in FileDialog's shape (title, nameFilters,
// currentFolder, selectedFile, open(), accepted, rejected), so a page
// changes only the type. On Android it asks DocumentPicker, which hands the
// picked content:// URI on as it came: Qt Quick's FileDialog refuses a
// document named with parentheses there and keeps its previous pick.
// Elsewhere it is Qt Quick's FileDialog. selectedName is the picked
// document's name (a provider's id like "msf:1234" says nothing);
// failed(words) says why nothing could be picked. A cancel says nothing.

import QtQuick
import QtQuick.Dialogs
import App

QtObject {
    id: root

    property string title
    property var nameFilters: []
    property url currentFolder
    property url selectedFile
    property string selectedName
    // The system's picker: Android's; a test sets it on the desktop
    property bool usePicker: DocumentPicker.available
    // This dialog's pick under way, 0 for none: another dialog's answer
    // isn't this one's
    property int request: 0

    signal accepted()
    signal rejected()
    signal failed(string words)

    function open() {
        if (!usePicker) {
            fileDialog.open()
            return
        }
        request = DocumentPicker.pick()
    }

    // A URL's last part, as a name to show
    function nameOf(url) {
        const text = decodeURIComponent(url.toString())
        return text.substring(text.lastIndexOf("/") + 1)
    }

    readonly property FileDialog fileDialog: FileDialog {
        title: root.title
        nameFilters: root.nameFilters
        fileMode: FileDialog.OpenFile
        onAccepted: {
            root.selectedFile = selectedFile
            root.selectedName = root.nameOf(selectedFile)
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
            root.selectedName = name !== "" ? name : root.nameOf(url)
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
