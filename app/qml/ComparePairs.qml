// File/layer: app/qml/ComparePairs.qml | Qt6 Qml, UI layer
// Testing: QML: QmlTest.recordings_keys (test_qml)

pragma Singleton
import QtQml
import App

// The Recordings page's signal ids, Compare's pairs and the alignment's
// signal, as functions of the views' lists: no page state, no drawing
QtObject {
    // A signal's id: its key, or its name where the open has none (an
    // older recording's values as recorded, which has names alone)
    function idOf(s) {
        return s.key !== undefined && s.key !== "" ? s.key : s.name
    }
    // Each of here's signals' pair there by index, -1 for none
    // (RecordingView.pairsWith: a key, else a name each has once); a plain
    // array, [] while there isn't open
    function made(here, there) {
        return there.state === RecordingView.Open ? Array.from(here.pairsWith(there)) : []
    }
    // A signal's pair there, by index, or undefined
    function pairOf(pairs, index) {
        const j = pairs[index]
        return j === undefined || j < 0 ? undefined : j
    }
    // The number signals the alignment's choice lists, shown by name
    function numbers(list) {
        return list.filter(s => !s.isText)
    }
    // The alignment's signal in a list made again (another decode gives a
    // signal a key or takes it away): its id while a row has it, else the
    // id of the one row of its name, else the id unchanged
    function alignId(list, id, name) {
        if (id === "" || list.some(s => idOf(s) === id))
            return id
        const named = list.filter(s => s.name === name)
        return named.length === 1 ? idOf(named[0]) : id
    }
}
