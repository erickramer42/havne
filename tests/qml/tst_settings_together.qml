import QtCore
import QtQuick
import QtTest
import App
import AppTest

SettingsKit {
    id: testCase
    name: "SettingsTogether"

    function signalFileText(name, did, obd2) {
        const doc = {
            havne_signals: 1, name: name, source: { name: "t" },
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [{ key: "a", name: "A", read: { service: "22", number: did || "D100" },
                        field: { byte: 0, bits: 8 } }]
        }
        if (obd2)
            doc.extends = "obd2"
        return JSON.stringify(doc)
    }

    function importFile(page, fileName, name, did, obd2) {
        const url = Harness.writeScratchFile(fileName, signalFileText(name, did, obd2))
        verify(url.toString() !== "", "the scratch file is written")
        return page.importPart.importFile(url) && page.importPart.keepPending("", "")
    }

    function signalsRow(page) {
        return page.pageList.find(r => r.key === "signals")
    }

    function profileOf(uri) {
        const m = /[?&]profile=([^&]*)/.exec(uri)
        return m === null ? "" : m[1]
    }

    function test_theRowReadsTheVehiclesSet() {
        const page = createPage()
        press(page, "vehicleNew")
        compare(page.pickedVehicle.picks, ["obd2"])
        const field = find(page, "uriField")
        field.text = "elm327://COM9"
        compare(page.uriPart.profiles.map(p => p.name), ["default", "raw", "uds-generic"],
                "obd2 alone is what Default reads on an adapter")

        verify(importFile(page, "plus.json", "Plus", "D100", true))
        compare(page.picksPart.picks, ["obd2", "plugin.plus.base"])
        const set = page.picksPart.together
        compare(set.reads, "merged")
        verify(set.profile.startsWith("merged."), set.profile)
        compare(page.uriPart.profiles.map(p => p.name), ["default", set.profile, "raw", "uds-generic"])
        compare(find(page, "signalNote").text, "Plus 1.0.0 added: picked for Vehicle 1")
        compare(field.text, "elm327://COM9?profile=" + set.profile, "a pick under Default is read at once")
        const button = find(page, "profile_" + set.profile)
        compare(button.text, "Vehicle 1")
        verify(button.checked)
        compare(find(page, "profileHint").text,
                "obd2's standard signals and those of the plugin picked for Vehicle 1, read together")

        field.text = "elm327://COM9"
        verify(find(page, "profile_default").checked)
        compare(find(page, "profileHint").text,
                "Plain OBD-II, without the signals picked for Vehicle 1; choose Vehicle 1 to read them")
        verify(signalsRow(page).summary.endsWith("(Default reads plain OBD-II)"), signalsRow(page).summary)

        page.uriPart.chooseProfile(set.profile)
        verify(importFile(page, "more.json", "More", "D101", true))
        compare(find(page, "signalNote").text, "More 1.0.0 added: picked for Vehicle 1")
        const next = page.picksPart.together.profile
        verify(next !== set.profile, "a new pick is a new set")
        compare(field.text, "elm327://COM9?profile=" + next, "the field follows the vehicle's set")
        compare(page.uriPart.profiles.map(p => p.name), ["default", next, "raw", "uds-generic"])

        field.text = "mock://demo"
        verify(find(page, "profileHint").text.startsWith("Default reads OBD-II values"))
        verify(!signalsRow(page).summary.includes("Default reads"))
    }

    // Default is obd2 on an adapter already; a CAN interface that sends
    // reads the set, a listening one listens; a refused set refuses
    function test_aListConnectAddsTheVehiclesSet() {
        const page = createPage()
        page.canInterfaces = [{ name: "vcan0", kind: "vcan", up: true },
                              { name: "can0", kind: "can", up: true }]
        press(page, "vehicleNew")
        const from = page.connectFromPart
        compare(from.listConnectUri("elm327://COM5"), "elm327://COM5")
        compare(from.listConnectUri("socketcan://vcan0"), "socketcan://vcan0?profile=obd2")
        compare(from.listProfileWord("socketcan://vcan0"), "obd2")
        compare(from.listConnectUri("socketcan://can0"), "socketcan://can0")

        verify(importFile(page, "plus.json", "Plus", "D100", true))
        const set = page.picksPart.together.profile
        compare(from.listConnectUri("elm327://COM5"), "elm327://COM5?profile=" + set)
        compare(from.listConnectUri("socketcan://vcan0"), "socketcan://vcan0?profile=" + set)
        compare(from.listConnectUri("socketcan://can0"), "socketcan://can0")
        compare(from.listConnectUri("elm327://COM5?profile=raw"), "elm327://COM5?profile=raw")
        compare(from.listConnectUri("mock://demo"), "mock://demo")
        compare(from.connectProfileWords["socketcan://vcan0"], "Vehicle 1")

        page.saveVehicles(VehicleBook.setPicks(page.vehicles, ["obd2", "plugin.gone.base"]))
        compare(page.picksPart.together.reads, "refused")
        const words = page.picksPart.together.words
        verify(words.includes("plugin.gone.base") || words.includes("gone"), words)
        from.connectFromList("elm327://COM5")
        verify(!Bridge.connecting && !Bridge.connected, "nothing connects")
        compare(page.connectPart.errorLabel.text, words)
        verify(page.connectPart.errorLabel.visible)
        compare(from.setRefusal("elm327://COM5?profile=raw"), "", "a profile named reads as named")
    }

    // A kept merged name reads as the vehicle's set now, never dropped
    function test_aKeptListeningAddressKeepsItsSet() {
        const page = createPage()
        page.canInterfaces = [{ name: "can0", kind: "can", up: true }]
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "D100", true))
        const set = page.picksPart.together.profile
        const from = page.connectFromPart
        const kept = "socketcan://can0?profile=merged.0123456789abcdef"
        compare(from.listAddress(kept), kept)
        compare(from.listConnectUri(kept), "socketcan://can0?profile=" + set)
        compare(from.listProfileWord(kept), "Vehicle 1")
        compare(from.listConnectUri("mock://demo?profile=merged.0123456789abcdef"),
                "mock://demo?profile=merged.0123456789abcdef", "a typed mock reads as typed")

        const typed = "mock://demo?profile=" + set
        from.requestConnect(typed)
        tryVerify(() => Bridge.connected, 5000)
        compare(Bridge.currentUri, typed)
    }

    function test_theExportNeverListsObd2AsNotInTheStore() {
        const page = createPage()
        press(page, "vehicleNew")
        const dash = createTemporaryObject(dashComponent, testCase.parent)
        dash.vehicles = page.vehicles
        compare(VehicleBook.picked(dash.vehicles).picks, ["obd2"])
        const url = Harness.writeScratchFile("together-layout.json", "")
        verify(dash.layoutFilePart.exportLayoutTo(url), dash.layoutFilePart.layoutNote)
        verify(!dash.layoutFilePart.layoutNote.includes("aren't in the store"), dash.layoutFilePart.layoutNote)
    }

    function partOf(page, i) {
        return find(page, "togetherPart_" + i)
    }

    function test_obd2sRowAlwaysShowsAndUnpicksToModules() {
        const page = createPage()
        page.open("signals")
        press(page, "vehicleNew")
        verify(find(page, "settingsTogether").visible)
        compare(page.picksPart.together.reads, "obd2")
        compare(find(page, "togetherHeading").text, "Read together for Vehicle 1")
        compare(find(page, "togetherObd2Words").text, "the base: obd2's standard signals, read beside every plugin's")
        const pick = find(page, "togetherObd2Pick")
        compare(pick.text, "Unpick")
        verify(pick.enabled)
        pick.clicked()
        compare(page.picksPart.picks, [])
        compare(page.picksPart.together.base, "modules")
        verify(find(page, "togetherObd2").visible, "the row stays")
        compare(find(page, "togetherObd2Words").text, "Unpicked: its signals aren't read; the emissions codes still are")
        verify(find(page, "togetherNothing").visible)
        compare(pick.text, "Pick")
        pick.clicked()
        compare(page.picksPart.picks, ["obd2"])
        verify(!find(page, "togetherNothing").visible)
    }

    function test_obd2sUnpickIsDisabledWhileAPartExtendsIt() {
        const page = createPage()
        page.open("signals")
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "D100", true))
        const pick = find(page, "togetherObd2Pick")
        verify(!pick.enabled)
        compare(find(page, "togetherObd2Why").text, "read under Plus, which extends it")
        page.saveVehicles(VehicleBook.setPicks(page.vehicles, ["plugin.plus.base"]))
        compare(page.picksPart.together.base, "obd2", "the part brings obd2 in")
        compare(find(page, "togetherObd2Words").text, "the base: obd2's standard signals, read beside every plugin's")
        compare(pick.text, "Pick")
        verify(pick.enabled, "it can always be picked back")
    }

    function test_aMoveReordersThePicksAndKeepsTheFocus() {
        const page = createPage()
        page.open("signals")
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "D100"))
        verify(importFile(page, "more.json", "More", "D101"))
        compare(page.picksPart.picks, ["obd2", "plugin.plus.base", "plugin.more.base"])
        compare(find(partOf(page, 0), "togetherPartTitle").text, "Plus 1.0.0")
        verify(!find(partOf(page, 0), "togetherUp").enabled)
        verify(!find(partOf(page, 1), "togetherDown").enabled)
        const down = find(partOf(page, 0), "togetherDown")
        down.forceActiveFocus()
        down.clicked()
        compare(page.picksPart.picks, ["obd2", "plugin.more.base", "plugin.plus.base"])
        compare(find(partOf(page, 0), "togetherPartTitle").text, "More 1.0.0")
        compare(find(partOf(page, 1), "togetherPartTitle").text, "Plus 1.0.0")
        verify(find(partOf(page, 1), "togetherUp").activeFocus, "the focus goes with the moved row")
        find(partOf(page, 1), "togetherUp").clicked()
        compare(page.picksPart.picks, ["obd2", "plugin.plus.base", "plugin.more.base"])
        verify(find(partOf(page, 0), "togetherDown").activeFocus)
    }

    function test_theOwnVariantSitsLastWithNoMove() {
        const page = createPage()
        page.open("signals")
        press(page, "vehicleNew")
        verify(page.ownPart.saveOwn(JSON.parse(signalFileText("Mine", "D200")), ""))
        const own = page.pickedVehicle.own
        verify(own !== "")
        verify(importFile(page, "plus.json", "Plus", "D100"))
        compare(page.picksPart.picks, ["obd2", own, "plugin.plus.base"])
        const parts = page.picksPart.together.parts
        compare(parts.map(p => p.profile), ["plugin.plus.base", own], "the own variant is merged last")
        const mine = partOf(page, 1)
        verify(find(mine, "togetherPartWords").text.endsWith(
            " · yours: your signals replace a plugin's of the same name; obd2's standard signals are read beside yours"))
        verify(!find(mine, "togetherUp").visible && !find(mine, "togetherDown").visible)
        verify(!find(partOf(page, 0), "togetherDown").enabled, "the last plugin before the own one")
        compare(VehicleBook.movePick(page.vehicles, own, -1), page.vehicles)
    }

    function test_notesFoldByKindAndOpen() {
        const page = createPage()
        page.open("signals")
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "D100"))
        verify(importFile(page, "same.json", "Same", "D100"))
        const set = page.picksPart.together
        verify(set.folded.length > 0, JSON.stringify(set.notes))
        const kind = set.folded[0].kind
        const fold = find(page, "togetherFold_" + kind)
        compare(findChild(fold, "togetherNote"), null, "folded")
        find(fold, "togetherFoldButton").clicked()
        tryVerify(() => findChild(fold, "togetherNote") !== null)
        compare(find(fold, "togetherNote").text, set.notes.find(n => n.kind === kind).text)
        find(fold, "togetherFoldButton").clicked()
        tryVerify(() => findChild(fold, "togetherNote") === null, 1000, "closed again")
        verify(find(page, "togetherTotals").text.startsWith("2 plugins, "))
    }

    function test_theRowsFitAt384And320() {
        const page = createPage()
        page.open("signals")
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "D100"))
        verify(importFile(page, "more.json", "A plugin with a rather long name to wrap", "D101"))
        const part = find(page, "settingsTogether")
        for (const w of [384, 320]) {
            page.width = w
            tryVerify(() => part.width > 0 && part.width <= w)
            waitForPolish(part)
            compare(pastTheEdge(part, part, []), [], "at " + w)
            verify(!part.oneRow)
        }
    }

    function test_aButtonOnTheRowKeepsItsWholeWord() {
        const page = createPage()
        page.open("signals")
        press(page, "vehicleNew")
        verify(importFile(page, "plus.json", "Plus", "D100"))
        const part = find(page, "settingsTogether")
        // A width with a fraction, as a scaled screen's font gives
        for (const name of ["togetherObd2Pick", "togetherUp", "togetherDown", "togetherUnpick"])
            find(page, name).leftPadding += 0.4
        for (const w of [1100, 700]) {
            page.width = w
            tryVerify(() => part.oneRow && part.width <= w)
            waitForPolish(part)
            for (const name of ["togetherObd2Pick", "togetherUp", "togetherDown", "togetherUnpick"]) {
                const b = find(page, name)
                verify(b.width >= b.implicitWidth && b.contentItem.lineCount === 1, name + " at " + w + ": " + b.width + " < " + b.implicitWidth)
            }
        }
    }

    // Said once: the Log line and the Signals page's line, until Dismiss
    function test_theCarryOverLineShowsOnceAndDismisses() {
        const kept = Bridge.keepSignalText(JSON.stringify({
            havne_signals: 1, name: "Boostie", source: { name: "t" }, extends: "obd2", ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [{ key: "boost", name: "Boost", read: { service: "22", number: "D100" }, field: { byte: 0 } }]
        }), "", "", "")
        verify(kept.ok, kept.words)
        const profile = kept.profiles[0]
        let page = createPage()
        page.open("signals")
        press(page, "vehicleNew")
        const ahead = Bridge.vehicleSet(["obd2", profile], "", "Vehicle 1")
        compare(ahead.beside.length, 1, JSON.stringify(ahead.notes))
        const b = JSON.parse(JSON.stringify(ahead.beside[0]))
        page.saveVehicles(VehicleBook.change(page.vehicles, e => e.plotted = [b.baseTick]))
        const unit = Bridge.profileSignals("mock://?profile=obd2").find(r => r.ref === b.baseRef).unit
        const saved = Bridge.saveConditions(JSON.stringify({ havne_conditions: 1, conditions: [
            { key: "hi", name: "Boost high", when: { signal: [b.baseRef], unit: unit, above: 10 },
              alert: { level: "warning", banner: true } }] }))
        verify(saved.ok, saved.words)

        page.saveVehicles(VehicleBook.setPicks(page.vehicles, ["obd2", profile]))
        tryVerify(() => VehicleBook.stepDone(page.pickedVehicle, "beside-obd2"), 2000, "carried at the first merge")
        compare(page.pickedVehicle.plotted, [b.standingKey])
        compare(JSON.parse(Bridge.conditionsText()).conditions[0].when.signal, ["key:" + b.standingKey, b.baseRef])
        const line = "1 alert and 1 tick that showed Boostie's 'Boost' keep showing it; obd2's 'Boost' is read beside it"
        const carried = find(page, "togetherCarried")
        verify(carried.visible)
        compare(find(page, "togetherCarriedText").text, line)

        page.destroy()
        wait(10)
        page = createPage()
        page.open("signals")
        verify(find(page, "togetherCarried").visible, "kept until dismissed")
        find(page, "togetherCarriedDismiss").clicked()
        verify(!find(page, "togetherCarried").visible)
        verify(VehicleBook.stepDone(page.pickedVehicle, "beside-obd2-said"))
        page.destroy()
        wait(10)
        page = createPage()
        page.open("signals")
        verify(!find(page, "togetherCarried").visible, "said once")
        compare(page.pickedVehicle.plotted, [b.standingKey], "and carried once")
        Bridge.saveConditions('{"havne_conditions":1,"conditions":[]}')
    }
}
