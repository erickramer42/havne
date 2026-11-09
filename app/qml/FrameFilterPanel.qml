// File/layer: app/qml/FrameFilterPanel.qml | Qt Quick, UI layer
// Testing: QML: QmlTest.log, QmlTest.recordings, QmlTest.phone (test_qml)

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The filter's fields over a FrameListModel (framefilter.h): the columns a
// bus is read by. The Log page and the recording viewer share it.
GridLayout {
    id: panel
    objectName: "filterPanel"
    Layout.fillWidth: true
    columns: 2
    columnSpacing: Theme.spacing
    rowSpacing: Theme.spacing / 2

    // The FrameListModel the fields filter
    required property var frames
    // FrameFilter::Direction's order: All, Received, Sent
    property int direction: 0

    // The fields to the model, which keeps the last good part of a field
    // that doesn't parse and says why
    function apply() {
        const r = panel.frames.setFilter(filterIds.text, filterData.text,
                                         panel.direction, filterFlag.currentIndex,
                                         filterLengths.text, filterTimes.text)
        filterIdsProblem.text = r.ids
        filterDataProblem.text = r.data
        filterLengthsProblem.text = r.lengths
        filterTimesProblem.text = r.times
    }

    function clear() {
        filterIds.text = ""
        filterData.text = ""
        filterLengths.text = ""
        filterTimes.text = ""
        filterFlag.currentIndex = 0
        panel.direction = 0
        apply()
    }

    // One ID's frames alone. The ID filter takes a number of either
    // width, so the row's own width is the flag: a standard 0x7E8 leaves
    // an extended 0x7E8 out, as an extended one leaves the standard one out
    function showId(id, extended) {
        filterIds.text = id.toString(16).toUpperCase()
        filterFlag.currentIndex = extended ? 1 : 2   // Extended (29-bit), Standard (11-bit)
        apply()
    }

    Label { text: qsTr("ID"); color: Theme.textDim; font.pixelSize: Theme.fontCaption }
    TextField {
        id: filterIds
        objectName: "filterIds"
        Layout.fillWidth: true
        Layout.preferredWidth: 0   // the panel's width, not its text's
        placeholderText: qsTr("7E8, 7E0-7EF")
        font.family: Theme.monoFamily
        inputMethodHints: Qt.ImhPreferUppercase | Qt.ImhNoPredictiveText
        onTextEdited: panel.apply()
    }
    Item { visible: filterIdsProblem.visible }  // the label column, empty
    Label {
        id: filterIdsProblem
        objectName: "filterIdsProblem"
        visible: text !== ""
        color: Theme.warning
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.preferredWidth: 0   // the panel's width, not its text's
    }

    Label { text: qsTr("Data"); color: Theme.textDim; font.pixelSize: Theme.fontCaption }
    TextField {
        id: filterData
        objectName: "filterData"
        Layout.fillWidth: true
        Layout.preferredWidth: 0   // the panel's width, not its text's
        placeholderText: qsTr("^62 F1 ??")
        font.family: Theme.monoFamily
        inputMethodHints: Qt.ImhPreferUppercase | Qt.ImhNoPredictiveText
        onTextEdited: panel.apply()
    }
    Item { visible: filterDataProblem.visible }  // the label column, empty
    Label {
        id: filterDataProblem
        objectName: "filterDataProblem"
        visible: text !== ""
        color: Theme.warning
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.preferredWidth: 0   // the panel's width, not its text's
    }

    // The data length in bytes, and a span of the table's seconds
    Label { text: qsTr("Length"); color: Theme.textDim; font.pixelSize: Theme.fontCaption }
    RowLayout {
        Layout.fillWidth: true
        Layout.preferredWidth: 0   // the panel's width, not its text's
        spacing: Theme.spacing
        TextField {
            id: filterLengths
            objectName: "filterLengths"
            Layout.fillWidth: true
            Layout.preferredWidth: 0   // half the row, not its text's
            placeholderText: qsTr("8, 1-7")
            font.family: Theme.monoFamily
            inputMethodHints: Qt.ImhPreferNumbers | Qt.ImhNoPredictiveText
            onTextEdited: panel.apply()
        }
        Label { text: qsTr("Time"); color: Theme.textDim; font.pixelSize: Theme.fontCaption }
        TextField {
            id: filterTimes
            objectName: "filterTimes"
            Layout.fillWidth: true
            Layout.preferredWidth: 0   // half the row, not its text's
            placeholderText: qsTr("12.5-30")
            font.family: Theme.monoFamily
            inputMethodHints: Qt.ImhFormattedNumbersOnly | Qt.ImhNoPredictiveText
            onTextEdited: panel.apply()
        }
    }
    Item { visible: filterLengthsProblem.visible }  // the label column, empty
    Label {
        id: filterLengthsProblem
        objectName: "filterLengthsProblem"
        visible: text !== ""
        color: Theme.warning
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.preferredWidth: 0   // the panel's width, not its text's
    }
    Item { visible: filterTimesProblem.visible }  // the label column, empty
    Label {
        id: filterTimesProblem
        objectName: "filterTimesProblem"
        visible: text !== ""
        color: Theme.warning
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.preferredWidth: 0   // the panel's width, not its text's
    }

    Flow {
        Layout.columnSpan: 2
        Layout.fillWidth: true
        Layout.preferredWidth: 0   // the panel's width, not its text's
        spacing: Theme.spacing

        Rectangle {
            implicitWidth: directionSwitch.implicitWidth + 4
            implicitHeight: directionSwitch.implicitHeight + 4
            radius: Theme.radius
            color: Theme.surface

            Row {
                id: directionSwitch
                anchors.centerIn: parent
                spacing: 2
                Repeater {
                    model: [qsTr("All"), qsTr("Received"), qsTr("Sent")]
                    SegmentButton {
                        required property string modelData
                        required property int index
                        objectName: "direction_" + index
                        text: modelData
                        selected: panel.direction === index
                        onClicked: {
                            panel.direction = index
                            panel.apply()
                        }
                    }
                }
            }
        }
        ComboBox {
            id: filterFlag
            objectName: "filterFlag"
            // FrameFilter::Flag's order
            model: [qsTr("Any flag"), qsTr("Extended (29-bit)"),
                    qsTr("Standard (11-bit)"), qsTr("CAN FD"),
                    qsTr("Reassembled"), qsTr("Truncated"),
                    qsTr("After a loss"), qsTr("Error")]
            implicitHeight: Theme.controlHeight
            onActivated: panel.apply()
        }
        ThemedButton {
            objectName: "filterClear"
            text: qsTr("Clear the filter")
            enabled: panel.frames.filterTerms > 0 || filterIds.text !== ""
                     || filterData.text !== "" || filterLengths.text !== ""
                     || filterTimes.text !== ""
            onClicked: panel.clear()
        }
    }
    // No transport reports the frames it sends yet, so Sent
    // shows none: said, so an empty table doesn't read as lost
    Label {
        objectName: "sentNote"
        visible: panel.direction === 2
        Layout.columnSpan: 2
        Layout.fillWidth: true
        Layout.preferredWidth: 0   // the panel's width, not its text's
        text: qsTr("No adapter reports the frames the session sends yet, so Sent shows none")
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
    }
}
