// File/layer: app/qml/ThemedCheckBox.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.fields, QmlTest.settings (test_qml)
// A CheckBox with a drawn indicator, a phone's touch size inside a page that asks for one

import QtQuick
import QtQuick.Controls
import QtQuick.Shapes

CheckBox {
    id: control

    // The nearest ancestor's touchMin (SettingsSubpage sets one), as
    // ThemedButton takes it
    // qmllint disable missing-property
    readonly property real touchMin: {
        for (let p = control.parent; p; p = p.parent) {
            if (p.touchMin !== undefined)
                return p.touchMin
        }
        return 0
    }
    // qmllint enable missing-property
    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding, touchMin)
    // A box with no text is a touch's width on a phone too, not just its
    // indicator and padding (too narrow to hit)
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding,
                            control.text === "" ? Theme.touchTarget : 0)
    opacity: enabled ? 1.0 : 0.45

    indicator: Rectangle {
        implicitWidth: 20
        implicitHeight: 20
        x: control.text ? (control.mirrored ? control.width - width - control.rightPadding
                                            : control.leftPadding)
                        : control.leftPadding + (control.availableWidth - width) / 2
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: Theme.radius
        color: control.checked ? Theme.primary : "transparent"
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? Theme.text : Theme.outline

        // A path, not a glyph, so every font and theme draws the same mark
        Shape {
            anchors.fill: parent
            visible: control.checked
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: Theme.textOnPrimary
                strokeWidth: 2
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                startX: 5
                startY: 10.5
                PathLine { x: 8.5; y: 14 }
                PathLine { x: 15; y: 6.5 }
            }
        }
    }

    contentItem: Text {
        leftPadding: control.indicator && !control.mirrored ? control.indicator.width + control.spacing : 0
        rightPadding: control.indicator && control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        font.pixelSize: Theme.fontBody
        color: Theme.text
        wrapMode: Text.Wrap
        verticalAlignment: Text.AlignVCenter
    }
}
