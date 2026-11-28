pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: section
    required property var ecu
    required property string plainFamily

    readonly property var e: JSON.parse(JSON.stringify(ecu.enhanced))

    function statusText(i) {
        const cls = e.cls[i]
        return cls !== "" ? qsTr("%1, %2").arg(e.state[i]).arg(cls.toLowerCase()) : e.state[i]
    }
    function notesText(i) {
        const notes = []
        if (e.lamp[i])
            notes.push(qsTr("warning lamp requested"))
        const lists = e.emissions[i]
        if (lists.length > 0)
            notes.push(qsTr("also an emissions code (%1)").arg(lists.join(", ")))
        return notes.join("  ·  ")
    }
    function outcomeText() {
        switch (e.outcome) {
        case "answered":
            if (e.codes.length === 0)
                return qsTr("None")
            return e.more > 0 ? qsTr("and %1 more").arg(e.more) : ""
        case "refused":
            if (e.extendedOnly)
                return e.sessionOffered
                       ? qsTr("Refused (%1): the module reads its codes only in its extended session, connect with ?session=03").arg(e.nrc)
                       : qsTr("Refused (%1): the module reads its codes only in its extended session").arg(e.nrc)
            return qsTr("Refused (%1)").arg(e.nrc)
        case "no_response": return qsTr("No response")
        case "not_opened":  return qsTr("Not read: no room")
        default:            return e.reading ? qsTr("Reading…") : qsTr("Not read")
        }
    }

    Layout.fillWidth: true
    spacing: 2
    objectName: "enhanced"

    Label {
        textFormat: Text.PlainText
        Layout.fillWidth: true
        Layout.topMargin: 4
        objectName: "enhancedHeading"
        text: qsTr("Enhanced codes")
        font.pixelSize: Theme.fontCaption
        font.weight: Font.DemiBold
        color: Theme.textDim
    }
    Repeater {
        model: section.e.codes.length
        delegate: ColumnLayout {
            id: row
            required property int index
            objectName: "enhancedCode_" + section.e.codes[row.index]
            Layout.fillWidth: true
            spacing: 0
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing
                Label {
                    textFormat: Text.PlainText
                    text: section.e.codes[row.index]
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontEmphasis
                    color: Theme.text
                }
                Label {
                    textFormat: Text.PlainText
                    objectName: "enhancedStatus"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: section.statusText(row.index)
                    color: Theme.text
                }
            }
            Label {
                textFormat: Text.PlainText
                visible: text !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: section.e.titles[row.index]
                color: Theme.textDim
                font.pixelSize: Theme.fontCaption
            }
            Label {
                textFormat: Text.PlainText
                objectName: "enhancedNotes"
                visible: text !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: section.notesText(row.index)
                color: Theme.textDim
                font.pixelSize: Theme.fontCaption
            }
        }
    }
    Label {
        textFormat: Text.PlainText
        objectName: "enhancedOutcome"
        visible: text !== ""
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: section.outcomeText()
        font.family: section.plainFamily
        color: Theme.textDim
    }
}
