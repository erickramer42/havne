// File/layer: app/qml/AlertSignalPicker.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.alerts, QmlTest.phone_* (test_qml)
// A signal for an alert: the rows given, found with Live's filter (ADR-0050), one button each

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: picker
    // Bridge.profileSignals rows: { ref, name, unit, group, ... }
    property var rows: []
    // Why the list is empty when no row is given
    property string emptyText: ""
    // The width a button may take: the page's, never this column's or its
    // flow's, which a hidden flow takes from its buttons
    property real room: 0
    signal picked(var row)
    spacing: Theme.spacing
    Layout.fillWidth: true

    // Every filter property shares one change signal, so reading active
    // re-filters on any change
    readonly property var shownRows: {
        const f = rowFilter
        const all = picker.rows
        if (!f.active)
            return all
        return all.filter(r => f.matches(r.name, r.unit, r.group, false))
    }

    SignalFilter { id: rowFilter }

    // The text alone: a Popup doesn't take the page's zoom, and Plotted
    // only means nothing to an alert
    SignalFilterBar {
        objectName: "alertSignalFilter"
        Layout.fillWidth: true
        filter: rowFilter
        filterButtonShown: false
    }
    Label {
        objectName: "alertSignalNone"
        visible: picker.shownRows.length === 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        text: picker.rows.length > 0 ? qsTr("No signal matches.") : picker.emptyText
    }
    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        Repeater {
            model: picker.shownRows
            ThemedButton {
                required property var modelData
                objectName: "alertPick_" + modelData.ref
                width: Math.min(implicitWidth, picker.room)
                text: modelData.unit !== "" && modelData.unit !== modelData.name
                      ? qsTr("%1 (%2)").arg(modelData.name).arg(modelData.unit) : modelData.name
                onClicked: picker.picked(modelData)
            }
        }
    }
}
