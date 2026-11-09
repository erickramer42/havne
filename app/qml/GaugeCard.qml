// File/layer: app/qml/GaugeCard.qml | Qt6 Quick, UI layer
// Testing: visual, through DashboardPage

import QtQuick

// Panel behind each dashboard gauge: flat, one surface with a hairline
// edge, no gradient and no shadow. The gauge fills the padded body, so
// the dashboard grid sizes cards and never gauges.
Rectangle {
    id: card
    default property alias content: body.data
    property real padding: 12

    radius: Theme.radiusCard
    border.color: Theme.hairline
    border.width: 1
    color: Theme.surface

    Item {
        id: body
        anchors.fill: parent
        anchors.margins: card.padding
    }
}
