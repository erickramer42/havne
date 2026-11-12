// File/layer: app/qml/SignalHub.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard (test_qml)

import QtQuick

// Ref-keyed view of Bridge.signalModel. The model is keyed by row; a
// gauge wants "the RPM signal" by its ref (ADR-0057: `id:<n>` for core's
// def ID in the mock and obd2 sets and a file's extends base, `key:<k>`
// for a signal file's own def), whatever the set names it
// (Bridge.gaugeSignals). Each row is mirrored into an Entry whose
// required properties re-bind on the model's dataChanged, the same path
// LiveView's delegates use. A row whose ref is "" (uds-generic's, or any
// other set's) has no entry in byRef. A file's def that replaced a base
// one by name has a key and the base's ID (below the file's band), and is
// found by both: a cell that named the base def by ID still reads it.
// byRef is replaced, never mutated,
// so lookup() bindings re-run when rows come and go (a profile switch
// resets the model).
Instantiator {
    id: hub

    component Entry: QtObject {
        required property int id           // core's def ID
        required property string ref        // ADR-0057's ref, "" for none
        required property string name
        required property string label      // an enum value's label, "" none
        required property string unit       // as displayed, from core
        required property double value
        required property double windowHigh  // the last window's highest
        required property double windowLow   // and lowest
        required property int decimals      // core's decimal_places
        required property bool hasValue
        required property double ageMs   // -1 = never arrived
        required property bool hidden    // the ECU lacks it
        required property string refusedCode // the ECU's refusal ("7F 22 31"), "" none
        required property bool isText
        required property bool isEnum
        // The def's range as displayed; NaN for no range (test with isNaN)
        required property double rangeLow
        required property double rangeHigh
    }

    property var byRef: ({})

    // The ECU-set IDs below the file band (signal_sets.h's
    // ECUIO_USER_SIG_ID_BASE): a keyed def with one took a base def's place
    readonly property int fileIdBase: 30000

    // The refs an entry is found by: its own and, for a keyed def standing
    // in a base def's place, that def's "id:<n>"
    function refsOf(entry) {
        if (entry.ref === "") return []
        if (entry.ref.startsWith("key:") && entry.id < fileIdBase)
            return [entry.ref, "id:" + entry.id]
        return [entry.ref]
    }

    model: Bridge.signalModel
    delegate: Entry {}

    // The signal hands each object over as a QtObject: each is the
    // delegate's Entry, which says so to the type checker
    onObjectAdded: (index, object) => {
        const entry = object as Entry
        const refs = hub.refsOf(entry)
        if (refs.length === 0) return
        const m = Object.assign({}, hub.byRef)
        for (const ref of refs) m[ref] = entry
        hub.byRef = m
    }
    onObjectRemoved: (index, object) => {
        const entry = object as Entry
        const m = Object.assign({}, hub.byRef)
        let changed = false
        for (const ref of hub.refsOf(entry)) {
            if (hub.byRef[ref] !== entry) continue
            delete m[ref]
            changed = true
        }
        if (changed) hub.byRef = m
    }

    // First of `refs` the active signal set provides, else null
    function lookup(refs) {
        for (const ref of refs ?? []) {
            if (!Object.prototype.hasOwnProperty.call(byRef, ref)) continue
            const e = byRef[ref]
            if (e) return e
        }
        return null
    }
}
