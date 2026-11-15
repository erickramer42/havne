// File/layer: app/qml/DashSignalPanel.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit, QmlTest.phone,
//          QmlTest.linkhealth, QmlTest.driveview (test_qml); BENCH, Manual smoke

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The Dashboard's signal panel: a signal for Add's kind or the edited
// cell, which the page's panelHost loads

GaugeCard {
    id: signalCard
    required property DashboardPage host
    objectName: "dashSignalPanel"
    readonly property real touchMin: Theme.touchTarget
    // The rows the bar leaves: every filter property shares one
    // change signal, so reading active re-filters on any change
    readonly property var offered: signalCard.host.pickable(signalCard.host.addKind !== "" ? signalCard.host.addKind
                                                 : signalCard.host.selectedCell ? signalCard.host.selectedCell.kind : "")
    readonly property var rows: {
        const f = rowFilter
        const all = offered
        if (!f.active)
            return all
        return all.filter(r => f.matches(r.name, r.unit, r.group, false))
    }
    implicitHeight: signalColumn.implicitHeight + 2 * padding
    padding: 12
    SignalFilter { id: rowFilter }
    ColumnLayout {
        id: signalColumn
        width: parent.width
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                font.pixelSize: Theme.fontBody
                font.bold: true
                color: Theme.text
                elide: Text.ElideRight
                text: signalCard.host.addKind !== ""
                      ? qsTr("Add a gauge: a signal for the %1").arg(signalCard.host.kindNames[signalCard.host.addKind])
                      : qsTr("A signal for this gauge")
            }
            ThemedButton {
                objectName: "signalBack"
                text: qsTr("Back")
                onClicked: signalCard.host.panel = signalCard.host.addKind !== "" ? "add" : "cell"
            }
        }
        // Every signal of the listed set at once, with Add's kind
        ThemedButton {
            objectName: "addAllSignals"
            readonly property var plan: signalCard.host.addKind !== "" ? signalCard.host.addAllPlan(signalCard.host.addKind)
                                                             : ({ own: 0, missing: 0, adding: [] })
            visible: plan.own > 1 && plan.adding.length > 0
            Layout.fillWidth: true
            Layout.maximumWidth: implicitWidth
            text: signalCard.host.addAllWords(plan)
            onClicked: signalCard.host.addAllSignals()
        }
        // The text alone: a Popup doesn't take the page's zoom, and
        // Plotted only means nothing to a gauge
        SignalFilterBar {
            objectName: "signalFilter"
            Layout.fillWidth: true
            filter: rowFilter
            filterButtonShown: false
        }
        Label {
            objectName: "signalNone"
            visible: signalCard.rows.length === 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: signalCard.offered.length > 0 ? qsTr("No signal matches.")
                : Bridge.connected ? qsTr("This session's profile has no signal a gauge can show.")
                : qsTr("No signals: connect, or pick a plugin for your vehicle under Settings > Signals.")
        }
        Flow {
            Layout.fillWidth: true
            spacing: 8
            Repeater {
                model: signalCard.rows
                ThemedButton {
                    required property var modelData
                    objectName: "pickSignal_" + modelData.ref
                    text: modelData.unit !== "" && modelData.unit !== modelData.name ? qsTr("%1 (%2)").arg(modelData.name).arg(modelData.unit)
                                                : modelData.name
                    onClicked: signalCard.host.pickSignal(modelData)
                }
            }
        }
    }
}
