// File/layer: app/qml/DiagnosticsPage.qml | Qt6 Quick, UI layer
// Testing: tests/qml/tst_diagnostics.qml (the mock's report: the MIL,
//          each list's codes, the monitors, the freeze frame's values in
//          either unit system, Read again, ?diagnose=0 and the empty
//          state), which finds items by objectName; the entries
//          themselves by test_bridge (BridgeTest.DiagnosticsEntry*); the
//          look, manually (the run-havne skill)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// What each ECU reports of its emissions diagnostics (ADR-0037): the
// check-engine light, the stored, pending and permanent trouble codes,
// the readiness monitors and the freeze frame. Bridge.diagnostics holds
// one entry per ECU, its codes already named by core; the page formats
// nothing but the values' decimals, as Live does. Reading only: Havne
// has no way to clear a code.
Page {
    id: root
    background: Rectangle { color: Theme.bg }

    // Settings owns connecting; Main opens it
    signal connectRequested()

    readonly property var ecus: Bridge.diagnostics
    // Read again needs a session and a read that has settled
    readonly property bool canReadAgain: Bridge.connected &&
        ecus.some(e => e.asked && e.complete)

    function statusText() {
        if (Bridge.connecting) return qsTr("Connecting…")
        if (Bridge.disconnecting) return qsTr("Disconnecting…")
        if (!Bridge.connected)
            return ecus.length > 0 ? qsTr("Disconnected: the last read is shown")
                                   : qsTr("Connect a session to read trouble codes")
        if (ecus.length === 0)
            return qsTr("This session asks the vehicle nothing, so it reads no codes")
        return qsTr("Read at connect · Havne reads codes and can't clear them")
    }

    function ecuState(e) {
        if (!e.asked) return qsTr("Not read: diagnose=0, poll= or a profile that asks nothing")
        if (!e.complete) return qsTr("Reading…")
        return qsTr("Read")
    }

    function milText(e) {
        switch (e.readiness) {
        case "answered":
            if (!e.mil) return qsTr("Check-engine light off")
            return e.dtcCount === 1 ? qsTr("Check-engine light on · 1 confirmed code")
                                    : qsTr("Check-engine light on · %1 confirmed codes").arg(e.dtcCount)
        case "refused":    return qsTr("Readiness refused (%1)").arg(e.readinessNrc)
        case "unanswered": return qsTr("Readiness: no answer")
        default:           return qsTr("Reading…")
        }
    }

    function milColor(e) {
        if (e.readiness !== "answered") return Theme.textDim
        return e.mil ? Theme.danger : Theme.success
    }

    function listTitle(key) {
        switch (key) {
        case "stored":  return qsTr("Stored")
        case "pending": return qsTr("Pending")
        default:        return qsTr("Permanent")
        }
    }

    function listHint(key) {
        switch (key) {
        case "stored":  return qsTr("confirmed faults")
        case "pending": return qsTr("seen once, not yet confirmed")
        default:        return qsTr("cleared only by the ECU, once its monitor passes")
        }
    }

    // A list's codes, or what became of its request
    function codesText(list) {
        switch (list.outcome) {
        case "answered":
            if (list.codes.length === 0) return qsTr("None")
            return list.more > 0 ? list.codes.join("  ") + "  " + qsTr("and %1 more").arg(list.more)
                                 : list.codes.join("  ")
        case "refused":    return qsTr("Refused (%1)").arg(list.nrc)
        case "unanswered": return qsTr("No answer")
        default:           return qsTr("Not asked yet")
        }
    }

    function freezeText(f) {
        switch (f.outcome) {
        case "answered":
            return f.stored ? qsTr("Stored by %1").arg(f.code) : qsTr("None stored")
        case "refused":    return qsTr("Refused (%1)").arg(f.nrc)
        case "unanswered": return qsTr("No answer")
        default:           return qsTr("Not asked yet")
        }
    }

    function readinessSummary(e) {
        const done = e.monitors.filter(m => m.complete).length
        return qsTr("%1 of %2 complete").arg(done).arg(e.monitors.length)
    }

    // The monitors' and values' rows stop here, so a state or a value
    // stays near its name on a wide window
    readonly property int rowsMaxWidth: 480

    // Value plus unit at the def's decimal places, as Live formats them
    function valueText(v, decimals, unit) {
        const n = Number(v).toLocaleString(Qt.locale(), 'f', decimals)
        return unit !== "" ? n + " " + unit : n
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: 12

        PageHeader {
            title: qsTr("Diagnostics")
            Item { Layout.fillWidth: true }
            ThemedButton {
                objectName: "readAgain"
                visible: Bridge.connected
                enabled: root.canReadAgain
                text: qsTr("Read again")
                onClicked: Bridge.readDiagnostics()
            }
        }

        // Status line; disconnected, the empty state with a way to connect
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Label {
                objectName: "diagnosticsStatus"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: root.statusText()
                color: Theme.textDim
            }
            ThemedButton {
                visible: !Bridge.connected && !Bridge.connecting && !Bridge.disconnecting
                highlighted: true
                text: qsTr("Connect…")
                onClicked: root.connectRequested()
            }
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: cards.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                policy: flick.contentHeight > flick.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
            }

            ColumnLayout {
                id: cards
                width: flick.width - 12
                spacing: 12

                Repeater {
                    model: root.ecus
                    delegate: EcuCard {}
                }
            }
        }
    }

    // One ECU's report
    component EcuCard: Rectangle {
        id: card
        required property var modelData
        readonly property var ecu: modelData

        objectName: "ecu_" + ecu.ecu
        Layout.fillWidth: true
        implicitHeight: body.implicitHeight + 2 * Theme.pageMargin
        radius: Theme.radius
        color: Theme.surface

        ColumnLayout {
            id: body
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing
                Label {
                    text: qsTr("ECU %1").arg(card.ecu.ecu)
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    color: Theme.text
                }
                Label {
                    objectName: "ecuState"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.ecuState(card.ecu)
                    color: Theme.textDim
                }
            }

            ColumnLayout {
                visible: card.ecu.asked
                Layout.fillWidth: true
                spacing: 10

                // The check-engine light (the MIL), from readiness
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing
                    Rectangle {
                        implicitWidth: 12
                        implicitHeight: 12
                        radius: 6
                        color: root.milColor(card.ecu)
                    }
                    Label {
                        objectName: "milLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: root.milText(card.ecu)
                        color: Theme.text
                    }
                }

                SectionLabel { text: qsTr("Trouble codes") }
                Repeater {
                    model: card.ecu.lists
                    delegate: ColumnLayout {
                        id: listRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: root.listTitle(listRow.modelData.key) + "  ·  " +
                                  root.listHint(listRow.modelData.key)
                            color: Theme.textDim
                            font.pixelSize: 12
                        }
                        Label {
                            objectName: "codes_" + listRow.modelData.key
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: root.codesText(listRow.modelData)
                            font.family: listRow.modelData.codes.length > 0 ? Theme.monoFamily : ""
                            font.pixelSize: listRow.modelData.codes.length > 0 ? 16 : 14
                            color: listRow.modelData.codes.length > 0 ? Theme.text : Theme.textDim
                        }
                    }
                }

                SectionLabel {
                    text: card.ecu.readiness === "answered"
                          ? qsTr("Readiness monitors · %1").arg(root.readinessSummary(card.ecu))
                          : qsTr("Readiness monitors")
                }
                Label {
                    visible: card.ecu.readiness === "answered" && card.ecu.monitors.length === 0
                    text: qsTr("The ECU reports none")
                    color: Theme.textDim
                }
                ColumnLayout {
                    objectName: "monitors"
                    Layout.fillWidth: true
                    Layout.maximumWidth: root.rowsMaxWidth
                    spacing: 2
                    Repeater {
                        model: card.ecu.monitors
                        delegate: RowLayout {
                            id: monitorRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: Theme.spacing
                            Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                text: monitorRow.modelData.name
                                color: Theme.text
                            }
                            Label {
                                objectName: "monitorState"
                                text: monitorRow.modelData.complete ? qsTr("Complete")
                                                                    : qsTr("Incomplete")
                                color: monitorRow.modelData.complete ? Theme.success
                                                                     : Theme.warning
                            }
                        }
                    }
                }

                SectionLabel { text: qsTr("Freeze frame") }
                Label {
                    objectName: "freezeLabel"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.freezeText(card.ecu.freeze)
                    color: card.ecu.freeze.stored ? Theme.text : Theme.textDim
                }
                ColumnLayout {
                    objectName: "freezeValues"
                    Layout.fillWidth: true
                    Layout.maximumWidth: root.rowsMaxWidth
                    spacing: 2
                    Repeater {
                        model: card.ecu.freeze.values
                        delegate: RowLayout {
                            id: valueRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: Theme.spacing
                            Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                text: valueRow.modelData.name
                                color: Theme.text
                            }
                            Label {
                                objectName: "freezeValue"
                                text: root.valueText(valueRow.modelData.value,
                                                     valueRow.modelData.decimals,
                                                     valueRow.modelData.unit)
                                font.family: Theme.monoFamily
                                color: Theme.text
                            }
                        }
                    }
                }
            }
        }
    }

    component SectionLabel: Label {
        Layout.fillWidth: true
        Layout.topMargin: 4
        font.pixelSize: 13
        font.weight: Font.DemiBold
        color: Theme.textDim
    }
}
