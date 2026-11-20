// File/layer: app/qml/RecordingSpanStats.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)
// Each plotted signal's statistics over the span between the cursors, a table
// on the desktop and a grid of cards on a phone: one part of RecordingsPage,
// which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// Each plotted signal over the span: a table on the desktop, a
// grid of cards on a phone, whose rate the span line's length gives
ColumnLayout {
    id: root
    objectName: "spanStats"
    required property RecordingsPage host
    readonly property bool shown: root.host.spanRows.length > 0 && !root.host.chartMaximized
                                  && !root.host.framesShown && !root.host.aboutShown
    parent: root.host.pinnedViewer ? root.host.spanStatsPinned : root.host.spanStatsHome
    visible: shown
    Layout.fillWidth: true
    spacing: 0
    component StatCell: Label {
        // Shares, not widths: a width read from the page would
        // make the column lay itself out again as it sizes
        Layout.fillWidth: true
        Layout.preferredWidth: 10
        horizontalAlignment: Text.AlignRight
        elide: Text.ElideRight
        color: Theme.text
        font.family: Theme.valueFamily
        font.features: Theme.valueFeatures
    }
    RowLayout {
        visible: !root.host.phoneShape
        Layout.fillWidth: true
        Layout.leftMargin: 8
        Layout.rightMargin: 12
        Label { text: qsTr("Signal"); color: Theme.textDim; Layout.fillWidth: true; Layout.preferredWidth: 30 }
        StatCell { text: qsTr("Change"); color: Theme.textDim }
        StatCell { text: qsTr("Low"); color: Theme.textDim }
        StatCell { text: qsTr("High"); color: Theme.textDim }
        StatCell { text: qsTr("Mean"); color: Theme.textDim }
        StatCell { text: qsTr("Rate"); color: Theme.textDim }
    }
    Repeater {
        model: root.host.phoneShape ? [] : root.host.spanRows
        delegate: RowLayout {
            id: statRow
            required property var modelData
            readonly property var stat: root.host.spanStats[modelData.index]
            objectName: "statRow_" + modelData.index
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 12
            Label {
                text: statRow.modelData.name
                color: Theme.text
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.preferredWidth: 30
            }
            StatCell { objectName: "statChange"; text: root.host.textsPart.statChange(statRow.stat) }
            StatCell {
                objectName: "statLow"
                text: statRow.stat ? root.host.textsPart.statText(statRow.stat.lowest, statRow.stat) : qsTr("—")
            }
            StatCell {
                objectName: "statHigh"
                text: statRow.stat ? root.host.textsPart.statText(statRow.stat.highest, statRow.stat) : qsTr("—")
            }
            StatCell {
                objectName: "statMean"
                text: statRow.stat ? root.host.textsPart.statText(statRow.stat.mean, statRow.stat) : qsTr("—")
            }
            StatCell { objectName: "statRate"; text: root.host.textsPart.statRate(statRow.stat) }
        }
    }
    GridLayout {
        visible: root.host.phoneShape
        Layout.fillWidth: true
        columns: 2
        columnSpacing: 4
        rowSpacing: 4
        Repeater {
            model: root.host.phoneShape ? root.host.spanRows : []
            delegate: Rectangle {
                id: statCard
                required property var modelData
                readonly property var stat: root.host.spanStats[modelData.index]
                objectName: "statCard_" + modelData.index
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                // Taller where the three values take more than one line, and
                // as tall as the card beside it
                Layout.preferredHeight: Math.max(Theme.touchTarget, cardColumn.implicitHeight + 8)
                Layout.fillHeight: true
                color: Theme.surface
                radius: 4
                border.color: Theme.hairline
                border.width: 1
                ColumnLayout {
                    id: cardColumn
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    spacing: 0
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: statCard.modelData.name
                            color: Theme.textDim
                            elide: Text.ElideRight
                            font.pixelSize: Theme.fontCaption
                            Layout.fillWidth: true
                        }
                        Label {
                            objectName: "statChange"
                            text: root.host.textsPart.statChange(statCard.stat)
                            color: Theme.text
                            font.pixelSize: Theme.fontCaption
                            font.family: Theme.valueFamily
                            font.features: Theme.valueFeatures
                        }
                    }
                    Label {
                        objectName: "statLine"
                        Layout.fillWidth: true
                        // At most a value to a line: a narrow card cuts none
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        color: Theme.textDim
                        font.pixelSize: Theme.fontCaption
                        font.family: Theme.valueFamily
                        font.features: Theme.valueFeatures
                        text: !statCard.stat ? qsTr("—")
                              : [statCard.stat.lowest, statCard.stat.highest, statCard.stat.mean]
                                .map(v => isFinite(v) ? Number(v).toLocaleString(Qt.locale(), 'f', statCard.stat.decimals)
                                                      : qsTr("—")).join(" · ")
                    }
                }
            }
        }
    }
}
