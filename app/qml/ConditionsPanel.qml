// File/layer: app/qml/ConditionsPanel.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

pragma ComponentBehavior: Bound

// The viewer's Conditions panel (ADR-0063 section 10): each entry of the
// recording's conditionSpans, an ON span or a stretch with no new data
// while OFF, by its start. A row asks the page to show it; the page owns
// the view, the cursor and the pick, and draws the entries on the chart.
ColumnLayout {
    id: panel
    spacing: Theme.spacing

    // The page's plain copy of RecordingView.conditionSpans, and
    // RecordingView.info
    property var spans: []
    property var info: ({})
    // The page's words for a time and a length
    property var timeText: function(ms) { return "" }
    property var lengthText: function(ms) { return "" }
    // The entry last shown, -1 for none
    property int picked: -1
    signal showRequested(int index)
    signal closeRequested()

    // An entry's color: its level's alert role while ON, else textDim
    function colorOf(span) {
        if (!span.on)
            return Theme.textDim
        return span.level === 2 ? Theme.alertDangerFill
             : span.level === 1 ? Theme.alertWarnFill : Theme.textDim
    }
    function levelWord(span) {
        return span.level === 2 ? qsTr("danger") : span.level === 1 ? qsTr("warning") : ""
    }
    // How long it lasted, in the viewer's words: "ON for 10 s", "no new
    // data for 3.1 s", with where it meets the recording's edges
    function lengthOf(span) {
        const length = panel.lengthText(span.toMs - span.fromMs)
        if (span.on && span.heldStart && span.openEnd)
            return qsTr("ON for the whole recording")
        if (span.on && span.heldStart)
            return qsTr("ON from the start for %1").arg(length)
        const words = span.on ? qsTr("ON for %1").arg(length) : qsTr("no new data for %1").arg(length)
        return span.openEnd ? qsTr("%1, to the end").arg(words) : words
    }
    // The line over the list when it doesn't hold every entry, a span's
    // stretches with no new data are drawn as one to its end, or the
    // recorder lost events
    function cutText() {
        const shown = info.conditionsCut === true
            ? qsTr("Showing the first %1: the recording holds more condition events than the viewer reads")
              .arg(spans.length)
            : info.conditionSpansCut === true
              ? qsTr("Showing the first %1 of %2").arg(spans.length).arg(info.conditionSpanCount)
              : ""
        const parts = info.conditionPartsCut === true
            ? qsTr("Past %1 stretches with no new data, a span's later ones are drawn as one to its end")
              .arg(info.conditionBound)
            : ""
        const lost = info.conditionEventsLost === 1
            ? qsTr("1 condition event was lost while recording: an entry may be missing, or run on past its end")
            : info.conditionEventsLost > 0
              ? qsTr("%1 condition events were lost while recording: an entry may be missing, or run on past its end")
                .arg(info.conditionEventsLost)
              : ""
        return [shown, parts, lost].filter(t => t !== "").join(". ")
    }

    RowLayout {
        spacing: Theme.spacing
        Layout.fillWidth: true
        Label {
            objectName: "conditionsTitle"
            Layout.fillWidth: true
            text: qsTr("Conditions")
            color: Theme.text
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
        ThemedButton {
            objectName: "conditionsClose"
            compact: Theme.compact
            flat: true
            text: qsTr("Close")
            onClicked: panel.closeRequested()
        }
    }

    Label {
        textFormat: Text.PlainText
        objectName: "conditionsCut"
        visible: text !== ""
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
        text: panel.cutText()
    }

    ListView {
        id: list
        objectName: "conditionList"
        readonly property real rowH: Math.max(32, Theme.touchTarget)
        Layout.fillWidth: true
        // The desktop's list shows six rows and scrolls; a phone's fills
        // what the page has left
        Layout.preferredHeight: Theme.compact ? -1 : Math.min(count, 6) * rowH
        Layout.minimumHeight: Theme.compact ? rowH * 2 : 0
        Layout.fillHeight: Theme.compact
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: panel.spans
        ScrollBar.vertical: ScrollBar {
            policy: list.contentHeight > list.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
        }

        delegate: Rectangle {
            id: entry
            required property var modelData
            required property int index
            width: list.width
            // A phone's row holds the name on up to two lines, the level
            // and the length under it
            height: Theme.compact ? Math.max(list.rowH, rowGrid.implicitHeight + 8) : list.rowH
            color: area.hovered || panel.picked === entry.index ? Theme.elevatedLight : "transparent"
            AbstractButton {
                id: area
                objectName: "conditionRowArea"
                anchors.fill: parent
                hoverEnabled: true
                Accessible.name: qsTr("Show %1, %2").arg(entry.modelData.name).arg(panel.lengthOf(entry.modelData))
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                onClicked: panel.showRequested(entry.index)
            }
            GridLayout {
                id: rowGrid
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                columnSpacing: Theme.spacing
                rowSpacing: 0
                // The chart's mark: solid while ON, dashed with no new data
                Row {
                    objectName: "conditionSwatch"
                    Layout.row: 0
                    Layout.column: 0
                    spacing: entry.modelData.on ? 0 : 2
                    Repeater {
                        model: entry.modelData.on ? 1 : 2
                        delegate: Rectangle {
                            implicitWidth: entry.modelData.on ? 10 : 4
                            implicitHeight: 4
                            color: panel.colorOf(entry.modelData)
                        }
                    }
                }
                Label {
                    textFormat: Text.PlainText
                    objectName: "conditionTime"
                    Layout.row: 0
                    Layout.column: 1
                    text: panel.timeText(entry.modelData.fromMs)
                    color: Theme.textDim
                    font.family: Theme.valueFamily
                    font.features: Theme.valueFeatures
                }
                Label {
                    textFormat: Text.PlainText
                    objectName: "conditionName"
                    Layout.row: 0
                    Layout.column: 2
                    text: entry.modelData.name
                    color: Theme.text
                    wrapMode: Theme.compact ? Text.Wrap : Text.NoWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 48
                }
                RowLayout {
                    Layout.row: Theme.compact ? 1 : 0
                    Layout.column: Theme.compact ? 2 : 3
                    Layout.fillWidth: Theme.compact
                    spacing: Theme.spacing
                    Label {
                        textFormat: Text.PlainText
                        objectName: "conditionLevel"
                        visible: text !== ""
                        text: panel.levelWord(entry.modelData)
                        color: Theme.textDim
                        font.pixelSize: Theme.fontCaption
                    }
                    Label {
                        textFormat: Text.PlainText
                        objectName: "conditionLength"
                        text: panel.lengthOf(entry.modelData)
                        color: Theme.textDim
                        font.pixelSize: Theme.fontCaption
                        elide: Text.ElideRight
                        Layout.fillWidth: Theme.compact
                        Layout.maximumWidth: Theme.compact ? Number.POSITIVE_INFINITY
                                                           : Math.max(80, entry.width * 0.4)
                    }
                }
            }
        }
    }
}
