// File/layer: app/qml/RecordingEvents.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)

import QtQuick

pragma ComponentBehavior: Bound

// The Connections to the import and the bridge, a part of RecordingsPage

Item {
    id: root
    required property RecordingsPage host
    visible: false
    Connections {
        target: Bridge.recordingImport
        function onGuessFinished(result) {
            root.host.importGuessing = false
            // The panel was closed, or a map file chosen, meanwhile
            if (!root.host.importOpen || root.host.importMap.toString() !== ""
                    || root.host.importKind !== RecordingImport.Csv)
                return
            if (result.ok) {
                root.host.guessPreview.load(result)
            } else {
                root.host.importFailed = true
                root.host.importNote = qsTr("%1. Column map... picks a map instead.").arg(result.words)
            }
        }
        function onFinished(result) {
            root.host.importStopping = false
            root.host.importCanceled = result.canceled === true
            root.host.importFailed = !result.ok && !root.host.importCanceled
            if (result.ok) {
                root.host.importOpen = false
                root.host.openPart.openFolder(result.folder)
                root.host.importNote = root.host.openPart.importedWords(result)
            } else if (root.host.importCanceled) {
                // The panel stays, its choices kept, for another try
                root.host.importNote = qsTr("Import canceled: nothing was imported")
            } else {
                root.host.importNote = result.words
                if (root.host.importKind === RecordingImport.Candump)
                    root.host.importAsksInterface = true
            }
        }
    }

    Connections {
        target: Bridge
        // A recording that ended joins the list
        function onRecordingStateChanged() {
            if (!Bridge.recordingActive)
                root.host.refreshPart.refresh()
        }
    }
}
