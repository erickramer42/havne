// File/layer: tests/qml/tst_dashedit_plugpages.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashedit_plugpages (test_qml)

import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

// The Add page list's section of the pages a plugin carries (ADR-0052's
// appendix on pages): a page is the user's copy, whole
DashEditKit {
    id: testCase
    name: "DashboardEditPluginPages"

    function pageJson(form, name, refs) {
        return { format: 2, form: form, pages: [{ name: name, cells: refs.map((r, i) => ({
            kind: "digital", w: 3, h: 9, label: "Cell " + i, refs: [r], unit: "", options: {} })) }] }
    }
    // A packed plugin with two variants (base has oil, turbo adds boost), a
    // phone page and a desktop page; the desktop page reads both signals
    // `pages` replaces the phone and desktop pages' own text where given
    function packedText(id, name, pages) {
        const sig = (key, number) => ({ key: key, name: key, ecu: "pcm",
                                        read: { service: "22", number: number }, field: { byte: 0 } })
        return JSON.stringify({
            havne_plugin: 1, id: id, name: name, version: "1.0.0", requires: { core: "0.1.0" },
            source: { name: "t" }, tiers: ["data"], ecus: [{ id: "pcm", rx: "7E8" }],
            variants: [{ id: "base", files: ["data/base.json"] },
                       { id: "turbo", files: ["data/base.json", "data/turbo.json"] }],
            pages: ["pages/phone.json", "pages/desktop.json"],
            files: {
                "data/base.json": { havne_signals: 1, signals: [sig("oil", "F40D")] },
                "data/turbo.json": { havne_signals: 1, signals: [sig("boost", "F40E")] },
                "pages/phone.json": pages && pages.phone ? pages.phone : pageJson("phone", "Engine phone", ["key:oil"]),
                "pages/desktop.json": pages && pages.desktop ? pages.desktop
                                      : pageJson("desktop", "Engine", ["key:oil", "key:boost"])
            } })
    }
    function importPlugin(id, name, pages) {
        const r = Bridge.importPlugin(Harness.writeScratchFile(id + ".havne-plugin.json", packedText(id, name, pages)))
        verify(r.ok, r.words)
        const p = Bridge.plugins().find(e => e.id === id)
        compare(p.profiles.length, 2, "the cases read profiles[0] as base and [1] as turbo")
        return p
    }
    // A page of this vehicle's picks, the plugin's variants named by profile
    function pageWithPicks(props, picks) {
        const page = storedPageOf(props, docOf([cellOf("A")]))
        let v = VehicleBook.add(VehicleBook.empty(), "Car")
        v = VehicleBook.setPicks(v, picks)
        page.vehicles = v
        page.pickedSets = picks.map(p => ({ profile: p, name: p, variant: "" }))
        return page
    }
    function openList(page) {
        page.startEditing()
        page.editsPart.openPresets("add")
        verify(find(page, "dashPresetList").visible, "the list opens")
    }

    function test_theListOffersAPluginsPagesOfTheWindowsFormOnly() {
        const p = importPlugin("paged", "Paged")
        const desktop = pageWithPicks({}, [p.profiles[0]])
        openList(desktop)
        verify(find(desktop, "dashPluginPages").visible)
        compare(find(desktop, "plugpage_0_1").text, "Engine", "the desktop page")
        verify(!findChild(desktop, "plugpage_0_0"), "the phone page is not offered")
        desktop.destroy()

        const phone = pageWithPicks({ form: "phone", width: 384, height: 832 }, [p.profiles[0]])
        openList(phone)
        compare(find(phone, "plugpage_0_0").text, "Engine phone")
        verify(!findChild(phone, "plugpage_0_1"), "the desktop page is not offered")
    }

    function test_noSectionShowsWhenNoPluginHasAPage() {
        const page = pageWithPicks({}, [])
        openList(page)
        verify(!find(page, "dashPluginPages").visible)
    }

    function test_thePickedVehiclesPluginsComeFirst() {
        importPlugin("alpha", "Alpha")
        const b = importPlugin("beta", "Beta")
        const page = pageWithPicks({}, [b.profiles[0]])
        openList(page)
        compare(page.editsPart.pluginPageList.map(e => e.name), ["Beta", "Alpha"])
        compare(page.editsPart.pluginPageList.map(e => e.picked), [true, false])
    }

    function test_aPluginPageIsAddedAsACopyAfterTheShownPageAsOneUndoStep() {
        const p = importPlugin("paged", "Paged")
        const page = pageWithPicks({}, [p.profiles[1]])
        const first = page.doc.pages.length
        openList(page)
        click(page, "plugpage_0_1")
        verify(!find(page, "dashPresetList").visible, "the list shuts")
        compare(page.doc.pages.slice(0, 2).map(g => g.name), ["Test", "Engine"])
        compare(page.doc.pages.length, first + 1)
        compare(page.pageIndex, 1, "the new page shows")
        compare(page.doc.pages[1].cells.map(c => c.refs[0]), ["key:oil", "key:boost"])
        compare(storedText(page), DashboardBook.serialize(page.doc))
        compare(page.historyPart.undoWords, "added the page Engine")
        compare(page.layoutFilePart.layoutNote, "Added the page Engine from the plugin Paged")
        page.historyPart.undo()
        compare(page.doc.pages.length, first, "one Undo takes the page back")
        verify(!page.historyPart.canUndo, "and it was one step")
    }

    function test_aPageNamingASignalTheVehicleLacksIsAddedAndSaysSo() {
        const p = importPlugin("paged", "Paged")
        // base has oil only: the cell reading boost alone has no pick
        const page = pageWithPicks({}, [p.profiles[0]])
        verify(Bridge.pluginPageText("paged", 1) !== "")
        openList(page)
        click(page, "plugpage_0_1")
        compare(page.doc.pages[1].cells.length, 2, "added whole")
        compare(page.layoutFilePart.layoutNote,
                "Added the page Engine from the plugin Paged. 1 gauge reads a signal none of the vehicle's "
                + "picked plugins has: pick the plugin's variant under Signals")
        // A vehicle that picks turbo has both
        page.destroy()
        const turbo = pageWithPicks({}, [p.profiles[1]])
        openList(turbo)
        click(turbo, "plugpage_0_1")
        compare(turbo.layoutFilePart.layoutNote, "Added the page Engine from the plugin Paged")
    }

    function test_aPluginTheVehicleDoesntPickIsListedAfterThePickedOnes() {
        importPlugin("paged", "Paged")
        const page = pageWithPicks({}, [])
        openList(page)
        compare(page.editsPart.pluginPageList.length, 1)
        verify(!page.editsPart.pluginPageList[0].picked)
        click(page, "plugpage_0_1")
        compare(page.doc.pages.length, 2)
    }

    function test_theAddIsRefusedAtTheBound() {
        importPlugin("paged", "Paged")
        const d = docOf([cellOf("A")])
        while (d.pages.length < DashboardBook.maxPages)
            d.pages.push({ name: "P" + d.pages.length, cells: [] })
        const page = storedPageOf({}, d)
        page.startEditing()
        page.editsPart.openPresets("add")
        verify(!find(page, "plugpage_0_1").enabled, "no room for a page")
        verify(!page.editsPart.addPluginPage("paged", 1))
        compare(page.doc.pages.length, DashboardBook.maxPages)
        verify(page.layoutFilePart.layoutNoteBad)
    }

    function test_aPageThatCantBeReadAddsNothing() {
        const page = pageWithPicks({}, [])
        page.startEditing()
        verify(!page.editsPart.addPluginPage("nobody", 0))
        compare(page.layoutFilePart.layoutNote, "The page couldn't be read")
        verify(page.layoutFilePart.layoutNoteBad)
        verify(!page.historyPart.canUndo)
        compare(page.doc.pages.length, 1)
    }

    // Core refuses a page file of two pages at the import, so the text is
    // handed over directly: one page is added, two (or none) are not
    function test_aTextOfTwoPagesAddsNothing() {
        const page = pageWithPicks({}, [])
        page.startEditing()
        const one = pageJson("desktop", "Engine", ["key:oil"])
        const two = pageJson("desktop", "Engine", ["key:oil"])
        two.pages.push(pageJson("desktop", "Second", ["key:oil"]).pages[0])
        verify(!page.editsPart.addPluginPageText("Two", JSON.stringify(two)))
        compare(page.layoutFilePart.layoutNote, "The page couldn't be read")
        verify(page.layoutFilePart.layoutNoteBad)
        verify(!page.historyPart.canUndo)
        compare(page.doc.pages.length, 1)
        verify(page.editsPart.addPluginPageText("One", JSON.stringify(one)))
        compare(page.doc.pages.length, 2)
    }

    function test_aRemovedPluginLeavesTheCopyAsItWas() {
        const p = importPlugin("paged", "Paged")
        const page = pageWithPicks({}, [p.profiles[1]])
        openList(page)
        click(page, "plugpage_0_1")
        const copy = DashboardBook.serialize(page.doc)
        verify(Bridge.removePlugin(p.sha256).ok)
        compare(DashboardBook.serialize(page.doc), copy)
        page.editsPart.openPresets("add")
        verify(!find(page, "dashPluginPages").visible, "the removed plugin is no longer offered")
    }

    function test_theListFitsTheNarrowestPhone() {
        const p = importPlugin("paged", "A plugin with a rather long name that cannot fit")
        const page = pageWithPicks({ form: "phone", width: 240, height: 700 }, [p.profiles[0]])
        openList(page)
        verify(waitForPolish(page.Window.window))
        const bar = page.topBarItem
        for (const name of ["plugpage_name_0", "plugpage_0_0"]) {
            const item = find(page, name)
            const right = item.mapToItem(bar, item.width, 0).x
            verify(right <= bar.width + 0.5, name + " ends inside the bar: " + right + " of " + bar.width)
        }
        verify(find(page, "plugpage_name_0").width > 0)
    }

    function test_theDesktopsPhoneFrameOffersPhonePages() {
        const p = importPlugin("paged", "Paged")
        const page = pageWithPicks({}, [p.profiles[1]])
        page.startEditing()
        page.setPhoneEdit(true)
        compare(page.form, "phone")
        page.editsPart.openPresets("add")
        compare(find(page, "plugpage_0_0").text, "Engine phone")
        verify(!findChild(page, "plugpage_0_1"), "no desktop page in the frame")
        page.setPhoneEdit(false)
        page.editsPart.openPresets("add")
        compare(find(page, "plugpage_0_1").text, "Engine", "back on the desktop's own")
    }

    function test_aPageThatDoesntReadIsNotListed() {
        // A name the book refuses drops the page, so the text reads as no page at all
        const bad = pageJson("desktop", "Engine", ["key:oil"])
        bad.pages[0].name = 5
        importPlugin("broken", "Broken", { desktop: bad })
        const page = pageWithPicks({}, [])
        openList(page)
        compare(page.editsPart.pluginPageList.length, 0, "the plugin has no page that reads")
        verify(!find(page, "dashPluginPages").visible)
        verify(!page.editsPart.addPluginPage("broken", 1))
        compare(page.layoutFilePart.layoutNote, "The page couldn't be read")
        compare(page.doc.pages.length, 1)
    }

    function test_aPageWithACellTheBookDropsIsAddedAndSaysSo() {
        const text = pageJson("desktop", "Engine", ["key:oil", "key:oil"])
        text.pages[0].cells[1].w = "wide"
        const p = importPlugin("dropping", "Dropping", { desktop: text })
        const page = pageWithPicks({}, [p.profiles[0]])
        openList(page)
        click(page, "plugpage_0_1")
        compare(page.doc.pages[1].cells.length, 1, "the readable cell is kept")
        verify(page.layoutFilePart.layoutNote.indexOf("left out because they couldn't be read") >= 0,
               page.layoutFilePart.layoutNote)
    }

    function test_severalGaugesWithoutTheirSignalAreCounted() {
        const text = pageJson("desktop", "Engine", ["key:boost", "key:boost"])
        const p = importPlugin("boosty", "Boosty", { desktop: text })
        const page = pageWithPicks({}, [p.profiles[0]])
        openList(page)
        click(page, "plugpage_0_1")
        compare(page.layoutFilePart.layoutNote,
                "Added the page Engine from the plugin Boosty. 2 gauges read signals none of the vehicle's "
                + "picked plugins has: pick the plugin's variant under Signals")
    }
}
