// File/layer: tests/qml/tst_signalform.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.signalform (test_qml)

import QtQuick
import QtTest
import App
TestCase {
    name: "SignalForm"

    function fields(more) {
        const f = SignalForm.emptyFields()
        for (const k in more)
            f[k] = more[k]
        return f
    }

    function test_aKeyFromItsName_data() {
        return [
            { tag: "words", name: "Trans Fluid Temp", key: "trans_fluid_temp" },
            { tag: "punctuation", name: "  Boost (psi)!  ", key: "boost_psi" },
            { tag: "nothing usable", name: "°°", key: "signal" },
            { tag: "cut to 31", name: "a".repeat(40), key: "a".repeat(31) },
            { tag: "no trailing _ after the cut", name: "a".repeat(30) + " b", key: "a".repeat(30) }
        ]
    }
    function test_aKeyFromItsName(data) {
        compare(SignalForm.keyOf(data.name), data.key)
    }

    function test_anEnumsValuesBothWays() {
        compare(SignalForm.parseValues("0=Park, 1=Reverse,\n 3 = Drive "),
                { "0": "Park", "1": "Reverse", "3": "Drive" })
        compare(SignalForm.parseValues("7"), { "7": "" }, "kept, for the loader to name")
        compare(SignalForm.valuesText({ "0": "Park", "1": "Reverse" }), "0=Park, 1=Reverse")
    }

    function test_aNumberTakesItsKeysAndLeavesTheDefaultsOut() {
        compare(SignalForm.draft(fields({ name: " Trans Temp ", number: "1e1c", bits: "16" })),
                { key: "trans_temp", name: "Trans Temp",
                  read: { service: "22", number: "1E1C" }, field: { byte: 0, bits: 16 } })
        compare(SignalForm.draft(fields({ name: "Trim", service: "01", number: "06", byte: "1",
                                          bits: "16", order: "le", bit: "3", signed: true,
                                          scale: "0.0078125", offset: "-40", unit: "%",
                                          decimals: "1", group: "fuel" })),
                { key: "trim", name: "Trim", read: { service: "01", number: "06" },
                  field: { byte: 1, bits: 16, bit: 3, order: "le", signed: true },
                  scale: 0.0078125, offset: -40, unit: "%", decimals: 1, group: "fuel" })
    }

    function test_eachKindTakesItsOwnKeys() {
        compare(SignalForm.draft(fields({ name: "Part", kind: "text", number: "F187", bytes: "",
                                          scale: "2", unit: "x" })),
                { key: "part", name: "Part", kind: "text", read: { service: "22", number: "F187" },
                  field: { byte: 0 } }, "no bits, scale or unit")
        compare(SignalForm.draft(fields({ name: "Raw", kind: "hex", number: "F190", bytes: "4" })).field,
                { byte: 0, bytes: 4 })
        compare(SignalForm.draft(fields({ name: "Gear", kind: "enum", number: "D101",
                                          signed: true, values: "3=Drive" })),
                { key: "gear", name: "Gear", kind: "enum", read: { service: "22", number: "D101" },
                  field: { byte: 0, bits: 8 }, values: { "3": "Drive" } }, "never signed")
    }

    // A typed word stays text, so the loader says which key isn't a number
    function test_whatIsntANumberStaysAsTyped() {
        compare(SignalForm.draft(fields({ name: "A", number: "D100", byte: "two" })).field.byte, "two")
        compare(SignalForm.draft(fields({ name: "A", number: "D100", byte: "" })).field.byte, "")
    }

    function test_theModulesEntry() {
        compare(SignalForm.moduleOf(fields({ rx: " 7e9 " })), { rx: "7E9" })
        compare(SignalForm.moduleOf(fields({ rx: "7b8", tx: "7b0" })), { rx: "7B8", tx: "7B0" })
    }

    function test_theOwnFileGrowsReplacesAndShrinks() {
        let file = SignalForm.emptyFile("Daily driver")
        compare(file.name, "Daily driver own")
        compare(file.source, { name: "Written in Havne" })
        const a = SignalForm.draft(fields({ name: "A", number: "D100" }))
        const b = SignalForm.draft(fields({ name: "B", number: "D101" }))
        file = SignalForm.withSignal(file, a, { rx: "7E8" })
        file = SignalForm.withSignal(file, b, { rx: "7B8", tx: "7B0" })
        compare(file.ecus, [{ id: "m7e8", rx: "7E8" }, { id: "m7b8", rx: "7B8", tx: "7B0" }])
        compare(file.signals.map(s => s.key + "@" + s.ecu), ["a@m7e8", "b@m7b8"])
        compare(SignalForm.describe(file, file.signals[1]), "22 D101 on 7B8")

        // The same name again replaces its signal in place
        const a2 = SignalForm.draft(fields({ name: "A", number: "D102" }))
        const replaced = SignalForm.withSignal(file, a2, { rx: "7B8", tx: "7B0" })
        compare(replaced.signals.map(s => s.read.number), ["D102", "D101"])
        compare(replaced.ecus.map(e => e.id), ["m7b8"], "a module no signal reads goes")

        const shrunk = SignalForm.withoutSignal(file, "b")
        compare(shrunk.signals.map(s => s.key), ["a"])
        compare(shrunk.ecus.map(e => e.id), ["m7e8"])
        compare(file.signals.length, 2, "the file given is unchanged")
    }

    // One request ID to a module (the format's): a typed one is the
    // module's for every signal on it, and requestIdMove names those it
    // moves; a blank one keeps the module's while another signal reads it,
    // and is the standard's once none does
    function test_aModulesRequestIdIsItsSignalsOne() {
        let file = SignalForm.emptyFile("Daily driver")
        const a = SignalForm.draft(fields({ name: "A", number: "D100" }))
        const b = SignalForm.draft(fields({ name: "B", number: "D101" }))
        const c = SignalForm.draft(fields({ name: "C", number: "D102" }))
        compare(SignalForm.requestIdMove(file, a, { rx: "7E9", tx: "7E2" }), null, "a new module")
        file = SignalForm.withSignal(file, a, { rx: "7E9", tx: "7E2" })
        compare(SignalForm.requestIdMove(file, b, { rx: "7E9" }), null, "blank moves nothing")
        file = SignalForm.withSignal(file, b, { rx: "7E9" })
        compare(file.ecus, [{ id: "m7e9", rx: "7E9", tx: "7E2" }], "blank keeps the module's")
        compare(SignalForm.requestIdMove(file, c, { rx: "7E9", tx: "7E2" }), null, "the same one")
        compare(SignalForm.requestIdMove(file, c, { rx: "7E9", tx: "7E3" }),
                { rx: "7E9", tx: "7E3", names: ["A", "B"] })
        file = SignalForm.withSignal(file, c, { rx: "7E9", tx: "7E3" })
        compare(file.ecus[0].tx, "7E3", "typed, the module's")
        compare(file.signals.map(s => s.ecu), ["m7e9", "m7e9", "m7e9"])

        // The module's one signal, edited: nothing else moves, and a blank
        // request ID is the standard's again
        let one = SignalForm.withSignal(SignalForm.emptyFile("X"), a, { rx: "7E9", tx: "7E2" })
        compare(SignalForm.requestIdMove(one, a, { rx: "7E9", tx: "7E3" }, "a"), null)
        one = SignalForm.withSignal(one, a, { rx: "7E9" }, "a")
        compare(one.ecus, [{ id: "m7e9", rx: "7E9" }])
    }

    function test_aSignalBackIntoTheForm() {
        const f = fields({ name: "Trim", service: "01", number: "06", byte: "1", bits: "16",
                           order: "le", bit: "3", signed: true, scale: "0.5", offset: "-40",
                           unit: "%", decimals: "1", group: "fuel", rx: "7B8", tx: "7B0" })
        const file = SignalForm.withSignal(SignalForm.emptyFile("V"), SignalForm.draft(f),
                                           SignalForm.moduleOf(f))
        compare(SignalForm.fieldsOf(file, file.signals[0]), f)
        const e = fields({ name: "Gear", kind: "enum", number: "D101", values: "0=Park, 3=Drive" })
        const efile = SignalForm.withSignal(SignalForm.emptyFile("V"), SignalForm.draft(e),
                                            SignalForm.moduleOf(e))
        compare(SignalForm.fieldsOf(efile, efile.signals[0]), e)
    }

    function test_aDamagedCopyIsNoFile() {
        compare(SignalForm.parseFile("{"), null)
        compare(SignalForm.parseFile("{\"signals\": []}"), null)
        verify(SignalForm.parseFile(JSON.stringify(SignalForm.emptyFile("V"))) !== null)
    }
}
