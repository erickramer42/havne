// File/layer: app/qml/SettingsConnectFrom.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.settings_*  (test_qml)

import QtQuick

pragma ComponentBehavior: Bound

Item {
    required property SettingsPage host

    // kept: the address to keep once uri connects, where it isn't uri
    // itself (a list's row, whose profile the page added)
    property string connectKeeps: ""
    function requestConnect(uri, kept) {
        if (uri === "")
            return
        const refused = setRefusal(uri)
        if (refused !== "") {
            host.connectPart.errorLabel.text = refused
            host.connectPart.errorLabel.visible = true
            Bridge.logEvent(refused)
            return
        }
        const reads = withVehicleSet(uri)
        if (reads !== uri && host.connectPart.uriField.text === uri)
            host.connectPart.uriField.text = reads
        connectKeeps = kept === undefined ? "" : kept
        Bridge.connectSessionAsync(reads)
    }

    // The addresses kept from the connects that succeeded, newest first,
    // for Dash's connect list
    readonly property int maxRecentUris: 5
    property var recentUris: []
    // Recordings is a replay's way back
    function keptUri(uri) {
        return VehicleBook.isLiveUri(uri)
    }
    // The stored list, or `lastUri` alone when it holds none; a damaged
    // list reads as none, and anything but a non-empty kept string is left out
    function parseRecentUris(text, lastUri) {
        let list = []
        try {
            const read = JSON.parse(text)
            if (Array.isArray(read))
                list = read.filter(u => typeof u === "string" && u !== "" && keptUri(u))
        } catch (e) {
            // none, until the next connect writes one
        }
        if (list.length === 0 && lastUri !== "" && keptUri(lastUri))
            list = [lastUri]
        return list.slice(0, maxRecentUris)
    }
    // `uri` first and only once, the list cut to its most recent
    function withRecentUri(list, uri) {
        return [uri].concat(list.filter(u => u !== uri)).slice(0, maxRecentUris)
    }
    // What Dash lists: the recent addresses, the picked vehicle's saved one
    // first when absent. Display only: recentUris holds what was kept
    readonly property string vehicleUri: host.pickedVehicle !== null ? host.pickedVehicle.uri : ""
    function connectRows(recents, vehicle) {
        return vehicle === "" || recents.includes(vehicle) ? recents : [vehicle].concat(recents)
    }
    readonly property var connectUris: connectRows(recentUris, vehicleUri)
    // What the connect lists offer besides the recent addresses: the J2534
    // interfaces, the serial ports, the CAN interfaces and the phone's
    // Bluetooth adapters, as Connection's device list has them
    readonly property var connectAdapters: host.devices.map(d => ({
            uri: d.base, title: d.title, kind: d.kind, other: d.other === true
        }))
    readonly property var together: host.picksPart.together
    // On an adapter Default is obd2 already
    function vehicleProfileAt(uri) {
        if (together === null || !takesVehicleSet(uri))
            return ""
        return together.reads === "merged" ? together.profile
             : together.reads === "obd2" && !VehicleBook.isAdapterUri(uri) ? "obd2" : ""
    }

    // A listening CAN interface is left to listen: a set there would start sending
    function takesVehicleSet(uri) {
        if (VehicleBook.isAdapterUri(uri))
            return true
        const found = /^socketcan:\/\/([^?]+)/.exec(uri)
        const c = found === null ? undefined : host.canInterfaces.find(i => i.name === found[1])
        return c !== undefined && c.up === true
               && (c.kind === "vcan" || (c.modes || []).indexOf("one-shot") >= 0)
    }

    function withVehicleSet(uri) {
        return together === null ? uri : VehicleBook.withVehicleSet(uri, together.reads, together.profile)
    }

    function setRefusal(uri) {
        if (together === null || together.reads !== "refused")
            return ""
        const named = host.uriPart.queryValue(host.uriPart.splitUri(uri).pairs, "profile")
        const merged = typeof named === "string" && named.startsWith("merged.") && keptUri(uri)
        return merged || (named === undefined && takesVehicleSet(uri)) ? together.words : ""
    }

    function listAddress(uri) {
        const named = host.uriPart.queryValue(host.uriPart.splitUri(uri).pairs, "profile")
        return typeof named === "string" && (named.startsWith("plugin.") || named.startsWith("user."))
               && host.picksPart.picksNow().indexOf(named) < 0 ? VehicleBook.withoutFileProfile(uri) : uri
    }

    function listConnectUri(uri) {
        const plain = withVehicleSet(listAddress(uri))
        return VehicleBook.withProfile(plain, vehicleProfileAt(plain))
    }

    // What a list's row reads, where its kind doesn't say: "obd2 + <name>"
    // for a row the rule composes, a plugin's name for an address that
    // names its profile; "" otherwise
    function listProfileWord(uri) {
        const plain = listAddress(uri)
        const named = host.uriPart.queryValue(host.uriPart.splitUri(plain).pairs, "profile")
        if (typeof named === "string" && named.startsWith("plugin."))
            return host.picksPart.placeOf(named) !== null ? host.picksPart.profileName(named) : qsTr("a plugin not in the store")
        const reads = host.uriPart.queryValue(host.uriPart.splitUri(listConnectUri(uri)).pairs, "profile")
        if (typeof reads !== "string" || reads === named && !reads.startsWith("merged."))
            return ""
        return reads === "obd2" ? "obd2" : host.uriPart.profileNames[reads] ?? ""
    }
    // The lists' rows by address, for those with a word
    readonly property var connectProfileWords: {
        const words = {}
        for (const uri of connectUris.concat(connectAdapters.map(a => a.uri))) {
            const word = listProfileWord(uri)
            if (word !== "")
                words[uri] = word
        }
        return words
    }

    // A list's Connect: the field shows the address as typed text, with
    // the profile it reads, before the connect. The row's address is what
    // is kept: the profile added is the vehicle's pick of this moment
    function connectFromList(uri) {
        const composed = listConnectUri(uri)
        host.connectPart.uriField.text = composed
        requestConnect(composed, listAddress(uri))
    }
}
