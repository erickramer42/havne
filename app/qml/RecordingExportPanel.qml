// File/layer: app/qml/RecordingExportPanel.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import App

pragma ComponentBehavior: Bound

// The viewer's Export (ADR-0056 sections 5 and 7): the whole recording as a
// CSV, wide or long, pcapng or a folder copy, scrubbed by core unless Raw is
// ticked in Developer mode. The wide CSV's row interval is chosen with the
// file's exact size beside each choice. While it writes, Cancel stands in
// Save as...'s place. The row interval is kept between runs; everything
// else starts over at each open.
ColumnLayout {
    id: panel
    spacing: Theme.spacing

    // The opened recording
    required property RecordingView view
    // The viewer's ticked number signals, by name in the recording's order
    property var plottedNames: []
    signal closeRequested()

    property bool active: false
    property string format: "csv"
    property bool longLayout: false
    // "own": core's rule (the focus line, else every number signal)
    property string columnChoice: "own"
    property bool raw: false
    property bool hasValues: true
    property var sizes: []
    property string sizeProblem: ""
    property string note: ""
    property bool failed: false

    Settings {
        id: remembered
        category: "recordings"
        property string exportRows: "sweep"
        property string exportCustomMs: ""
    }
    property alias rowChoice: remembered.exportRows
    property alias customText: remembered.exportCustomMs

    readonly property bool wideCsv: format === "csv" && !longLayout
    readonly property bool rawOn: Theme.developerMode && raw
    // A decoded view's names aren't the recording's own columns
    readonly property bool plottedOffered: view.decodedWith === "" && plottedNames.length > 0
    readonly property bool usePlotted: columnChoice === "plotted" && plottedOffered
    // A typed interval as havne-cli's --rows takes it; -1 for none
    readonly property int customMs: {
        const t = customText.trim()
        if (!/^[0-9]+$/.test(t))
            return -1
        const v = Number(t)
        return v >= 1 && v <= 86400000 ? v : -1
    }
    readonly property int chosenMs: rowChoice === "custom" ? customMs
                                  : rowChoice === "100" ? 100
                                  : rowChoice === "250" ? 250 : 0
    readonly property bool ready: active && !exporter.busy && view.folder !== ""
                                  && (format !== "csv" || hasValues)
                                  && (!wideCsv || chosenMs >= 0)

    readonly property int units: Bridge.unitSystem
    onUnitsChanged: askSizes()
    onWideCsvChanged: askSizes()
    onUsePlottedChanged: sizeTimer.restart()
    onCustomMsChanged: sizeTimer.restart()
    onPlottedNamesChanged: {
        if (usePlotted)
            sizeTimer.restart()
    }

    // The panel as it opens
    function begin() {
        hasValues = exporter.holdsValues(view.folder)
        format = hasValues ? "csv" : view.hasFrames ? "pcapng" : "folder"
        longLayout = false
        columnChoice = "own"
        raw = false
        note = ""
        failed = false
        active = true
        askSizes()
    }
    function end() {
        active = false
        sizeTimer.stop()
        exporter.dropEstimate()
        sizes = []
    }

    function columnNames() {
        return usePlotted ? plottedNames : []
    }
    function askSizes() {
        sizeTimer.stop()
        if (!active || !wideCsv || !hasValues)
            return
        sizes = []
        sizeProblem = ""
        const asked = [0, 100, 250]
        if (customMs > 0)
            asked.push(customMs)
        exporter.estimate(view.folder, asked, Bridge.unitSystem, columnNames())
    }

    function bytesText(n) {
        if (n < 1000)
            return qsTr("%1 B").arg(n)
        if (n < 1000000)
            return qsTr("%1 KB").arg((n / 1000).toFixed(1))
        if (n < 1000000000)
            return qsTr("%1 MB").arg((n / 1000000).toFixed(1))
        return qsTr("%1 GB").arg((n / 1000000000).toFixed(1))
    }
    // "18,402 rows, 1.2 MB" for an interval; "" for none known
    function sizeOf(ms) {
        if (ms < 0)
            return ""
        const s = sizes.find(x => x.rowMs === ms)
        if (s === undefined)
            return exporter.sizing ? qsTr("Sizing...") : ""
        const rows = Number(s.rows).toLocaleString(Qt.locale(), 'f', 0)
        return s.rows === 1 ? qsTr("1 row, %1").arg(bytesText(s.bytes))
                            : qsTr("%1 rows, %2").arg(rows).arg(bytesText(s.bytes))
    }

    function fileName() {
        return exporter.fileStem(view.folder) + (format === "pcapng" ? ".pcapng" : ".csv")
    }

    // Asks where, as Save report does
    function go() {
        note = ""
        failed = false
        if (format === "folder") {
            folderDialog.open()
            return
        }
        saveDialog.selectedFile = saveDialog.currentFolder.toString() + "/" + fileName()
        saveDialog.open()
    }

    // The export to the file picked, or for a folder copy into the folder
    // picked; finished() follows, false when none started
    function exportTo(url) {
        const started = exporter.start({
            folder: view.folder,
            format: format === "pcapng" ? RecordingExport.Pcapng
                  : format === "folder" ? RecordingExport.Folder : RecordingExport.Csv,
            longLayout: format === "csv" && longLayout,
            rowMs: wideCsv ? chosenMs : 0,
            units: Bridge.unitSystem,
            columns: wideCsv ? columnNames() : [],
            raw: rawOn
        }, url)
        if (started) {
            failed = false
            note = qsTr("Exporting...")
        }
        return started
    }

    RecordingExport {
        id: exporter
        objectName: "exporter"
        onEstimated: result => {
            if (result.ok)
                panel.sizes = result.sizes
            else
                panel.sizeProblem = qsTr("The sizes aren't known: %1").arg(result.words)
        }
        onFinished: result => {
            Bridge.logEvent(result.line)
            if (result.folder !== panel.view.folder)
                return
            panel.failed = !result.ok && !result.canceled
            panel.note = result.canceled ? qsTr("Export canceled: nothing was written.")
                       : !result.ok ? qsTr("Not exported: %1").arg(result.words)
                       : result.name !== "" ? qsTr("Saved as %1: %2").arg(result.name).arg(result.summary)
                       : qsTr("Saved: %1").arg(result.summary)
        }
    }
    Timer {
        id: sizeTimer
        interval: 300
        onTriggered: panel.askSizes()
    }

    FileDialog {
        id: saveDialog
        title: qsTr("Export the recording")
        fileMode: FileDialog.SaveFile
        defaultSuffix: panel.format === "pcapng" ? "pcapng" : "csv"
        nameFilters: panel.format === "pcapng" ? [qsTr("pcapng (*.pcapng)")] : [qsTr("CSV (*.csv)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: panel.exportTo(saveDialog.selectedFile)
    }
    FolderDialog {
        id: folderDialog
        title: qsTr("Choose where to put the copy")
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: panel.exportTo(folderDialog.selectedFolder)
    }

    // A choice of the row interval with its file's size
    component IntervalRow: AbstractButton {
        id: intervalRow
        property string size: ""
        property bool chosen: false
        Layout.fillWidth: true
        implicitHeight: Theme.touchTarget
        hoverEnabled: true
        Accessible.checkable: true
        Accessible.checked: chosen
        Accessible.name: size !== "" ? text + ", " + size : text
        background: Rectangle {
            radius: Theme.radius
            color: intervalRow.chosen ? Theme.elevatedLight
                 : intervalRow.hovered ? Theme.elevated : Theme.surface
        }
        contentItem: RowLayout {
            spacing: Theme.spacing
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing
                elide: Text.ElideRight
                color: intervalRow.chosen ? Theme.text : Theme.textDim
                text: intervalRow.text
            }
            Label {
                objectName: "intervalSize"
                Layout.rightMargin: Theme.spacing
                color: Theme.textDim
                font.pixelSize: 13
                text: intervalRow.size
            }
        }
    }

    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.text
        text: qsTr("Export the whole recording")
    }

    // At the top, so the action and what it did stay on a phone's first screen
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing
        ThemedButton {
            objectName: "exportGo"
            visible: !exporter.busy
            Layout.preferredHeight: Theme.touchTarget
            highlighted: true
            enabled: panel.ready
            text: panel.format === "folder" ? qsTr("Choose a folder...") : qsTr("Save as...")
            onClicked: panel.go()
        }
        ThemedButton {
            objectName: "exportCancel"
            visible: exporter.busy
            Layout.preferredHeight: Theme.touchTarget
            text: qsTr("Cancel")
            onClicked: {
                panel.note = qsTr("Canceling...")
                exporter.cancel()
            }
        }
        ThemedButton {
            objectName: "exportClose"
            Layout.preferredHeight: Theme.touchTarget
            text: qsTr("Close")
            onClicked: panel.closeRequested()
        }
        Item { Layout.fillWidth: true }
    }

    Label {
        objectName: "exportNote"
        visible: panel.note !== ""
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: panel.failed ? Theme.danger : Theme.textDim
        text: panel.note
    }

    Rectangle {
        implicitWidth: formatSwitch.implicitWidth + 4
        implicitHeight: formatSwitch.implicitHeight + 4
        radius: Theme.radius
        color: Theme.surface
        Row {
            id: formatSwitch
            anchors.centerIn: parent
            spacing: 2
            SegmentButton {
                objectName: "exportFormat_csv"
                implicitHeight: Theme.touchTarget
                text: qsTr("CSV")
                selected: panel.format === "csv"
                enabled: panel.hasValues
                onClicked: panel.format = "csv"
            }
            SegmentButton {
                objectName: "exportFormat_pcapng"
                implicitHeight: Theme.touchTarget
                visible: panel.view.hasFrames
                text: qsTr("pcapng")
                selected: panel.format === "pcapng"
                onClicked: panel.format = "pcapng"
            }
            SegmentButton {
                objectName: "exportFormat_folder"
                implicitHeight: Theme.touchTarget
                text: qsTr("Folder")
                selected: panel.format === "folder"
                onClicked: panel.format = "folder"
            }
        }
    }

    Label {
        objectName: "exportNoValues"
        visible: !panel.hasValues
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        text: qsTr("No CSV: this recording has frames alone, no values were recorded.")
    }

    Rectangle {
        visible: panel.format === "csv"
        implicitWidth: layoutSwitch.implicitWidth + 4
        implicitHeight: layoutSwitch.implicitHeight + 4
        radius: Theme.radius
        color: Theme.surface
        Row {
            id: layoutSwitch
            anchors.centerIn: parent
            spacing: 2
            SegmentButton {
                objectName: "exportLayout_wide"
                implicitHeight: Theme.touchTarget
                text: qsTr("Wide")
                selected: !panel.longLayout
                onClicked: panel.longLayout = false
            }
            SegmentButton {
                objectName: "exportLayout_long"
                implicitHeight: Theme.touchTarget
                text: qsTr("Long")
                selected: panel.longLayout
                onClicked: panel.longLayout = true
            }
        }
    }

    Label {
        objectName: "exportHint"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        font.pixelSize: 13
        text: (panel.format === "pcapng" ? qsTr("Its frames, for Wireshark.")
               : panel.format === "folder" ? qsTr("A copy to send, without the vehicle's name.")
               : panel.longLayout ? qsTr("A row a value, as recorded.")
               : qsTr("A column a signal, in the units the app shows."))
              + (panel.rawOn ? "" : " " + qsTr("Identification is left out."))
    }

    ColumnLayout {
        visible: panel.wideCsv && panel.hasValues
        Layout.fillWidth: true
        spacing: 2

        IntervalRow {
            objectName: "exportRows_sweep"
            text: qsTr("Every sweep")
            chosen: panel.rowChoice === "sweep"
            size: panel.sizeOf(0)
            onClicked: panel.rowChoice = "sweep"
        }
        IntervalRow {
            objectName: "exportRows_100"
            text: qsTr("Every 100 ms")
            chosen: panel.rowChoice === "100"
            size: panel.sizeOf(100)
            onClicked: panel.rowChoice = "100"
        }
        IntervalRow {
            objectName: "exportRows_250"
            text: qsTr("Every 250 ms")
            chosen: panel.rowChoice === "250"
            size: panel.sizeOf(250)
            onClicked: panel.rowChoice = "250"
        }
        IntervalRow {
            objectName: "exportRows_custom"
            text: qsTr("Custom")
            chosen: panel.rowChoice === "custom"
            size: panel.sizeOf(panel.customMs)
            onClicked: panel.rowChoice = "custom"
        }
        RowLayout {
            visible: panel.rowChoice === "custom"
            Layout.fillWidth: true
            spacing: Theme.spacing
            ThemedTextField {
                objectName: "exportCustomMs"
                Layout.preferredWidth: 120
                Layout.preferredHeight: Theme.touchTarget
                placeholderText: qsTr("500")
                inputMethodHints: Qt.ImhDigitsOnly
                text: panel.customText
                onTextEdited: panel.customText = text
            }
            Label {
                objectName: "exportCustomProblem"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: panel.customMs < 0 && panel.customText !== "" ? Theme.danger : Theme.textDim
                text: panel.customMs < 0 && panel.customText !== ""
                      ? qsTr("ms, 1 to 86400000") : qsTr("ms between rows")
            }
        }
        Label {
            objectName: "exportSizeProblem"
            visible: panel.sizeProblem !== ""
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.danger
            text: panel.sizeProblem
        }
    }

    // The columns: the log's own, or the signals ticked in the viewer
    Label {
        objectName: "exportColumnsLabel"
        visible: panel.wideCsv && panel.hasValues && panel.plottedOffered
        text: qsTr("Signals:")
        color: Theme.textDim
    }
    Rectangle {
        visible: panel.wideCsv && panel.hasValues && panel.plottedOffered
        implicitWidth: columnSwitch.implicitWidth + 4
        implicitHeight: columnSwitch.implicitHeight + 4
        radius: Theme.radius
        color: Theme.surface
        Row {
            id: columnSwitch
            anchors.centerIn: parent
            spacing: 2
            SegmentButton {
                objectName: "exportColumns_own"
                implicitHeight: Theme.touchTarget
                text: qsTr("The log's own")
                selected: !panel.usePlotted
                onClicked: panel.columnChoice = "own"
            }
            SegmentButton {
                objectName: "exportColumns_plotted"
                implicitHeight: Theme.touchTarget
                text: qsTr("The %1 plotted").arg(panel.plottedNames.length)
                selected: panel.usePlotted
                onClicked: panel.columnChoice = "plotted"
            }
        }
    }

    Label {
        objectName: "exportDecodedNote"
        visible: panel.format === "csv" && panel.hasValues && panel.view.decodedWith !== ""
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.warning
        text: qsTr("The CSV holds the values as recorded, not this decode.")
    }

    ThemedCheckBox {
        objectName: "exportRaw"
        visible: Theme.developerMode
        implicitHeight: Theme.touchTarget
        text: qsTr("Raw, not scrubbed")
        checked: panel.raw
        onToggled: panel.raw = checked
    }
    Label {
        objectName: "exportRawHint"
        visible: Theme.developerMode
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
        text: qsTr("Scrubbed, the file leaves the vehicle's identification out; raw keeps it.")
    }
    Label {
        objectName: "exportRawWarning"
        visible: panel.rawOn
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.warning
        text: qsTr("The file can hold the VIN and calibration IDs: keep it to yourself.")
    }
}
