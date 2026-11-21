// File/layer: tests/qml/tst_plaintext.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.plaintext (test_qml)

import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

// A name from a file is drawn as text by the app's own ComboBox and MenuItem
// (the stock ones draw AutoText: markup with an image crashes Qt 6.12's
// Basic style), and a URL naming another host never reaches a loader
TestCase {
    id: testCase
    name: "PlainText"
    when: windowShown
    width: 400
    height: 300
    visible: true

    readonly property string markup: "<img src=\"qrc:/n.png\"><b>x</b>"

    function findText(item, wanted) {
        if (item.text === wanted && item.textFormat !== undefined)
            return item
        for (let i = 0; i < item.children.length; i++) {
            const f = findText(item.children[i], wanted)
            if (f)
                return f
        }
        return null
    }

    function test_aComboBoxDrawsANameAsText() {
        const box = createTemporaryObject(plainBox, testCase)
        verify(box)
        const shown = findText(box, markup)
        verify(shown, "the shown name is a Text")
        compare(shown.textFormat, Text.PlainText)
        box.popup.open()
        tryVerify(() => box.popup.opened)
        const row = findText(box.popup.contentItem, markup)
        verify(row, "the row's name is a Text")
        compare(row.textFormat, Text.PlainText)
        box.popup.close()
    }

    function test_aComboBoxOfMapsDrawsTheNamesRole() {
        const box = createTemporaryObject(plainMapBox, testCase)
        verify(box)
        box.popup.open()
        tryVerify(() => box.popup.opened)
        const row = findText(box.popup.contentItem, markup)
        verify(row, "a role's value is the row's Text")
        compare(row.textFormat, Text.PlainText)
        box.popup.close()
    }

    function test_aMenuItemDrawsANameAsText() {
        const menu = createTemporaryObject(plainMenu, testCase)
        verify(menu)
        menu.popup(10, 10)
        tryVerify(() => menu.opened)
        const item = menu.itemAt(0)
        verify(item)
        compare(item.contentItem.text, markup)
        compare(item.contentItem.textFormat, Text.PlainText)
        menu.close()
    }

    function test_aUrlNamingAnotherHostLoadsNothing() {
        const local = Harness.writeScratchFile("one.ppm", "P3
1 1
255
255 0 0
")
        const before = Harness.urlsRefused()
        const here = Qt.createQmlObject("import QtQuick; Image { asynchronous: false; source: \"" + local + "\" }", testCase)
        compare(here.status, Image.Ready, "a file on this machine still loads")
        compare(Harness.urlsRefused(), before, "and is not refused")
        here.destroy()
        // The engine's interceptor empties each of these before a loader
        // can open it as a UNC path
        for (const url of ["file://127.0.0.1/share/a.ppm", "file:////127.0.0.1/share/a.ppm",
                           "file://localhost.invalid/share/a.ppm"]) {
            const was = Harness.urlsRefused()
            const probe = Qt.createQmlObject("import QtQuick; Image {}", testCase)
            probe.source = url
            tryVerify(() => probe.status === Image.Error, 5000, url)
            verify(Harness.urlsRefused() > was, "the interceptor emptied " + url)
            probe.destroy()
        }
    }

    function test_theRuleNamesEveryOtherHost() {
        // Win32 reads "/" and "\" alike, so a UNC path may be written with
        // either, mixed, or percent-encoded; a local extended-length path
        // (a "\\?\C:" path) is refused with them, which costs nothing here
        const another = ["file://host/share/a.png", "file:////host/share/a.png", "file://127.0.0.1/a.png",
                         "file:///\\host\\share\\a.png", "file:///\\\\host/share/a.png",
                         "file:///%5C%5Chost/share/a.png", "file:///%5Chost%5Cshare%5Ca.png",
                         "file:///\\\\?\\UNC\\host\\share\\a.png", "file:////?/UNC/host/share/a.png",
                         "file:///\\\\.\\pipe\\x", "file:////./pipe/x", "file:///\\\\?\\C:\\x\\a.png"]
        for (const u of another)
            verify(Harness.namesAnotherHost(u), u)
        const here = ["file:///D:/data/a.png", "file:///tmp/a.png", "qrc:/qt/qml/App/a.png", "data:image/png;base64,AA=="]
        for (const u of here)
            verify(!Harness.namesAnotherHost(u), u)
    }

    // A QML, JS or qmldir file on a host-named URL passes: an install run
    // from a share keeps its qml/ folder there. A string URL does not.
    function test_onlyAStringUrlIsEmptied() {
        const url = "file://host/share/qml/QtQuick/qmldir"
        for (const kind of ["qml", "js", "qmldir"])
            compare(Harness.intercepted(url, kind).toString(), url, kind)
        compare(Harness.intercepted(url, "string").toString(), "")
        compare(Harness.intercepted("file:///D:/data/a.png", "string").toString(), "file:///D:/data/a.png")
    }

    // A load the interceptor never sees (an XMLHttpRequest) is refused by
    // the network manager on the same rule
    function test_theNetworkManagerRefusesTheSameUrls() {
        verify(Harness.networkStays("file:///D:/data/a.png"))
        verify(Harness.networkStays("qrc:/qt/qml/App/a.png"))
        verify(Harness.networkStays("data:text/plain,x"))
        verify(!Harness.networkStays("file:////host/share/a.png"))
        verify(!Harness.networkStays("file://host/share/a.png"))
        verify(!Harness.networkStays("http://127.0.0.1/a.png"))
    }

    function test_aMixedSeparatorUncPathIsRefusedBeforeALoaderOpensIt() {
        for (const url of ["file:///\\host\\share\\a.ppm", "file:///%5C%5Chost/share/a.ppm"]) {
            const was = Harness.urlsRefused()
            const probe = Qt.createQmlObject("import QtQuick; Image {}", testCase)
            probe.source = url
            tryVerify(() => probe.status === Image.Error, 5000, url)
            verify(Harness.urlsRefused() > was, "the interceptor emptied " + url)
            probe.destroy()
        }
    }

    Component {
        id: plainBox
        PlainComboBox { model: [testCase.markup] }
    }
    Component {
        id: plainMapBox
        PlainComboBox { textRole: "name"; model: [{ name: testCase.markup }] }
    }
    Component {
        id: plainMenu
        Menu { PlainMenuItem { text: testCase.markup } }
    }
}
