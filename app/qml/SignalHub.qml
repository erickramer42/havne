// File/layer: app/qml/SignalHub.qml | Qt6 Quick, UI layer
// Testing: test_qml (tests/qml/tst_dashboard.qml): lookup by def ID,
//          values following the model, and a profile switch, alone and
//          under the Dashboard's gauges, a set whose defs are named
//          otherwise too

import QtQuick

// ID-keyed view of Bridge.signalModel. The model is keyed by row; a
// gauge wants "the RPM signal" by core's stable def ID, whatever the
// set names it (Bridge.gaugeSignals). Each row is mirrored into an
// Entry whose required properties re-bind on the model's dataChanged,
// the same path LiveView's delegates use. byId is replaced, never
// mutated, so lookup() bindings re-run when rows come and go (a
// profile switch resets the model).
Instantiator {
    id: hub

    component Entry: QtObject {
        required property int id           // core's def ID
        required property string name
        required property string unit       // as displayed, from core
        required property double value
        required property double windowHigh  // the last window's highest
        required property double windowLow   // and lowest
        required property int decimals      // core's decimal_places
        required property bool hasValue
        required property double ageMs   // -1 = never arrived
    }

    property var byId: ({})

    model: Bridge.signalModel
    delegate: Entry {}

    onObjectAdded: (index, object) => {
        const m = Object.assign({}, hub.byId)
        m[object.id] = object
        hub.byId = m
    }
    onObjectRemoved: (index, object) => {
        if (hub.byId[object.id] !== object) return
        const m = Object.assign({}, hub.byId)
        delete m[object.id]
        hub.byId = m
    }

    // First of `ids` the active signal set provides, else null
    function lookup(ids) {
        for (const id of ids ?? []) {
            const e = byId[id]
            if (e) return e
        }
        return null
    }
}
