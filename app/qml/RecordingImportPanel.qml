// File/layer: app/qml/RecordingImportPanel.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)
// Import: the file picked, its kind, a CSV's column map and the guess's
// preview, then Import and Cancel; one part of RecordingsPage, which holds the
// state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// Import: the file picked, its kind, a CSV's column map
ColumnLayout {
    id: root
    objectName: "importPanel"
    required property RecordingsPage host
    property alias guessPreview: guessPreview
    property alias importInterface: importInterface
    property alias importLabel: importLabel
    visible: root.host.importOpen && !root.host.viewing
    Layout.fillWidth: true
    spacing: Theme.spacing

    Label {
        objectName: "importFileName"
        Layout.fillWidth: true
        wrapMode: Text.WrapAnywhere
        color: Theme.text
        text: qsTr("Import %1").arg(root.host.importFileName)
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing
        ThemedButton {
            objectName: "importStart"
            text: qsTr("Import")
            highlighted: true
            enabled: !Bridge.recordingImport.busy && root.host.importReady
            onClicked: root.host.openPart.startImport()
        }
        ThemedButton {
            objectName: "importCancel"
            text: qsTr("Cancel")
            // An import under way is stopped; a guess is short and
            // runs to its end
            enabled: !root.host.importGuessing && !root.host.importStopping
            onClicked: {
                if (Bridge.recordingImport.busy) {
                    root.host.importStopping = true
                    Bridge.recordingImport.cancel()
                    return
                }
                root.host.importOpen = false
                root.host.importNote = ""
            }
        }
        Label {
            objectName: "importBusy"
            visible: Bridge.recordingImport.busy
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: root.host.importGuessing ? qsTr("Reading...")
                  : root.host.importStopping ? qsTr("Stopping...") : qsTr("Importing...")
        }
    }
    // An optional name for the recording, kept as its folder's label
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing
        Label { text: qsTr("Label"); color: Theme.textDim; font.pixelSize: Theme.fontCaption }
        ThemedTextField {
            id: importLabel
            objectName: "importLabel"
            Layout.fillWidth: true
            Layout.preferredWidth: 0   // the row's width, not its text's
            placeholderText: qsTr("Optional, shown on the recording's row")
            inputMethodHints: Qt.ImhNoPredictiveText
            enabled: !Bridge.recordingImport.busy
            onTextChanged: Bridge.recordingImport.label = text
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing
        SegmentRow {
            SegmentButton {
                tapHeight: root.host.segmentHeight
                objectName: "importKind_csv"
                text: qsTr("CSV")
                selected: root.host.importKind === RecordingImport.Csv
                enabled: !Bridge.recordingImport.busy
                onClicked: {
                    root.host.importKind = RecordingImport.Csv
                    if (guessPreview.guess === null && !root.host.importGuessing)
                        root.host.openPart.guessImportFile()
                }
            }
            SegmentButton {
                tapHeight: root.host.segmentHeight
                objectName: "importKind_candump"
                text: qsTr("candump")
                selected: root.host.importKind === RecordingImport.Candump
                enabled: !Bridge.recordingImport.busy
                onClicked: {
                    root.host.importKind = RecordingImport.Candump
                    root.host.importNote = ""
                }
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: Theme.fontCaption
            text: qsTr("Guessed from its first line")
        }
    }

    // A CSV's columns come through the logger's column map
    RowLayout {
        visible: root.host.importKind === RecordingImport.Csv
        Layout.fillWidth: true
        spacing: Theme.spacing
        ThemedButton {
            objectName: "importMapButton"
            text: qsTr("Column map...")
            enabled: !Bridge.recordingImport.busy
            onClicked: root.host.mapDialog.open()
        }
        Label {
            objectName: "importMapName"
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            color: root.host.importMap.toString() !== "" ? Theme.text : Theme.textDim
            text: root.host.importMap.toString() !== "" ? root.host.importMapFileName
                                                 : qsTr("Guessed from the header")
        }
    }
    // A map kept earlier, or back to the guess
    RowLayout {
        visible: root.host.importKind === RecordingImport.Csv
                 && (root.host.savedMaps.length > 0 || root.host.importMap.toString() !== "")
        Layout.fillWidth: true
        spacing: Theme.spacing
        ComboBox {
            implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                     implicitContentHeight + topPadding + bottomPadding,
                                     implicitIndicatorHeight + topPadding + bottomPadding, root.host.touchMin)
            id: savedMapChoice
            objectName: "savedMaps"
            visible: root.host.savedMaps.length > 0
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            Layout.preferredHeight: Theme.touchTarget
            enabled: !Bridge.recordingImport.busy
            model: [qsTr("Saved maps...")].concat(root.host.savedMaps.map(m => m.name))
            currentIndex: 0
            onActivated: index => {
                if (index > 0)
                    root.host.openPart.chooseImportMap(root.host.savedMaps[index - 1].url)
                currentIndex = 0
            }
        }
        ThemedButton {
            objectName: "useTheGuess"
            visible: root.host.importMap.toString() !== ""
            compact: true
            Layout.preferredHeight: Theme.touchTarget
            text: qsTr("Use the guess")
            enabled: !Bridge.recordingImport.busy
            onClicked: root.host.openPart.useTheGuess()
        }
    }
    Label {
        objectName: "importGuessing"
        visible: root.host.importGuessing
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        text: qsTr("Reading the file's header...")
    }
    CsvGuessPreview {
        id: guessPreview
        objectName: "guessPreview"
        visible: root.host.importKind === RecordingImport.Csv && root.host.importMap.toString() === ""
        Layout.fillWidth: true
        onMapSaved: root.host.savedMaps = Bridge.recordingImport.savedMaps()
    }

    // A candump log's bus, once core has said the log holds several
    RowLayout {
        visible: root.host.importKind === RecordingImport.Candump && root.host.importAsksInterface
        Layout.fillWidth: true
        spacing: Theme.spacing
        Label { text: qsTr("Interface"); color: Theme.textDim; font.pixelSize: Theme.fontCaption }
        ThemedTextField {
            id: importInterface
            objectName: "importInterface"
            Layout.fillWidth: true
            Layout.preferredWidth: 0   // the row's width, not its text's
            placeholderText: qsTr("can0")
            font.family: Theme.monoFamily
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            enabled: !Bridge.recordingImport.busy
        }
    }
}
