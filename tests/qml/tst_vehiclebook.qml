// File/layer: tests/qml/tst_vehiclebook.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.vehiclebook (test_qml)

import QtQuick
import QtTest
import App
import AppTest
TestCase {
    name: "VehicleBook"

    // Variants as a vehicle picks them, by their profiles
    readonly property string extras: "plugin.daily-driver-extras.base"
    readonly property string simBase: "plugin.example-sim.base"
    readonly property string sim2019: "plugin.example-sim.my2019"
    readonly property string own: "plugin.daily-driver-signals.base"
    // A signal file as the stores name it, by its content's SHA-256
    readonly property string shaA: "a".repeat(64)
    readonly property string shaB: "b".repeat(64)
    readonly property string shaC: "c".repeat(64)
    // A recording's folder as the Recordings page names it
    readonly property string baselineA: "/recordings/20261106-090000 Daily driver"

    function notMoved(sha, name) {
        return { sha256: sha, name: name, words: "Its copy is missing: import the file again" }
    }

    // Two vehicles, the first picked and recognized, with picks, an own
    // variant, a file that didn't move and a key a later version adds
    function book() {
        return {
            format: 2,
            salt: "5a17",
            picked: "v1",
            list: [
                { id: "v1", label: "Daily driver", vinKey: "key-a",
                  uri: "elm327://COM6?profile=obd2", plotted: ["Timing Advance"],
                  unplotted: ["Intake Air Temp"], picks: [extras, sim2019, own], own: own,
                  vinMatches: [simBase, sim2019], baseline: baselineA,
                  unmigrated: [notMoved(shaA, "Old extras")], later: { kept: [1, 2] } },
                { id: "v2", label: "Weekend car", vinKey: "", uri: "", plotted: [],
                  unplotted: [], picks: [], own: "", vinMatches: [], baseline: "" }
            ]
        }
    }

    function test_aStoredDocumentReadsBack() {
        const d = VehicleBook.parse(VehicleBook.serialize(book()))
        compare(d, book())
        compare(VehicleBook.copy(book()), book(), "a copy is the document")
    }

    // The contract with the migration: every book it writes reads, each
    // entry with its picks, its own variant, its address and what didn't
    // move, and a key this version doesn't know is carried
    function test_theMigrationsBooksRead() {
        const cases = Harness.vehicleBookCases()
        verify(cases.length >= 6, cases.join(", "))
        for (const name of cases) {
            const text = Harness.vehicleBookText(name, "expect.json")
            verify(text !== "", name)
            const raw = JSON.parse(text)
            const d = VehicleBook.parse(text)
            compare(d.format, 2, name)
            compare(d.list.length, raw.list.length, name + ": every entry is an entry")
            compare(d.salt, raw.salt, name)
            compare(d.picked, raw.picked, name)
            for (let i = 0; i < raw.list.length; ++i) {
                const was = raw.list[i]
                const is = d.list[i]
                for (const key of ["id", "label", "vinKey", "uri", "own"])
                    compare(is[key], was[key], name + " " + key)
                compare(is.picks, was.picks, name + " picks")
                compare(is.unplotted, was.unplotted, name)
                compare(is.plotted, was.plotted === undefined ? [] : was.plotted, name)
                compare(is.baseline, was.baseline === undefined ? "" : was.baseline, name)
                compare(is.vinMatches, was.vinMatches === undefined ? [] : was.vinMatches, name)
                compare(is.unmigrated, was.unmigrated, name + " what didn't move")
                compare(is.later, was.later, name + " a later version's key")
            }
            compare(d.later, raw.later, name + " the book's own later key")
            compare(VehicleBook.parse(VehicleBook.serialize(d)), d, name + ": stored and read")
        }
        const two = VehicleBook.parse(Harness.vehicleBookText("two-vehicles", "expect.json"))
        compare(two.list[0].picks.length, 4)
        compare(two.list[0].own, "plugin.daily-driver-own.base")
        compare(two.list[0].later, { kept: true })
        const odd = VehicleBook.parse(Harness.vehicleBookText("carried-odd", "expect.json"))
        compare(odd.later, { kept: true })
        const moved = VehicleBook.parse(Harness.vehicleBookText("unmigrated", "expect.json"))
        compare(moved.list[0].unmigrated.length, 1)
        compare(moved.list[0].unmigrated[0].name, "Project car own")
    }

    // An entry stored without a field reads with none
    function test_anEntryWithoutAFieldReadsAsNone() {
        const old = book()
        for (const key of ["plotted", "unplotted", "picks", "own", "vinMatches", "baseline",
                           "vinKey", "uri", "unmigrated"])
            delete old.list[0][key]
        const v = VehicleBook.parse(JSON.stringify(old)).list[0]
        compare(v.plotted, [])
        compare(v.unplotted, [])
        compare(v.picks, [])
        compare(v.own, "")
        compare(v.vinMatches, [])
        compare(v.baseline, "")
        compare(v.vinKey, "")
        compare(v.uri, "")
        compare(v.unmigrated, undefined, "the key is there only with a file")
        compare(v.label, "Daily driver", "the rest as stored")
    }

    // Each entry is judged by itself: one bad entry is left out and the
    // rest of the book stays (never the whole book for one entry)
    function test_aBadEntryIsLeftOutAndTheRestStay_data() {
        const bad = (tag, change) => {
            const d = book()
            change(d.list[1])
            return { tag: tag, text: JSON.stringify(d) }
        }
        const mixed = book()
        mixed.list.splice(1, 0, 7, "text", null, [])
        return [
            bad("no id", v => delete v.id),
            bad("an empty id", v => v.id = ""),
            bad("an id twice", v => v.id = "v1"),
            bad("a blank label", v => v.label = "  "),
            bad("no label", v => delete v.label),
            bad("a VIN key that isn't text", v => v.vinKey = 7),
            bad("an address that isn't text", v => v.uri = 7),
            bad("a name that isn't text", v => v.unplotted = ["RPM", 3]),
            bad("plotted that isn't a list", v => v.plotted = "RPM"),
            bad("picks that aren't a list", v => v.picks = extras),
            bad("a pick that isn't text", v => v.picks = [extras, 7]),
            bad("own that isn't text", v => v.own = 7),
            bad("matches that aren't a list", v => v.vinMatches = {}),
            bad("a baseline that isn't text", v => v.baseline = ["a"]),
            { tag: "what isn't an entry", text: JSON.stringify(mixed), second: "v2" }
        ]
    }
    function test_aBadEntryIsLeftOutAndTheRestStay(data) {
        const d = VehicleBook.parse(data.text)
        const good = VehicleBook.parse(JSON.stringify(book()))
        compare(d.list[0], good.list[0], "the first entry is whole")
        compare(d.list.map(v => v.id), data.second ? ["v1", "v2"] : ["v1"])
        compare(d.salt, "5a17")
        compare(d.picked, "v1")
    }

    // The picked vehicle's entry left out: none is picked
    function test_aPickedEntryLeftOutIsNoPick() {
        const d = book()
        d.picked = "v2"
        d.list[1].label = ""
        compare(VehicleBook.parse(JSON.stringify(d)).picked, "")
        d.picked = "v9"
        d.list[1].label = "Weekend car"
        compare(VehicleBook.parse(JSON.stringify(d)).picked, "", "an id that's gone")
    }

    // Only text that is no book at all reads as empty
    function test_whatIsNoBookReadsAsEmpty_data() {
        return [
            { tag: "not JSON", text: "{vehicles" },
            { tag: "empty", text: "" },
            { tag: "an array", text: "[]" },
            { tag: "null", text: "null" },
            { tag: "no list", text: JSON.stringify({ format: 2, salt: "", picked: "" }) },
            { tag: "a list that isn't one", text: JSON.stringify({ format: 2, list: {} }) }
        ]
    }
    function test_whatIsNoBookReadsAsEmpty(data) {
        compare(VehicleBook.parse(data.text), VehicleBook.empty())
        verify(!VehicleBook.isLater(VehicleBook.parse(data.text)))
    }

    // A salt that isn't text reads as none; a book with no format is read
    // as format 2
    function test_theBooksOwnFieldsAreJudgedToo() {
        const d = book()
        d.salt = 7
        delete d.format
        const read = VehicleBook.parse(JSON.stringify(d))
        compare(read.salt, "")
        compare(read.format, 2)
        compare(read.list.length, 2)
    }

    // A book whose list is missing or isn't one has no vehicles, and
    // still its own files that didn't move and a later version's keys
    function test_aBookWithNoListKeepsWhatElseItHolds_data() {
        return [{ tag: "no list" }, { tag: "a list that isn't one", list: {} }]
    }
    function test_aBookWithNoListKeepsWhatElseItHolds(data) {
        const stored = { format: 2, salt: "5a17", picked: "v1", later: { kept: true },
                         unmigrated: [notMoved(shaB, "Loose")] }
        if (data.list !== undefined)
            stored.list = data.list
        const read = VehicleBook.parse(JSON.stringify(stored))
        compare(read.list, [])
        compare(read.picked, "")
        compare(read.salt, "5a17")
        compare(read.unmigrated, [notMoved(shaB, "Loose")])
        compare(read.later, { kept: true })
        verify(!VehicleBook.isLater(read))
    }

    // A format that isn't a number is another version's, as a later
    // one is: the book reads as empty with it and is never written over
    function test_aFormatThatIsNoNumberIsNotRead_data() {
        return [{ tag: "digits as text", format: "3" }, { tag: "a word", format: "next" },
                { tag: "null", format: null }, { tag: "true", format: true }]
    }
    function test_aFormatThatIsNoNumberIsNotRead(data) {
        const d = book()
        d.format = data.format
        const read = VehicleBook.parse(JSON.stringify(d))
        verify(VehicleBook.isLater(read))
        compare(read.list, [])
        compare(read.format, data.format)
    }

    // A book a later Havne saved reads as empty and says so, so the page
    // never writes over it
    function test_aLaterFormatsBookIsNotRead() {
        const d = book()
        d.format = 3
        const read = VehicleBook.parse(JSON.stringify(d))
        verify(VehicleBook.isLater(read))
        compare(read.list, [])
        compare(read.format, 3)
        verify(!VehicleBook.isLater(VehicleBook.parse(JSON.stringify(book()))))
        verify(!VehicleBook.isLater(VehicleBook.empty()))
    }

    // More picks than a vehicle may make are kept as stored
    function test_morePicksThanTheMostAreKept() {
        const d = book()
        d.list[0].picks = []
        for (let i = 0; i < 20; ++i)
            d.list[0].picks.push("plugin.p" + i + ".base")
        compare(VehicleBook.parse(JSON.stringify(d)).list[0].picks.length, 20)
    }

    // A record of a file that didn't move keeps kept false, and what
    // isn't a record is left out without its entry
    function test_theFilesThatDidntMove() {
        const d = book()
        d.list[0].unmigrated = [notMoved(shaA, "A"), { sha256: "short", name: "B", words: "w" }, 7,
                                { sha256: shaB, name: "B", words: "w", kept: false },
                                { sha256: shaB, name: "C", words: "w", kept: true, more: 1 }]
        d.unmigrated = [{ sha256: shaB, name: "Loose", words: "w", kept: false }]
        const read = VehicleBook.parse(JSON.stringify(d))
        compare(read.list[0].unmigrated,
                [notMoved(shaA, "A"), { sha256: shaB, name: "B", words: "w", kept: false },
                 { sha256: shaB, name: "C", words: "w" }])
        compare(read.unmigrated, [{ sha256: shaB, name: "Loose", words: "w", kept: false }])

        verify(VehicleBook.hashListed(read, shaA))
        verify(VehicleBook.hashListed(read, shaB))
        verify(!VehicleBook.hashListed(read, "c".repeat(64)))
        const dropped = VehicleBook.dropUnmigrated(read, shaA)
        compare(VehicleBook.picked(dropped).unmigrated.map(r => r.name), ["B", "C"])
        verify(!VehicleBook.hashListed(dropped, shaA))
        const none = VehicleBook.dropUnmigrated(dropped, shaB)
        compare(VehicleBook.picked(none).unmigrated, undefined, "the key goes with the last")
        verify(VehicleBook.hashListed(none, shaB), "the book's own list still names it")
        const loose = VehicleBook.dropLooseUnmigrated(none, shaB)
        compare(loose.unmigrated, undefined)
        verify(!VehicleBook.hashListed(loose, shaB))
        compare(VehicleBook.picked(read).unmigrated.length, 3, "the given document is unchanged")

        d.list[0].unmigrated = "none"
        compare(VehicleBook.parse(JSON.stringify(d)).list.length, 2,
                "a list that isn't one costs no entry")
    }

    // The variants a VIN matched, kept on its vehicle (ADR-0052's appendix
    // of 2026-11-13): none in an entry stored before them; anything but a
    // list of text leaves that entry out; set on the picked vehicle alone;
    // gone with Forget VIN (they came from it) and from Duplicate (which
    // leaves the VIN key); a removed plugin's profiles leave every vehicle,
    // and a replaced one's matches, its pick kept
    function test_aVehiclesVinMatches() {
        const old = book()
        delete old.list[0].vinMatches
        compare(VehicleBook.parse(JSON.stringify(old)).list[0].vinMatches, [])
        for (const wrong of [7, "plugin.example-sim.base", [7], null]) {
            const bad = book()
            bad.list[1].vinMatches = wrong
            compare(VehicleBook.parse(JSON.stringify(bad)).list.map(v => v.id), ["v1"], JSON.stringify(wrong))
        }

        const d = VehicleBook.setVinMatches(VehicleBook.pick(book(), "v2"), ["plugin.other.a"])
        compare(VehicleBook.picked(d).vinMatches, ["plugin.other.a"])
        compare(VehicleBook.find(d, "v1").vinMatches, [simBase, sim2019], "the others unchanged")
        compare(VehicleBook.picked(VehicleBook.setVinMatches(d, [])).vinMatches, [])

        compare(VehicleBook.picked(VehicleBook.forgetVin(book())).vinMatches, [])
        compare(VehicleBook.picked(VehicleBook.duplicate(book(), "Copy")).vinMatches, [])

        const dropped = VehicleBook.dropProfiles(d, [sim2019, "plugin.other.a"])
        compare(VehicleBook.find(dropped, "v1").vinMatches, [simBase])
        compare(VehicleBook.find(dropped, "v2").vinMatches, [])

        // A replaced plugin's matches leave every vehicle, its pick stays
        const replaced = VehicleBook.dropVinMatches(d, [sim2019, "plugin.other.a"])
        compare(VehicleBook.find(replaced, "v1").vinMatches, [simBase])
        compare(VehicleBook.find(replaced, "v2").vinMatches, [])
        compare(VehicleBook.find(replaced, "v1").picks, [extras, sim2019, own], "the pick kept")
        compare(VehicleBook.find(d, "v1").vinMatches, [simBase, sim2019], "its argument unchanged")
    }

    // A vehicle's baseline recording (ADR-0049's appendix of 2026-11-06):
    // the picked vehicle's set and cleared, the others' left; Duplicate
    // doesn't copy it (a recording of that car)
    function test_aVehiclesBaseline() {
        const later = "/recordings/20261106-100000 Weekend car"
        const d = VehicleBook.setBaseline(VehicleBook.pick(book(), "v2"), later)
        compare(VehicleBook.picked(d).baseline, later)
        compare(VehicleBook.find(d, "v1").baseline, baselineA, "the others unchanged")
        compare(VehicleBook.parse(VehicleBook.serialize(d)), d, "stored and read")
        compare(VehicleBook.picked(VehicleBook.clearBaseline(d)).baseline, "")
        compare(VehicleBook.find(VehicleBook.clearBaseline(d), "v1").baseline, baselineA)
        compare(VehicleBook.setBaseline(VehicleBook.pick(book(), ""), later),
                VehicleBook.pick(book(), ""), "no vehicle picked: nothing changes")
        compare(VehicleBook.picked(VehicleBook.duplicate(book(), "Copy")).baseline, "")
        compare(book().list[1].baseline, "", "the given document is unchanged")
    }

    function test_theLabelsRule() {
        const d = book()
        compare(VehicleBook.labelProblem(d, "Track day", ""), "")
        verify(VehicleBook.labelProblem(d, "   ", "") !== "", "blank")
        compare(VehicleBook.labelProblem(d, "x".repeat(40), ""), "")
        verify(VehicleBook.labelProblem(d, "x".repeat(41), "") !== "", "over 40")
        verify(VehicleBook.labelProblem(d, " weekend CAR ", "") !== "", "taken, ignoring case and spaces")
        compare(VehicleBook.labelProblem(d, "weekend car", "v2"), "", "its own, renaming")
    }

    function test_theDefaultLabels() {
        const d = book()
        compare(VehicleBook.defaultNewLabel(VehicleBook.empty()), "Vehicle 1")
        d.list[1].label = "Vehicle 1"
        compare(VehicleBook.defaultNewLabel(d), "Vehicle 2", "the lowest free number")
        compare(VehicleBook.defaultCopyLabel(d, "Daily driver"), "Daily driver (copy)")
        d.list[1].label = "Daily driver (copy)"
        compare(VehicleBook.defaultCopyLabel(d, "Daily driver"), "Daily driver (copy 2)")
        compare(VehicleBook.defaultCopyLabel(d, "Daily driver (copy)"), "Daily driver (copy 2)",
                "a copy's copy continues the count")
        const long = "y".repeat(40)
        const copy = VehicleBook.defaultCopyLabel(d, long)
        compare(copy.length, 40, "cut to fit")
        verify(copy.endsWith(" (copy)"))
    }

    // Duplicate copies the connection, the plotted signals and the picks,
    // and nothing that is one vehicle's alone
    function test_newAndDuplicate() {
        const d = book()
        const added = VehicleBook.add(d, "  Track day ")
        compare(added.list.length, 3)
        compare(added.picked, "v3")
        compare(VehicleBook.picked(added),
                { id: "v3", label: "Track day", vinKey: "", uri: "", plotted: [],
                  unplotted: [], picks: [], own: "", vinMatches: [], baseline: "" })

        const copied = VehicleBook.duplicate(d, "Daily driver (copy)")
        const c = VehicleBook.picked(copied)
        compare(c.uri, "elm327://COM6?profile=obd2", "the connection")
        compare(c.plotted, ["Timing Advance"], "the plotted signals")
        compare(c.unplotted, ["Intake Air Temp"], "and the unplotted")
        compare(c.picks, [extras, sim2019, own], "the picks, in their order")
        compare(c.vinKey, "", "never the VIN key")
        compare(c.vinMatches, [], "or what it matched")
        compare(c.own, "", "the form writes one vehicle's variant")
        compare(c.unmigrated, undefined, "what didn't move is the original's")
        compare(c.later, undefined)
        compare(d, book(), "the given document is unchanged")
        compare(VehicleBook.duplicate(VehicleBook.pick(d, ""), "Copy"), VehicleBook.pick(d, ""),
                "none picked: nothing to copy")
    }

    // ADR-0027 section 5's trap: an entry rebuilt from the fields it knew
    // would drop the rest at the next change
    function test_everyChangeKeepsWhatAnEntryHolds_data() {
        return [
            { tag: "pick", apply: d => VehicleBook.pick(d, "v1") },
            { tag: "rename", apply: d => VehicleBook.rename(d, "Commuter") },
            { tag: "a connect", apply: d => VehicleBook.rememberUri(d, "j2534://OBDX Pro - VX") },
            { tag: "plotting", apply: d => VehicleBook.rememberPlotting(d, ["MAP"], []) },
            { tag: "another vehicle added", apply: d => VehicleBook.pick(VehicleBook.add(d, "New"), "v1") },
            { tag: "another vehicle forgotten",
              apply: d => VehicleBook.pick(VehicleBook.forget(VehicleBook.pick(d, "v2")), "v1") },
            { tag: "recognized", apply: d => VehicleBook.recognize(d, "key-a", "elm327://COM6").doc },
            { tag: "another vehicle's picks",
              apply: d => VehicleBook.pick(VehicleBook.setPicks(VehicleBook.pick(d, "v2"), [extras]), "v1") },
            { tag: "a profile no vehicle has dropped", apply: d => VehicleBook.dropProfiles(d, ["plugin.x.y"]) },
            { tag: "a file elsewhere dropped", apply: d => VehicleBook.dropLooseUnmigrated(d, shaA) },
            { tag: "stored and read",
              apply: d => VehicleBook.parse(VehicleBook.serialize(VehicleBook.copy(d))) }
        ]
    }
    function test_everyChangeKeepsWhatAnEntryHolds(data) {
        const top = book()
        top.later = "the book's"
        const v = VehicleBook.picked(data.apply(top))
        compare(v.picks, [extras, sim2019, own])
        compare(v.own, own)
        compare(v.vinMatches, [simBase, sim2019])
        compare(v.baseline, baselineA)
        compare(v.unmigrated, [notMoved(shaA, "Old extras")])
        compare(v.later, { kept: [1, 2] }, "a later version's key")
        compare(data.apply(top).later, "the book's", "and the book's own")
    }

    // The picked vehicle's picks and own variant; a plugin or a variant
    // removed is no vehicle's pick, own variant or match any more
    function test_picksOwnAndWhatARemovalUndoes() {
        const d = VehicleBook.setPicks(VehicleBook.pick(book(), "v2"), [sim2019, extras])
        compare(VehicleBook.picked(d).picks, [sim2019, extras], "in the order given")
        compare(VehicleBook.find(d, "v1").picks, [extras, sim2019, own], "the others unchanged")
        compare(VehicleBook.picked(VehicleBook.setOwn(d, extras)).own, extras)
        compare(VehicleBook.setPicks(VehicleBook.pick(book(), ""), [extras]),
                VehicleBook.pick(book(), ""), "no vehicle picked: nothing changes")

        verify(VehicleBook.pickedBy(d, [sim2019]))
        verify(VehicleBook.pickedBy(d, ["plugin.none.base", own]))
        verify(!VehicleBook.pickedBy(d, [simBase]), "matched by a VIN is not picked")
        verify(!VehicleBook.pickedBy(d, []))

        const gone = VehicleBook.dropProfiles(d, [sim2019, own])
        compare(VehicleBook.find(gone, "v1").picks, [extras])
        compare(VehicleBook.find(gone, "v1").own, "", "the own variant went with it")
        compare(VehicleBook.find(gone, "v1").vinMatches, [simBase])
        compare(VehicleBook.find(gone, "v2").picks, [extras], "in every vehicle")
        compare(VehicleBook.find(VehicleBook.dropProfiles(d, [extras]), "v1").own, own)

        const matches = VehicleBook.dropVinMatches(d, [sim2019])
        compare(VehicleBook.find(matches, "v1").vinMatches, [simBase])
        compare(VehicleBook.find(matches, "v1").picks, [extras, sim2019, own], "its picks kept")
        compare(VehicleBook.picked(VehicleBook.setVinMatches(d, [simBase])).vinMatches, [simBase])
        compare(VehicleBook.find(d, "v1").picks, [extras, sim2019, own], "the given document is unchanged")
    }

    // An import that took another's place: picks and the own variant
    // follow it, once in a vehicle's picks
    function test_picksFollowAnImportThatTookAnothersPlace() {
        const to = "plugin.daily-driver-obdb.my2021"
        let d = VehicleBook.movePicks(book(), [sim2019, own], to)
        compare(VehicleBook.find(d, "v1").picks, [extras, to], "both named it: once")
        compare(VehicleBook.find(d, "v1").own, to)
        compare(VehicleBook.find(d, "v1").vinMatches, [simBase])
        d = VehicleBook.movePicks(book(), [extras], to)
        compare(VehicleBook.find(d, "v1").picks, [to, sim2019, own], "in its place")
        compare(VehicleBook.find(d, "v1").own, own)
        compare(VehicleBook.find(d, "v2").picks, [])
    }

    function test_renameForgetTheVinForgetAndForgetAll() {
        const d = book()
        compare(VehicleBook.picked(VehicleBook.rename(d, " Commuter ")).label, "Commuter")
        const noVin = VehicleBook.picked(VehicleBook.forgetVin(d))
        compare(noVin.vinKey, "")
        compare(noVin.vinMatches, [], "what the VIN matched goes with it")
        compare(noVin.picks, [extras, sim2019, own], "the picks stay")
        const forgotten = VehicleBook.forget(d)
        compare(forgotten.list.map(v => v.id), ["v2"])
        compare(forgotten.picked, "")
        compare(forgotten.unmigrated, [notMoved(shaA, "Old extras")],
                "its file that didn't move is the book's own now")
        compare(VehicleBook.forget(VehicleBook.pick(d, "v2")).unmigrated, undefined, "none to keep")
        const bare = book()
        delete bare.list[0].unmigrated
        compare(VehicleBook.forgetAll(bare), VehicleBook.empty(), "the salt too")
        compare(VehicleBook.add(forgotten, "Next").picked, "v3", "an id isn't reused")
        // The files that didn't move stay until each is removed: the
        // book's own and every vehicle's, each once
        const loose = book()
        loose.unmigrated = [notMoved(shaB, "Loose"), notMoved(shaA, "Old extras")]
        loose.list[1].unmigrated = [notMoved(shaC, "Weekend's")]
        const none = VehicleBook.forgetAll(loose)
        compare(none.unmigrated,
                [notMoved(shaB, "Loose"), notMoved(shaA, "Old extras"), notMoved(shaC, "Weekend's")])
        compare(none.list, [])
        compare(none.salt, "")
        compare(VehicleBook.forget(loose).unmigrated,
                [notMoved(shaB, "Loose"), notMoved(shaA, "Old extras")], "one the book lists already, once")
        compare(d, book())
    }

    function test_onlyAnAdaptersSessionIsRemembered() {
        const d = book()
        compare(VehicleBook.picked(VehicleBook.rememberUri(d, "j2534://OBDX Pro - VX")).uri,
                "j2534://OBDX Pro - VX")
        compare(VehicleBook.rememberUri(d, "mock://demo"), d)
        compare(VehicleBook.rememberUri(d, "replay://C:/r/frames.csv"), d)
        const plotting = VehicleBook.picked(VehicleBook.rememberPlotting(d, ["MAP"], ["RPM"]))
        compare(plotting.plotted, ["MAP"])
        compare(plotting.unplotted, ["RPM"])
    }

    // ADR-0027 section 4, row by row
    function test_theVinKeySettlesTheVehicle() {
        let r = VehicleBook.recognize(book(), "key-a", "elm327://COM6")
        compare(r.outcome, "same")
        compare(VehicleBook.logLine(r), "Vehicle: Daily driver")

        const unlearned = VehicleBook.pick(book(), "v2")
        r = VehicleBook.recognize(unlearned, "key-b", "elm327://COM6")
        compare(r.outcome, "learned")
        compare(VehicleBook.picked(r.doc).vinKey, "key-b")
        compare(VehicleBook.logLine(r), "Vehicle: Weekend car (recognized from now on)")

        r = VehicleBook.recognize(unlearned, "key-a", "elm327://COM6")
        compare(r.outcome, "switched", "another saved vehicle's key wins over learning")
        compare(r.doc.picked, "v1")
        compare(VehicleBook.logLine(r), "Vehicle: Daily driver (picked; Weekend car was)")
        compare(VehicleBook.pickNote(r),
                "The connected car's VIN picked Daily driver (Weekend car was picked)")

        r = VehicleBook.recognize(VehicleBook.pick(book(), ""), "key-a", "elm327://COM6")
        compare(VehicleBook.logLine(r), "Vehicle: Daily driver (picked)")
        compare(VehicleBook.pickNote(r), "The connected car's VIN picked Daily driver")
        // Only a switch is the car's pick to say
        compare(VehicleBook.pickNote(VehicleBook.recognize(book(), "key-a", "elm327://COM6")), "")
        compare(VehicleBook.pickNote(VehicleBook.recognize(book(), "key-c", "elm327://COM6")), "")

        r = VehicleBook.recognize(book(), "key-c", "elm327://COM7?profile=raw")
        compare(r.outcome, "new")
        compare(VehicleBook.picked(r.doc),
                { id: "v3", label: "Vehicle 1", vinKey: "key-c",
                  uri: "elm327://COM7?profile=raw", plotted: [], unplotted: [],
                  picks: [], own: "", vinMatches: [], baseline: "" })
        compare(VehicleBook.logLine(r), "Vehicle: not Daily driver (its VIN differs); saved as Vehicle 1")

        r = VehicleBook.recognize(VehicleBook.pick(book(), ""), "key-c", "elm327://COM7")
        compare(VehicleBook.logLine(r), "Vehicle: saved as Vehicle 1")
    }

    // A vehicle a VIN adds picks no plugin, so the URI it keeps reads
    // with Default: another vehicle's plugin profile goes, a built-in one
    // and every other key stay
    function test_aVehicleTheVinAddsKeepsNoOtherVehiclesProfile() {
        let r = VehicleBook.recognize(book(), "key-c",
                                      "elm327://COM7?baud=38400&profile=user.extras&identify=0")
        compare(r.outcome, "new")
        compare(VehicleBook.picked(r.doc).uri, "elm327://COM7?baud=38400&identify=0")
        r = VehicleBook.recognize(book(), "key-c", "j2534://VX?profile=plugin.example-sim.base")
        compare(VehicleBook.picked(r.doc).uri, "j2534://VX")
        r = VehicleBook.recognize(book(), "key-c", "j2534://VX?profile=uds-generic&did=1E1C")
        compare(VehicleBook.picked(r.doc).uri, "j2534://VX?profile=uds-generic&did=1E1C")
        compare(VehicleBook.withoutFileProfile("j2534://VX"), "j2534://VX")
    }

    // An address given a profile: profile= goes last and every other key
    // stays where it was; one that names a profile, with a value or not,
    // is left as it is
    function test_anAddressTakesAProfileOnlyWhenItNamesNone_data() {
        return [
            { tag: "bare", uri: "elm327://COM5", to: "elm327://COM5?profile=" + extras },
            { tag: "with keys", uri: "j2534://VX?baud=500000&identify=0",
              to: "j2534://VX?baud=500000&identify=0&profile=" + extras },
            { tag: "an empty query", uri: "elm327://COM5?", to: "elm327://COM5?profile=" + extras },
            { tag: "a query ending in &", uri: "elm327://COM5?baud=38400&",
              to: "elm327://COM5?baud=38400&profile=" + extras },
            { tag: "names obd2", uri: "elm327://COM5?profile=obd2", to: "elm327://COM5?profile=obd2" },
            { tag: "names it first", uri: "elm327://COM5?profile=raw&baud=38400",
              to: "elm327://COM5?profile=raw&baud=38400" },
            { tag: "names it last", uri: "elm327://COM5?baud=38400&profile=uds-generic",
              to: "elm327://COM5?baud=38400&profile=uds-generic" },
            { tag: "names none", uri: "elm327://COM5?profile=", to: "elm327://COM5?profile=" },
            { tag: "the key alone", uri: "elm327://COM5?baud=1&profile", to: "elm327://COM5?baud=1&profile" },
            { tag: "a key that only starts so", uri: "elm327://COM5?profiled=1",
              to: "elm327://COM5?profiled=1&profile=" + extras }
        ]
    }
    function test_anAddressTakesAProfileOnlyWhenItNamesNone(data) {
        compare(VehicleBook.withProfile(data.uri, extras), data.to)
        compare(VehicleBook.withProfile(data.uri, ""), data.uri, "no profile to give")
        compare(VehicleBook.namesProfile(data.uri), data.to === data.uri)
    }
}
