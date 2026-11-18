// File/layer: app/qml/DashHistory.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit and its parts (QmlTest.dashedit_*), QmlTest.phone,
//          QmlTest.linkhealth, QmlTest.driveview (test_qml); BENCH, Manual smoke

import QtQuick

pragma ComponentBehavior: Bound

// The Dashboard's undo history (ADR-0057's appendix "undo in edit mode as built"), a part of DashboardPage

Item {
    id: part
    required property DashboardPage host
    visible: false

    // ---- Undo (ADR-0057's appendix "undo in edit mode as built"). An
    // entry keeps the text stored before its step, so an undo stores what
    // was there: "" (nothing, the built-in layout) included

    property var undoStack: []       // [{ text, step }], the newest last
    property var redoStack: []
    property string lastKey: ""      // the last step's grouping key
    property double lastAt: 0        // when it was made (Date.now())
    readonly property int undoLimit: 50
    readonly property int groupMs: 1500
    property int undoLineMs: 5000    // the line's time; a test shortens it
    property string undoLine: ""     // the line's words; "" hides it
    readonly property bool canUndo: undoStack.length > 0
    readonly property bool canRedo: redoStack.length > 0
    readonly property string undoWords: canUndo ? undoStack[undoStack.length - 1].step.words : ""
    readonly property string redoWords: canRedo ? redoStack[redoStack.length - 1].step.words : ""
    // The options whose control steps or toggles: a run of them is one step
    readonly property var steppedOptions: ["decimals", "peak", "cells", "digits", "window",
                                           "count", "marks", "vertical"]
    // An option's word in a step's words, as its caption on the panel says it
    readonly property var optionWords: ({
        "from": qsTr("range"), "to": qsTr("range"), "coldBelow": qsTr("zones"),
        "warnFrom": qsTr("zones"), "dangerFrom": qsTr("zones"), "warnBelow": qsTr("zones"),
        "dangerBelow": qsTr("zones"), "decimals": qsTr("decimals"),
        "peak": qsTr("peak hold"), "color": qsTr("color"), "majorStep": qsTr("major step"),
        "minorStep": qsTr("major step"), "labelDivisor": qsTr("major step"),
        "caption": qsTr("caption"), "description": qsTr("description"), "cells": qsTr("cells"),
        "rise": qsTr("curve"), "digits": qsTr("digits"), "window": qsTr("window"),
        "vertical": qsTr("standing up"), "marks": qsTr("scale marks"), "count": qsTr("lights")
    })

    // A step on the page shown: its grouping key ("" never groups), its
    // words, and its cell before and after (-1 for none)
    function stepOf(key, words, cell, cellAfter, pageAfter) {
        return { key: key, words: words, page: part.host.pageIndex,
                 pageAfter: pageAfter ?? part.host.pageIndex, cell: cell, cellAfter: cellAfter }
    }

    // The text the store holds for what's shown
    function shownText() {
        return part.host.layoutStore[part.host.formKey] === "" ? "" : DashboardBook.serialize(part.host.doc)
    }
    function record(before, after, step) {
        const now = Date.now()
        const n = undoStack.length
        if (n > 0 && step.key !== "" && step.key === lastKey && now - lastAt <= groupMs) {
            // A run: the top already holds the text from before it
            const top = undoStack[n - 1]
            const rest = undoStack.slice(0, n - 1)
            if (after === top.text) {
                // Back where the run began, so an Undo would change nothing
                undoStack = rest
                lastKey = ""
            } else {
                undoStack = rest.concat([{ text: top.text, step: Object.assign({}, top.step,
                    { pageAfter: step.pageAfter, cellAfter: step.cellAfter }) }])
                lastKey = step.key
            }
        } else {
            undoStack = undoStack.concat([{ text: before, step: step }]).slice(-undoLimit)
            lastKey = step.key
        }
        lastAt = now
        redoStack = []
        // Edit mode's bar has Undo; outside it the line is the only way back
        if (part.host.editing)
            undoLine = ""
        else
            showLine(step.words.charAt(0).toUpperCase() + step.words.slice(1))
    }
    function restore(text) {
        part.host.layoutStore[part.host.formKey] = text
        if (text === "")
            showStoredLayout()
        else
            part.host.doc = DashboardBook.parse(text, part.host.form).doc
    }
    // With nothing stored the document is the stored read itself, so the
    // built-in layout is drawn as at first run (builtInShown)
    function showStoredLayout() {
        part.host.doc = Qt.binding(() => part.host.parsed.doc)
    }
    // An undo or a redo goes from (page, cell) to (toPage, toCell). The
    // cell panel follows only when it was on the step's cell, so it never
    // shows another cell at a moved index; Add's and a signal's close
    function travel(text, page, cell, toPage, toCell) {
        const shown = part.host.panel
        const follows = part.host.selected >= 0 && part.host.selected === cell && page === part.host.pageIndex
                        && (shown === "cell" || shown === "")
        restore(text)
        part.host.goTo(toPage)
        if (follows && toCell >= 0 && toCell < part.host.shownCells.length) {
            part.host.selected = toCell
            part.host.panel = shown
            return
        }
        part.host.panel = ""
        part.host.addKind = ""
        part.host.selected = -1
    }
    function undo() {
        // A field still open commits as its own step, which this then takes back
        part.host.commitTyping()
        if (!canUndo)
            return
        const top = undoStack[undoStack.length - 1]
        const s = top.step
        undoStack = undoStack.slice(0, -1)
        redoStack = redoStack.concat([{ text: shownText(), step: s }])
        travel(top.text, s.pageAfter, s.cellAfter, s.page, s.cell)
        lastKey = ""
        showLine(qsTr("Undid: %1").arg(s.words))
    }
    function redo() {
        part.host.commitTyping()
        if (!canRedo)
            return
        const top = redoStack[redoStack.length - 1]
        const s = top.step
        redoStack = redoStack.slice(0, -1)
        undoStack = undoStack.concat([{ text: shownText(), step: s }])
        travel(top.text, s.page, s.cell, s.pageAfter, s.cellAfter)
        lastKey = ""
        showLine(qsTr("Redid: %1").arg(s.words))
    }
    function clearHistory() {
        undoStack = []
        redoStack = []
        lastKey = ""
        undoLine = ""
    }
    function showLine(words) {
        undoLine = words
        undoLineTimer.restart()
    }
    Timer {
        id: undoLineTimer
        interval: part.undoLineMs
        onTriggered: part.undoLine = ""
    }
}
