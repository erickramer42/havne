// File/layer: app/qml/DashAddPanel.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit, QmlTest.phone,
//          QmlTest.linkhealth, QmlTest.driveview (test_qml); BENCH, Manual smoke

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The Dashboard's Add panel: the kinds a new cell can be, each with a
// still preview, which the page's panelHost loads

GaugeCard {
    id: addCard
    required property DashboardPage host
    objectName: "dashAddPanel"
    readonly property real touchMin: Theme.touchTarget
    implicitHeight: addColumn.implicitHeight + 2 * padding
    padding: 12
    ColumnLayout {
        id: addColumn
        width: parent.width
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                font.pixelSize: Theme.fontBody
                font.bold: true
                color: Theme.text
                text: addCard.host.changingKind ? qsTr("Change this gauge's kind")
                                        : qsTr("Add a gauge: its kind")
            }
            ThemedButton {
                objectName: "addClose"
                text: addCard.host.changingKind ? qsTr("Back") : qsTr("Close")
                onClicked: {
                    if (addCard.host.changingKind) {
                        addCard.host.addFlowPart.leaveKindList()
                        return
                    }
                    addCard.host.commitTyping()
                    addCard.host.panel = ""
                }
            }
        }
        Flow {
            Layout.fillWidth: true
            spacing: 8
            Repeater {
                // A blank has no signal to keep, so a change never offers it
                model: addCard.host.changingKind ? addCard.host.drawnKinds.filter(k => k !== "blank")
                                         : addCard.host.drawnKinds
                ColumnLayout {
                    id: kindTile
                    required property string modelData
                    // Comparisons, not an indexOf on an array literal: the QML
                    // compiler's C++ for that narrows qsizetype to int, which
                    // Xcode's -Wshorten-64-to-32 rejects
                    readonly property bool wide: modelData === "bar"
                                                 || modelData === "shift-light"
                                                 || modelData === "segments"
                                                 || modelData === "sparkline"
                    spacing: 4
                    Loader {
                        objectName: "preview_" + kindTile.modelData
                        Layout.preferredWidth: kindTile.wide ? 200 : 120
                        Layout.preferredHeight: kindTile.modelData === "shift-light" ? 40
                                              : kindTile.modelData === "segments"
                                                || kindTile.modelData === "sparkline" ? 100
                                              : kindTile.wide || kindTile.modelData === "seven-segment" ? 70
                                              : 120
                        sourceComponent: addCard.host.kindComponent(kindTile.modelData)
                        // Still: 60 percent of a 0 to 100 range, never live
                        onLoaded: {
                            const g = item as GaugeBase
                            g.from = 0
                            g.to = 100
                            g.hasValue = true
                            g.value = 60
                            g.label = addCard.host.kindNames[kindTile.modelData]
                            // A sparkline's still is a made-up trace, and it
                            // reads nothing (no source)
                            if (kindTile.modelData === "sparkline") {
                                const spark = item as SparkGauge
                                spark.shown = false
                                spark.windowS = 12   // 120 points at 10 Hz fill the box
                                const still = []
                                for (let i = 0; i < 120; ++i)
                                    still.push(i * 100, 55 + 25 * Math.sin(i / 10))
                                spark.samples = still
                            }
                        }
                    }
                    ThemedButton {
                        objectName: "addKind_" + kindTile.modelData
                        Layout.fillWidth: true
                        text: addCard.host.kindNames[kindTile.modelData]
                        // The cell's own kind is marked, and no change to it
                        readonly property bool current: addCard.host.changingKind && addCard.host.selectedCell
                                                        && addCard.host.selectedCell.kind === kindTile.modelData
                        checkable: current
                        checked: current
                        onClicked: addCard.host.addFlowPart.pickKind(kindTile.modelData)
                    }
                }
            }
        }
    }
}
