// File/layer: app/qml/FrameIdList.qml | Qt Quick, UI layer
// Testing: QML: QmlTest.log, QmlTest.recordings, QmlTest.phone (test_qml)

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One row per ID over the frames a table shows (FrameIdModel), the bytes
// that changed from the ID's frame before marked.
Rectangle {
    id: idTable
    // The FrameIdModel: one row per ID over the frames a table shows
    required property var ids
    // True while the list is in view: it reads the model again twice a
    // second then
    property bool refreshing: false
    // Shown while there is no row
    property string emptyText: ""
    // A row tapped
    signal idTapped(var id, bool extended)

    // Frames a second, as the rows show them
    function rateText(rate) {
        return rate <= 0 ? "-" : rate < 10 ? rate.toFixed(1) + "/s" : Math.round(rate) + "/s"
    }

    // A row's payload with the bytes that changed from the frame before
    // marked, as styled text: the payload's spaced hex and the changed
    // positions, both from the model
    function markedPayload(hex, changed) {
        if (!changed || changed.length === 0)
            return hex
        const bytes = hex.split(" ")
        for (const at of changed) {
            if (at < bytes.length)
                bytes[at] = "<font color=\"" + Theme.warning + "\"><b>" + bytes[at] + "</b></font>"
        }
        return bytes.join(" ")
    }

    Layout.fillWidth: true
    Layout.fillHeight: true
    radius: Theme.radius
    color: Theme.surface
    clip: true

    // Read again twice a second while it shows (FrameIdModel)
    Timer {
        interval: 500
        repeat: true
        running: idTable.refreshing && idTable.visible
        onTriggered: idTable.ids.refresh()
    }

    Rectangle {
        id: idHeader
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: 32
        z: 2
        color: Theme.elevated

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 8
            // Widths MUST match the delegate's columns below
            Label { text: qsTr("ID"); color: Theme.textDim; font.pixelSize: Theme.fontCaption;
                    Layout.preferredWidth: 104 }   // a 29-bit ID and its E
            Label { text: qsTr("Frames"); color: Theme.textDim; font.pixelSize: Theme.fontCaption;
                    Layout.preferredWidth: 52 }
            Label { text: qsTr("Rate"); color: Theme.textDim; font.pixelSize: Theme.fontCaption;
                    Layout.preferredWidth: 52 }
            Label { text: qsTr("Last payload"); color: Theme.textDim; font.pixelSize: Theme.fontCaption;
                    Layout.fillWidth: true; elide: Text.ElideRight }
        }
    }

    ListView {
        id: idList
        objectName: "idList"
        anchors { top: idHeader.bottom; left: parent.left
                  right: parent.right; bottom: parent.bottom }
        anchors.margins: 4
        clip: true
        model: idTable.ids
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        delegate: Rectangle {
            id: idRow
            readonly property bool narrow: ListView.view !== null
                                           && ListView.view.width < 480
            width: ListView.view !== null ? ListView.view.width : 0
            height: Math.max(Theme.rowHeight, idPayload.implicitHeight + 8)
            radius: 4
            color: "transparent"

            required property var model
            required property int index

            // A tap reads this ID's frames: the filter takes it
            MouseArea {
                objectName: "idRowArea"
                anchors.fill: parent
                onClicked: idTable.idTapped(idRow.model.arbId, idRow.model.extended)
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 8
                Label {
                    text: "0x" + idRow.model.arbId.toString(16).toUpperCase()
                          + (idRow.model.extended ? " E" : "")
                    color: Theme.text
                    font.family: Theme.monoFamily; font.pixelSize: Theme.fontCaption
                    Layout.preferredWidth: 104
                    elide: Text.ElideRight
                }
                Label {
                    text: idRow.model.frames
                    color: Theme.textDim
                    font.family: Theme.monoFamily; font.pixelSize: Theme.fontCaption
                    Layout.preferredWidth: 52
                }
                Label {
                    text: idTable.rateText(idRow.model.rate)
                    color: Theme.textDim
                    font.family: Theme.monoFamily; font.pixelSize: Theme.fontCaption
                    Layout.preferredWidth: 52
                }
                // The bytes that changed from the ID's frame
                // before, in the warning color and bold
                Label {
                    id: idPayload
                    objectName: "idPayload"
                    text: idTable.markedPayload(idRow.model.payloadHex,
                                             idRow.model.changedBytes)
                    textFormat: Text.StyledText
                    color: Theme.text
                    font.family: Theme.monoFamily; font.pixelSize: Theme.fontCaption
                    Layout.fillWidth: true
                    wrapMode: idRow.narrow ? Text.Wrap : Text.NoWrap   // between bytes
                    elide: idRow.narrow ? Text.ElideNone : Text.ElideRight
                }
            }
        }

        Label {
            anchors.centerIn: parent
            visible: idList.count === 0
            text: idTable.emptyText
            color: Theme.textDim
        }
    }
}
