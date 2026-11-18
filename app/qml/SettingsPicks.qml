// File/layer: app/qml/SettingsPicks.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.settings_*  (test_qml)

import QtQuick

pragma ComponentBehavior: Bound

Item {
    required property SettingsPage host

    // ---- The plugin store (ADR-0064): every import is a plugin, and a
    // vehicle picks variants of them ----
    // Raised after each change of the store and when the session's VIN
    // comes or goes, so what the bridge answered is asked again
    property int storeRev: 0
    property int vinRev: 0
    // The bridge's list: each loaded plugin with its variants, or a copy
    // that no longer loads
    readonly property var pluginList: storeRev >= 0 ? Bridge.plugins() : []
    readonly property var pluginBySha: {
        const map = {}
        for (const p of pluginList) {
            if (p.ok)
                map[p.sha256] = p
        }
        return map
    }
    // The user's own plugins, which take a variant
    readonly property var ownPlugins: pluginList.filter(p => p.ok && p.mine)
    onOwnPluginsChanged: {
        if (host.importPart.importInto !== "" && !ownPlugins.some(p => p.sha256 === host.importPart.importInto))
            host.importPart.importInto = ""
    }
    // The copies that no longer load, each with the loader's words
    readonly property var refusedPlugins: pluginList.filter(p => !p.ok)
    readonly property var picks: picksNow()
    // The picked vehicle's picks read from the vehicle itself: a handler
    // of the pick's change runs before the bindings on it are evaluated
    function picksNow() {
        return host.pickedVehicle !== null && host.pickedVehicle.picks !== undefined ? host.pickedVehicle.picks : []
    }
    // The picks as the bridge reads them: each one's plugin, whether the
    // store still has it and, on an adapter, the one standing in for Default
    readonly property var pickRows: storeRev >= 0 ? Bridge.pickRow(picks, host.uriPart.adapterUri) : []
    // What the last import, pick, save or removal said
    property string signalProblem: ""
    property string signalNote: ""

    // The loaded plugin and variant a profile names: { plugin, variant },
    // null for none
    function placeOf(profile) {
        for (const p of pluginList) {
            const at = p.ok ? p.profiles.indexOf(profile) : -1
            if (at >= 0)
                return { plugin: p, variant: p.variants[at] }
        }
        return null
    }

    // A variant as the user knows it: its plugin's name, with the variant
    // only where the plugin has several
    function variantName(plugin, variant) {
        return plugin.variants.length > 1 ? qsTr("%1: %2").arg(plugin.name, variant) : plugin.name
    }
    function profileName(profile) {
        const place = placeOf(profile)
        return place === null ? profile : variantName(place.plugin, place.variant)
    }

    // The list's rows, one for each variant of each plugin. First, whole,
    // the plugins with a variant of this car's: one a VIN matches (the
    // lookup's, else the session's and those kept on the vehicle), one the
    // vehicle picks, or any of a plugin imported for the vehicle.
    // caption heads a group; unused marks a plugin's last row when no
    // vehicle picks any of its variants
    readonly property var variantRows: {
        const offered = storeRev >= 0 && vinRev >= 0
            ? Bridge.offeredVariants(host.ownPart.pickedId, host.vinLookupField.text, picks) : []
        const kept = host.pickedVehicle !== null && host.pickedVehicle.vinMatches !== undefined
                     && host.vinLookupField.text.trim() === "" ? host.pickedVehicle.vinMatches : []
        const rows = []
        for (const o of offered) {
            const p = pluginBySha[o.sha256]
            if (p === undefined)
                continue
            const at = p.profiles.indexOf(o.profile)
            rows.push({ sha256: o.sha256, id: o.id, name: o.name, version: o.version,
                        variant: o.variant, profile: o.profile, mine: o.mine === true,
                        obd2: o.extends === "obd2", count: at >= 0 ? p.counts[at] : 0,
                        own: at >= 0 ? p.owns[at] : 0, replacedNames: at >= 0 ? p.replacedNames[at] : [],
                        several: p.variants.length > 1,
                        forCar: host.pickedVehicle !== null
                                && (o.forThisCar === true || kept.indexOf(o.profile) >= 0
                                    || picks.indexOf(o.profile) >= 0),
                        picked: picks.indexOf(o.profile) >= 0, caption: "", unused: false })
        }
        // Whole: a plugin's variants never split across the two groups
        const carPlugins = rows.filter(r => r.forCar).map(r => r.sha256)
        rows.forEach(r => r.forCar = carPlugins.indexOf(r.sha256) >= 0)
        const forCar = rows.filter(r => r.forCar)
        const all = forCar.concat(rows.filter(r => !r.forCar))
        const last = {}
        all.forEach((r, i) => last[r.sha256] = i)
        all.forEach((r, i) => {
            if (forCar.length > 0 && i === 0)
                r.caption = qsTr("For this car:")
            else if (forCar.length > 0 && i === forCar.length)
                r.caption = qsTr("Other plugins:")
            r.unused = last[r.sha256] === i
                       && !VehicleBook.pickedBy(host.vehicles, pluginBySha[r.sha256].profiles)
        })
        return all
    }

    function rowTitle(r) {
        return r.several ? qsTr("%1 %2: %3").arg(r.name, r.version, r.variant)
                         : qsTr("%1 %2").arg(r.name, r.version)
    }
    // A profile by its row's title; the profile itself when no plugin has it
    function profileTitle(profile) {
        const place = placeOf(profile)
        return place === null ? profile
             : rowTitle({ name: place.plugin.name, version: place.plugin.version,
                          variant: place.variant, several: place.plugin.variants.length > 1 })
    }
    // What a row reads, and whose it is
    function rowReads(r) {
        const yours = r.mine ? qsTr(" · yours") : ""
        if (!r.obd2)
            return (r.count === 1 ? qsTr("1 signal") : qsTr("%1 signals").arg(r.count)) + yours
        // A file on obd2 adds signals and replaces some of obd2's by name
        const swapped = r.replacedNames.length === 0 ? ""
            : r.replacedNames.length <= 2 ? qsTr("%1 replaced").arg(r.replacedNames.join(qsTr(" and ")))
            : qsTr("%1 replaced").arg(qsTr("%1 signals").arg(r.replacedNames.length))
        const added = r.own === 1 ? qsTr("1 signal") : qsTr("%1 signals").arg(r.own)
        if (swapped === "")
            return qsTr("obd2 + %1").arg(added) + yours
        return (r.own === 0 ? qsTr("obd2, %1").arg(swapped)
                            : qsTr("obd2, %1, %2 more").arg(swapped, r.own)) + yours
    }

    // The picks no plugin here has (a version that dropped the variant, a
    // plugin removed elsewhere), so each can be seen and undone
    readonly property var missingPicks: pickRows.filter(r => r.missing).map(r => {
        const m = /^plugin\.([^.]+)\.(.+)$/.exec(r.profile)
        const p = m === null ? undefined : pluginList.find(e => e.ok && e.id === m[1])
        return { profile: r.profile,
                 text: p !== undefined ? qsTr("%1 is no longer in %2").arg(m[2], p.name)
                                       : qsTr("%1: not in the store").arg(r.profile) }
    })

    // The Signals row's summary: the plugins counted, then the picked
    // vehicle's first pick by name (no translation is loaded to pick a
    // plural form)
    function signalsSummary() {
        const n = pluginList.filter(p => p.ok).length
        const plugins = n === 1 ? qsTr("1 plugin") : qsTr("%1 plugins").arg(n)
        if (host.pickedVehicle === null)
            return n === 0 ? qsTr("none yet") : plugins
        const named = pickRows.filter(r => !r.missing)
        const missing = pickRows.length - named.length
        const first = named.length > 0 ? profileName(named[0].profile) : ""
        const said = named.length === 0 ? (n === 0 ? qsTr("none yet") : qsTr("%1, none picked").arg(plugins))
                   : named.length === 1 ? qsTr("%1: %2").arg(plugins, first)
                   : qsTr("%1: %2 and %3 more").arg(plugins, first).arg(named.length - 1)
        return said + (missing === 0 ? "" : missing === 1 ? qsTr(", 1 pick missing")
                                                           : qsTr(", %1 picks missing").arg(missing))
    }

    // A press on a row: the variant picked for the vehicle, or its pick
    // undone. A pick made while the field reads Default is read at once
    function togglePick(profile) {
        if (host.pickedVehicle === null || !host.ownPart.idle)
            return
        signalProblem = ""
        signalNote = ""
        if (picks.indexOf(profile) >= 0) {
            host.saveVehicles(VehicleBook.setPicks(host.vehicles, Bridge.unpickVariant(picks, profile)))
            if (host.uriPart.profileChoice === profile)
                host.uriPart.chooseProfile("default")
            return
        }
        pickProfile(profile)
    }

    // profile into the picked vehicle's picks, last: "" when refused (the
    // words in signalProblem), "read" when the field, reading Default,
    // now reads it, else "picked"
    function pickProfile(profile) {
        const r = Bridge.pickVariant(picks, profile)
        if (!r.ok) {
            signalProblem = r.words
            return ""
        }
        const readsDefault = host.uriPart.profileChoice === "default"
            && host.uriPart.queryValue(host.uriPart.splitUri(host.uriField.text).pairs, "profile") === undefined
        host.saveVehicles(VehicleBook.setPicks(host.vehicles, r.picks))
        if (!readsDefault)
            return "picked"
        host.uriPart.chooseProfile(profile)
        return "read"
    }
}
