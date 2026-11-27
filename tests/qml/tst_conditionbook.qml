import QtQuick
import QtTest
import App

TestCase {
    name: "ConditionBook"

    readonly property var beside: [{ baseDef: 7, baseRef: "id:23", baseTick: "7E8:4133:2.0:8", standing: 40, standingKey: "boost", name: "Boost", part: "Example import" }]
    function condition(key, when, enabled) {
        return { key: key, name: key, enabled: enabled !== false, when: when }
    }

    function test_carryOverPutsTheKeyFirst() {
        const doc = { havne_conditions: 1, conditions: [
            condition("one", { signal: ["id:23"], above: 10 }),
            condition("two", { any: [{ signal: ["id:12"], above: 1 }, { signal: ["id:23"], below: 2 }], notes: "kept" }),
            condition("three", { signal: ["id:12"], above: 1 })] }
        const r = ConditionBook.carryOver(doc, beside, r => true)
        compare(r.count, 2)
        compare(r.doc.conditions[0].when.signal, ["key:boost", "id:23"])
        compare(r.doc.conditions[1].when.any[1].signal, ["key:boost", "id:23"])
        compare(r.doc.conditions[1].when.any[0].signal, ["id:12"])
        compare(r.doc.conditions[1].when.notes, "kept", "a group keeps its other keys")
        compare(r.doc.conditions[2].when.signal, ["id:12"])
        compare(doc.conditions[0].when.signal, ["id:23"], "the argument is unchanged")
        const full = ["id:23", "key:a", "key:b", "key:c", "key:d", "key:e", "key:f", "key:g"]
        const capped = ConditionBook.carryOver({ havne_conditions: 1, conditions: [condition("c", { signal: full, above: 1 })] },
                                               beside, r => true)
        compare(capped.doc.conditions[0].when.signal.length, ConditionBook.maxRefs)
    }

    function test_anAlertWithNoSignalInTheSetIsMarked() {
        const has = r => r === "id:12"
        verify(ConditionBook.cantFire(condition("a", { signal: ["id:99"], above: 1 }), has))
        verify(!ConditionBook.cantFire(condition("b", { signal: ["id:99", "id:12"], above: 1 }), has))
        verify(!ConditionBook.cantFire(condition("c", { signal: ["id:99"], above: 1 }, false), has), "a disabled one")
        verify(ConditionBook.cantFire(condition("d", { all: [{ signal: ["id:12"], above: 1 }, { signal: ["id:99"], above: 1 }] }), has))
        verify(!ConditionBook.cantFire(condition("e", { any: [{ signal: ["id:12"], above: 1 }, { signal: ["id:99"], above: 1 }] }), has))
        verify(!ConditionBook.cantFire(condition("f", { link: "timeout" }), has), "no signal term")
    }
}
