// File/layer: app/qml/RecordingDialogs.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick
import QtCore

pragma ComponentBehavior: Bound

// The file dialogs: import's file and map, and Save report's file, a part of RecordingsPage

Item {
    id: root
    required property RecordingsPage host
    visible: false
    property alias importDialog: importDialog
    property alias mapDialog: mapDialog
    property alias reportDialog: reportDialog
    // Import's file and a CSV's column map
    DocumentOpenDialog {
        id: importDialog
        objectName: "importDialog"
        title: qsTr("Import a log")
        nameFilters: [qsTr("Logs (*.csv *.log *.txt)"), qsTr("All files (*)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: root.host.openPart.chooseImportFile(root.importDialog.selectedFile, root.importDialog.selectedName)
        onFailed: words => root.host.openPart.importPickFailed(words)
    }
    DocumentOpenDialog {
        id: mapDialog
        objectName: "mapDialog"
        title: qsTr("The logger's column map")
        nameFilters: [qsTr("Column maps (*.json)"), qsTr("All files (*)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: root.host.openPart.chooseImportMap(root.mapDialog.selectedFile, root.mapDialog.selectedName)
        onFailed: words => root.host.openPart.importPickFailed(words)
    }

    // Save report's file: text first, the one a shop or a buyer reads
    DocumentSaveDialog {
        id: reportDialog
        objectName: "reportDialog"
        title: qsTr("Save the health report")
        defaultSuffix: "txt"
        nameFilters: [qsTr("Text (*.txt)"), qsTr("JSON (*.json)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        currentFile: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
                     + "/health-report.txt"
        onAccepted: root.host.reportPart.saveReportTo(root.reportDialog.selectedFile,
                                      root.reportDialog.selectedNameFilter.index)
        onFailed: words => { root.host.reportFailed = true; root.host.reportNote = qsTr("Report not saved: %1").arg(words) }
    }
}
