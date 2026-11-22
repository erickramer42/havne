// File/layer: tests/qml/DashboardKit.qml | Qt Quick Test, QML tier
// Testing: QML: the base of the tst_dashboard*.qml files (test_qml); not a test itself

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

// The shared setup and helpers of tst_dashboard*.qml: a file derives from it and sets its `name`;
// `init` and `cleanup` are the kit's. Not DashEditKit: its init and cleanup differ, so deriving would change what a case starts from.
TestCase {
    id: testCase
    property alias hubComponent: hubComponent
    property alias pageComponent: pageComponent
    property alias storeComponent: storeComponent

    when: windowShown

    Component { id: hubComponent; SignalHub {} }

    Component { id: pageComponent; DashboardPage { width: 1280; height: 800 } }

    // The page's own store, to put each case's back
    Component {
        id: storeComponent
        Settings {
            category: "dashboards"
            property string phone: ""
            property string desktop: ""
            property int phonePage: 0
            property int desktopPage: 0
        }
    }

    // Each built-in gauge's label and its key in Bridge.gaugeSignals, with
    // the name the mock set gives the signal
    readonly property var gaugeSignals: ({
        "Engine speed": ["rpm", "RPM"],
        "Vehicle speed": ["speed", "Vehicle Speed"],
        "Coolant": ["coolant", "Coolant Temp"],
        "Oil temp": ["oil", "Engine Oil Temp"],
        "Boost": ["boost", "Boost"],
        "IAT": ["iat", "Intake Air Temp"],
        "MAF": ["maf", "MAF"],
        "Lambda": ["lambda", "Lambda"],
        "Battery": ["battery", "Battery Voltage"]
    })

    // The refs the gauge labeled `label` looks up
    function refsOf(label) {
        return Bridge.gaugeSignals[gaugeSignals[label][0]]
    }

    // A binding error in the app's QML fails the test
    // The page's Settings holds a write half a second and stores it when
    // the page goes, so the store is put back once the last case's pages
    // are gone, through setValue, which writes at once
    function init() {
        wait(0)
        const store = createTemporaryObject(storeComponent, testCase)
        store.setValue("phone", "")
        store.setValue("desktop", "")
        store.setValue("phonePage", 0)
        store.setValue("desktopPage", 0)
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        failOnWarning(/^Model size of /)   // a Repeater handed a negative count
    }

    function cleanup() {
        Theme.night = "off"
        Theme.nightStyle = "amber"
        Theme.look = "dark"
        Theme.nightFrom = "19:00"
        Theme.nightUntil = "07:00"
        Theme.now = () => new Date()
        Theme.shortLayout = false
        Bridge.unitSystem = 0
        Bridge.pickGauges = []
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Bridge.saveConditions('{"havne_conditions":1,"conditions":[]}')
        for (const p of Bridge.plugins())     // the store starts empty
            Bridge.removePlugin(p.sha256)
        Harness.clearFakeKnobs()
    }

    // A page with these properties, showing `doc` when one is given (a
    // document passed as a property at creation would reach the page as
    // a variant list, not a JS array). In the window, an item's `visible`
    // is its own; under the TestCase it reads false
    function pageOf(props, doc, inWindow) {
        const page = createTemporaryObject(pageComponent,
                                           inWindow ? testCase.Window.window.contentItem
                                                    : testCase, props)
        if (doc)
            page.doc = doc
        return page
    }

    function connect(uri) {
        compare(Bridge.connectSession(uri), "", uri + " connects")
    }

    // The page's gauges (GaugeBase items), by label
    function gauges(page) {
        const found = {}
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i]
                if (child.label !== undefined && child.note !== undefined
                        && child.valueText !== undefined)
                    found[child.label] = child
                walk(child)
            }
        }
        walk(page)
        for (const label in gaugeSignals)
            verify(found[label], "the Dashboard has a gauge labeled " + label)
        return found
    }

    // A signal file's text kept as a plugin of the user's own; profile: its one variant's
    function keptFile(text) {
        const r = Bridge.keepSignalText(text, "", "", "")
        return { ok: r.ok, words: r.words, sha256: r.sha256,
                 profile: r.ok ? r.profiles[0] : "" }
    }

    // ---- The renderer (ADR-0057 sections 3 and 4)

    // A document of the built-in page and `n - 1` more like it
    function pagesDoc(n) {
        const d = DashboardBook.builtIn("desktop")
        for (let i = 1; i < n; ++i)
            d.pages.push({ name: "Page " + (i + 1),
                           cells: JSON.parse(JSON.stringify(d.pages[0].cells)) })
        return d
    }

    // A one-page document of these cells
    function docOf(cells) {
        const d = DashboardBook.builtIn("desktop")
        d.pages = [{ name: "Test", cells: cells }]
        return d
    }

    // The gauge a cell's Loader made, or null
    function gaugeOf(cell) {
        return findChild(cell, "gauge").item
    }

    // Every cell's gauge Loader, on every page
    function loaders(page) {
        const found = []
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i]
                if (child.objectName === "gauge" && child.status !== undefined)
                    found.push(child)
                walk(child)
            }
        }
        walk(page)
        return found
    }

    function allLoaded(page) {
        return loaders(page).every(l => l.status === Loader.Ready)
    }
}
