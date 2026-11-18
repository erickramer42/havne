// File/layer: app/qml/SettingsOwn.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.settings_*  (test_qml)

import QtQuick

pragma ComponentBehavior: Bound

Item {
    required property SettingsPage host

    property string formService: "22"
    property string formOrder: "be"
    property string formKind: "number"
    property string formGroup: ""
    property string formProblem: ""
    property string formNote: ""
    property var readResult: null
    // The key of the signal Edit filled the form with, "" for a new one:
    // its Save replaces that signal, renamed or not, and ends the Edit
    property string editingKey: ""
    // The vehicle a Read once was asked for: its reply shows only there
    property string readOnceFor: ""

    // The form and the page's notes are the picked vehicle's: a pick,
    // New, Duplicate, Forget or a car recognized starts them afresh, and a
    // question asked about the last one goes with it
    readonly property string pickedId: host.pickedVehicle !== null ? host.pickedVehicle.id : ""
    onPickedIdChanged: {
        host.confirming = ""
        host.importPart.discardImport()   // a file waiting was read for the last vehicle
        host.openRow = ""
        clearForm()
        dropAnotherVehiclesProfile()
    }

    // A plugin's profile in the field is one vehicle's pick: once another
    // vehicle is picked (or none), the field reads Default, so a connect
    // never decodes with the last vehicle's signals and remembers them as
    // this one's. A vehicle with a connection of its own then sets the
    // field (pickVehicle); a session's URI is left as it is until the
    // session ends, since a VIN may pick another vehicle while it runs
    onIdleChanged: {
        if (idle)
            dropAnotherVehiclesProfile()
    }

    function dropAnotherVehiclesProfile() {
        if (!idle)
            return
        const p = host.profileChoice
        if (!p.startsWith("user.") && !p.startsWith("plugin."))
            return
        if (host.picksPart.picksNow().indexOf(p) < 0)
            host.chooseProfile("default")
    }

    function clearForm() {
        fillForm(SignalForm.emptyFields())
    }

    function readForm() {
        return { name: host.formName.text, rx: host.formRx.text, tx: host.formTx.text, service: formService,
                 number: host.formNumber.text, byte: host.formByte.text, bits: host.formBits.text,
                 bit: host.formBit.text, order: formOrder, signed: host.formSigned.checked,
                 bytes: host.formBytes.text, scale: host.formScale.text, offset: host.formOffset.text,
                 unit: host.formUnit.text, decimals: host.formDecimals.text, group: formGroup,
                 kind: formKind, values: host.formValues.text, expr: host.exprEditor.formExpr.text }
    }

    function fillForm(f) {
        host.formName.text = f.name
        host.formRx.text = f.rx
        host.formTx.text = f.tx
        formService = f.service
        host.formNumber.text = f.number
        host.formByte.text = f.byte
        host.formBits.text = f.bits
        host.formBit.text = f.bit
        formOrder = f.order
        host.formSigned.checked = f.signed
        host.formBytes.text = f.bytes
        host.formScale.text = f.scale
        host.formOffset.text = f.offset
        host.formUnit.text = f.unit
        host.formDecimals.text = f.decimals
        formGroup = f.group
        formKind = f.kind
        host.formValues.text = f.values
        host.exprEditor.formExpr.text = f.expr
        formProblem = ""
        formNote = ""
        readResult = null
        editingKey = ""
    }

    // The adapter Read once asks on: the field's, when it names one, else
    // the vehicle's last
    function readOnceAdapter() {
        const typed = host.uriField.text.trim()
        if (/^(j2534|elm327|socketcan):\/\//i.test(typed))
            return typed
        return host.pickedVehicle !== null ? host.pickedVehicle.uri : ""
    }

    function readOnceDraft() {
        formProblem = ""
        formNote = ""
        readResult = null
        const f = readForm()
        readOnceFor = pickedId
        const why = Bridge.readOnce(readOnceAdapter(), SignalForm.draft(f), SignalForm.moduleOf(f))
        if (why !== "")
            formProblem = why
    }

    function readResultText(r) {
        if (r === null)
            return ""
        if (r.ok)
            return qsTr("Asked %1, got %2: %3").arg(r.request, r.reply, r.value)
        return r.reply !== "" ? qsTr("Asked %1, got %2: %3").arg(r.request, r.reply, r.words)
                              : qsTr("Asked %1: %2").arg(r.request, r.words)
    }

    // The loader's words without a line and column of a file the user
    // never sees
    function formWords(words) {
        return String(words).replace(/^line \d+, column \d+: /, "")
    }

    // doc as the vehicle's own variant: put in its place, or with none
    // yet made a plugin of the user's own, picked for the vehicle. The
    // loader's words when it refuses it. True when saved
    function saveOwn(doc, note) {
        const file = Object.assign({}, doc)
        delete file.notes   // the variant's written header line, not the file's
        const place = host.ownPlace
        const r = Bridge.keepSignalText(JSON.stringify(file, null, 2),
                                        place !== null ? place.plugin.sha256 : "",
                                        place !== null ? place.variant : "", pickedId)
        if (!r.ok) {
            formProblem = formWords(r.words)
            return false
        }
        ++host.picksPart.storeRev
        if (place === null) {
            const profile = r.profiles[0]
            let next = VehicleBook.setOwn(host.vehicles, profile)
            const picked = Bridge.pickVariant(VehicleBook.picked(next).picks, profile)
            if (picked.ok)
                next = VehicleBook.setPicks(next, picked.picks)
            host.saveVehicles(next)
        }
        formProblem = ""
        formNote = note
        return true
    }

    // The name the own plugin is made under: "<label> signals"
    function ownName(label) {
        return qsTr("%1 signals").arg(String(label).trim())
    }

    // The form's signal into the own variant, made with the first; the
    // signal Edit filled the form with replaced under the key it has
    // (SignalForm.keyFor), or a new one's of the same name. An Edit can't
    // take another saved signal's name: the lists tell signals by name
    function saveDraft() {
        formNote = ""
        readResult = null
        if (host.pickedVehicle === null)
            return false
        const f = readForm()
        if (f.kind === "computed") return host.exprEditor.save(f)
        const signal = SignalForm.draft(f)
        const named = host.ownSignals.find(s => s.name === signal.name && s.key !== editingKey)
        if (editingKey !== "" && named !== undefined) {
            formProblem = qsTr("%1 is saved already: give this one another name").arg(named.name)
            return false
        }
        signal.key = SignalForm.keyFor(host.ownSignals, signal.name, editingKey)
        const base = host.ownFile !== null ? host.ownFile
            : SignalForm.emptyFile(host.pickedVehicle.label, ownName(host.pickedVehicle.label))
        const module = SignalForm.moduleOf(f)
        // One request ID to a module: a typed one that differs moves the
        // module's other signals too, and the note names them
        const move = SignalForm.requestIdMove(base, signal, module, editingKey)
        const note = move === null ? qsTr("%1 saved in %2").arg(signal.name, base.name)
            : qsTr("%1 saved in %2; %3 asks on %4 for %5 too")
                  .arg(signal.name, base.name, move.rx, move.tx, move.names.join(", "))
        if (!saveOwn(SignalForm.withSignal(base, signal, module, editingKey), note))
            return false
        editingKey = ""   // saved: the Edit is over
        return true
    }

    function editOwnSignal(key) {
        const s = host.ownSignals.find(x => x.key === key)
        if (s !== undefined) {
            fillForm(SignalForm.fieldsOf(host.ownFile, s))
            editingKey = key
        }
    }

    // A copy of the picked vehicle. Its own signals are copied into a
    // plugin of its own, picked in the place of the original's, so each
    // vehicle's own signals change apart; that plugin not made, the copy
    // starts with none
    function duplicateVehicle() {
        const from = host.pickedVehicle
        const text = host.ownFile === null ? "" : (() => {
            const file = Object.assign({}, host.ownFile)
            delete file.notes
            file.name = ownName(host.copyLabel)
            return JSON.stringify(file, null, 2)
        })()
        let doc = VehicleBook.duplicate(host.vehicles, host.copyLabel)
        if (from !== null && from.own !== "") {
            const id = VehicleBook.picked(doc).id
            const r = text === "" ? { ok: false } : Bridge.keepSignalText(text, "", "", id)
            doc = VehicleBook.setPicks(doc, VehicleBook.picked(doc).picks.map(
                      p => p === from.own ? (r.ok ? r.profiles[0] : "") : p).filter(p => p !== ""))
            if (r.ok)
                doc = VehicleBook.setOwn(doc, r.profiles[0])
            ++host.picksPart.storeRev
        }
        changeVehicles(doc)
    }

    // The last signal's removal takes the variant with it, once asked: it
    // was written here and has no copy elsewhere
    function removeOwnSignal(key) {
        const doc = SignalForm.withoutSignal(host.ownFile, key)
        if (doc.signals.length > 0) {
            saveOwn(doc, qsTr("Removed from %1").arg(doc.name))
            return
        }
        host.confirming = "removeOwn:" + host.ownPlace.plugin.sha256
    }

    // Forget and Forget all: what the store holds as imported for the
    // vehicles goes with them; the plugins stay
    function forgetVehicles(all) {
        const id = pickedId
        changeVehicles(all ? VehicleBook.forgetAll(host.vehicles) : VehicleBook.forget(host.vehicles))
        if (all)
            Bridge.forgetAllVehicleImports()
        else if (id !== "")
            Bridge.forgetVehicleImports(id)
        ++host.picksPart.storeRev
    }

    // Picking fills the field with the vehicle's connection as a list
    // would connect it, read back like typed text so the profile row and
    // the DID field follow
    function pickVehicle(id) {
        host.confirming = ""
        vinPickNote = ""
        host.saveVehicles(VehicleBook.pick(host.vehicles, id))
        if (host.pickedVehicle !== null && host.pickedVehicle.uri !== "")
            host.uriField.text = host.listConnectUri(host.pickedVehicle.uri)
    }

    // A change of the list from the page's buttons: the field empties
    // for the next label
    function changeVehicles(doc) {
        host.confirming = ""
        host.saveVehicles(doc)
        host.vehicleLabelField.text = ""
    }

    // Live's ticks and unticks, the picked vehicle's from now on
    function rememberPlotting() {
        host.saveVehicles(VehicleBook.rememberPlotting(host.vehicles, Bridge.signalModel.plottedKeys(),
                                                  Bridge.signalModel.unplottedKeys()))
    }

    // Said beside the vehicles while the car's VIN, not the user, made
    // the pick (VehicleBook.pickNote); "" once the user picks
    property string vinPickNote: ""

    // The VIN settled which vehicle this is (ADR-0027 section 4)
    function recognizeVehicle() {
        if (!VehicleBook.isAdapterUri(Bridge.currentUri))
            return
        let doc = host.vehicles
        if (doc.salt === "") {
            doc = VehicleBook.copy(doc)
            doc.salt = Bridge.newSalt()
        }
        const r = VehicleBook.recognize(doc, Bridge.vinKey(doc.salt), Bridge.currentUri)
        host.saveVehicles(r.doc)
        vinPickNote = VehicleBook.pickNote(r)
        if (r.outcome === "new" && host.plottedApplied) {
            // This session's ticks are the new vehicle's; what the one
            // picked before kept for other sets stays its own
            Bridge.signalModel.dropCarriedChoices()
            rememberPlotting()
        }
        else if (r.outcome === "switched" && host.plottedApplied)
            Bridge.signalModel.setPlotChoices(host.pickedVehicle.plotted, host.pickedVehicle.unplotted)
        Bridge.logEvent(VehicleBook.logLine(r))
    }

    // No session, and none starting or ending. Every control that chooses
    // or starts a session is enabled by this one value, so none can be
    // used during a multi-second vendor block: the bridge refuses a
    // connect then anyway, but a device row or a preset would otherwise
    // rewrite the grayed field under the connect in flight.
    readonly property bool idle: !Bridge.connected && !Bridge.connecting && !Bridge.disconnecting
}
