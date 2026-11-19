// File/layer: app/qml/RecordingSignalRows.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)
// The recording's signals, a row each with its swatch, tick and readouts:
// one part of RecordingsPage, which holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The recording's signals: a tick plots one, and beside each its
// value at the cursor
ListView {
    id: root
    objectName: "signalRows"
    required property RecordingsPage host
    visible: root.host.recording.state === RecordingView.Open && !root.host.chartMaximized && !root.host.framesShown
             && !root.host.aboutShown
    Layout.fillWidth: true
    // On the pinned form the rest of the page, its head included
    Layout.preferredHeight: root.host.pinnedViewer ? 0 : count * root.host.rowHeight
    Layout.minimumHeight: root.host.pinnedViewer ? root.host.pinnedListMin : 0
    Layout.maximumHeight: root.host.pinnedViewer ? Number.POSITIVE_INFINITY
                                            : Math.max(root.host.rowHeight, root.host.column.height * 0.3)
    Layout.fillHeight: root.host.pinnedViewer
    clip: true
    currentIndex: -1
    highlightFollowsCurrentItem: false
    boundsBehavior: Flickable.StopAtBounds
    model: root.host.shownSignals
    header: Component {
        ViewHead {
            host: root.host
            role: "values"
            // The pinned list's scrollbar lies over its right edge: the head keeps clear
            width: root.width - (root.host.pinnedViewer ? 12 : 0)
            list: root
        }
    }
    footer: Component {
        ColumnLayout {
            id: valuesFootItem
            width: root.width
            Item { implicitHeight: 6; visible: root.host.filterCount.parent === valuesFootItem && root.host.filterCount.shown }
            Component.onCompleted: root.host.valuesFoot = valuesFootItem
        }
    }
    ScrollBar.vertical: ScrollBar {
        policy: root.host.pinnedViewer && root.contentHeight > root.height
                ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
    }

    delegate: Rectangle {
        id: signalRow
        required property var modelData
        required property int index
        readonly property int signalIndex: modelData.index
        readonly property var at: root.host.readoutFrom[signalIndex]
        width: root.width
        height: root.host.rowHeight
        color: index % 2 === 0 ? Theme.surface : Theme.bg

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 12
            ThemedCheckBox {
                objectName: "plotCheck"
                checked: root.host.plotted[signalRow.signalIndex] === true
                enabled: !signalRow.modelData.isText
                onToggled: root.host.cursorsPart.togglePlotted(signalRow.signalIndex)
                Accessible.name: qsTr("Plot %1").arg(signalRow.modelData.name)
            }
            Rectangle {
                objectName: "swatch"
                readonly property bool shown: root.host.plotted[signalRow.signalIndex] === true
                implicitWidth: 10; implicitHeight: 10
                radius: 2
                color: shown ? Theme.seriesColor(root.host.panesPart.slotOf(signalRow.signalIndex)) : "transparent"
                border.width: 1
                border.color: shown ? Theme.seriesColor(root.host.panesPart.slotOf(signalRow.signalIndex))
                                    : Theme.textDim
                opacity: shown ? 1.0 : 0.5
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: signalRow.modelData.name
                        color: Theme.text
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        objectName: "cursorValue"
                        text: root.host.atText(signalRow.at, signalRow.modelData)
                        Layout.maximumWidth: Math.round(signalRow.width * (root.host.compareAligned ? 0.3 : 0.5))
                        elide: Text.ElideRight
                        color: Theme.text
                        font.family: Theme.valueFamily
                        font.features: Theme.valueFeatures
                    }
                }
                // On a phone B and the change have a line of their own
                Label {
                    objectName: "cursorSpanLine"
                    visible: root.host.phoneShape && root.host.hasSpan
                    text: visible ? root.host.spanCaption(signalRow.modelData) : ""
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideLeft
                    color: Theme.textDim
                    font.pixelSize: Theme.fontCaption
                    font.family: Theme.valueFamily
                    font.features: Theme.valueFeatures
                }
            }
            // On the desktop, two more columns
            Label {
                objectName: "cursorBValue"
                visible: !root.host.phoneShape && root.host.hasSpan
                text: visible ? root.host.atText(root.host.readoutTo[signalRow.signalIndex], signalRow.modelData) : ""
                Layout.preferredWidth: Math.round(signalRow.width * 0.18)
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                color: Theme.text
                font.family: Theme.valueFamily
                font.features: Theme.valueFeatures
            }
            Label {
                objectName: "cursorDelta"
                visible: !root.host.phoneShape && root.host.hasSpan
                text: visible && !signalRow.modelData.isText
                      ? root.host.statChange(root.host.spanStats[signalRow.signalIndex]) : ""
                Layout.preferredWidth: Math.round(signalRow.width * 0.18)
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                color: Theme.textDim
                font.family: Theme.valueFamily
                font.features: Theme.valueFeatures
            }
            // Which pane the signal is drawn in
            ThemedButton {
                id: paneButton
                objectName: "paneButton"
                visible: root.host.plotted[signalRow.signalIndex] === true && !signalRow.modelData.isText
                flat: true
                compact: true
                text: qsTr("Pane")
                Layout.preferredHeight: Theme.touchTarget
                Layout.alignment: Qt.AlignVCenter
                Accessible.name: qsTr("Pane of %1: %2").arg(signalRow.modelData.name)
                                     .arg(root.host.panesPart.paneLabelOf(signalRow.signalIndex))
                onClicked: root.host.panesPart.openPaneMenu(signalRow.signalIndex, paneButton)
            }
            // The compared recording's, at the cursor
            Label {
                objectName: "compareValue"
                readonly property var at: root.host.compareReadout[signalRow.signalIndex]
                visible: root.host.compareAligned
                text: !at || at.atMs < 0 ? qsTr("—")
                      : signalRow.modelData.isText ? at.text
                      : at.label ? at.label
                      : root.host.valueText(Number(at.text), signalRow.modelData.decimals, at.unit)
                Layout.maximumWidth: Math.round(signalRow.width * 0.3)
                elide: Text.ElideRight
                color: Theme.textDim
                font.family: Theme.valueFamily
                font.features: Theme.valueFeatures
            }
        }
    }
}
