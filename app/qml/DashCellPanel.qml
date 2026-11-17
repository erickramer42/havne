// File/layer: app/qml/DashCellPanel.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit, QmlTest.phone,
//          QmlTest.linkhealth, QmlTest.driveview (test_qml); BENCH, Manual smoke

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The Dashboard's panel for the selected cell (ADR-0057's appendix "edit
// mode as built"), which the page's panelHost loads

GaugeCard {
    id: panelCard
    required property DashboardPage host
    objectName: "dashCellPanel"
    readonly property real touchMin: Theme.touchTarget
    readonly property var cell: panelCard.host.selectedCell
    readonly property string kind: cell ? cell.kind : ""
    readonly property string unitShown: cell ? panelCard.host.shownUnit(cell.unit ?? "") : ""
    readonly property var range: DashboardBook.defaultRange(cell ? cell.unit ?? "" : "")
    // The row the cell shows, whose decimals it uses until it has its own
    readonly property SignalHub.Entry entry: panelCard.host.signalHub.lookup(panelCard.host.lookupRefs(cell && cell.refs ? cell.refs : []))
    // With no session the hub has no row: the listed profile's row gives the decimals
    readonly property int listedDecimals: {
        const refs = cell && Array.isArray(cell.refs) ? cell.refs : []
        for (const r of panelCard.host.profileRows) {
            if (refs.indexOf(r.ref) >= 0)
                return r.decimals
        }
        return 0
    }
    readonly property int decimalsNow: cell && cell.options && cell.options.decimals !== undefined
                                       ? cell.options.decimals
                                       : entry ? entry.decimals : listedDecimals
    readonly property bool phone: panelCard.host.form === "phone"
    // Any of the zone levels is set
    readonly property bool hasZoneLevel: {
        const o = cell && cell.options ? cell.options : null
        if (o === null)
            return false
        for (const k of ["coldBelow", "warnBelow", "dangerBelow", "warnFrom", "dangerFrom"]) {
            if (typeof o[k] === "number" && isFinite(o[k]))
                return true
        }
        return false
    }
    readonly property bool hasSignal: kind !== "blank"
    // Every kind but the shift lights draws the value's text and tone, which
    // the zones, the decimals and the color shape; the lights read only the range
    readonly property bool drawsValue: hasSignal && kind !== "shift-light"
    // The cell's label, or its kind's name when it has none
    readonly property string name: cell && cell.label ? cell.label
                                   : (panelCard.host.kindNames[kind] ?? kind)
    implicitHeight: panelColumn.implicitHeight + 2 * padding
    padding: 12

    ColumnLayout {
        id: panelColumn
        width: parent.width
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.pixelSize: Theme.fontBody
                font.bold: true
                color: Theme.text
                text: (panelCard.host.kindNames[panelCard.kind] ?? panelCard.kind)
                      + (panelCard.cell && panelCard.cell.label ? ": " + panelCard.cell.label : "")
            }
            ThemedButton {
                objectName: "panelClose"
                text: qsTr("Close")
                onClicked: panelCard.host.closePanel()
            }
        }

        // The cell itself, live, at a fixed size, so range, zones,
        // color and size are seen as they change
        DashCell {
            host: panelCard.host
            objectName: "panelPreview"
            visible: panelCard.hasSignal
            cell: panelCard.cell
            shown: false
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: panelCard.kind === "bar" || panelCard.kind === "shift-light"
                                   || panelCard.kind === "segments" || panelCard.kind === "sparkline" ? 200 : 120
            Layout.preferredHeight: panelCard.kind === "shift-light" ? 40
                                  : panelCard.kind === "segments" || panelCard.kind === "sparkline" ? 100
                                  : panelCard.kind === "bar" || panelCard.kind === "seven-segment" ? 70 : 120
        }

        // A copy of the whole cell, for this run (section 5)
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            ThemedButton {
                objectName: "panelCopy"
                Layout.fillWidth: true
                text: qsTr("Copy")
                onClicked: panelCard.host.copySelected()
            }
            ThemedButton {
                objectName: "panelPaste"
                Layout.fillWidth: true
                enabled: panelCard.host.canPaste
                text: qsTr("Paste after")
                onClicked: panelCard.host.pasteAfter(panelCard.host.selected)
            }
            ThemedButton {
                objectName: "panelAlert"
                readonly property var condition:
                    panelCard.host.alertOf(panelCard.cell, panelCard.cell && panelCard.cell.label
                                 ? panelCard.cell.label
                                 : panelCard.entry !== null ? panelCard.entry.name : panelCard.name)
                // A kind that draws no zone keeps a level it can't show; a
                // row of another unit than the cell's levels has no alert
                readonly property bool mismatch:
                    panelCard.entry !== null && panelCard.entry.unit !== ""
                    && panelCard.unitShown !== "" && panelCard.entry.unit !== panelCard.unitShown
                visible: panelCard.drawsValue && condition !== null && !mismatch
                Layout.fillWidth: true
                text: qsTr("Alert")
                Accessible.name: qsTr("Make an alert from this gauge's levels")
                onClicked: panelCard.host.alertRequested(condition)
            }
        }
        Label {
            objectName: "dashAlertNote"
            visible: panelCard.host.alertNote !== ""
            text: panelCard.host.alertNote
            color: Theme.danger
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        GridLayout {
            Layout.fillWidth: true
            columns: panelCard.phone ? 2 : 4
            columnSpacing: 8
            rowSpacing: 8

            Caption { visible: panelCard.hasSignal; text: qsTr("Label") }
            ThemedTextField {
                id: labelField
                objectName: "panelLabel"
                visible: panelCard.hasSignal
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.touchTarget
                maximumLength: DashboardBook.maxLabel
                text: panelCard.cell && panelCard.cell.label ? panelCard.cell.label : ""
                onEditingFinished: {
                    panelCard.host.setLabel(text)
                    text = Qt.binding(() => panelCard.cell && panelCard.cell.label
                                            ? panelCard.cell.label : "")
                }
            }
            Caption { visible: panelCard.hasSignal; text: qsTr("Signal") }
            RowLayout {
                visible: panelCard.hasSignal
                Layout.fillWidth: true
                spacing: 8
                ThemedButton {
                    objectName: "panelSignal"
                    Layout.fillWidth: true
                    text: qsTr("Change signal")
                    onClicked: panelCard.host.changeSignal()
                }
                ThemedButton {
                    objectName: "panelKind"
                    Layout.fillWidth: true
                    text: qsTr("Change kind")
                    onClicked: panelCard.host.changeKind()
                }
            }

            Caption {
                objectName: "panelSignalName"
                visible: panelCard.hasSignal && panelCard.entry !== null
                Layout.fillWidth: true
                Layout.columnSpan: panelCard.phone ? 2 : 4
                Layout.preferredWidth: 0
                elide: Text.ElideRight
                text: panelCard.entry ? qsTr("Signal: %1").arg(panelCard.entry.name) : ""
            }
            Caption {
                objectName: "panelFromCaption"
                visible: panelCard.hasSignal
                text: panelCard.kind === "shift-light"
                      ? qsTr("First light at (%1)").arg(panelCard.unitShown)
                      : qsTr("From (%1)").arg(panelCard.unitShown)
            }
            OptionField {
                objectName: "panelFrom"
                visible: panelCard.hasSignal
                option: "from"
                clearable: false
                fallback: panelCard.range.from
            }
            Caption {
                objectName: "panelToCaption"
                visible: panelCard.hasSignal
                text: panelCard.kind === "shift-light"
                      ? qsTr("Shift at (%1)").arg(panelCard.unitShown)
                      : qsTr("To (%1)").arg(panelCard.unitShown)
            }
            OptionField {
                objectName: "panelTo"
                visible: panelCard.hasSignal
                option: "to"
                clearable: false
                fallback: panelCard.range.to
            }
            Caption {
                objectName: "panelShiftHint"
                visible: panelCard.kind === "shift-light"
                Layout.fillWidth: true
                Layout.columnSpan: panelCard.phone ? 2 : 4
                Layout.preferredWidth: 0
                wrapMode: Text.WordWrap
                text: qsTr("The lights fill green, amber, red between the two and flash at the shift point.")
            }

            Caption { objectName: "panelColdBelowCaption"; visible: panelCard.drawsValue; text: qsTr("Cold below") }
            OptionField { objectName: "panelColdBelow"; visible: panelCard.drawsValue; option: "coldBelow" }
            Caption { objectName: "panelWarnBelowCaption"; visible: panelCard.drawsValue; text: qsTr("Warning below") }
            OptionField { objectName: "panelWarnBelow"; visible: panelCard.drawsValue; option: "warnBelow" }
            Caption { objectName: "panelDangerBelowCaption"; visible: panelCard.drawsValue; text: qsTr("Danger below") }
            OptionField { objectName: "panelDangerBelow"; visible: panelCard.drawsValue; option: "dangerBelow" }
            Caption { objectName: "panelWarnFromCaption"; visible: panelCard.drawsValue; text: qsTr("Warning from") }
            OptionField { objectName: "panelWarnFrom"; visible: panelCard.drawsValue; option: "warnFrom" }
            Caption { objectName: "panelDangerFromCaption"; visible: panelCard.drawsValue; text: qsTr("Danger from") }
            OptionField { objectName: "panelDangerFrom"; visible: panelCard.drawsValue; option: "dangerFrom" }

            Caption { objectName: "panelDecimalsCaption"; visible: panelCard.drawsValue; text: qsTr("Decimals") }
            SpinBox {
                objectName: "panelDecimals"
                visible: panelCard.drawsValue
                Accessible.name: qsTr("Decimals")
                Layout.preferredHeight: Theme.touchTarget
                from: 0
                to: 6
                value: panelCard.decimalsNow
                onValueModified: panelCard.host.setOptions({ decimals: value })
            }

            // The span, in the grid's units, which the words name
            Caption { text: qsTr("Columns") }
            SpinBox {
                objectName: "panelW"
                Accessible.name: qsTr("Columns")
                Layout.preferredHeight: Theme.touchTarget
                from: 1
                to: DashboardBook.maxColumns
                value: panelCard.cell ? panelCard.cell.w : 1
                onValueModified: panelCard.host.resize(value, panelCard.cell.h)
            }
            Caption { text: panelCard.host.phoneForm ? qsTr("Height") : qsTr("Rows") }
            SpinBox {
                objectName: "panelH"
                Accessible.name: panelCard.host.phoneForm ? qsTr("Height") : qsTr("Rows")
                // The phone reads it in columns, the unit Columns uses
                textFromValue: v => panelCard.host.phoneForm ? DashboardBook.heightInColumns(v) : String(v)
                Layout.preferredHeight: Theme.touchTarget
                // A stored h under the phone's step stays reachable
                from: panelCard.host.phoneForm && panelCard.cell
                      ? Math.min(panelCard.host.spanScale, panelCard.cell.h) : 1
                // The desktop's 16, or a taller stored cell's own height
                to: panelCard.host.phoneForm ? DashboardBook.maxRows
                    : Math.max(DashboardBook.format1MaxRows, panelCard.cell ? panelCard.cell.h : 1)
                stepSize: panelCard.host.spanScale
                value: panelCard.cell ? panelCard.cell.h : 1
                onValueModified: panelCard.host.resize(panelCard.cell.w, panelCard.host.phoneForm
                    ? DashboardBook.stepRows(panelCard.cell.h, value > panelCard.cell.h)
                    : value)
            }

            // A checkbox, as every other yes-or-no in the app
            ThemedCheckBox {
                objectName: "panelPeak"
                Layout.columnSpan: panelCard.phone ? 2 : 4
                text: qsTr("Peak hold")
                Accessible.name: qsTr("Peak hold")
                visible: panelCard.host.peakKinds.indexOf(panelCard.kind) >= 0
                implicitHeight: Math.max(implicitIndicatorHeight + topPadding + bottomPadding,
                                         Theme.touchTarget)
                checked: panelCard.cell && panelCard.cell.options
                         ? panelCard.cell.options.peak === true : false
                onToggled: panelCard.host.setOptions({ peak: checked })
            }

            // The kind's own
            Caption { visible: panelCard.kind === "analog"; text: qsTr("Major step") }
            OptionField {
                objectName: "panelMajorStep"
                visible: panelCard.kind === "analog"
                option: "majorStep"
                converted: false
            }
            Caption { visible: panelCard.kind === "analog"; text: qsTr("Caption") }
            ThemedTextField {
                objectName: "panelCaption"
                visible: panelCard.kind === "analog"
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.touchTarget
                maximumLength: 40
                readonly property string current: panelCard.cell && panelCard.cell.options
                                                  && panelCard.cell.options.caption
                                                  ? panelCard.cell.options.caption : ""
                text: current
                onEditingFinished: {
                    panelCard.host.setOptions({ caption: text === "" ? null : text })
                    text = Qt.binding(() => current)
                }
            }
            Caption { visible: panelCard.kind === "digital"; text: qsTr("Description") }
            ThemedTextField {
                objectName: "panelDescription"
                visible: panelCard.kind === "digital"
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.touchTarget
                maximumLength: 80
                readonly property string current: panelCard.cell && panelCard.cell.options
                                                  && panelCard.cell.options.description
                                                  ? panelCard.cell.options.description : ""
                text: current
                onEditingFinished: {
                    panelCard.host.setOptions({ description: text === "" ? null : text })
                    text = Qt.binding(() => current)
                }
            }
            Caption { visible: panelCard.kind === "segments"; text: qsTr("Cells") }
            SpinBox {
                objectName: "panelCells"
                Accessible.name: qsTr("Cells")
                visible: panelCard.kind === "segments"
                Layout.preferredHeight: Theme.touchTarget
                from: 2
                to: 64
                value: panelCard.cell && panelCard.cell.options
                       && panelCard.cell.options.cells !== undefined
                       ? panelCard.cell.options.cells : 24
                onValueModified: panelCard.host.setOptions({ cells: value })
            }
            Caption { visible: panelCard.kind === "segments"; text: qsTr("Curve") }
            OptionField {
                objectName: "panelRise"
                Accessible.name: qsTr("Curve")
                visible: panelCard.kind === "segments"
                option: "rise"
                converted: false
                fallback: 1.6
            }
            Caption { visible: panelCard.kind === "seven-segment"; text: qsTr("Digits") }
            SpinBox {
                objectName: "panelDigits"
                Accessible.name: qsTr("Digits")
                visible: panelCard.kind === "seven-segment"
                Layout.preferredHeight: Theme.touchTarget
                from: 1
                to: 8
                value: panelCard.cell && panelCard.cell.options
                       && panelCard.cell.options.digits !== undefined
                       ? panelCard.cell.options.digits : panelCard.host.selectedDigits
                onValueModified: panelCard.host.setOptions({ digits: value })
            }
            Caption { visible: panelCard.kind === "sparkline"; text: qsTr("Window (s)") }
            SpinBox {
                objectName: "panelWindow"
                Accessible.name: qsTr("Window (s)")
                visible: panelCard.kind === "sparkline"
                Layout.preferredHeight: Theme.touchTarget
                from: 10
                to: 120
                stepSize: 10
                value: panelCard.cell && panelCard.cell.options
                       && panelCard.cell.options.window !== undefined
                       ? panelCard.cell.options.window : 30
                onValueModified: panelCard.host.setOptions({ window: value })
            }
            Caption { visible: panelCard.kind === "bar"; text: qsTr("Standing up") }
            Switch {
                objectName: "panelVertical"
                Accessible.name: qsTr("Standing up")
                visible: panelCard.kind === "bar"
                implicitHeight: Math.max(implicitIndicatorHeight + topPadding + bottomPadding,
                                         Theme.touchTarget)
                checked: panelCard.cell && panelCard.cell.options
                         ? panelCard.cell.options.vertical === true : false
                onToggled: {
                    // Standing up is a tall, narrow bar: the default
                    // span follows the toggle, and a size the user
                    // chose stays; one write, so one undo step
                    // (the write rebinds this switch, so its state is read first)
                    const on = checked
                    const c = panelCard.cell
                    const s = panelCard.host.spanScale
                    let base = panelCard.host.doc
                    if (c && on && c.w === 2 * s && c.h === 2 * s)
                        base = DashboardBook.resizeCell(base, panelCard.host.pageIndex, panelCard.host.selected, s, 4 * s)
                    else if (c && !on && c.w === s && c.h === 4 * s)
                        base = DashboardBook.resizeCell(base, panelCard.host.pageIndex, panelCard.host.selected, 2 * s, 2 * s)
                    panelCard.host.setOptions({ vertical: on }, base)
                }
            }
            Caption { visible: panelCard.kind === "bar"; text: qsTr("Scale marks") }
            Switch {
                objectName: "panelMarks"
                Accessible.name: qsTr("Scale marks")
                visible: panelCard.kind === "bar"
                implicitHeight: Math.max(implicitIndicatorHeight + topPadding + bottomPadding,
                                         Theme.touchTarget)
                checked: panelCard.cell && panelCard.cell.options
                         ? panelCard.cell.options.marks !== false : true
                onToggled: panelCard.host.setOptions({ marks: checked })
            }
            Caption { visible: panelCard.kind === "shift-light"; text: qsTr("Lights") }
            SpinBox {
                objectName: "panelCount"
                Accessible.name: qsTr("Lights")
                visible: panelCard.kind === "shift-light"
                Layout.preferredHeight: Theme.touchTarget
                from: 1
                to: 32
                value: panelCard.cell && panelCard.cell.options
                       && panelCard.cell.options.count !== undefined
                       ? panelCard.cell.options.count : panelCard.host.selectedCount
                onValueModified: panelCard.host.setOptions({ count: value })
            }
        }

        // The value's color from a fixed set; zones and Night still win.
        // Shift lights keep their own green, amber and red
        RowLayout {
            Layout.fillWidth: true
            visible: panelCard.drawsValue
            spacing: 8

            Caption { Layout.alignment: Qt.AlignTop; Layout.topMargin: 10; text: qsTr("Color") }
            Flow {
                id: colorRow
                objectName: "panelColors"
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                Layout.minimumWidth: 0
                spacing: 8
                // Night draws no pick: the row dims, and stays choosable
                opacity: Theme.nightActive ? 0.45 : 1
                // A name this version doesn't know reads as Default, as it draws
                readonly property string current: panelCard.cell && panelCard.cell.options
                                                  && Theme.gaugeColorNames.indexOf(panelCard.cell.options.color) >= 0
                                                  ? panelCard.cell.options.color : ""
                // A custom color: "#rrggbb" as stored, else ""
                readonly property string custom: panelCard.cell && panelCard.cell.options
                                                 && Theme.isGaugeHex(panelCard.cell.options.color)
                                                 ? panelCard.cell.options.color : ""
                // The Custom entry's fields show while it is open or a custom color is set
                property bool customOpen: false
                readonly property bool customShown: customOpen || custom !== ""
                // The panel is one instance for every gauge: another gauge starts closed
                Connections {
                    target: panelCard.host
                    function onSelectedChanged() { colorRow.customOpen = false }
                    function onPageIndexChanged() { colorRow.customOpen = false }
                }
                ThemedButton {
                    objectName: "panelColor_default"
                    text: qsTr("Default")
                    Accessible.name: qsTr("Default color")
                    checkable: true
                    checked: colorRow.current === "" && colorRow.custom === ""
                    onClicked: { colorRow.customOpen = false; panelCard.host.setOptions({ color: null }) }
                }
                Repeater {
                    model: Theme.gaugeColorNames
                    AbstractButton {
                        id: swatch
                        required property string modelData
                        objectName: "panelColor_" + modelData
                        width: Theme.touchTarget
                        height: Theme.touchTarget
                        checkable: true
                        checked: colorRow.current === modelData
                        Accessible.name: qsTr("Color %1").arg(Theme.gaugeColorLabel(modelData))
                        onToggled: { toggle(); colorRow.customOpen = false; panelCard.host.setOptions({ color: modelData }) }
                        contentItem: Item {}
                        background: Rectangle {
                            anchors.centerIn: parent
                            width: Math.min(parent.width, 32)
                            height: width
                            radius: 6
                            color: Theme.gaugeSwatch(swatch.modelData)
                            border.width: swatch.checked ? 3 : 1
                            border.color: swatch.checked ? Theme.text : Theme.outline
                        }
                    }
                }
                ThemedButton {
                    objectName: "panelColor_custom"
                    text: qsTr("Custom...")
                    Accessible.name: qsTr("Custom color")
                    // Not checkable: a click would flip the state the row's own rules decide
                    checked: colorRow.customShown
                    onClicked: colorRow.customOpen = !colorRow.customShown || colorRow.custom === ""
                }
            }
        }

        // Three whole numbers make a color of the user's own, stored as
        // "#rrggbb"; nothing is refused for being hard to see, it is said
        RowLayout {
            id: customRow
            objectName: "panelColorCustom"
            Layout.fillWidth: true
            visible: panelCard.drawsValue && colorRow.customShown
            spacing: 4
            // The color the fields start from: the stored custom one, else what the gauge draws now
            readonly property color base: colorRow.custom !== "" ? colorRow.custom
                : Theme.gaugeColor(panelCard.cell && panelCard.cell.options ? panelCard.cell.options.color : "") || Theme.gaugeValue
            function channel(i) { return Math.round([base.r, base.g, base.b][i] * 255) }
            // True once a field's text was edited by the user since it was bound: only then is a
            // color equal to the drawn one worth storing (a pass-through sends editingFinished too)
            property bool edited: false
            // "#rrggbb" for three well-formed whole numbers 0 to 255, else ""
            function typed() {
                let out = "#"
                for (const f of [fieldR, fieldG, fieldB]) {
                    const t = f.text.trim()
                    if (!/^[0-9]{1,3}$/.test(t) || Number(t) > 255)
                        return ""
                    out += (Number(t) < 16 ? "0" : "") + Number(t).toString(16)
                }
                return out
            }
            readonly property string previewHex: { void fieldR.text; void fieldG.text; void fieldB.text; return typed() }
            // The fields show the color again; what was typed is gone
            function rebind() {
                edited = false
                fieldR.text = Qt.binding(() => String(customRow.channel(0)))
                fieldG.text = Qt.binding(() => String(customRow.channel(1)))
                fieldB.text = Qt.binding(() => String(customRow.channel(2)))
            }
            // A value that isn't a whole number 0 to 255 stays typed, with its reason under the fields
            function commit() {
                const hex = typed()
                if (hex === "")
                    return
                // A focus pass-through sends editingFinished with nothing typed: nothing to store
                if (edited || !Qt.colorEqual(hex, base))
                    panelCard.host.setOptions({ color: hex })
                rebind()
            }
            // Another gauge, or the row hidden: what was left typed doesn't follow
            Connections {
                target: colorRow
                function onCustomShownChanged() { customRow.rebind() }
            }
            Connections {
                target: panelCard.host
                function onSelectedChanged() { customRow.rebind() }
                function onPageIndexChanged() { customRow.rebind() }
            }
            Caption { text: "R" }
            ChannelField { id: fieldR; objectName: "panelColorR"; index: 0; row: customRow }
            Caption { text: "G" }
            ChannelField { id: fieldG; objectName: "panelColorG"; index: 1; row: customRow }
            Caption { text: "B" }
            ChannelField { id: fieldB; objectName: "panelColorB"; index: 2; row: customRow }
        }
        // The preview, and under 3:1 what it means
        RowLayout {
            Layout.fillWidth: true
            visible: customRow.visible
            spacing: 8
            Rectangle {
                objectName: "panelColorPreview"
                Layout.preferredWidth: 32
                Layout.preferredHeight: 32
                radius: 6
                color: customRow.previewHex !== "" ? customRow.previewHex : "transparent"
                border.width: 1
                border.color: Theme.outline
            }
            Caption {
                objectName: "panelColorWhy"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: customRow.previewHex === ""
                color: Theme.danger
                text: qsTr("Each value is a whole number from 0 to 255")
            }
            Caption {
                objectName: "panelColorHard"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                // The look it is hard to read in (a layout can be opened under the other one); no
                // color is hard in both, since the two surfaces sit at the two ends of the lightness
                readonly property var hardIn: customRow.previewHex === "" ? []
                    : Theme.lookNames.filter(l => Theme.gaugeContrast(customRow.previewHex, l) < 3)
                visible: hardIn.length > 0
                text: hardIn[0] === "dark" ? qsTr("Hard to read in Dark") : qsTr("Hard to read in Light")
            }
        }

        Caption {
            objectName: "panelColorNight"
            Layout.fillWidth: true
            visible: panelCard.drawsValue && Theme.nightActive
            wrapMode: Text.Wrap
            text: qsTr("Night draws every gauge in its own tint")
        }

        // A zone's own color wins over the pick while its level is reached
        Caption {
            objectName: "panelColorZones"
            Layout.fillWidth: true
            visible: panelCard.drawsValue && !Theme.nightActive
                     && (colorRow.current !== "" || colorRow.custom !== "")
                     && panelCard.hasZoneLevel
            wrapMode: Text.Wrap
            text: qsTr("A zone's color replaces this one while its level is reached")
        }

        // The order: both forms' grids follow it
        GridLayout {
            Layout.fillWidth: true
            columns: panelCard.phone ? 2 : 4
            columnSpacing: 8
            rowSpacing: 8
            ThemedButton {
                objectName: "panelEarlier"
                Layout.fillWidth: true
                enabled: panelCard.host.selected > 0
                text: qsTr("Move earlier")
                onClicked: panelCard.host.moveTo(panelCard.host.selected - 1)
            }
            ThemedButton {
                objectName: "panelLater"
                Layout.fillWidth: true
                enabled: panelCard.host.selected < panelCard.host.shownCells.length - 1
                text: qsTr("Move later")
                onClicked: panelCard.host.moveTo(panelCard.host.selected + 1)
            }
            ThemedButton {
                objectName: "panelStart"
                Layout.fillWidth: true
                enabled: panelCard.host.selected > 0
                text: qsTr("To the start")
                onClicked: panelCard.host.moveTo(0)
            }
            ThemedButton {
                objectName: "panelEnd"
                Layout.fillWidth: true
                enabled: panelCard.host.selected < panelCard.host.shownCells.length - 1
                text: qsTr("To the end")
                onClicked: panelCard.host.moveTo(panelCard.host.shownCells.length - 1)
            }
        }

        // Remove, last and quiet, in edit mode or out of it; Undo
        // takes it back, so it asks nothing
        ThemedButton {
            objectName: "panelRemove"
            flat: true
            text: qsTr("Remove")
            Accessible.name: qsTr("Remove %1").arg(panelCard.name)
            onClicked: panelCard.host.removeCell(panelCard.host.selected)
        }
    }

    // A number option, typed in the unit shown when `converted`; empty
    // removes it when `clearable`, else puts the shown value back
    component OptionField: ThemedTextField {
        id: field
        required property string option
        property bool converted: true
        property bool clearable: true
        property real fallback: NaN
        readonly property var cell: panelCard.host.selectedCell
        readonly property var value: {
            const o = cell && cell.options ? cell.options[field.option] : undefined
            return o !== undefined ? o : field.fallback
        }
        // A converted end shows to the signal's decimals plus one (161.5565
        // mph at 1 decimal reads 161.56), 1 place with no row to take them from
        readonly property var entry: cell ? panelCard.host.signalHub.lookup(panelCard.host.lookupRefs(cell.refs ?? [])) : null
        readonly property int places: panelCard.decimalsNow + 1
        readonly property string shownText: cell === null ? ""
            : converted ? panelCard.host.fieldText(panelCard.host.shown(cell.unit ?? "", value), places)
                        : panelCard.host.fieldText(value)
        Layout.fillWidth: true
        Layout.preferredHeight: Theme.touchTarget
        text: shownText
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        onEditingFinished: {
            const t = text.trim()
            if (t === "" && clearable) {
                panelCard.host.setOptions({ [option]: null })
            } else {
                const v = Number(t)
                if (t !== "" && isFinite(v))
                    panelCard.host.setOptions({ [option]: converted ? panelCard.host.stored(cell.unit ?? "", v) : v })
            }
            text = Qt.binding(() => field.shownText)
        }
    }

    // One channel of the custom color: a whole number 0 to 255, committed with its two siblings
    component ChannelField: ThemedTextField {
        required property int index
        required property var row
        Layout.fillWidth: true
        Layout.minimumWidth: 40
        Layout.preferredWidth: 64
        Layout.preferredHeight: Theme.touchTarget
        inputMethodHints: Qt.ImhDigitsOnly
        maximumLength: 3
        text: String(row.channel(index))
        Accessible.name: [qsTr("Red value of the color, 0 to 255"), qsTr("Green value of the color, 0 to 255"),
                          qsTr("Blue value of the color, 0 to 255")][index]
        onTextEdited: row.edited = true
        onEditingFinished: row.commit()
    }

    component Caption: Label {
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
    }
}
