// File/layer: app/qml/HiddenSignals.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.liveview, QmlTest.phone (test_qml)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// What Live's list leaves out, and why: rows an ECU's supported-PID
// bitmaps mark unsupported, and fields its replies come without (core's
// absent signals). The note counts them; a tap opens them inline, each
// with its reason. The Dashboard keeps them.
ColumnLayout {
    id: part
    objectName: "hiddenSignals"

    // Whether the list is open; it closes when nothing is hidden
    property bool open: false
    readonly property int hiddenCount: Bridge.signalModel.hiddenCount
    // At most this many rows' room shows before the list scrolls
    property int maxRows: 4
    property real rowHeight: Theme.rowHeight
    // The hidden rows, { name, why }; reading hiddenCount makes a judgment
    // of the model (a reason that moved too) run this again
    // A plain JS copy, made once per change: a delegate never reads a C++ map
    readonly property var rows: part.open ? JSON.parse(JSON.stringify((part.hiddenCount, Bridge.signalModel.hiddenSignals()))) : []

    function reasonText(why) {
        return why === "unsupported" ? qsTr("the ECU's supported-PID list leaves it out")
             : why === "absent" ? qsTr("its replies come without this field")
             : ""
    }

    spacing: 2
    onHiddenCountChanged: if (hiddenCount === 0) open = false

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Label {
            id: note
            objectName: "hiddenNote"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: part.hiddenCount === 1
                  ? qsTr("1 signal hidden: the ECU doesn't send it")
                  : qsTr("%1 signals hidden: the ECU doesn't send them")
                        .arg(part.hiddenCount)
        }
        ThemedButton {
            objectName: "hiddenToggle"
            flat: true
            Layout.preferredHeight: Theme.touchTarget
            Layout.minimumWidth: Theme.touchTarget
            text: part.open ? qsTr("Hide") : qsTr("Show")
            Accessible.name: part.open ? qsTr("Hide the hidden signals")
                                       : qsTr("Show the hidden signals")
            onClicked: part.open = !part.open
        }
    }

    ListView {
        id: list
        objectName: "hiddenList"
        visible: part.open
        Layout.fillWidth: true
        // A row and a half's room each (a name and a reason of one or two
        // lines), up to maxRows, then it scrolls. Not contentHeight: a list
        // of no height builds no rows to measure.
        Layout.preferredHeight: part.open
                                ? Math.min(count, part.maxRows) * part.rowHeight * 1.5 : 0
        clip: true
        model: part.rows
        boundsBehavior: Flickable.StopAtBounds
        delegate: ColumnLayout {
            id: row
            required property var modelData
            width: ListView.view.width
            spacing: 0
            Accessible.role: Accessible.ListItem
            Accessible.name: qsTr("%1: %2").arg(modelData.name).arg(part.reasonText(modelData.why))
            Label {
                objectName: "hiddenName"
                Layout.fillWidth: true
                elide: Text.ElideRight
                color: Theme.text
                text: row.modelData.name
            }
            // The reason is the point of the list: it wraps, never elides
            Label {
                objectName: "hiddenWhy"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontCaption
                color: Theme.textDim
                text: part.reasonText(row.modelData.why)
            }
            Item { Layout.preferredHeight: 6 }
        }
    }
}
