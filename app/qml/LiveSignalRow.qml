import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: signalDelegate
    required property LiveViewPage host
    required property int index
    // The signal model's row: its plot box and its trace
    // color, which chartData's `row` names
    required property int row
    required property string name
    required property string unit
    required property double value
    required property int decimals
    required property string text          // a text row's value
    required property string label         // an enum value's word
    required property string refused       // its ECU's refusal in words, or ""
    required property string refusedCode   // that reply's bytes ("7F 22 31"), or ""
    required property bool isText
    required property bool subscribed
    required property bool hasValue
    required property bool unasked          // the focus leaves it unasked
    // The model notifies AgeMsRole per batch and on every
    // coalesce tick (refreshAges), so a row that stops
    // updating still re-evaluates and dims. A delegate-side
    // clock stamped in onValueChanged would dim steady values
    // that are still arriving.
    required property double ageMs          // AgeMsRole; -1 = never arrived
    // The row's roles as one object, for its def id (a role
    // named id can't be a property beside this item's id)
    required property var model
    // The highest alert ON that reads it: 1 warning, 2 danger
    readonly property int alertLevel: Bridge.connected && hasValue
                                      ? (host.alertLevels[String(model.id)] ?? 0) : 0

    height: host.rowHeight
    color: signalDelegate.index % 2 === 0 ? Theme.surface : Theme.bg
    // One node a row, named by its value: the Texts below are
    // ignored, so a value changing at the model's rate is a name
    // change and never a node made, hidden or destroyed
    Accessible.role: Accessible.ListItem
    Accessible.name: signalDelegate.name + ": " + valueLabel.text
                     + (alertLevel === 2 ? qsTr(", danger alert")
                        : alertLevel === 1 ? qsTr(", warning alert") : "")

    RowLayout {
        id: rowLayout
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12

        // Plots the row on the chart and asks it; the value
        // keeps updating either way, here and on the
        // Dashboard. A text row's tick asks it alone
        ThemedCheckBox {
            id: plotBox
            objectName: "plotCheck"
            checked: signalDelegate.subscribed
            onToggled: Bridge.signalModel.toggleSubscribed(signalDelegate.row)
            Accessible.name: signalDelegate.isText
                             ? qsTr("Ask %1").arg(signalDelegate.name)
                             : qsTr("Plot %1").arg(signalDelegate.name)
        }
        // The chart's legend: this row's trace color, hollow
        // while the signal isn't plotted
        Rectangle {
            id: swatch
            objectName: "swatch"
            // index is -1 while a model reset (a connect)
            // removes the delegate
            readonly property color trace: signalDelegate.index >= 0
                                           ? host.seriesColor(signalDelegate.row)
                                           : "transparent"
            implicitWidth: 10; implicitHeight: 10   // layout-managed
            radius: 2
            // Filled for a plotted trace: a ticked text row is asked, never drawn
            readonly property bool drawn: signalDelegate.subscribed && !signalDelegate.isText
            color: swatch.drawn ? swatch.trace : "transparent"
            border.width: 1
            border.color: swatch.trace
            opacity: swatch.drawn ? 1.0 : 0.5
        }
        Label {
            textFormat: Text.PlainText
            id: nameLabel
            objectName: "nameLabel"
            Accessible.ignored: true
            text: signalDelegate.name
            color: Theme.text
            Layout.fillWidth: true
            // A long value (a refusal's words, a text) leaves
            // the name a third of the row
            Layout.minimumWidth: Math.min(implicitWidth,
                                          Math.round(signalDelegate.width / 3))
            elide: Text.ElideRight
        }
        // The refusal's words at the value's font: on a row
        // too narrow for them beside the name (a phone), the
        // value is "Refused (7F 22 31)" instead
        TextMetrics {
            id: refusedWords
            font: valueLabel.font
            text: signalDelegate.refused
        }
        Label {
            textFormat: Text.PlainText
            id: valueLabel
            objectName: "valueLabel"
            Accessible.ignored: true
            // Room past the whole name (its x is the checkbox's and
            // the swatch's), within the value's cap below
            readonly property bool refusedFits:
                refusedWords.advanceWidth
                <= Math.min(Math.round(signalDelegate.width * 0.6),
                            signalDelegate.width - 24 - nameLabel.x
                            - nameLabel.implicitWidth - 12)
            // A signal its ECU refused says so, with the code
            // by name (ADR-0051 section 3); one the session
            // isn't asking says that, whatever it last read;
            // one not yet answered renders "—", not a
            // fabricated 0.
            text: !Bridge.connected ? qsTr("—")
                  : signalDelegate.refused !== ""
                    ? (refusedFits || signalDelegate.refusedCode === ""
                       ? signalDelegate.refused
                       : qsTr("Refused (%1)").arg(signalDelegate.refusedCode))
                  : signalDelegate.unasked ? qsTr("Not asked")
                  : !signalDelegate.hasValue
                    ? (Bridge.replayFinished ? qsTr("Not in this recording") : qsTr("—"))
                  : signalDelegate.isText ? signalDelegate.text
                  : signalDelegate.label !== "" ? signalDelegate.label
                  : host.valueText(signalDelegate.value, signalDelegate.decimals,
                                   signalDelegate.unit)
            // A text value can run to kilobytes: the name
            // keeps its room
            // and, on a row too narrow for its cap beside the
            // box, the swatch and the name's least, what is left
            // Each neighbor's fractional width rounds up in the
            // layout, so the room left rounds down
            Layout.maximumWidth: Math.max(0, Math.min(
                Math.round(signalDelegate.width * 0.6),
                Math.floor(signalDelegate.width - 24 - Math.ceil(plotBox.width)
                           - Math.ceil(swatch.width)
                           - Math.ceil(nameLabel.Layout.minimumWidth)
                           - 3 * rowLayout.spacing)))
            elide: Text.ElideRight
            color: signalDelegate.alertLevel === 2 ? Theme.textOnAlertDanger
                   : signalDelegate.alertLevel === 1 ? Theme.textOnAlertWarn
                   : signalDelegate.refused !== ""
                     || (signalDelegate.unasked && Bridge.connected)
                   ? Theme.textDim : Theme.text
            // An alert ON fills the value's cell in its level's
            // role, the mark beside a danger; the name keeps its color
            leftPadding: signalDelegate.alertLevel === 2 ? 26
                         : signalDelegate.alertLevel === 1 ? 6 : 0
            rightPadding: signalDelegate.alertLevel > 0 ? 6 : 0
            background: Rectangle {
                objectName: "valueAlert"
                visible: signalDelegate.alertLevel > 0
                color: signalDelegate.alertLevel === 2 ? Theme.alertDangerFill
                                                       : Theme.alertWarnFill
                radius: Theme.radius
                DangerMark {
                    objectName: "valueAlertMark"
                    visible: signalDelegate.alertLevel === 2
                    x: 6
                    anchors.verticalCenter: parent.verticalCenter
                    size: 14
                    color: Theme.textOnAlertDanger
                    cutout: Theme.alertDangerFill
                }
            }
            font.pixelSize: Theme.fontEmphasis
            // The value face, its digits tabular, so a reading
            // doesn't jitter as its digits change
            font.family: Theme.valueFamily
            font.features: Theme.valueFeatures
            opacity: signalDelegate.hasValue
                     && signalDelegate.ageMs > host.staleMs
                         ? 0.45 : 1.0
        }
    }
}
