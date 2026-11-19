// File/layer: app/qml/RecordingOpen.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick
import App

pragma ComponentBehavior: Bound

// Opening a recording, the import and the baseline: the page's file functions, a part of RecordingsPage

Item {
    id: part
    required property RecordingsPage host
    visible: false

    // A URL's last part, as the picker named it (a content:// URI's too)
    function fileNameOf(url) {
        const text = decodeURIComponent(url.toString())
        return text.substring(text.lastIndexOf("/") + 1)
    }

    // A file picked to import: its kind guessed, the rest asked for. name is
    // the picker's for it, else the URL's last part
    function chooseImportFile(url, name) {
        part.host.importFile = url
        part.host.importFileName = name || part.fileNameOf(url)
        part.host.importKind = Bridge.recordingImport.guessKind(url)
        part.host.importMap = ""
        part.host.importAsksInterface = false
        part.host.importInterface.text = ""
        part.host.importLabel.text = ""
        part.host.importNote = ""
        part.host.importFailed = false
        part.host.importOpen = true
        part.host.savedMaps = Bridge.recordingImport.savedMaps()
        part.guessImportFile()
    }

    // A CSV's map guessed from its header, unless a map file is chosen
    function guessImportFile() {
        part.host.guessPreview.clear()
        if (part.host.importKind !== RecordingImport.Csv || part.host.importMap.toString() !== "")
            return
        part.host.importGuessing = Bridge.recordingImport.guess(part.host.importFile, "")
        if (!part.host.importGuessing) {
            part.host.importFailed = true
            part.host.importNote = qsTr("The file couldn't be read. Column map... picks a map instead.")
        }
    }

    // A map file chosen (the dialog, or one saved earlier) instead of the
    // guess
    function chooseImportMap(url, name) {
        part.host.importMap = url
        part.host.importMapFileName = name || part.fileNameOf(url)
        part.host.importNote = ""
        part.host.importFailed = false
        part.host.guessPreview.clear()
    }

    // The picker couldn't give a file or a map
    function importPickFailed(words) {
        part.host.importNote = words
        part.host.importFailed = true
    }

    // Back to the guess after a map file was chosen
    function useTheGuess() {
        part.host.importMap = ""
        part.host.importNote = ""
        part.host.importFailed = false
        part.guessImportFile()
    }

    function startImport() {
        part.host.importNote = ""
        part.host.importFailed = false
        const started = part.host.importUsesGuess
            ? Bridge.recordingImport.startWithMap(part.host.importFile, part.host.guessPreview.mapText())
            : Bridge.recordingImport.start(part.host.importFile, part.host.importKind, part.host.importMap,
                                           part.host.importKind === RecordingImport.Candump
                                           ? part.host.importInterface.text : "")
        if (!started) {
            part.host.importFailed = true
            part.host.importNote = qsTr("Not imported: the import couldn't start")
        }
    }

    // What an import made, in words
    function importedWords(result) {
        if (part.host.importKind === RecordingImport.Candump)
            return qsTr("Imported %1 frames over %2 s")
                       .arg(result.frames).arg((result.durationUs / 1e6).toFixed(1))
        const words = result.columnsIgnored > 0
                      ? qsTr("Imported %1 values; columns ignored: %2")
                            .arg(result.values).arg(result.columnsIgnored)
                      : qsTr("Imported %1 values").arg(result.values)
        if (!(result.rowsNoTime > 0))
            return words
        // A row with no time has nowhere to go: core leaves it out, counted.
        // havne-cli's words; after a count of columns a comma would read as its
        const left = result.rowsNoTime === 1
                     ? qsTr("1 row with no time left out")
                     : qsTr("%1 rows with no time left out").arg(result.rowsNoTime)
        return words + (result.columnsIgnored > 0 ? "; " : ", ") + left
    }

    function checkBaseline() {
        // Not a ternary of the map and ({}): qmlcachegen 6.12.0 crashes on it
        if (part.host.baselineFolder === "") {
            part.host.baselineRow = null
            return
        }
        const row = Bridge.recordingRow(part.host.baselineFolder)
        part.host.baselineRow = row.folder !== undefined ? row : null
    }
    function useAsBaseline() {
        part.host.vehiclesEdited(VehicleBook.setBaseline(part.host.vehicles, part.host.recording.folder))
    }
    function clearBaseline() {
        part.host.vehiclesEdited(VehicleBook.clearBaseline(part.host.vehicles))
    }
    // The baseline overlaid on the open recording, lined up on their
    // engine starts
    function compareWithBaseline() {
        part.checkBaseline()
        if (part.host.baselineRow === null)
            return
        part.host.alignChoice.currentIndex = part.host.alignChoice.indexOfValue("engine")
        part.host.comparePart.setAlignOn("engine")
        part.host.comparePart.compareWith(part.host.baselineRow)
    }

    function open(row) {
        part.checkBaseline()
        // An import's words belong to the import, not to the recording opened next
        part.host.importNote = ""
        part.host.importFailed = false
        part.host.openedTitle = row.title
        part.host.openedUri = row.uri !== undefined ? row.uri : ""
        part.host.recording.open(row.folder)
    }

    // A folder just recorded (a stopped log), opened from the list as a
    // tap on its row would; one the list can't show stays on the list
    function openFolder(folder) {
        if (part.host.viewing)
            part.backToList()
        else
            part.host.refreshPart.refresh()
        const row = Bridge.recordingRow(folder)
        if (row.folder !== undefined)
            part.open(row)
    }

    function backToList() {
        part.host.importNote = ""
        part.host.reportPart.closeExport()
        part.host.stretchesOpen = false
        part.host.recording.close()
        part.host.viewFrames.clear()
        part.host.detailView = "values"
        part.host.decodeReopen = false
        part.host.comparePart.stopComparing()
        part.host.alignChoice.currentIndex = 0
        part.host.alignOn = "start"
        part.host.alignLabel = ""
        part.host.alignSignal = ""
        part.host.alignSignalName = ""
        part.host.alignLevel = NaN
        part.host.alignHysteresis = 0
        part.host.openedTitle = ""
        part.host.openedUri = ""
        part.host.signalFilter.clear()
        part.host.refreshPart.refresh()
    }

    // The opened recording's replay:// URI for the part in view
    // (ADR-0023's appendix "a stretch replayed"): from and to in the
    // recording's own seconds, none while the whole recording is in view
    function stretchUri() {
        const from = Math.max(0, Math.floor(part.host.viewT0))
        const to = Math.min(part.host.cursorsPart.durationMs(), Math.ceil(part.host.viewT1))
        if (from <= 0 && to >= part.host.cursorsPart.durationMs())
            return part.host.openedUri
        return part.host.openedUri + (part.host.openedUri.indexOf("?") >= 0 ? "&" : "?")
               + "from=" + part.host.textsPart.secondsText(from) + "&to=" + part.host.textsPart.secondsText(to)
    }
}
