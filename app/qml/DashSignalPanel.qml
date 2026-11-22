// File/layer: app/qml/DashSignalPanel.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard{,_pages,_panel}, QmlTest.dashedit, QmlTest.phone_*,
//          QmlTest.linkhealth, QmlTest.driveview{,_sheet,_platform} (test_qml); BENCH, Manual smoke

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
    // The kind the signal is for
    readonly property string kindNow: signalCard.host.addKind !== "" ? signalCard.host.addKind
                                      : signalCard.host.selectedCell ? signalCard.host.selectedCell.kind : ""
    // The listed profile's rows; every filter property shares one change
    // signal, so reading active re-filters on any change
    readonly property var offered: signalCard.host.addFlowPart.pickable(signalCard.kindNow)
    // The vehicle's picked sets' own signals: sections of their own, first
    readonly property var ownSets: signalCard.host.addFlowPart.pickedOwnRows(signalCard.kindNow)
    // A section's open state once the user has pressed its header
    property var toggled: ({})
    // Sections [{ key, title, rows, open, hiddenFrom }]: each picked set,
    // then the listed profile's other rows by group. The groups fold away
    // while a picked set is on top and nothing is typed in the filter
    readonly property var sections: {
        const f = rowFilter
        const filtering = f.active
        const out = []
        const taken = {}
        const hub = signalCard.host.signalHub
        const hide = ref => Bridge.connected && hub.lookup([ref])?.hidden === true
        const match = r => !filtering || f.matches(r.name, r.unit, r.group, false)
        for (let i = 0; i < ownSets.length; ++i) {
            for (const r of ownSets[i].rows)
                taken[r.ref] = true
            const rs = ownSets[i].rows.filter(match)
            if (rs.length > 0)
                out.push({ key: "set" + i, title: ownSets[i].title, rows: rs, hiddenFrom: -1,
                           open: signalCard.toggled["set" + i] ?? true })
        }
        const rest = offered.filter(r => !taken[r.ref]).filter(match)
        const byGroup = {}
        const order = []
        for (const r of rest) {
            const c = f.groupChoices([r.group])[0]
            if (byGroup[c.code] === undefined) {
                byGroup[c.code] = { key: "group" + c.code, title: c.name, rows: [] }
                order.push(c.code)
            }
            byGroup[c.code].rows.push(r)
        }
        const folded = ownSets.length > 0 && !filtering
        for (const c of order.sort((x, y) => x - y)) {
            const g = byGroup[c]
            // The ones the ECU doesn't send go last in their group, dim
            const sent = g.rows.filter(r => !hide(r.ref))
            g.hiddenFrom = sent.length < g.rows.length ? sent.length : -1
            g.rows = sent.concat(g.rows.filter(r => hide(r.ref)))
            g.open = signalCard.toggled[g.key] ?? !folded
            out.push(g)
        }
        return out
    }
    readonly property int shownCount: signalCard.sections.reduce((n, sec) => n + sec.rows.length, 0)
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
                textFormat: Text.PlainText
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
            readonly property var plan: signalCard.host.addKind !== "" ? signalCard.host.addFlowPart.addAllPlan(signalCard.host.addKind)
                                                             : ({ own: 0, missing: 0, adding: [] })
            visible: plan.own > 1 && plan.adding.length > 0
            Layout.fillWidth: true
            Layout.maximumWidth: implicitWidth
            text: signalCard.host.addFlowPart.addAllWords(plan)
            onClicked: signalCard.host.addFlowPart.addAllSignals()
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
            textFormat: Text.PlainText
            objectName: "signalNone"
            visible: signalCard.shownCount === 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: signalCard.offered.length > 0 || signalCard.ownSets.length > 0 ? qsTr("No signal matches.")
                : Bridge.connected ? qsTr("This session's profile has no signal a gauge can show.")
                : qsTr("No signals: connect, or pick a plugin for your vehicle under Settings > Signals.")
        }
        Repeater {
            model: signalCard.sections
            ColumnLayout {
                id: section
                required property var modelData
                Layout.fillWidth: true
                spacing: 8
                ThemedButton {
                    objectName: "signalSection_" + section.modelData.key
                    flat: true
                    Layout.fillWidth: true
                    Layout.maximumWidth: implicitWidth
                    checkable: true
                    checked: section.modelData.open
                    text: qsTr("%1 (%2)").arg(section.modelData.title).arg(section.modelData.rows.length)
                    Accessible.name: section.modelData.open
                        ? qsTr("%1: %2 signals, shown").arg(section.modelData.title).arg(section.modelData.rows.length)
                        : qsTr("%1: %2 signals, folded").arg(section.modelData.title).arg(section.modelData.rows.length)
                    onClicked: {
                        const next = Object.assign({}, signalCard.toggled)
                        next[section.modelData.key] = !section.modelData.open
                        signalCard.toggled = next
                    }
                }
                Flow {
                    objectName: "signalFlow_" + section.modelData.key
                    visible: section.modelData.open
                    Layout.fillWidth: true
                    spacing: 8
                    Repeater {
                        model: section.modelData.open ? section.modelData.rows : []
                        ThemedButton {
                            id: chip
                            required property var modelData
                            required property int index
                            objectName: "pickSignal_" + modelData.ref
                            // Dim: the ECU doesn't send it (a group's last ones)
                            opacity: section.modelData.hiddenFrom >= 0 && chip.index >= section.modelData.hiddenFrom ? 0.5 : 1
                            text: modelData.unit !== "" && modelData.unit !== modelData.name ? qsTr("%1 (%2)").arg(modelData.name).arg(modelData.unit)
                                                        : modelData.name
                            onClicked: signalCard.host.addFlowPart.pickSignal(modelData)
                        }
                    }
                }
            }
        }
    }
}
