// File/layer: app/qml/CsvGuessPreview.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The Import panel's preview of a CSV's guessed column map (ADR-0054's
// appendix "a column map guessed from the header", section 3): the time
// column and its unit, then each header column with a checkbox, its name,
// unit and kind, and the first rows of its cells. An edit changes the map
// the import gets (mapText()); Save as map keeps it for next time. The
// cells shown are the file's text as it splits here, for looking at only:
// the import's reader is core's.
ColumnLayout {
    id: preview

    // Bridge.recordingImport.guess's result, ok; null for none
    property var guess: null
    // s, ms or us for a time of plain numbers; "" until one is chosen
    property string timeUnit: ""
    // One entry a header column but the time's, in the header's order:
    // { column, include, used, why, canPutBack, name, unit, kind, samples }.
    // Edited in place by the rows below; replaced by load()
    property var columns: []
    property int included: 0
    property bool notesOpen: false
    // The words the last Save as map gave
    property string saveNote: ""
    property bool saveFailed: false

    readonly property bool unitReady: guess === null ? false
                                    : guess.timeNumbers ? timeUnit !== ""
                                    : guess.unitKnown
    readonly property bool ready: guess !== null && unitReady && included > 0
    // Why Import waits, "" when it doesn't
    readonly property string waiting: {
        if (guess === null)
            return ""
        if (!unitReady)
            return guess.timeNumbers
                   ? qsTr("Choose the time column's unit to import")
                   : qsTr("This file's time needs a column map that says its zone: Column map... picks one")
        if (included === 0)
            return qsTr("Choose a column to import")
        return ""
    }

    // A map was saved: the page lists the saved maps again
    signal mapSaved()

    spacing: Theme.spacing

    function load(result) {
        const list = []
        let time = null
        for (const c of result.columns) {
            if (c.isTime) {
                time = c
                continue
            }
            list.push({ column: c.column, include: c.used, used: c.used, why: c.why,
                        canPutBack: c.canPutBack, name: c.name, unit: c.unit,
                        kind: c.kind, samples: c.samples })
        }
        guess = result
        timeColumn = time
        // Emptied first so the rows below are made anew, never kept with
        // bindings an edit has replaced
        columns = []
        columns = list
        timeUnit = ["s", "ms", "us"].includes(result.timeUnit) ? result.timeUnit : ""
        notesOpen = false
        saveNote = ""
        saveFailed = false
        recount()
    }
    function clear() {
        guess = null
        timeColumn = null
        columns = []
        timeUnit = ""
        included = 0
        notesOpen = false
        saveNote = ""
        saveFailed = false
        saveName.text = ""
    }
    property var timeColumn: null

    function recount() {
        let n = 0
        for (const c of columns)
            n += c.include ? 1 : 0
        included = n
    }

    // The map text the import takes: the guess with the edits
    function mapText() {
        const edits = columns.map(c => ({ column: c.column, include: c.include, name: c.name,
                                          unit: c.unit, kind: c.kind }))
        return Bridge.recordingImport.composeMap(guess.mapText, edits,
                                                 guess.timeNumbers ? timeUnit : "")
    }

    function saveAs(name) {
        const r = Bridge.recordingImport.saveMap(name, mapText())
        saveFailed = !r.ok
        saveNote = r.ok ? qsTr("Saved as %1").arg(r.name) : r.words
        if (r.ok)
            mapSaved()
    }

    // ---- The time column and its unit ----
    Label {
        objectName: "guessTime"
        Layout.fillWidth: true
        wrapMode: Text.WrapAnywhere
        color: Theme.text
        text: preview.timeColumn === null ? ""
              : qsTr("Time: %1").arg(preview.timeColumn.column)
    }
    Label {
        objectName: "guessTimeSamples"
        visible: preview.timeColumn !== null
        Layout.fillWidth: true
        wrapMode: Text.WrapAnywhere
        maximumLineCount: 2
        elide: Text.ElideRight
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
        text: preview.timeColumn === null ? "" : preview.timeColumn.samples.join("  ")
    }
    RowLayout {
        visible: preview.guess !== null && preview.guess.timeNumbers
        Layout.fillWidth: true
        spacing: Theme.spacing
        Label {
            text: qsTr("Unit")
            color: Theme.textDim
            font.pixelSize: Theme.fontCaption
        }
        SegmentRow {
            Repeater {
                model: ["s", "ms", "us"]
                SegmentButton {
                    required property string modelData
                    objectName: "guessTimeUnit_" + modelData
                    text: modelData
                    implicitWidth: Math.max(Theme.touchTarget, implicitContentWidth + 24)
                    implicitHeight: Theme.touchTarget
                    selected: preview.timeUnit === modelData
                    onClicked: preview.timeUnit = modelData
                }
            }
        }
    }
    Label {
        objectName: "guessTimeFixed"
        visible: preview.guess !== null && !preview.guess.timeNumbers
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
        text: preview.guess === null ? ""
              : preview.guess.timeUnit === "iso8601" ? qsTr("Its times are dates with a UTC offset")
              : preview.guess.unitKnown ? qsTr("Its unit: %1").arg(preview.guess.timeUnit)
              : ""
    }
    Label {
        objectName: "guessWaiting"
        visible: preview.waiting !== ""
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.danger
        font.pixelSize: Theme.fontCaption
        text: preview.waiting
    }

    // ---- The columns ----
    Repeater {
        id: rows
        objectName: "guessRows"
        model: preview.columns.length

        Rectangle {
            id: row
            required property int index
            readonly property var col: preview.columns[index]
            // The checkbox's state, kept here so the fields follow it
            property bool on: col.include

            objectName: "guessRow_" + index
            Layout.fillWidth: true
            implicitHeight: body.implicitHeight + 16
            radius: Theme.radius
            color: Theme.surface

            ColumnLayout {
                id: body
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 8
                spacing: 4

                ThemedCheckBox {
                    id: includeBox
                    objectName: "include"
                    Layout.fillWidth: true
                    Layout.minimumHeight: Theme.touchTarget
                    checked: row.on
                    enabled: row.col.used || row.col.canPutBack
                    onToggled: {
                        row.col.include = checked
                        row.on = checked
                        preview.recount()
                    }
                    contentItem: Label {
                        text: row.col.column === "" ? qsTr("(no header)") : row.col.column
                        color: Theme.text
                        wrapMode: Text.WrapAnywhere
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: includeBox.indicator.width + includeBox.spacing
                    }
                }
                // Why a column was left out, and whether it can be put back
                Label {
                    objectName: "why"
                    visible: !row.col.used
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    color: Theme.textDim
                    font.pixelSize: Theme.fontCaption
                    text: row.col.canPutBack ? qsTr("Left out: %1. Check it to put it back.").arg(row.col.why)
                                             : qsTr("Left out: %1").arg(row.col.why)
                }
                RowLayout {
                    visible: row.on
                    Layout.fillWidth: true
                    spacing: Theme.spacing
                    ThemedTextField {
                        objectName: "name"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 0   // the row's width, not its text's
                        Layout.preferredHeight: Theme.touchTarget
                        placeholderText: qsTr("Name")
                        text: row.col.name
                        onTextChanged: row.col.name = text
                    }
                    ThemedTextField {
                        objectName: "unit"
                        Layout.preferredWidth: 72
                        Layout.preferredHeight: Theme.touchTarget
                        placeholderText: qsTr("Unit")
                        text: row.col.unit
                        onTextChanged: row.col.unit = text
                    }
                    ThemedButton {
                        id: kindButton
                        objectName: "kind"
                        compact: true
                        Layout.preferredHeight: Theme.touchTarget
                        Layout.minimumWidth: Theme.touchTarget
                        property string kind: row.col.kind
                        text: kind
                        Accessible.name: qsTr("Kind of %1: %2, press to change").arg(row.col.column).arg(kind)
                        onClicked: {
                            kind = kind === "number" ? "text" : "number"
                            row.col.kind = kind
                        }
                    }
                }
                Label {
                    objectName: "samples"
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    color: Theme.textDim
                    font.pixelSize: Theme.fontCaption
                    font.family: Theme.monoFamily
                    text: row.col.samples.join("  ")
                }
            }
        }
    }

    // ---- What the guess noted ----
    ThemedButton {
        objectName: "guessNotesToggle"
        visible: preview.guess !== null && preview.guess.notes.length > 0
        compact: true
        Layout.preferredHeight: Theme.touchTarget
        text: preview.notesOpen ? qsTr("Hide notes") : qsTr("Notes (%1)").arg(preview.guess === null ? 0 : preview.guess.notes.length)
        onClicked: preview.notesOpen = !preview.notesOpen
    }
    Label {
        objectName: "guessNotes"
        visible: preview.notesOpen && preview.guess !== null
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
        text: preview.guess === null ? "" : preview.guess.notes.join("\n")
    }

    // ---- Keep the map ----
    RowLayout {
        visible: preview.guess !== null
        Layout.fillWidth: true
        spacing: Theme.spacing
        ThemedTextField {
            id: saveName
            objectName: "saveMapName"
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            Layout.preferredHeight: Theme.touchTarget
            placeholderText: qsTr("Name for the map")
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
        }
        ThemedButton {
            objectName: "saveMapButton"
            compact: true
            Layout.preferredHeight: Theme.touchTarget
            text: qsTr("Save as map")
            enabled: preview.ready && !Bridge.recordingImport.busy
            onClicked: preview.saveAs(saveName.text)
        }
    }
    Label {
        objectName: "saveMapNote"
        visible: preview.saveNote !== ""
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: preview.saveFailed ? Theme.danger : Theme.success
        font.pixelSize: Theme.fontCaption
        text: preview.saveNote
    }
}
