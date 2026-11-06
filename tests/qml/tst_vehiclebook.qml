// File/layer: tests/qml/tst_vehiclebook.qml | Qt Quick Test, QML tier
// Testing: VehicleBook's rules (ADR-0027) on documents, no page and no
//          bridge: a stored document read back or dropped whole, the
//          label's rule and its defaults, each change returning a new
//          document and leaving the one it was given alone, a connection
//          and plotted signals kept for adapter sessions only, and each
//          row of section 4's table with its log line; a vehicle's
//          signal files (ADR-0051) through every change, Duplicate
//          copying them, a clashing profile refused, the form's own file
//          replaced, and the files the store keeps.

import QtQuick
import QtTest
import App
TestCase {
    name: "VehicleBook"

    // Signal files as the store names them, by their content's SHA-256
    readonly property string shaA: "a".repeat(64)
    readonly property string shaB: "b".repeat(64)
    readonly property string shaC: "c".repeat(64)
    function fileA() { return { sha256: shaA, name: "Transmission", profile: "user.transmission" } }

    // Two vehicles, the first picked and recognized, with a signal file
    function book() {
        return {
            salt: "5a17",
            picked: "v1",
            list: [
                { id: "v1", label: "Daily driver", vinKey: "key-a",
                  uri: "elm327://COM6?profile=obd2", plotted: ["Timing Advance"],
                  unplotted: ["Intake Air Temp"], signals: [fileA()], own: "",
                  plugin: "plugin.example-sim.base" },
                { id: "v2", label: "Weekend car", vinKey: "", uri: "", plotted: [],
                  unplotted: [], signals: [], own: "", plugin: "" }
            ]
        }
    }

    function test_aStoredDocumentReadsBack() {
        const d = VehicleBook.parse(VehicleBook.serialize(book()))
        compare(d, book())
    }

    // An entry stored before plotted existed ticks nothing past the
    // default (ADR-0039 section 8)
    function test_anEntryWithoutPlottedReadsAsNone() {
        const old = book()
        delete old.list[0].plotted
        const d = VehicleBook.parse(JSON.stringify(old))
        compare(d.list[0].plotted, [])
        compare(d.list[0].unplotted, ["Intake Air Temp"])
    }

    // An entry stored before plugins existed has none picked; a plugin
    // that isn't text drops the document, as any bad field does; the
    // picked vehicle's is set by its profile, and Duplicate copies it
    function test_aVehiclesPluginVariant() {
        const old = book()
        delete old.list[0].plugin
        compare(VehicleBook.parse(JSON.stringify(old)).list[0].plugin, "")
        const bad = book()
        bad.list[1].plugin = 7
        compare(VehicleBook.parse(JSON.stringify(bad)), VehicleBook.empty())

        const d = VehicleBook.setPlugin(VehicleBook.pick(book(), "v2"), "plugin.example-sim.my2019")
        compare(VehicleBook.picked(d).plugin, "plugin.example-sim.my2019")
        compare(VehicleBook.find(d, "v1").plugin, "plugin.example-sim.base", "the others unchanged")
        compare(VehicleBook.picked(VehicleBook.setPlugin(d, "")).plugin, "")
        compare(VehicleBook.picked(VehicleBook.duplicate(book(), "Copy")).plugin,
                "plugin.example-sim.base")
    }

    // An entry stored before signal files existed has none
    function test_anEntryWithoutSignalFilesReadsAsNone() {
        const old = book()
        delete old.list[0].signals
        delete old.list[0].own
        const d = VehicleBook.parse(JSON.stringify(old))
        compare(d.list[0].signals, [])
        compare(d.list[0].own, "")
        compare(d.list[0].uri, "elm327://COM6?profile=obd2", "the rest as stored")
    }

    // As a picked id that's gone: the file isn't the vehicle's
    function test_anOwnThatNamesNoFileIsNone() {
        const d = book()
        d.list[0].own = shaB
        compare(VehicleBook.parse(JSON.stringify(d)).list[0].own, "")
        d.list[0].own = shaA
        compare(VehicleBook.parse(JSON.stringify(d)).list[0].own, shaA)
    }

    function test_aDamagedDocumentIsDroppedWhole_data() {
        const missing = book(); delete missing.list[1].vinKey
        const badSignals = book(); badSignals.list[0].signals = fileA()
        const badSha = book(); badSha.list[0].signals[0].sha256 = "A".repeat(64)
        const shortSha = book(); shortSha.list[0].signals[0].sha256 = "a".repeat(63)
        const noProfile = book(); delete noProfile.list[0].signals[0].profile
        const noName = book(); noName.list[0].signals[0].name = ""
        const badOwn = book(); badOwn.list[0].own = 7
        const twice = book(); twice.list[1].id = "v1"
        const unnamed = book(); unnamed.list[0].label = "  "
        const badList = book(); badList.list[0].unplotted = ["RPM", 3]
        const badPlotted = book(); badPlotted.list[1].plotted = "RPM"
        return [
            { tag: "not JSON", text: "{vehicles" },
            { tag: "empty", text: "" },
            { tag: "an array", text: "[]" },
            { tag: "no list", text: JSON.stringify({ salt: "", picked: "" }) },
            { tag: "an entry missing a field", text: JSON.stringify(missing) },
            { tag: "an id twice", text: JSON.stringify(twice) },
            { tag: "a blank label", text: JSON.stringify(unnamed) },
            { tag: "a name that isn't text", text: JSON.stringify(badList) },
            { tag: "plotted that isn't a list", text: JSON.stringify(badPlotted) },
            { tag: "signals that aren't a list", text: JSON.stringify(badSignals) },
            { tag: "a SHA-256 in upper case", text: JSON.stringify(badSha) },
            { tag: "a SHA-256 a digit short", text: JSON.stringify(shortSha) },
            { tag: "a file without its profile", text: JSON.stringify(noProfile) },
            { tag: "a file without a name", text: JSON.stringify(noName) },
            { tag: "own that isn't text", text: JSON.stringify(badOwn) }
        ]
    }
    function test_aDamagedDocumentIsDroppedWhole(data) {
        compare(VehicleBook.parse(data.text), VehicleBook.empty())
    }

    function test_aPickedIdThatsGoneIsNone() {
        const d = book()
        d.picked = "v9"
        compare(VehicleBook.parse(JSON.stringify(d)).picked, "")
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

    function test_newAndDuplicate() {
        const d = book()
        const added = VehicleBook.add(d, "  Track day ")
        compare(added.list.length, 3)
        compare(added.picked, "v3")
        compare(VehicleBook.picked(added),
                { id: "v3", label: "Track day", vinKey: "", uri: "", plotted: [],
                  unplotted: [], signals: [], own: "", plugin: "" })

        const copied = VehicleBook.duplicate(d, "Daily driver (copy)")
        const c = VehicleBook.picked(copied)
        compare(c.uri, "elm327://COM6?profile=obd2", "the connection")
        compare(c.plotted, ["Timing Advance"], "the plotted signals")
        compare(c.unplotted, ["Intake Air Temp"], "and the unplotted")
        compare(c.vinKey, "", "never the VIN key")
        compare(c.signals, [fileA()], "the signal files")
        compare(d, book(), "the given document is unchanged")

        const owned = VehicleBook.setOwn(d, { sha256: shaB, name: "Mine", profile: "user.mine" })
        const ownCopy = VehicleBook.picked(VehicleBook.duplicate(owned, "Daily driver (copy)"))
        compare(ownCopy.signals.map(f => f.sha256), [shaA, shaB])
        compare(ownCopy.own, shaB, "and which one the form writes")
    }

    // ADR-0027 section 5's trap: an entry rebuilt from the fields it knew
    // would drop the files at the next change
    function test_everyChangeKeepsTheSignalFiles_data() {
        return [
            { tag: "pick", apply: d => VehicleBook.pick(d, "v1") },
            { tag: "rename", apply: d => VehicleBook.rename(d, "Commuter") },
            { tag: "forget the VIN", apply: d => VehicleBook.forgetVin(d) },
            { tag: "a connect", apply: d => VehicleBook.rememberUri(d, "j2534://OBDX Pro - VX") },
            { tag: "plotting", apply: d => VehicleBook.rememberPlotting(d, ["MAP"], []) },
            { tag: "another vehicle added", apply: d => VehicleBook.pick(VehicleBook.add(d, "New"), "v1") },
            { tag: "another vehicle forgotten",
              apply: d => VehicleBook.pick(VehicleBook.forget(VehicleBook.pick(d, "v2")), "v1") },
            { tag: "recognized", apply: d => VehicleBook.recognize(d, "key-a", "elm327://COM6").doc },
            { tag: "stored and read",
              apply: d => VehicleBook.parse(VehicleBook.serialize(VehicleBook.copy(d))) }
        ]
    }
    function test_everyChangeKeepsTheSignalFiles(data) {
        const d = VehicleBook.setOwn(book(), { sha256: shaB, name: "Mine", profile: "user.mine" })
        const v = VehicleBook.picked(data.apply(d))
        compare(v.signals, [fileA(), { sha256: shaB, name: "Mine", profile: "user.mine" }])
        compare(v.own, shaB)
    }

    function test_aClashingProfileIsRefused() {
        const d = book()
        const clash = { sha256: shaB, name: "Transmission v2", profile: "user.transmission" }
        compare(VehicleBook.profileProblem(d, clash, ""),
                "user.transmission is made by another file already (Transmission, in Daily driver)")
        compare(VehicleBook.addSignals(d, clash), d, "refused into its own vehicle")
        const other = VehicleBook.pick(d, "v2")
        verify(VehicleBook.profileProblem(other, clash, "") !== "", "and from another vehicle")
        compare(VehicleBook.addSignals(other, clash), other)

        const shared = VehicleBook.addSignals(other, fileA())
        compare(VehicleBook.picked(shared).signals, [fileA()], "the same file in two vehicles")
        compare(VehicleBook.addSignals(d, fileA()), d, "a file the vehicle has is kept once")

        const another = { sha256: shaC, name: "Body", profile: "user.body" }
        compare(VehicleBook.picked(VehicleBook.addSignals(d, another)).signals,
                [fileA(), another])
        compare(d, book(), "the given document is unchanged")
    }

    // The form saves its file again under the same name: a new hash, the
    // same profile, replacing its last; another vehicle's file still clashes
    function test_theFormsOwnFileIsReplaced() {
        let d = VehicleBook.setOwn(book(), { sha256: shaB, name: "Mine", profile: "user.mine" })
        compare(VehicleBook.picked(d).own, shaB)
        d = VehicleBook.setOwn(d, { sha256: shaC, name: "Mine", profile: "user.mine" })
        compare(VehicleBook.picked(d).signals.map(f => f.sha256), [shaA, shaC])
        compare(VehicleBook.picked(d).own, shaC)

        const theirs = { sha256: shaB, name: "Mine", profile: "user.transmission" }
        compare(VehicleBook.setOwn(d, theirs), d, "an imported file's profile is not the form's")

        const removed = VehicleBook.removeSignals(d, shaC)
        compare(VehicleBook.picked(removed).signals, [fileA()])
        compare(VehicleBook.picked(removed).own, "", "removing the own file leaves none")
        compare(VehicleBook.picked(VehicleBook.removeSignals(d, shaA)).own, shaC)
    }

    function test_theFilesTheStoreKeeps() {
        let d = VehicleBook.addSignals(VehicleBook.pick(book(), "v2"), fileA())
        d = VehicleBook.setOwn(d, { sha256: shaC, name: "Mine", profile: "user.mine" })
        compare(VehicleBook.referencedFiles(d), [shaA, shaC], "each once, sorted")
        compare(VehicleBook.referencedFiles(VehicleBook.forget(d)), [shaA])
        compare(VehicleBook.referencedFiles(VehicleBook.forgetAll(d)), [])
    }

    function test_renameForgetTheVinForgetAndForgetAll() {
        const d = book()
        compare(VehicleBook.picked(VehicleBook.rename(d, " Commuter ")).label, "Commuter")
        compare(VehicleBook.picked(VehicleBook.forgetVin(d)).vinKey, "")
        const forgotten = VehicleBook.forget(d)
        compare(forgotten.list.map(v => v.id), ["v2"])
        compare(forgotten.picked, "")
        compare(VehicleBook.forgetAll(d), VehicleBook.empty(), "the salt too")
        compare(VehicleBook.add(forgotten, "Next").picked, "v3", "an id isn't reused")
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

        r = VehicleBook.recognize(VehicleBook.pick(book(), ""), "key-a", "elm327://COM6")
        compare(VehicleBook.logLine(r), "Vehicle: Daily driver (picked)")

        r = VehicleBook.recognize(book(), "key-c", "elm327://COM7?profile=raw")
        compare(r.outcome, "new")
        compare(VehicleBook.picked(r.doc),
                { id: "v3", label: "Vehicle 1", vinKey: "key-c",
                  uri: "elm327://COM7?profile=raw", plotted: [], unplotted: [],
                  signals: [], own: "", plugin: "" })
        compare(VehicleBook.logLine(r), "Vehicle: not Daily driver (its VIN differs); saved as Vehicle 1")

        r = VehicleBook.recognize(VehicleBook.pick(book(), ""), "key-c", "elm327://COM7")
        compare(VehicleBook.logLine(r), "Vehicle: saved as Vehicle 1")
    }
}
