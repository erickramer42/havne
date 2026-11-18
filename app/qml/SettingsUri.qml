// File/layer: app/qml/SettingsUri.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.settings_*  (test_qml)

import QtQuick

pragma ComponentBehavior: Bound

Item {
    required property SettingsPage host

    // The URI is composed from three choices, so none of it needs typing:
    // a base (a preset or a discovered device), a profile, and
    // uds-generic's DIDs. The field stays editable both ways: a
    // hand-written URI connects as written and its profile= and did= set
    // the row and the DID field, and the next preset, device or profile
    // click recomposes it, keeping every other query key where it was.
    // The choices live only while the app runs.
    property string profileChoice: "default"
    // uds-generic's extended session (session=03), composed as did= is
    property bool extSession: false

    // The profiles the row offers (ADR-0013, ADR-0020), then the picked
    // vehicle's picks the store has (ADR-0064 Q5). "default" is the
    // scheme's own profile, so it adds no profile= key. A typed name
    // outside the list highlights no button; core judges it at connect.
    readonly property var profiles: builtInProfiles.concat(pickProfiles)

    // On an adapter, Default is obd2, and a variant that extends obd2
    // reads all of obd2 and more, so the first such pick takes obd2's
    // place there (ADR-0064 Q5): null for none or on another scheme,
    // where Default isn't obd2. Default's button stays only while the
    // field reads it, so the row always shows the URI's
    readonly property bool adapterUri: VehicleBook.isAdapterUri(host.uriField.text)
    readonly property var standIn: host.pickRows.find(r => r.standIn) || null
    readonly property var defaultProfile: ({
        name: "default", label: qsTr("Default"),
        hint: qsTr("Default reads OBD-II values on an adapter; a recording or a "
                   + "CAN interface shows its frames") })

    // A pick as the row's entry: one that extends obd2 says so on an
    // adapter, where obd2 is what Default reads
    function pickEntry(r) {
        const name = host.profileName(r.profile)
        const obd2 = r.extends === "obd2"
        return { name: r.profile,
                 label: obd2 && adapterUri ? qsTr("obd2 + %1").arg(name) : name,
                 hint: obd2 ? qsTr("obd2's signals and those of %1, picked for this vehicle")
                                  .arg(host.profileTitle(r.profile))
                            : qsTr("The signals of %1, picked for this vehicle")
                                  .arg(host.profileTitle(r.profile)) }
    }
    // The picks but the stand-in, which sits where Default does
    readonly property var pickProfiles: host.pickRows.filter(r => !r.missing && !r.standIn)
                                                .map(r => pickEntry(r))
    readonly property var builtInProfiles: (standIn === null ? [defaultProfile]
        : profileChoice === "default" ? [defaultProfile, pickEntry(standIn)]
        : [pickEntry(standIn)]).concat([
        { name: "raw", label: qsTr("raw (log only)"),
          hint: qsTr("Raw (log only): no decode, frames reach the Activity page only. On an adapter it listens "
                     + "to the engine ECU (0x7E8); add ?filter= for another ID. An ELM327 "
                     + "hears only replies to its requests, so add ?poll= there") },
        { name: "uds-generic", label: qsTr("uds-generic (DIDs)"),
          hint: qsTr("UDS DIDs (uds-generic): Service 0x22 reads of the DIDs below, polled; a range is scanned "
                     + "once and the DIDs the ECU answers join the polling") }
    ])

    // The picked vehicle's sets as [{ profile, name, variant }]: its picks
    // the store has, in their order
    readonly property var pickedSets: host.pickRows.filter(r => !r.missing)
        .map(r => ({ profile: r.profile, name: r.name, variant: r.variant }))
    // A profile's name as the user knows it: every loaded plugin's
    // variants by the plugin's name
    readonly property var profileNames: {
        const names = {}
        for (const p of host.pluginList) {
            for (const profile of (p.ok ? p.profiles : []))
                names[profile] = p.name
        }
        return names
    }

    // The first picked set that reads all of obd2, which stands in for
    // Default on an adapter (ADR-0064)
    readonly property var obd2PickedSet: {
        const first = host.pickRows.find(r => !r.missing && r.extends === "obd2")
        return first === undefined ? null
             : { profile: first.profile, name: first.name, variant: first.variant }
    }

    // What a page that works with no session lists its signals from: the
    // field's URI, which carries the chosen profile, except an adapter's
    // read under Default, where the vehicle's obd2 stand-in is listed;
    // with no URI typed the chosen profile alone, or under Default the
    // picked vehicle's first set, which a restart doesn't forget as it
    // does the choice
    readonly property string offlineUri: {
        const typed = host.uriField.text.trim()
        if (typed !== "") {
            return profileChoice === "default" && VehicleBook.isAdapterUri(typed) && obd2PickedSet !== null
                   ? "mock://?profile=" + obd2PickedSet.profile : typed
        }
        if (profileChoice !== "default")
            return composeUri("mock://", [])
        return pickedSets.length > 0 ? "mock://?profile=" + pickedSets[0].profile : ""
    }

    // True while a typed URI is being read into the choices, so the DID
    // field's change doesn't recompose the URI that set it; and true
    // while the page writes the field from the choices, so that text
    // isn't read back into them
    property bool syncing: false
    property bool composing: false

    function setUri(text) {
        composing = true
        host.uriField.text = text
        composing = false
    }

    function profileHint() {
        for (const p of profiles) {
            if (p.name === profileChoice)
                return p.hint
        }
        return qsTr("The address names profile '%1'; core judges it at connect").arg(profileChoice)
    }

    // A URI as its base and its query's [key, value] pairs, in order
    // (a key with no "=" keeps null; core will name it)
    function splitUri(text) {
        const q = text.indexOf("?")
        const pairs = []
        if (q >= 0) {
            for (const part of text.substring(q + 1).split("&")) {
                if (part === "")
                    continue
                const eq = part.indexOf("=")
                pairs.push(eq < 0 ? [part, null] : [part.substring(0, eq), part.substring(eq + 1)])
            }
        }
        return { base: q < 0 ? text : text.substring(0, q), pairs: pairs }
    }

    function joinUri(base, pairs) {
        if (pairs.length === 0)
            return base
        return base + "?" + pairs.map(p => p[1] === null ? p[0] : p[0] + "=" + p[1]).join("&")
    }

    // The first value of key, or undefined
    function queryValue(pairs, key) {
        for (const p of pairs) {
            if (p[0] === key)
                return p[1]
        }
        return undefined
    }

    // Sets key in place, appends it, or with null removes it
    function setQuery(pairs, key, value) {
        for (let i = 0; i < pairs.length; ++i) {
            if (pairs[i][0] !== key)
                continue
            if (value === null)
                pairs.splice(i, 1)
            else
                pairs[i] = [key, value]
            return
        }
        if (value !== null)
            pairs.push([key, value])
    }

    // The field's text before its query
    function uriBase() {
        return splitUri(host.uriField.text).base
    }

    // base plus the query, with profile= and did= following the choices
    // and every other key kept in place. uds-generic's DIDs go in as
    // typed, minus whitespace; core judges them (ADR-0020) and the error
    // line shows its words.
    function composeUri(base, pairs) {
        if (base === "")
            return ""
        const out = pairs.slice()
        setQuery(out, "profile", profileChoice === "default" ? null : profileChoice)
        const dids = profileChoice === "uds-generic" ? host.didField.text.replace(/\s+/g, "") : ""
        setQuery(out, "did", dids === "" ? null : dids)
        // Core refuses session= under any other profile; extSession keeps the
        // choice for the way back to uds-generic
        if (profileChoice !== "uds-generic")
            setQuery(out, "session", null)
        else if (sessionOffered(base))
            setQuery(out, "session", extSession ? "03" : null)
        return joinUri(base, out)
    }

    // The extended session is requested over an adapter or a CAN interface;
    // core refuses session= on mock:// and replay://
    function sessionOffered(base) {
        const scheme = schemeOf(base)
        return scheme !== "mock" && scheme !== "replay"
    }

    // The query keys each scheme takes, as core lists them
    // (core/src/mock_transport.cpp, replay_transport.cpp,
    // j2534_transport.cpp, elm327_transport.cpp)
    readonly property var schemeKeys: ({
        "mock": ["profile", "did", "diagnose"],
        "replay": ["profile", "did", "speed", "loop"],
        "j2534": ["baud", "flags", "filter", "txid", "poll", "probe", "identify", "diagnose",
                  "pack", "scan", "gap", "session", "keepalive_ms", "profile", "did"],
        "elm327": ["baud", "serial", "filter", "txid", "poll", "probe", "identify", "diagnose",
                   "pack", "scan", "gap", "session", "keepalive_ms", "profile", "did"],
        "socketcan": ["profile", "did", "baud", "filter", "txid", "poll", "probe", "identify",
                      "diagnose", "pack", "scan", "gap", "session", "keepalive_ms"]
    })
    // What a SocketCAN session that only listens takes: Default is raw
    // there, and core refuses it the keys that ask an ECU
    readonly property var socketcanListenKeys: ["profile", "did", "baud", "filter"]

    function schemeOf(text) {
        const i = text.indexOf("://")
        return i < 0 ? "" : text.substring(0, i).trim().toLowerCase()
    }

    // Row click / preset click: selection is INTENT, never action. A row
    // that only fills the URI field can never accidentally cost 8 seconds.
    // The query the field holds comes along, less what another scheme
    // can't use: keys it doesn't take (a replay's speed= on an adapter),
    // and the mock profile on an adapter, where no frame is the mock's
    // and it asks the ECU nothing. Kept, they failed the next connect
    // (docs/bench-sessions/2026-10-28-android-5.md); the profile row
    // shows Default then
    function fillUri(base) {
        let pairs = splitUri(host.uriField.text).pairs
        const scheme = schemeOf(base)
        const keys = schemeKeys[scheme]
        if (keys !== undefined && scheme !== schemeOf(host.uriField.text)) {
            pairs = pairs.filter(p => keys.indexOf(p[0]) >= 0)
            if (profileChoice === "mock" && (scheme === "j2534" || scheme === "elm327"))
                profileChoice = "default"
            // SocketCAN asks an ECU through core's ISO-TP (ADR-0040) under a
            // profile that requests (uds-generic, a file's); one that only
            // listens takes no request key
            if (scheme === "socketcan" && ["default", "raw", "mock"].indexOf(profileChoice) >= 0)
                pairs = pairs.filter(p => socketcanListenKeys.indexOf(p[0]) >= 0)
        }
        setUri(composeUri(base, pairs))
    }

    function chooseProfile(name) {
        const uri = splitUri(host.uriField.text)
        profileChoice = name
        setUri(composeUri(uri.base, uri.pairs))
    }

    function recompose() {
        const uri = splitUri(host.uriField.text)
        setUri(composeUri(uri.base, uri.pairs))
    }

    // A URI the page didn't compose (typed, pasted, or set through UI
    // Automation) sets the choices, never the other way round: the
    // field's text is left as it is. The DID field follows did= whenever
    // it's shown (uds-generic), so it never shows DIDs the URI lacks;
    // under another profile it keeps what was typed for later.
    function readChoices() {
        const pairs = splitUri(host.uriField.text).pairs
        const profile = queryValue(pairs, "profile")
        const did = queryValue(pairs, "did")
        syncing = true
        profileChoice = (profile === undefined || profile === null || profile === "")
                        ? "default" : profile
        if (profileChoice === "uds-generic") {
            host.didField.text = did === undefined || did === null ? "" : did
            const session = queryValue(pairs, "session")
            extSession = session !== undefined && session !== null && Number(session) === 3
        }
        else if (did !== undefined && did !== null)
            host.didField.text = did
        syncing = false
    }
}
