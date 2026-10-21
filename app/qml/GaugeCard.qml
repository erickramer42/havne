// File/layer: app/qml/GaugeCard.qml | Qt6 Quick, UI layer
// Testing: visual, through DashboardPage

import QtQuick

// Panel behind each dashboard gauge. The gauge fills the padded body,
// so the dashboard grid sizes cards and never gauges.
Rectangle {
    id: card
    default property alias content: body.data
    property real padding: 12

    radius: 12
    border.color: Qt.alpha(Theme.text, 0.06)
    border.width: 1
    gradient: Gradient {
        GradientStop { position: 0.0; color: Qt.lighter(Theme.surface, 1.12) }
        GradientStop { position: 1.0; color: Theme.surface }
    }

    Item {
        id: body
        anchors.fill: parent
        anchors.margins: card.padding
    }
}
