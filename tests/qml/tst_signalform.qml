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

    // A key is made from the name once: an Edit keeps it whatever the
    // new name, a new signal of a saved one's name takes that one's, and a
    // name whose key the file has gets the next free one
    function test_aKeyIsMadeOnceAndKeptThroughARename() {
        compare(SignalForm.keyFor([], "Oil Temp", ""), "oil_temp")
        const saved = [{ key: "oil_temp", name: "Oil temperature" }]
        compare(SignalForm.keyFor(saved, "Oil temp, sump", "oil_temp"), "oil_temp", "an Edit keeps it")
        compare(SignalForm.keyFor(saved, " Oil temperature ", ""), "oil_temp",
                "a saved one's name, its key")
        compare(SignalForm.keyFor(saved, "Oil Temp", ""), "oil_temp_2", "the kept key is taken")
        compare(SignalForm.keyFor(saved.concat([{ key: "oil_temp_2", name: "Oil Temp" }]), "Oil-temp", ""),
                "oil_temp_3")
        compare(SignalForm.keyFor([{ key: "by_hand", name: "Typed" }], "Typed again", "by_hand"), "by_hand",
                "a key written by hand is kept too")
        // A numbered key stays inside a key's 31 characters
        const long = "a".repeat(40)
        const first = SignalForm.keyFor([], long, "")
        compare(first, "a".repeat(31))
        compare(SignalForm.keyFor([{ key: first, name: "another" }], long, ""), "a".repeat(29) + "_2",
                "the name's part is cut, never the number")
        const nine = [{ key: first, name: "n1" }]
        for (let n = 2; n <= 9; ++n)
            nine.push({ key: "a".repeat(29) + "_" + n, name: "n" + n })
        compare(SignalForm.keyFor(nine, long, ""), "a".repeat(28) + "_10")
        // One under the limit keeps its whole name beside a one-digit number
        compare(SignalForm.keyFor([{ key: "b".repeat(29), name: "x" }], "b".repeat(29), ""),
                "b".repeat(29) + "_2")
        compare(SignalForm.keyFor([{ key: "a_b_c", name: "x" }], "a b c", ""), "a_b_c_2")
    }

    function test_aDamagedCopyIsNoFile() {
        compare(SignalForm.parseFile("{"), null)
        compare(SignalForm.parseFile("{\"signals\": []}"), null)
        verify(SignalForm.parseFile(JSON.stringify(SignalForm.emptyFile("V"))) !== null)
    }

    // A file written by hand may leave a signal's ECU out, which the
    // loader reads as the file's first: read so, an edit keeps that ECU
    // and the list names it
    function test_aSignalWithNoEcuReadsTheFilesFirst() {
        const file = SignalForm.parseFile(JSON.stringify({
            havne_signals: 1, name: "By hand", source: { name: "t" },
            ecus: [{ id: "pcm", rx: "7E8" }, { id: "tcm", rx: "7E9" }],
            signals: [
                { key: "a", name: "A", read: { service: "22", number: "D100" }, field: { byte: 0, bits: 8 } },
                { key: "b", name: "B", ecu: "tcm", read: { service: "22", number: "D101" },
                  field: { byte: 0, bits: 8 } },
                { key: "c", name: "C", kind: "computed", expr: "a + b" }] }))
        compare(file.signals.map(s => s.ecu), ["pcm", "tcm", undefined], "a computed signal reads none")
        compare(SignalForm.describe(file, file.signals[0]), "22 D100 on 7E8")
        compare(SignalForm.fieldsOf(file, file.signals[0]).rx, "7E8")
        compare(SignalForm.trimEcus(SignalForm.copy(file)).ecus.map(e => e.id), ["pcm", "tcm"],
                "its ECU is one the file uses")
    }

    // A computed signal has no read and no field: Edit and the list take it
    function test_aComputedSignalReadsBackWithoutAReadOrAField() {
        const file = SignalForm.parseFile(JSON.stringify({
            havne_signals: 2, name: "By hand", source: { name: "t" },
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [
                { key: "a", name: "A", read: { service: "22", number: "D100" }, field: { byte: 0, bits: 8 } },
                { key: "c", name: "C", kind: "computed", expr: "a * 2", unit: "kPa" }] }))
        const f = SignalForm.fieldsOf(file, file.signals[1])
        compare(f.name, "C")
        compare(f.kind, "computed")
        compare(f.unit, "kPa")
        compare(f.number, "")
        compare(SignalForm.describe(file, file.signals[1]), "computed from a * 2")
    }

    // A renamed computed signal takes the place of the one it was, under the
    // key it had; a new one goes at the end
    function test_aComputedSaveReplacesTheKeyItWasEditedUnder() {
        const file = { signals: [{ key: "user.a", name: "A", kind: "computed", expr: "1" },
                                 { key: "user.b", name: "B", kind: "computed", expr: "2" }], ecus: [] }
        const renamed = SignalForm.withComputed(file, { key: "user.c", name: "C", kind: "computed", expr: "3" }, "user.a")
        compare(renamed.signals.map(s => s.key).join(), "user.c,user.b")
        const added = SignalForm.withComputed(file, { key: "user.c", name: "C", kind: "computed", expr: "3" }, "")
        compare(added.signals.map(s => s.key).join(), "user.a,user.b,user.c")
    }

    function ownWith(signals, format) {
        return { havne_signals: format || 2, name: "Mine", source: { name: "t" },
                 ecus: [{ id: "pcm", rx: "7E8", name: "Engine" }], signals: signals }
    }
    function did(key, name, number, byte) {
        return { key: key, name: name, ecu: "pcm", read: { service: "22", number: number }, field: { byte: byte || 0 } }
    }
    function computed(key, name, expr) {
        return { key: key, name: name, kind: "computed", expr: expr }
    }
    readonly property var reads: ({ "boost - obd2.baro + gear": ["boost", "obd2.baro", "gear"],
                                    "gear * 2": ["gear"], "a + b": ["a", "b"], "a * 2": ["a"] })
    function names(e) {
        return reads[e] || []
    }

    function test_needsAreTheNamesNoOwnKeyNorObd2Has() {
        compare(SignalForm.needsOf(["boost", "obd2.baro", "gear", "boost", "gear"], ["boost"]), ["gear"])
        const r = SignalForm.withSaveNeeds(ownWith([did("boost", "Boost", "D100"),
                                                    computed("c", "C", "boost - obd2.baro + gear")]),
                                           names, [])
        compare(r.refused, "")
        compare(r.doc.needs, ["gear"])
    }

    function test_theLastReaderTakesItsNeedAway() {
        const file = ownWith([did("a", "A", "D100"), computed("c", "C", "a * 2")], 3)
        file.needs = ["gear"]
        const r = SignalForm.withSaveNeeds(file, names, [])
        compare(r.doc.needs, undefined, "no reader, no need")
        compare(file.needs, ["gear"], "the argument is unchanged")
        compare(SignalForm.withNeeds(file, []).needs, undefined)
        compare(SignalForm.withNeeds(file, ["x"]).needs, ["x"])
    }

    // Lowering would turn "beside" back into "replaces" for the file read alone
    function test_needsRaiseToFormat3AndAFileStaysThere() {
        const two = ownWith([did("a", "A", "D100"), computed("c", "C", "gear * 2")])
        const raised = SignalForm.withSaveNeeds(two, names, [])
        compare(raised.doc.havne_signals, 3)
        compare(SignalForm.withSaveNeeds(ownWith([did("a", "A", "D100"), computed("c", "C", "a * 2")]), names, [])
                    .doc.havne_signals, 2, "no need, no raise")
        const without = SignalForm.withoutSignal(raised.doc, "c")
        const after = SignalForm.withSaveNeeds(without, names, [])
        compare(after.doc.havne_signals, 3, "a file with no needs left stays at 3")
        compare(after.doc.needs, undefined)
        compare(SignalForm.withFormat(raised.doc, 2).havne_signals, 3)
    }

    function test_aSaveWithTwoSignalsAtOneAddressIsRefusedBeforeTheRaise() {
        const file = ownWith([did("a", "A", "F40C"), did("b", "B", "f40c"), computed("c", "C", "gear * 2")])
        const r = SignalForm.withSaveNeeds(file, names, [])
        compare(r.refused, "'A' and 'B' read the same value (Engine, 22 f40c, byte 0): a file that reads "
                           + "another plugin's signals gives one address to one signal. Change or remove one first.")
        compare(r.doc, file, "nothing raised")
        compare(SignalForm.sharedAddress(ownWith([did("a", "A", "F40C"), did("b", "B", "F40C", 1)])), null,
                "another byte is another address")
        verify(SignalForm.withSaveNeeds(ownWith([did("a", "A", "F40C"), did("b", "B", "F40C")]), names, [])
                   .refused === "", "no need, no format 3, no refusal")
    }

    function test_theSaveSaysWhatTheRaiseMadeBeside() {
        const file = ownWith([did("boost", "Boost", "D100"), computed("c", "C", "gear * 2")])
        compare(SignalForm.withSaveNeeds(file, names, ["Boost", "Engine RPM"]).beside, ["Boost"])
        compare(SignalForm.withSaveNeeds(file, names, ["Engine RPM"]).beside, [], "nothing beside")
        const three = ownWith(file.signals, 3)
        compare(SignalForm.withSaveNeeds(three, names, ["Boost"]).beside, [], "already format 3: no change to say")
        compare(SignalForm.withSaveNeeds(ownWith([did("boost", "Boost", "D100")]), names, ["Boost"]).beside, [],
                "no raise")
    }
}
