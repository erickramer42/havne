// File/layer: app/qml/RecordingFramesView.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)
// The frames of the part in view, read from frames.csv: the table's tools, the
// Frames | By ID switch and the two lists; one part of RecordingsPage, which
// holds the state

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// ---- The frames of the part in view, read from frames.csv ----
ColumnLayout {
    id: root
    objectName: "viewerFrames"
    required property RecordingsPage host
    property alias framesFilterPanel: framesFilterPanel
    property alias viewerFrameIds: viewerFrameIds
    property alias viewerFrameTable: viewerFrameTable
    visible: root.host.framesShown
    Layout.fillWidth: true
    Layout.fillHeight: root.host.chartMaximized || root.host.pinnedViewer
    spacing: Theme.spacing

    // The table's tools: here, or in the table's head on the pinned form
    ColumnLayout {
        id: framesToolsHome
        visible: framesTools.parent === framesToolsHome
        Layout.fillWidth: true
        // A layout fills by default: this one keeps its content's height
        Layout.fillHeight: false
        ColumnLayout {
            id: framesTools
            parent: root.host.currentHead && root.host.framesShown ? root.host.currentHead.toolsSlot
                                                         : framesToolsHome
            Layout.fillWidth: true
            spacing: Theme.spacing
            Flow {
                Layout.fillWidth: true
                spacing: Theme.spacing

                SegmentRow {
                    SegmentButton {
                        tapHeight: root.host.segmentHeight
                        objectName: "framesTable_frames"
                        text: qsTr("List")
                        selected: !root.host.framesById
                        onClicked: root.host.framesById = false
                    }
                    SegmentButton {
                        tapHeight: root.host.segmentHeight
                        objectName: "framesTable_byId"
                        text: qsTr("By ID")
                        selected: root.host.framesById
                        onClicked: {
                            root.host.framesById = true
                            root.host.viewFrames.ids.refresh()
                        }
                    }
                }
                ThemedButton {
                    objectName: "framesFilterToggle"
                    text: root.host.viewFrames.frames.filterTerms > 0
                          ? qsTr("Filter (%1)").arg(root.host.viewFrames.frames.filterTerms)
                          : qsTr("Filter")
                    checkable: true
                    checked: root.host.framesFilterOpen
                    onClicked: root.host.framesFilterOpen = !root.host.framesFilterOpen
                }
            }

            FrameFilterPanel {
                id: framesFilterPanel
                visible: root.host.framesFilterOpen
                frames: root.host.viewFrames.frames
            }

            // How many rows the part in view has, and when it has more than
            // the table holds, where the table begins
            Label {
                objectName: "framesWords"
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                wrapMode: Text.WordWrap
                color: root.host.viewFrames.error !== "" ? Theme.danger : Theme.textDim
                font.pixelSize: Theme.fontCaption
                text: root.host.viewFrames.error !== "" ? root.host.viewFrames.error
                      : root.host.viewFrames.loading ? qsTr("Reading the frames…")
                      : root.host.viewFrames.note !== "" ? root.host.viewFrames.note
                      : root.host.viewFrames.frames.filterTerms > 0
                        ? qsTr("Showing %1 of %2 frames").arg(root.host.viewFrames.frames.count)
                                                         .arg(root.host.viewFrames.frames.heldCount)
                        : root.host.viewFrames.rows === 1 ? qsTr("1 frame in view")
                                               : qsTr("%1 frames in view").arg(root.host.viewFrames.rows)
            }
        }
    }

    // A fixed height, the page scrolling and the table inside it;
    // maximized, what the page has left
    FrameTable {
        id: viewerFrameTable
        objectName: "viewerFrameTable"
        visible: !root.host.framesById
        Layout.preferredHeight: root.host.chartMaximized || root.host.pinnedViewer ? 0 : 280
        Layout.minimumHeight: root.host.pinnedViewer ? root.host.pinnedListMin : 0
        Layout.fillHeight: root.host.chartMaximized || root.host.pinnedViewer
        head: root.host.framesHeadComponent
        headScrolls: root.host.pinnedViewer
        live: false
        frames: root.host.viewFrames.frames
        emptyText: root.host.viewFrames.loading ? qsTr("Reading the frames…")
                 : root.host.viewFrames.frames.heldCount > 0 ? qsTr("No frame the filter shows")
                 : qsTr("No frame in view")
        // A tap puts the chart's cursor at the frame's time, whether
        // or not a signal has a sample there
        onRowClicked: (index, modifiers, timestampUs) => root.host.setCursor(timestampUs / 1000)
    }
    FrameIdList {
        id: viewerFrameIds
        objectName: "viewerFrameIds"
        visible: root.host.framesById
        Layout.preferredHeight: root.host.chartMaximized || root.host.pinnedViewer ? 0 : 280
        Layout.minimumHeight: root.host.pinnedViewer ? root.host.pinnedListMin : 0
        Layout.fillHeight: root.host.chartMaximized || root.host.pinnedViewer
        head: root.host.idsHeadComponent
        headScrolls: root.host.pinnedViewer
        ids: root.host.viewFrames.ids
        refreshing: root.host.framesById && root.host.framesShown
        emptyText: root.host.viewFrames.frames.heldCount > 0 ? qsTr("No frame the filter shows")
                                                   : qsTr("No frame in view")
        onIdTapped: (id, extended) => {
            framesFilterPanel.showId(id, extended)
            root.host.framesFilterOpen = true
            root.host.framesById = false
        }
    }
}
