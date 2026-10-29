// File/layer: tests/qml/tst_vehiclebook.qml | Qt Quick Test, QML tier
// Testing: VehicleBook's rules (ADR-0027) on documents, no page and no
//          bridge: a stored document read back or dropped whole, the
//          label's rule and its defaults, each change returning a new
//          document and leaving the one it was given alone, a connection
//          and plotted signals kept for adapter sessions only, and each
//          row of §4's table with its log line.

import QtQuick
import QtTest
import App
TestCase {
    name: "VehicleBook"

    // Two vehicles, the first picked and recognized
    function book() {
        return {
            salt: "5a17",
            picked: "v1",
            list: [
                { id: "v1", label: "Daily driver", vinKey: "key-a",
                  uri: "elm327://COM6?profile=obd2", unplotted: ["Intake Air Temp"] },
                { id: "v2", label: "Weekend car", vinKey: "", uri: "", unplotted: [] }
            ]
        }
    }

    function test_aStoredDocumentReadsBack() {
        const d = VehicleBook.parse(VehicleBook.serialize(book()))
        compare(d, book())
    }

    function test_aDamagedDocumentIsDroppedWhole_data() {
        const missing = book(); delete missing.list[1].vinKey
        const twice = book(); twice.list[1].id = "v1"
        const unnamed = book(); unnamed.list[0].label = "  "
        const badList = book(); badList.list[0].unplotted = ["RPM", 3]
        return [
            { tag: "not JSON", text: "{vehicles" },
            { tag: "empty", text: "" },
            { tag: "an array", text: "[]" },
            { tag: "no list", text: JSON.stringify({ salt: "", picked: "" }) },
            { tag: "an entry missing a field", text: JSON.stringify(missing) },
            { tag: "an id twice", text: JSON.stringify(twice) },
            { tag: "a blank label", text: JSON.stringify(unnamed) },
            { tag: "a name that isn't text", text: JSON.stringify(badList) }
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
                { id: "v3", label: "Track day", vinKey: "", uri: "", unplotted: [] })

        const copied = VehicleBook.duplicate(d, "Daily driver (copy)")
        const c = VehicleBook.picked(copied)
        compare(c.uri, "elm327://COM6?profile=obd2", "the connection")
        compare(c.unplotted, ["Intake Air Temp"], "the plotted signals")
        compare(c.vinKey, "", "never the VIN key")
        compare(d, book(), "the given document is unchanged")
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
        compare(VehicleBook.picked(VehicleBook.rememberUnplotted(d, ["RPM"])).unplotted, ["RPM"])
    }

    // ADR-0027 §4, row by row
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
                  uri: "elm327://COM7?profile=raw", unplotted: [] })
        compare(VehicleBook.logLine(r), "Vehicle: not Daily driver (its VIN differs); saved as Vehicle 1")

        r = VehicleBook.recognize(VehicleBook.pick(book(), ""), "key-c", "elm327://COM7")
        compare(VehicleBook.logLine(r), "Vehicle: saved as Vehicle 1")
    }
}
