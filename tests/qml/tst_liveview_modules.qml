import QtQuick
import QtTest
import App
import AppTest

LiveViewKit {
    id: testCase
    name: "LiveViewModules"

    function sig(key, name, ecu, did) {
        return { key: key, name: name, ecu: ecu, read: { service: "22", number: did }, field: { byte: 0 } }
    }
    readonly property string twoModules: JSON.stringify({
        havne_signals: 3, name: "Two", source: { name: "t" },
        ecus: [{ id: "pcm", rx: "7E8", name: "Engine" }, { id: "bcm", rx: "7EA", name: "Body" }],
        signals: [sig("ev", "Battery voltage", "pcm", "D100"), sig("bv", "Battery voltage", "bcm", "D100"),
                  sig("fuel", "Fuel level", "bcm", "D101"),
                  { key: "sum", name: "Sum", kind: "computed", expr: "ev + bv" },
                  { key: "twice", name: "Twice", kind: "computed", expr: "ev * 2" }]
    })
    function roomSig(key, ecu, did, plot) {
        const s = { key: key, name: "N " + key, ecu: ecu, read: { service: "22", number: did }, field: { byte: 0 } }
        if (plot !== undefined)
            s.plot = plot
        return s
    }
    readonly property string roomFile: JSON.stringify({
        havne_signals: 1, name: "Room", source: { name: "t" },
        ecus: [{ id: "m1", rx: "7E9", name: "M1" }, { id: "m2", rx: "7EA", name: "M2" },
               { id: "m3", rx: "7EB", name: "M3" }, { id: "door", rx: "7EC" },
               { id: "m5", rx: "7ED", name: "M5" }, { id: "m6", rx: "7EE", name: "M6" }],
        signals: [roomSig("s1", "m1", "1000", 1), roomSig("s1b", "m1", "1009"), roomSig("s2", "m2", "1001", 2),
                  roomSig("s2b", "m2", "1007"), roomSig("s3", "m3", "1002", 3), roomSig("s3b", "m3", "1006"),
                  roomSig("s4", "door", "1003"), roomSig("s5", "m5", "1004"), roomSig("s5b", "m5", "1008")]
    })

    function keptProfile(text) {
        const kept = Bridge.keepSignalText(text, "", "", "")
        verify(kept.ok, kept.words)
        return kept.profiles[0]
    }
    function pageInWindow() {
        return createTemporaryObject(pageComponent, testCase.Window.window.contentItem)
    }
    function rowNamed(list, name) {
        for (let i = 0; i < list.count; ++i) {
            const row = list.itemAtIndex(i)
            if (row !== null && row.name === name)
                return row
        }
        return null
    }
    function connectTwo(page) {
        compare(Bridge.connectSession("mock://?profile=" + keptProfile(twoModules)), "")
        const list = findChild(page, "signalList")
        tryVerify(() => list.count === 5 && list.itemAtIndex(4) !== null, 3000, "every row has a delegate")
        return list
    }
    function connectRoom(max) {
        Harness.armFakeKnob("ECUIO_FAKE_MODULE_DIDS",
            "7E9:1000=11;7E9:1009=19;7EA:1001=12;7EA:1007=17;7EB:1002=13;"
            + "7EB:1006=16;7EC:1003=14;7ED:1004=15;7ED:1008=18")
        Harness.armFakeKnob("ECUIO_FAKE_FILTER_GATING", "1")
        Harness.armFakeKnob("ECUIO_FAKE_FILTER_MAX", max)
        compare(Bridge.connectSession("j2534://" + Harness.fakeDllPath + "?profile=" + keptProfile(roomFile)
                                      + "&identify=0&diagnose=0"), "")
    }
    function tick(list, name) {
        tryVerify(() => rowNamed(list, name) !== null, 3000, name + " has a row")
        const box = findChild(rowNamed(list, name), "plotCheck")
        box.toggle()
        box.toggled()
    }

    function test_theModulesPartShowsOnlyForTwoModules() {
        const page = pageInWindow()
        const list = connectTwo(page)
        const bar = findChild(page, "liveFilter")
        compare(bar.modules.map(m => m.name), ["Engine", "Body"])
        bar.options.open()
        tryVerify(() => findChild(bar.options.contentItem, "modulesCaption") !== null
                        && findChild(bar.options.contentItem, "modulesCaption").visible)
        const body = findChild(bar.options.contentItem, "module_" + bar.modules[1].index)
        verify(body !== null && body.visible)
        body.toggled()
        verify(Bridge.liveSignalModel.filter.moduleChosen(bar.modules[1].index))
        verify(findChild(bar, "filterButton").checked, "Filter reads checked")
        tryCompare(list, "count", 2, 2000, "Body's two rows")
        bar.options.close()
        Bridge.liveSignalModel.filter.clear()

        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000)
        const one = JSON.stringify({
            havne_signals: 3, name: "One", source: { name: "t" }, ecus: [{ id: "pcm", rx: "7E8", name: "Engine" }],
            signals: [sig("ev", "Battery voltage", "pcm", "D100"), sig("fuel", "Fuel level", "pcm", "D101")]
        })
        compare(Bridge.connectSession("mock://?profile=" + keptProfile(one)), "")
        tryCompare(Bridge.liveSignalModel.modules, "length", 1)
        bar.options.open()
        tryVerify(() => bar.options.opened)
        verify(!findChild(bar.options.contentItem, "modulesCaption").visible, "one module shows no part")
        bar.options.close()
    }

    function test_aRepeatedNameShowsItsTag() {
        const page = pageInWindow()
        const list = connectTwo(page)
        const engine = list.itemAtIndex(0)
        const body = list.itemAtIndex(1)
        compare(findChild(engine, "tagLabel").text, "Engine")
        verify(findChild(engine, "tagLabel").visible)
        compare(findChild(body, "tagLabel").text, "Body")
        verify(engine.Accessible.name.startsWith("Battery voltage, Engine: "), engine.Accessible.name)
        const fuel = rowNamed(list, "Fuel level")
        verify(!findChild(fuel, "tagLabel").visible, "a name one row has")
        verify(fuel.Accessible.name.startsWith("Fuel level: "))
    }

    function test_aComputedRowPassesItsLeavesModule() {
        const page = pageInWindow()
        const list = connectTwo(page)
        const engine = Bridge.liveSignalModel.modules.find(m => m.name === "Engine")
        Bridge.liveSignalModel.filter.toggleModule(engine.index)
        tryCompare(list, "count", 2, 2000, "ev and Twice")
        tryVerify(() => rowNamed(list, "Twice") !== null, 2000, "Twice reads Engine's ev")
        compare(rowNamed(list, "Sum"), null, "Sum reads two modules")
    }

    function test_aHeldRowReadsNoRoomAndStaysTicked() {
        if (Harness.fakeDllPath === "")
            skip("the fake J2534 DLL is Windows-only")
        const page = pageInWindow()
        connectRoom("2")
        const list = findChild(page, "signalList")
        tryVerify(() => Bridge.signalModel.noRoomCount === 1, 8000, "one ranked row waits for room")
        tryVerify(() => rowNamed(list, "N s3") !== null, 2000)
        const row = rowNamed(list, "N s3")
        tryCompare(findChild(row, "valueLabel"), "text", "No room")
        verify(findChild(row, "plotCheck").checked, "its box stays ticked")
        compare(findChild(row, "valueLabel").color, Theme.textDim)
        const unasked = findChild(page, "unaskedNote")
        verify(!unasked.visible || !unasked.text.startsWith("7 signals"), "a room row isn't counted not asked")
        const note = findChild(page, "noRoomNote")
        tryVerify(() => note.visible)
        verify(note.text.startsWith("M3 isn't read: the adapter listens to 2 modules at a time and all 2 are in use."),
               note.text)

        for (const name of ["N s1b", "N s2b", "N s3b", "N s4", "N s5", "N s5b"])
            tick(list, name)
        tryVerify(() => Bridge.signalModel.unaskedCount === 0, 8000, "every row is asked or waits for room")
        verify(Bridge.signalModel.noRoomCount > 0)
        verify(note.visible, "the line follows the rows with no room, not the unasked ones")
    }

    function test_theNoRoomLineListsOnlyFreeableModules() {
        if (Harness.fakeDllPath === "")
            skip("the fake J2534 DLL is Windows-only")
        const page = pageInWindow()
        connectRoom("4")
        const list = findChild(page, "signalList")
        tick(list, "N s5")
        tick(list, "N s1b")
        tryVerify(() => Bridge.roomHolders([]).budget === 4, 8000)
        tick(list, "N s4")
        const note = findChild(page, "noRoomNote")
        tryVerify(() => note.visible && note.text.startsWith("DOOR isn't read"), 8000)
        compare(note.text, Bridge.roomHolders(page.roomPages).words)
        verify(note.text.includes("To read it, free one: M5 (1 tick)."), note.text)
        verify(!note.text.includes("M1"), "M1 holds a ranked signal")

        page.roomPages = [{ name: "Track", cells: [["key:s5"]] }]
        tryVerify(() => note.text.includes("M5 (1 tick, 1 gauge on Track)"), 2000, note.text)

        verify(Bridge.startLog())
        tryVerify(() => note.text.startsWith("Logging 1 signal the adapter has no room for: DOOR's N s4: "
                                             + "their columns stay empty."), 2000, note.text)
        verify(note.text.endsWith("The log keeps reading what it started with: stop it to change what is read."),
               note.text)
        Bridge.stopLog()
    }

    function test_theNoRoomLineSaysWhyWhenNoneCanBeFreed() {
        if (Harness.fakeDllPath === "")
            skip("the fake J2534 DLL is Windows-only")
        const page = pageInWindow()
        connectRoom("3")
        const list = findChild(page, "signalList")
        tryVerify(() => Bridge.roomHolders([]).budget === 3, 8000)
        tick(list, "N s4")
        const note = findChild(page, "noRoomNote")
        tryVerify(() => note.visible && note.text.startsWith("DOOR isn't read"), 8000)
        verify(note.text.includes(": nothing on Live frees one."), note.text)
        verify(!note.text.includes("free one:"), note.text)
    }
}
