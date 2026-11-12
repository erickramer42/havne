// File/layer: tests/qml/tst_segments.qml | Qt Quick Test, QML tier
// Testing: SegmentRow and SegmentButton, the one look of a closed choice:
//          the surface track with the hairline edge, the picked segment's
//          fill and outline as a selected ThemedButton draws them, in Dark
//          and Daylight. No session.

import QtQuick
import QtTest
import App

TestCase {
    id: testCase
    name: "Segments"
    when: windowShown
    width: 400
    height: 200

    Component {
        id: rowMaker
        Item {
            id: host
            property alias row: row
            property alias first: first
            property alias second: second
            property string picked: "a"
            width: 400
            height: 100
            SegmentRow {
                id: row
                SegmentButton {
                    id: first
                    text: "One"
                    selected: host.picked === "a"
                    onClicked: host.picked = "a"
                }
                SegmentButton {
                    id: second
                    text: "Two"
                    selected: host.picked === "b"
                    onClicked: host.picked = "b"
                }
            }
        }
    }

    // Whatever a case sets of the shared Theme, put back, pass or fail
    function cleanup() {
        Theme.night = "off"
        Theme.look = "dark"
    }

    function sameColor(actual, expected, what) {
        verify(Qt.colorEqual(actual, expected), what + ": " + actual + ", expected " + expected)
    }

    function test_theSharedLook_data() {
        return ["dark", "daylight"].map(n => ({ tag: n, look: n }))
    }
    function test_theSharedLook(data) {
        Theme.night = "off"
        Theme.look = data.look
        const h = createTemporaryObject(rowMaker, testCase.parent)
        verify(h)
        waitForPolish(h)
        sameColor(h.row.color, Theme.surface, "track fill")
        sameColor(h.row.border.color, Theme.hairline, "track edge")
        compare(h.row.border.width, 1)
        compare(h.row.radius, Theme.radius)
        // the picked segment is a selected ThemedButton's rank
        sameColor(h.first.background.color, Theme.elevatedLight, "picked fill")
        compare(h.first.background.border.width, 1, "picked outline")
        sameColor(h.first.background.border.color, Theme.outline, "picked outline color")
        sameColor(h.first.contentItem.color, Theme.text, "picked label")
        compare(h.second.background.color.a, 0, "unpicked has no fill")
        compare(h.second.background.border.width, 0, "unpicked has no outline")
        sameColor(h.second.contentItem.color, Theme.textDim, "unpicked label")
        // the segments sit 2 px inside the track, 2 px apart
        compare(h.first.mapToItem(h.row, 0, 0).x, 2)
        compare(h.second.x, h.first.x + h.first.width + 2, "2 px apart")
        h.second.clicked()
        compare(h.picked, "b")
        sameColor(h.second.background.color, Theme.elevatedLight, "picked moves")
        compare(h.first.background.border.width, 0)
    }

    // A locked row draws dim, as the buttons under it do
    function test_aLockedRowDims() {
        const h = createTemporaryObject(rowMaker, testCase.parent)
        verify(h)
        compare(h.first.opacity, 1, "enabled segments are full")
        h.row.enabled = false
        compare(h.first.opacity, 0.45, "a disabled picked segment dims")
        compare(h.second.opacity, 0.45, "and so does the other")
        h.row.enabled = true
        compare(h.second.opacity, 1, "enabled again")
    }

    function test_equalSharesFillTheRow() {
        const h = createTemporaryObject(rowMaker, testCase.parent)
        h.row.width = 300
        h.first.share = true
        h.second.share = true
        tryVerify(() => h.first.width > 100, 2000, "the shares fill the row")
        verify(Math.abs(h.first.width - h.second.width) < 0.5, "equal shares")
        verify(h.first.width + h.second.width + 6 <= 300 + 0.5, "inside the track")
    }
}
