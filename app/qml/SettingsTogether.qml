import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    objectName: "settingsTogether"
    required property SettingsPage host
    required property bool canChange
    readonly property var together: host.picksPart.together
    readonly property var parts: together !== null ? together.parts : []
    readonly property string label: host.pickedVehicle !== null ? host.pickedVehicle.label : ""
    readonly property bool obd2Picked: host.picksPart.picks.indexOf("obd2") >= 0
    readonly property var extender: host.picksPart.pickRows.find(
        r => !r.missing && !r.builtIn && r.extends === "obd2") || null
    readonly property bool oneRow: width >= 520
    property var openKinds: []
    visible: together !== null
    Layout.fillWidth: true
    spacing: Theme.spacing

    function count(n) {
        return Number(n).toLocaleString(Qt.locale(), "f", 0)
    }
    function totalsText(t) {
        return [t.plugins === 1 ? qsTr("1 plugin") : qsTr("%1 plugins").arg(count(t.plugins)),
                t.modules === 1 ? qsTr("1 module") : qsTr("%1 modules").arg(count(t.modules)),
                t.signals === 1 ? qsTr("1 signal") : qsTr("%1 signals").arg(count(t.signals))].join(", ")
    }
    function partCounts(p) {
        return (p.modules === 1 ? qsTr("1 module") : qsTr("%1 modules").arg(count(p.modules))) + ", "
               + (p.signals === 1 ? qsTr("1 signal") : qsTr("%1 signals").arg(count(p.signals)))
    }
    function foldText(kind, n) {
        const one = n === 1
        switch (kind) {
        case "replacedByAddress":
            return one ? qsTr("1 signal replaced by a later plugin's at its address")
                       : qsTr("%1 signals replaced by a later plugin's at their address").arg(count(n))
        case "replacedByKey":
            return one ? qsTr("1 signal replaced by a later plugin's of its key")
                       : qsTr("%1 signals replaced by a later plugin's of their key").arg(count(n))
        case "keyTaken":
            return one ? qsTr("1 key an earlier plugin took") : qsTr("%1 keys an earlier plugin took").arg(count(n))
        case "besideBase":
            return one ? qsTr("1 signal read beside obd2's of its name")
                       : qsTr("%1 signals read beside obd2's of their name").arg(count(n))
        case "moduleRenamed":
            return one ? qsTr("1 module an earlier plugin named") : qsTr("%1 modules an earlier plugin named").arg(count(n))
        case "moduleLeftOut":
            return one ? qsTr("1 module left out") : qsTr("%1 modules left out").arg(count(n))
        case "partLeftOut":
            return one ? qsTr("1 plugin left out") : qsTr("%1 plugins left out").arg(count(n))
        case "signalLeftOut":
            return one ? qsTr("1 signal left out") : qsTr("%1 signals left out").arg(count(n))
        }
        return one ? qsTr("1 note") : qsTr("%1 notes").arg(count(n))
    }
    function toggleKind(kind) {
        openKinds = openKinds.indexOf(kind) >= 0 ? openKinds.filter(k => k !== kind) : openKinds.concat([kind])
    }
    function move(profile, by, index) {
        host.saveVehicles(VehicleBook.movePick(host.vehicles, profile, by))
        const row = partRows.itemAt(index + by) as TogetherPartRow
        if (row !== null)
            row.takeFocus(by)
    }
    function togglePick(profile) {
        if (host.pickedVehicle === null || !host.ownPart.idle)
            return
        if (host.picksPart.picks.indexOf(profile) >= 0)
            host.saveVehicles(VehicleBook.setPicks(host.vehicles, Bridge.unpickVariant(host.picksPart.picks, profile)))
        else
            host.picksPart.pickProfile(profile)
    }

    Label {
        textFormat: Text.PlainText
        objectName: "togetherHeading"
        text: qsTr("Read together for %1").arg(root.label)
        font.bold: true
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Label {
        textFormat: Text.PlainText
        objectName: "togetherRefused"
        visible: text !== ""
        text: root.together !== null && root.together.reads === "refused" ? root.together.words : ""
        color: Theme.warning
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Label {
        textFormat: Text.PlainText
        objectName: "togetherNothing"
        visible: root.together !== null && root.together.reads === "merged" && root.together.totals.plugins === 0
                 && root.together.base !== "obd2"
        text: qsTr("Nothing is picked: Live reads no signals. Pick obd2 or a plugin for signals; the emissions codes are still read.")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    GridLayout {
        objectName: "togetherObd2"
        Layout.fillWidth: true
        columns: root.oneRow ? 2 : 1
        columnSpacing: Theme.spacing
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                textFormat: Text.PlainText
                text: qsTr("OBD-II standard signals")
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                textFormat: Text.PlainText
                objectName: "togetherObd2Words"
                text: root.obd2Picked || root.extender !== null
                      ? qsTr("the base: obd2's standard signals, read beside every plugin's")
                      : qsTr("Unpicked: its signals aren't read; the emissions codes still are")
                color: Theme.textDim
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Label {
                textFormat: Text.PlainText
                objectName: "togetherObd2Why"
                visible: root.extender !== null
                text: root.extender !== null
                      ? qsTr("read under %1, which extends it").arg(root.host.picksPart.profileName(root.extender.profile))
                      : ""
                color: Theme.textDim
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
        }
        ThemedButton {
            objectName: "togetherObd2Pick"
            flat: true
            text: root.obd2Picked ? qsTr("Unpick") : qsTr("Pick")
            enabled: root.canChange && !(root.obd2Picked && root.extender !== null)
            Accessible.name: root.obd2Picked ? qsTr("Unpick obd2") : qsTr("Pick obd2")
            Layout.fillWidth: true
            Layout.maximumWidth: root.oneRow ? Math.ceil(implicitWidth) : Number.POSITIVE_INFINITY
            onClicked: root.togglePick("obd2")
        }
    }

    Repeater {
        id: partRows
        model: root.parts.length
        TogetherPartRow {
            required property int index
            host: root.host
            canChange: root.canChange
            oneRow: root.oneRow
            part: index < root.parts.length ? root.parts[index] : null
            first: index === 0
            last: index === root.parts.filter(p => !p.own).length - 1
            counts: part !== null ? root.partCounts(part) : ""
            objectName: "togetherPart_" + index
            Layout.fillWidth: true
            onMoved: by => root.move(part.profile, by, index)
            onUnpicked: root.togglePick(part.profile)
        }
    }

    Label {
        textFormat: Text.PlainText
        objectName: "togetherTotals"
        visible: root.together !== null && root.together.reads === "merged"
        text: visible ? root.totalsText(root.together.totals) : ""
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    Repeater {
        model: root.together !== null ? root.together.folded.length : 0
        ColumnLayout {
            id: fold
            required property int index
            readonly property var entry: root.together !== null && index < root.together.folded.length
                                         ? root.together.folded[index] : ({ kind: "", count: 0 })
            readonly property bool open: root.openKinds.indexOf(entry.kind) >= 0
            objectName: "togetherFold_" + entry.kind
            Layout.fillWidth: true
            spacing: 2
            ThemedButton {
                objectName: "togetherFoldButton"
                flat: true
                text: root.foldText(fold.entry.kind, fold.entry.count)
                checkable: true
                checked: fold.open
                onClicked: root.toggleKind(fold.entry.kind)
            }
            Repeater {
                model: fold.open ? root.together.notes.filter(n => n.kind === fold.entry.kind).map(n => n.text) : []
                Label {
                    required property string modelData
                    textFormat: Text.PlainText
                    objectName: "togetherNote"
                    text: modelData
                    color: Theme.textDim
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spacing * 2
                }
            }
        }
    }

    RowLayout {
        objectName: "togetherCarried"
        readonly property string words: root.host.pickedVehicle !== null
                                        && root.host.pickedVehicle.besideNote !== undefined
                                        ? root.host.pickedVehicle.besideNote : ""
        visible: words !== ""
        Layout.fillWidth: true
        spacing: Theme.spacing
        Label {
            textFormat: Text.PlainText
            objectName: "togetherCarriedText"
            text: parent.words
            color: Theme.textDim
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        ThemedButton {
            objectName: "togetherCarriedDismiss"
            flat: true
            text: qsTr("Dismiss")
            onClicked: root.host.saveVehicles(VehicleBook.dismissBesideNote(root.host.vehicles))
        }
    }

    Label {
        textFormat: Text.PlainText
        objectName: "togetherShareAlike"
        visible: text !== ""
        text: root.together !== null && root.together.shareAlike.length > 0
              ? qsTr("%1's signals are CC BY-SA 4.0: a recording or a copy that passes them on carries that license")
                    .arg(root.together.shareAlike.join(qsTr(", ")))
              : ""
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
}
