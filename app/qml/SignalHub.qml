// File/layer: app/qml/SignalHub.qml | Qt6 Quick, UI layer
// Testing: manual smoke — dashboard gauges follow mock://demo, and
//          reconnecting under another profile rebinds them

import QtQuick

// Name-keyed view of QtTune.signalModel. The model is keyed by row;
// a gauge wants "the RPM signal", whichever profile named it. Each row
// is mirrored into an Entry whose required properties re-bind on the
// model's dataChanged, the same path LiveView's delegates use.
// byName is replaced, never mutated, so lookup() bindings re-run when
// rows come and go (a profile switch resets the model).
Instantiator {
    id: hub

    component Entry: QtObject {
        required property string name
        required property string unit
        required property double value
        required property bool hasValue
        required property double ageMs   // -1 = never arrived
    }

    property var byName: ({})

    model: QtTune.signalModel
    delegate: Entry {}

    onObjectAdded: (index, object) => {
        const m = Object.assign({}, hub.byName)
        m[object.name] = object
        hub.byName = m
    }
    onObjectRemoved: (index, object) => {
        if (hub.byName[object.name] !== object) return
        const m = Object.assign({}, hub.byName)
        delete m[object.name]
        hub.byName = m
    }

    // First of `names` the active signal set provides, else null
    function lookup(names) {
        for (const n of names) {
            const e = byName[n]
            if (e) return e
        }
        return null
    }

    function prettyUnit(u) {
        return u === "degC" ? "°C" : u === "degF" ? "°F" : u
    }
}
