import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

GridLayout {
    id: row
    required property SettingsPage host
    required property bool canChange
    required property bool oneRow
    required property var part
    required property bool first
    required property bool last
    required property string counts
    readonly property bool own: part !== null && part.own === true
    signal moved(int by)
    signal unpicked()
    columns: oneRow ? 2 : 1
    columnSpacing: Theme.spacing

    function takeFocus(by) {
        const button = by < 0 && upButton.enabled ? upButton : downButton.enabled ? downButton : upButton
        button.forceActiveFocus()
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2
        Label {
            textFormat: Text.PlainText
            objectName: "togetherPartTitle"
            text: row.part !== null ? row.host.picksPart.profileTitle(row.part.profile) : ""
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Label {
            textFormat: Text.PlainText
            objectName: "togetherPartWords"
            text: row.part === null ? ""
                  : row.own ? qsTr("%1 · yours: your signals replace a plugin's of the same name; obd2's standard signals are read beside yours")
                                  .arg(row.counts)
                  : row.counts
            color: Theme.textDim
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }
    RowLayout {
        visible: !row.own
        spacing: Theme.spacing
        Layout.fillWidth: true
        Layout.maximumWidth: row.oneRow ? Math.ceil(implicitWidth) : Number.POSITIVE_INFINITY
        ThemedButton {
            id: upButton
            objectName: "togetherUp"
            flat: true
            text: qsTr("Up")
            enabled: row.canChange && !row.first
            Accessible.name: qsTr("Move %1 up").arg(row.part !== null ? row.part.name : "")
            Layout.fillWidth: !row.oneRow
            onClicked: row.moved(-1)
        }
        ThemedButton {
            id: downButton
            objectName: "togetherDown"
            flat: true
            text: qsTr("Down")
            enabled: row.canChange && !row.last
            Accessible.name: qsTr("Move %1 down").arg(row.part !== null ? row.part.name : "")
            Layout.fillWidth: !row.oneRow
            onClicked: row.moved(1)
        }
        ThemedButton {
            objectName: "togetherUnpick"
            flat: true
            text: qsTr("Unpick")
            enabled: row.canChange
            Accessible.name: qsTr("Unpick %1").arg(row.part !== null ? row.part.name : "")
            Layout.fillWidth: !row.oneRow
            onClicked: row.unpicked()
        }
    }
}
