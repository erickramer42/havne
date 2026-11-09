// File/layer: app/qml/GaugeCard.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard (test_qml)

import QtQuick

// Panel behind each dashboard gauge: flat, one surface with a hairline
// edge, no gradient and no shadow; `plain`, it paints none of them and
// what's behind it does (the phone's joined panel, ADR-0057's appendix
// section 3). The gauge fills the padded body, so the dashboard grid
// sizes cards and never gauges.
Rectangle {
    id: card
    default property alias content: body.data
    property real padding: 12
    property bool plain: false

    radius: plain ? 0 : Theme.radiusCard
    border.color: Theme.hairline
    border.width: plain ? 0 : 1
    color: plain ? "transparent" : Theme.surface

    Item {
        id: body
        anchors.fill: parent
        anchors.margins: card.padding
    }
}
