// File/layer: app/sessionbridge.h | Qt6 Core, app layer, C++17
// Testing: UNIT: CoreTest (test_core); BRIDGE: BridgeTest (test_bridge);
//          QML: QmlTest.dashboardbook{,_writes,_files}

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QVariantList>
#include <QVariantMap>

#include "readonce.h"
#include "pluginstore.h"
#include "framefilter.h"
#include "recordingimport.h"
#include "signalfilter.h"
#include "alerttone.h"
#include "conditionmodel.h"
#include "conditionstore.h"
#include "consentflow.h"
#include "framelistmodel.h"
#include "hostclock.h"
#include "signallistmodel.h"
#include <QTimer>
#include <QHash>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "ecuio/recording.h"
#include "ecuio/conditions.h"
#include "latencystats.h"
#include "bridgeformats.h"
#include "bridgeseams.h"
#include "bridgestate.h"

class QSettings;

class SessionBridge : public QObject, public BridgeFormats, public BridgeSeams
{
    Q_OBJECT
    Q_PROPERTY(QString coreVersion READ coreVersion CONSTANT)
    // The age past which a value is stale (ECUIO_VALUE_STALE_US), in ms:
    // Theme.staleMs, so the UI dims what core's computed signals refuse
    Q_PROPERTY(int valueStaleMs READ valueStaleMs CONSTANT)
    // A layout file's byte bound (layoutFileText), which DashboardBook's
    // maxBytes + fileKeysBytes must equal
    Q_PROPERTY(int layoutFileMaxBytes READ layoutFileBound CONSTANT)
    // A Read once is running (readOnce); no session connects meanwhile
    Q_PROPERTY(bool readingOnce READ readingOnce NOTIFY readingOnceChanged)
    // The signals the Dashboard's gauges read: each gauge's key ("rpm",
    // "speed", ...) to the refs (RefRole) of the defs core's sets give
    // it ("id:<n>", signal_sets.h), the first one the session's set has
    // winning. By ID, so a def core names otherwise keeps its gauge
    Q_PROPERTY(QVariantMap gaugeSignals READ gaugeSignals CONSTANT)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY connectingChanged)
    Q_PROPERTY(bool disconnecting READ disconnecting NOTIFY disconnectingChanged)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY frameCountChanged)
    Q_PROPERTY(int droppedFrames READ droppedFrames NOTIFY droppedFramesChanged)
    // This session's frames that came after a loss at the adapter
    // (ECUIO_FRAME_FLAG_OVERFLOW: its buffer overflowed, frames were lost)
    Q_PROPERTY(int overflowFrames READ overflowFrames NOTIFY overflowFramesChanged)
    Q_PROPERTY(FrameListModel* model READ model CONSTANT)
    // The Log page's By ID view over model's frames
    Q_PROPERTY(FrameIdModel* frameIds READ frameIds CONSTANT)
    // The Recordings page's Import: another logger's file made a recording
    // in recordingsRoot()
    Q_PROPERTY(RecordingImport* recordingImport READ recordingImport CONSTANT)
    Q_PROPERTY(int guiNotifyCount READ guiNotifyCount NOTIFY notifyCountChanged)
    Q_PROPERTY(int signalBatchCount READ signalBatchCount NOTIFY signalBatchChanged)
    Q_PROPERTY(SignalListModel* signalModel READ signalModel CONSTANT)
    // signalModel's rows Live lists: those an ECU hasn't marked unsupported
    Q_PROPERTY(LiveSignalModel* liveSignalModel READ liveSignalModel CONSTANT)
    Q_PROPERTY(QString currentUri READ currentUri NOTIFY currentUriChanged)
    // currentUri as the header pill and the Dashboard show it (uriLabelFor)
    Q_PROPERTY(QString uriLabel READ uriLabel NOTIFY currentUriChanged)
    // The connected adapter's versions (versionsText), "" with none read
    // or no session; Settings keeps the last seen for each device
    Q_PROPERTY(QString adapterVersions READ adapterVersions NOTIFY currentUriChanged)
    Q_PROPERTY(QString linkState READ linkState NOTIFY linkStateChanged)
    Q_PROPERTY(int linkErrorCode READ linkErrorCode NOTIFY linkStateChanged)
    Q_PROPERTY(int errorEpisodes READ errorEpisodes NOTIFY linkStateChanged)
    // Log page Pause: frames still count and their values still arrive,
    // but none are added to the frame log. Every session starts unpaused.
    Q_PROPERTY(bool logPaused READ logPaused WRITE setLogPaused NOTIFY logPausedChanged)
    // Keep-awake (ADR-0033's appendix), on by default: the Settings switch.
    // A desktop's connect holds the host from sleeping on its idle timer
    // for a live session; cleared, it releases at once. Set while
    // connected, or after a connect's take passed it by, it holds from the
    // next connect. On Android AndroidSession gates the service's wake
    // lock on it instead.
    Q_PROPERTY(bool keepAwake READ keepAwake WRITE setKeepAwake NOTIFY keepAwakeChanged)
    // Session recording (ADR-0017), off by default: the Settings switch.
    // It takes effect at the next connect and can't change while a
    // session is up or in flight, so a recording always covers a whole
    // session. Core writes it (ADR-0049): the bridge opens the recorder,
    // attaches it before the session starts and notes its activity lines.
    Q_PROPERTY(bool recording READ recording WRITE setRecording NOTIFY recordingChanged)
    // The current or last recording, for display: relative to Documents
    // when it's there, so no account name shows. Empty before the first.
    Q_PROPERTY(QString recordingFolder READ recordingFolder NOTIFY recordingStateChanged)
    Q_PROPERTY(QUrl recordingFolderUrl READ recordingFolderUrl NOTIFY recordingStateChanged)
    Q_PROPERTY(bool recordingActive READ recordingActive NOTIFY recordingStateChanged)
    // The marks put in the recording under way, or the last one
    Q_PROPERTY(int markCount READ markCount NOTIFY markCountChanged)
    // The analysis log (ADR-0056 section 1): Start log records the running
    // session from that moment, its requests narrowed to the rows Live
    // plots then (the focus), and Stop log closes it. It takes the
    // recorder slot Record sessions uses (recordingActive holds while it
    // runs): one recorder a session.
    Q_PROPERTY(bool logActive READ logActive NOTIFY logStateChanged)
    // A replay without loop has played its last frame and its values have
    // reached the model: a row with no value by then is one the
    // recording doesn't hold ("not in this recording"), not one to wait for
    Q_PROPERTY(bool replayFinished READ replayFinished NOTIFY replayFinishedChanged)
    // The log's time so far and its folder's size in bytes, counted once
    // a second; the last log's once it ends
    Q_PROPERTY(qint64 logElapsedMs READ logElapsedMs NOTIFY logProgressChanged)
    Q_PROPERTY(qint64 logBytes READ logBytes NOTIFY logProgressChanged)
    // What the Dashboard's header says about the log: "Log not started:
    // <why>" after a refusal, "Log ended: <why>" after an end the user
    // didn't ask for (a failed write), "Log ended" after the user's
    // Disconnect, "" otherwise (Stop log). A log that starts clears it, as
    // does clearLogNotice (Dismiss). A property, not a signal: Start log
    // sits in Main's header, on every page, so the Dashboard may not exist
    // when the refusal lands
    Q_PROPERTY(QString logNotice READ logNotice NOTIFY logNoticeChanged)
    // The armed recorder (ADR-0063 section 7): Armed holds the one recorder
    // slot as a log does (recordingActive holds), and writes nothing until
    // a condition that records fires. armedRecording: a window is writing
    // (the only time a Mark goes anywhere); armedWindows: the windows
    // opened so far; armedFolder: the newest window's folder name, "" before
    // the first
    Q_PROPERTY(bool armedActive READ armedActive NOTIFY armedStateChanged)
    Q_PROPERTY(bool armedRecording READ armedRecording NOTIFY armedStateChanged)
    Q_PROPERTY(int armedWindows READ armedWindows NOTIFY armedStateChanged)
    Q_PROPERTY(QString armedFolder READ armedFolder NOTIFY armedStateChanged)
    // Why Armed can't start now, "" when it can (and while it runs): not
    // connected, a log or Record sessions holds the recorder, a replay,
    // or no enabled condition records
    Q_PROPERTY(QString armedWhy READ armedWhy NOTIFY armedWhyChanged)
    // The picked vehicle's label (ADR-0027 section 2), "" for none: an adapter
    // session's recording folder is named "<stamp> <label>". Settings
    // sets it before each connect.
    Q_PROPERTY(QString vehicleLabel READ vehicleLabel WRITE setVehicleLabel NOTIFY vehicleLabelChanged)
    // Display units (ADR-0022): ECUIO_UNITS_METRIC (0) or _IMPERIAL (1).
    // Applies to what the UI shows at once, connected or not; recordings
    // keep the decoded values. Main.qml persists it.
    Q_PROPERTY(int unitSystem READ unitSystem WRITE setUnitSystem NOTIFY unitSystemChanged)
    // The minimum time between sweeps (ADR-0056 section 4) of a log and of
    // the default pick, ms, 0 off; Settings' Advanced page and Main.qml,
    // which keeps it
    Q_PROPERTY(int minSweepMs READ minSweepMs WRITE setMinSweepMs NOTIFY
                   minSweepMsChanged)
    Q_PROPERTY(QVariantList minSweepChoices READ minSweepChoices CONSTANT)
    // The Dashboard's cells for the default pick: one list of refs per cell
    // (DashboardPage's lookupRefs, a blank cell's empty), every page of the
    // form. Main sets it, connected or not, so a start's pick has them.
    Q_PROPERTY(QVariantList pickGauges READ pickGauges WRITE setPickGauges
                   NOTIFY pickGaugesChanged)
    // What each channel's ECU has reported of its diagnostics (ADR-0037),
    // one map per ECU (diagnosticsEntry), for the Diagnostics page:
    // refreshed on the tick as a read settles each item, and when the
    // unit system changes; emptied at connect, kept after disconnect
    Q_PROPERTY(QVariantList diagnostics READ diagnostics NOTIFY diagnosticsChanged)
    // The ECUs a clear goes to (ADR-0038), as "0x7E8": each whose shown
    // report is a clear target (clearTarget), the rule clearDiagnostics
    // sends by, so the confirmation names exactly them
    Q_PROPERTY(QStringList clearTargets READ clearTargets NOTIFY diagnosticsChanged)
    // True when every ECU a clear goes to said it holds nothing a clear
    // erases (nothingStored): the confirmation then says a clear would
    // only set the monitors back and reset what the manufacturer chose
    Q_PROPERTY(bool clearFindsNothing READ clearFindsNothing NOTIFY diagnosticsChanged)
    // The car's readiness against the federal inspection rule (ADR-0045):
    // every shown report judged together, for the model year of the VIN,
    // as inspectionEntry gives it
    Q_PROPERTY(QVariantMap inspection READ inspection NOTIFY diagnosticsChanged)
    // The ECUs whose clear stopped at the vehicle speed (ADR-0038's
    // appendix of 2026-11-01), moving or giving none, as "0x7E8", once
    // every clear asked has ended; the page asks once more, naming
    // clearMovingSpeed ("48 km/h" in the unit system, "" when no ECU gave
    // one), and clearDiagnosticsMoving sends to exactly them
    Q_PROPERTY(QStringList clearMoving READ clearMoving NOTIFY clearMovingChanged)
    Q_PROPERTY(QString clearMovingSpeed READ clearMovingSpeed NOTIFY clearMovingChanged)
    // The refusals past an ECU's report list, whose signals Live can't name
    // and so leaves blank (cli/refused_signals.h's refused_unlisted):
    // "ECU 0x7E8 refused 3 more requests", "" when every refusal is named
    Q_PROPERTY(
        QString refusedUnnamed READ refusedUnnamed NOTIFY refusedUnnamedChanged)
    // The conditions (ADR-0063 section 9): the set the app keeps
    // (ConditionStore) or, on a replay, the recording's own copy, one row
    // per condition, judged while a session runs
    Q_PROPERTY(ConditionModel* conditionModel READ conditionModel CONSTANT)
    // A plugin's consent and its unlocks (ADR-0066, consentflow.h)
    Q_PROPERTY(ConsentFlow* consent READ consent CONSTANT)
    // Why a saved edit isn't judged yet ("applies when this recording
    // ends"), "" when the set judged is the one saved
    Q_PROPERTY(QString conditionsPending READ conditionsPending NOTIFY
                   conditionsChanged)
    // Why the saved file wasn't read at startup, "" when it was or there's none
    Q_PROPERTY(QString conditionsProblem READ conditionsProblem NOTIFY
                   conditionsChanged)
    // A replay judging its recording's own conditions.json, not the saved set
    Q_PROPERTY(bool conditionsFromRecording READ conditionsFromRecording NOTIFY
                   conditionsChanged)
    // For each def id (as text) an alert ON reads, the highest level
    // (ECUIO_ALERT_WARNING 1, _DANGER 2); a def with none is absent
    Q_PROPERTY(
        QVariantMap alertLevels READ alertLevels NOTIFY alertLevelsChanged)

public:
    explicit SessionBridge(QObject* parent = nullptr);
    ~SessionBridge() override;

    QString coreVersion() const;
    static int valueStaleMs()
    {
        return static_cast<int>(ECUIO_VALUE_STALE_US / 1000u);
    }
    bool readingOnce() const { return m_readOnce.busy(); }
    // Read once (ADR-0051's form, ReadOnce): signal and module as a
    // signal file holds them, read on adapterUri's adapter in a short
    // session of its own, only while no session is connected. Returns
    // "" when the read started, its result coming in readOnceResult
    // (ReadOnce::evaluate's map, plus "uri"); else why it didn't
    Q_INVOKABLE QString readOnce(const QString& adapterUri,
                                 const QVariantMap& signal,
                                 const QVariantMap& module);
    // A dashboard layout file (ADR-0057's appendix on the layout file): the
    // text DashboardBook.exportText made, written where the user chose (as
    // exportPlugin writes), and a picked file's text back. { ok, words }
    // and { ok, words, text }, words never naming a path. The size is
    // bounded at layoutFileMaxBytes either way, a text that isn't UTF-8 is
    // refused unread, and a leading UTF-8 BOM is skipped. A local file is
    // written whole or not at all and is never removed; a provider's document
    // is written in place (it can't be atomic) and a refused or failed save
    // removes it only when healthreport.h's pickedDocumentRemovable says so.
    // Each logs one line
    Q_INVOKABLE QVariantMap saveLayoutFile(const QString& text, const QUrl& url);
    // A save a page refuses before it calls the bridge: the document its pick
    // made goes by the same rule
    Q_INVOKABLE void dropPicked(const QUrl& url);
    Q_INVOKABLE QVariantMap layoutFileText(const QUrl& url);
    // A Dashboard preset's layout file from the app's resources
    // (app/layouts/<form>/<name>.json), "" for a name or form that isn't one
    // of the six: XMLHttpRequest refuses a resource unless file reads are on
    Q_INVOKABLE QString presetLayoutText(const QString& name,
                                         const QString& form) const;
    static constexpr qint64 layoutFileMaxBytes = 66016 + 16384;
    static int layoutFileBound()
    {
        return static_cast<int>(layoutFileMaxBytes);
    }
    // The Log page's Export CSV (FrameListModel::scrubbedCsv) to the file
    // at url (a local file, or Android's content:// by the content resolver): { ok,
    // words, written, leftOut, unattributed }, leftOut the identification
    // replies', text signals' and VIN-shaped messages' frames, words never
    // naming the path. A
    // failed check writes nothing. Logs one line
    Q_INVOKABLE QVariantMap exportFrames(const QUrl& url);
    // Diagnostics' Save report (ADR-0054's appendix "the health report as
    // built"): the session's health to the file at url (a local file, or
    // Android's content:// by the content resolver), in the format healthFormatFor
    // picks from it and the dialog's filterIndex, with the values Live's
    // rows hold. No wait: a report still being read says so itself.
    // baselineFolder, the picked vehicle's baseline recording ("" none),
    // which the report names by its start, never its folder. The map
    // writeHealthReport gives (healthreport.h), its words never naming a
    // path. Logs one line
    Q_INVOKABLE QVariantMap
    saveHealthReport(const QUrl& url, int filterIndex,
                     const QString& baselineFolder = QString());
    // An OBDb set imported (ADR-0051's appendix of 2026-11-06,
    // ObdbImport): preview reads the file at url as readImport does and
    // converts it for modelYear (0: none), named name, giving
    // ObdbImport::report's map; the converted text is held as the one
    // pending import, nothing kept, until keepObdbPlugin keeps it. drop
    // forgets the pending import. Preview logs one line, never naming
    // the path
    Q_INVOKABLE QVariantMap previewObdb(const QUrl& url, int modelYear,
                                        const QString& name);
    Q_INVOKABLE void dropObdb() { m_obdbPending.clear(); }
    // The model year of the typed VIN, or with "" the session's (which
    // stays here, ADR-0027 section 3), by ADR-0043's decode; 0 for none
    Q_INVOKABLE int vinModelYear(const QString& typedVin) const;
    // The app's plugins (ADR-0052, PluginStore): every kept one loaded
    // and its variants registered at the first call, then the list, one
    // entry a plugin { sha256, ok, words, id, name, version, variants,
    // profiles }. Import reads a packed plugin (<id>.havne-plugin.json)
    // at url (a local file, or Android's content:// through QFile) and
    // keeps a copy when core loads it (the entry, kept, replaced: the
    // version it replaced, or ""); Remove frees it and deletes its
    // copy. Both refuse, with words, an id a session decodes with. Each
    // logs one line.
    Q_INVOKABLE QVariantList plugins();
    Q_INVOKABLE QVariantMap importPlugin(const QUrl& url);
    Q_INVOKABLE QVariantMap removePlugin(const QString& sha);
    // The variants the plugins propose for a car (ADR-0052 section 5):
    // the VIN typed, or with "" the session's, which stays here (ADR-0027
    // section 3); { sha256, id, name, variant, profile } each, none
    // without a VIN. A proposal only: the user picks
    Q_INVOKABLE QVariantList pluginMatches(const QString& typedVin) const;
    // One store (ADR-0064): every import ends in the plugin store.
    // readImport reads the file at url as importPlugin does, once, and
    // tells its kind by its first key: { ok, words, kind }; a byte-order
    // mark before the text is passed by. A package ("plugin") goes in at
    // once, importPlugin's result with it (updated: the version of the
    // user's own plugin it updated in place; replacedProfiles: the
    // profiles of the version it took the place of). A signal file
    // ("signals") is held as the one pending import, nothing kept: name,
    // id (the plugin id a wrap gives it) and variant (the id a variant
    // made from it takes) say what it would be. An OBDb set ("obdb") is
    // held too, for previewObdbImport, whose report comes first: it
    // converts the held bytes as previewObdb converts a file's, as often
    // as the year or the name is changed. Words never name the path.
    // Each logs one line. only, when given ("signals", Replace's read),
    // refuses a file of any other kind with nothing kept or held
    Q_INVOKABLE QVariantMap readImport(const QUrl& url,
                                       const QString& only = QString());
    Q_INVOKABLE QVariantMap previewObdbImport(int modelYear,
                                              const QString& name);
    // The pending signal file kept: as a new plugin of the user's own
    // (into ""), imported for vehicle (the vehicle's id, "" for none),
    // or as the variant named of the user's own plugin with the hash
    // into, added or put in that variant's place. The plugin's entry
    // (plugins()), plus kept, replaced and, for a variant, notes (core's
    // words when an ECU of the file's took another id). A file that made
    // a plugin here already answers with that plugin (kept), and with
    // words when the plugin has changed since; words on a kept result
    // are for the user, and reach the log. A refusal leaves the file
    // pending, so a changed variant id or a disconnect can be followed
    // by Keep again. keepSignalText is the same for a file given as text
    // (the form's), with nothing pending. Each logs one line
    Q_INVOKABLE QVariantMap keepSignals(const QString& into,
                                        const QString& variant,
                                        const QString& vehicle);
    Q_INVOKABLE QVariantMap keepSignalText(const QString& text,
                                           const QString& into,
                                           const QString& variant,
                                           const QString& vehicle);
    // Everything an import holds forgotten: the file read and an OBDb
    // set converted from it
    Q_INVOKABLE void dropImport()
    {
        m_importPending.clear();
        m_importKind.clear();
        m_obdbPending.clear();
    }
    // The pending converted set (previewObdbImport's or previewObdb's)
    // kept as a plugin of the user's own, imported for vehicle: its
    // variant is my<year> when the model year was typed (yearTyped) and
    // base when it was decoded from a VIN or there is none, so no
    // variant id carries a VIN's year. It goes to that vehicle's earlier
    // conversion of the same name: in its place (replaced, with
    // replacedProfiles for a pick to follow) while that plugin has one
    // variant, as a variant of it (merged) once it has more. A refusal
    // leaves the set pending. Logs one line
    Q_INVOKABLE QVariantMap keepObdbPlugin(const QString& vehicle,
                                           bool yearTyped);
    // A variant removed from a plugin of the user's own (never its last),
    // and a plugin marked as the user's own; each logs one line
    Q_INVOKABLE QVariantMap dropPluginVariant(const QString& sha,
                                              const QString& variant);
    Q_INVOKABLE QVariantMap markPluginMine(const QString& sha);
    // A vehicle forgotten (its id), or every vehicle: what the store
    // holds as imported for it is released, so a vehicle later given
    // that id starts with no imports of its own. The plugins stay.
    // { ok, words, count }: count the plugins released. Logs one line
    // when any was
    Q_INVOKABLE QVariantMap forgetVehicleImports(const QString& vehicle);
    Q_INVOKABLE QVariantMap forgetAllVehicleImports();
    // ADR-0064 Q6 (storemigration.h), prefs the app's preferences: the
    // earlier version's data removed once an earlier start moved it, then
    // the move, once. The lines for the activity log; called before QML
    // loads, so the caller logs them when the Log page listens
    QStringList migrateStores(QSettings& prefs);
    // A signal file the move left behind (the vehicle book's unmigrated
    // record, by its hash) saved where the user chose, as saveVariantFile
    // writes: its bytes as the move kept them, else the copy still in the
    // earlier version's folder. { ok, words }, words never naming a path.
    // Logs one line. Remove deletes the kept bytes alone, never the
    // earlier folder's copy: true when none is left
    Q_INVOKABLE QVariantMap saveUnmigratedFile(const QString& sha,
                                               const QUrl& url);
    Q_INVOKABLE bool removeUnmigratedFile(const QString& sha);
    // Export plugin: the store's packed copy written to url byte for
    // byte (a local file, or Android's content:// through QFile), so the
    // file loads wherever the copy does and holds nothing of the store's
    // index. { ok, words }, words never naming a path. Logs one line
    Q_INVOKABLE QVariantMap exportPlugin(const QString& sha, const QUrl& url);
    // The note Export plugin shows before its save dialog: the store's
    // exportNote, { ok, words, note, shareAlike }
    Q_INVOKABLE QVariantMap pluginExportNote(const QString& sha);
    // A variant's signal-file text, which the form edits: { ok, words,
    // text }
    Q_INVOKABLE QVariantMap pluginVariantText(const QString& sha,
                                              const QString& variant);
    // Save as a signal file: the variant's signal file written to url as
    // exportPlugin writes. { ok, words }, words never naming a path. Logs
    // one line
    Q_INVOKABLE QVariantMap saveVariantFile(const QString& sha,
                                            const QString& variant,
                                            const QUrl& url);
    // Copy to my plugin: the store's copyVariant, into "" a new plugin.
    // Logs one line, as a keep does
    Q_INVOKABLE QVariantMap copyVariant(const QString& sha,
                                        const QString& variant,
                                        const QString& into,
                                        const QString& newVariant);
    // A vehicle's picks (ADR-0064 Q5), kept by the vehicle book as an
    // ordered list of profiles. offeredVariants lists every variant of
    // every plugin for the vehicle (PluginStore::offered: forThisCar
    // first, picked marked), the VIN typed or with "" the session's;
    // pickRow gives the picks as the profile row shows them (missing,
    // and on an adapter the one that stands in for Default); pickVariant
    // adds a profile last ({ ok, words, picks }) and unpickVariant
    // takes one out
    Q_INVOKABLE QVariantList offeredVariants(const QString& vehicle,
                                             const QString& typedVin,
                                             const QStringList& picks);
    Q_INVOKABLE QVariantList pickRow(const QStringList& picks, bool adapter);
    Q_INVOKABLE QVariantMap pickVariant(const QStringList& picks,
                                        const QString& profile);
    Q_INVOKABLE QStringList unpickVariant(const QStringList& picks,
                                          const QString& profile);
    // The picks the store has as a layout file names what it needs
    // (PluginStore::pickFiles): { profile, name, sha256, from }
    Q_INVOKABLE QVariantList pickFiles(const QStringList& picks);
    // pages: [{ name, cells }], the Dashboard's, cells as setPickGauges takes them
    Q_INVOKABLE QVariantMap roomHolders(const QVariantList& pages) const;
    // The registered profile's merged file's text, "" for any other profile
    Q_INVOKABLE QString vehicleSetText(const QString& profile) const;
    Q_INVOKABLE QVariantMap vehicleSet(const QStringList& picks,
                                       const QString& own,
                                       const QString& label = QString());
    // A page's text from the loaded plugin with the id, by its entry's
    // pages index (PluginStore::pageText): "" for no such plugin or page
    Q_INVOKABLE QString pluginPageText(const QString& id, int index);
    // The profiles a recording's frames can decode through in the viewer
    // (RecordingView::openFrames; ADR-0049's appendix of 2026-11-06): the
    // built-in obd2 and mock, each signal file the vehicles' book has
    // registered, then each loaded plugin's variants; { profile, name }
    // each, name the file's or "<plugin> <variant>", "" for a built-in
    Q_INVOKABLE QVariantList decodeProfiles();
    QVariantMap gaugeSignals() const;
    // The signals a session on uri would show, without connecting: the
    // profile the URI selects (profileOfUri), its registered set, and one
    // map per def as the model's rows give it: { id, ref, name, unit,
    // decimals, isText, isEnum, group, rangeLow, rangeHigh }, unit,
    // decimals and range in the current unit system, the range NaN for
    // none (RangeLowRole); and decodedUnit, decodedLow, decodedHigh, the
    // def's own unit and range as core decodes them in any unit system
    // (what a dashboard cell stores, ADR-0057 section 6), NaN for none;
    // and gauge, the signal file's own gauge for the def in that same unit
    // ({ min, max, warnLow, warnHigh }, the levels only when the file gives
    // them), {} when there is none.
    // [] when profileOfUri gives "" (a scheme or profile= core refuses),
    // for an unknown profile, and for a profile whose set is a session's
    // own (uds-generic) or none (raw); a URI core refuses for another
    // reason (an unknown key, too many segments) still gives its profile's
    // rows. A replay's own signals.json is registered only when it
    // connects, so its profile gives [] before then and its rows after. A
    // snapshot: call it again on unitSystemChanged, a signal file's change
    // and a connect.
    Q_INVOKABLE QVariantList profileSignals(const QString& uri) const;
    // The signal file whose set set is: a plugin variant's or a replay's
    // own signals.json; nullptr for any other set
    const ecuio_signal_file_t* fileOfSet(const ecuio_signal_set_t* set) const;
    // That file's text, copied; "" for any other set. What the viewer's
    // frames open loads its own copy from (RecordingFrameSet)
    QByteArray signalFileTextOfSet(const ecuio_signal_set_t* set) const;
    bool connected() const { return m_connected.load(std::memory_order_acquire); }
    int frameCount() const { return m_currentFrameCount; }
    int droppedFrames() const { return m_droppedFrames.load(std::memory_order_relaxed); }
    int overflowFrames() const { return m_reportedOverflow; }
    FrameListModel* model() const { return m_frameLog; }
    FrameIdModel* frameIds() const { return m_frameIds; }
    RecordingImport* recordingImport() const { return m_recordingImport; }
    int guiNotifyCount() const { return m_guiNotifyCount; }
    int signalBatchCount() const { return m_signalBatchCount; }
    SignalListModel* signalModel() const { return m_signalModel; }
    LiveSignalModel* liveSignalModel() const { return m_liveSignalModel; }
    QString currentUri() const { return m_currentUri; }
    QString uriLabel() const { return uriLabelFor(m_currentUri); }
    QString adapterVersions() const { return m_adapterVersions; }
    bool connecting() const { return m_connecting; }
    bool disconnecting() const { return m_disconnecting; }
    void dispatchFrameForTesting(const ecuio_frame_t& frame);
    // onValues on the calling thread, as core's worker calls it
    void dispatchValuesForTesting(const ecuio_value_batch_t& batch);
    // onConditions on the calling thread, as core's worker calls it
    void dispatchConditionsForTesting(const ecuio_condition_event_t* events,
                                      uint32_t count);

    ConditionModel* conditionModel() const { return m_conditionModel; }
    ConsentFlow* consent() const { return m_consent; }
    QString conditionsPending() const { return m_conditionsPending; }
    QString conditionsProblem() const { return m_conditionsProblem; }
    bool conditionsFromRecording() const { return m_conditionsFromRecording; }
    QVariantMap alertLevels() const { return m_alertLevels; }
    // The saved condition file's text, "" with none: what the Alerts page
    // edits and saves again; while conditionsProblem says core refused the
    // file, the refused file's text
    Q_INVOKABLE QString conditionsText() const;
    // A condition file in place of the saved one: ConditionStore::save's
    // map plus "pending" (conditionsPending's words). Kept, it is judged at
    // once by a session with no recorder; a recording holds its set
    // (ADR-0063 section 6), so with one the edit waits for its end, and
    // a replay judging its recording's copy keeps it to the end
    Q_INVOKABLE QVariantMap saveConditions(const QString& text);
    // A condition file the user picked, in place of the saved one (the
    // Alerts page's Import): read as layoutFileText reads, within
    // ECUIO_CONDITION_FILE_MAX_BYTES, then saveConditions; its map, words
    // naming no path. A file core refuses leaves the saved one as it was
    Q_INVOKABLE QVariantMap importConditions(const QUrl& url);
    // The saved file written where the user chose, by saveLayoutFile's
    // rules: { ok, words }; a file core refused, as it is. Logs one line
    Q_INVOKABLE QVariantMap exportConditions(const QUrl& url);
    // The saved file read again as at the start, for a test that writes it
    // with no session
    void reloadSavedConditionsForTesting();
    // alertLevels' level for one def, 0 for none
    Q_INVOKABLE int alertLevelOf(int defId) const;
    static constexpr int defaultValueSampleLimit() { return DefaultValueSampleLimit; }
    QString linkState() const;
    /* A direct-load DLL or replay path cut to its last name, a Bluetooth address to its last two octets */
    Q_INVOKABLE static QString uriLabelFor(const QString& uri);

    // A session's delivery so far: what the worker handed the bridge,
    // what the in-flight cap dropped, and how long each frame took to
    // reach the GUI thread (onFrame's stamp to handleFrameInternal) and
    // each value to reach the model (onValues' stamp to the tick that
    // drains it). Zeroed at each session's start; GUI thread only.
    struct DeliveryStats {
        quint64 frames = 0;          // every frame onFrame accepted, dropped ones too
        quint64 dropped = 0;         // at the in-flight cap
        quint64 queuedAtEnd = 0;     // still queued when the session ended
        quint64 values = 0;          // every value onValues staged
        quint64 valuesMissed = 0;    // past the staging's limits: not timed or recorded
        LatencyStats frameToGui;
        LatencyStats valueToModel;
    };
    DeliveryStats deliveryStats() const;
    static QString deliveryText(const DeliveryStats& stats);  /* the activity log's "Delivery:" line */

    Q_INVOKABLE QString statusString(int statusCode) const;
    Q_INVOKABLE int maxFrameCount() const { return FrameListModel::MaxFrames; }
    Q_INVOKABLE void forceFrameCountForTesting(int count);
    // Test seam: the VIN a session's identification would read, and its
    // vehicleIdentified, for the QML tests (the fakes' VIN is no real one)
    Q_INVOKABLE void identifyForTesting(const QString& vin)
    {
        m_vin = vin;
        emit vehicleIdentified();
    }

    // J2534 device discovery (Windows; empty elsewhere): one map per
    // installed Pass-Thru driver, { vendorName, dllPath, architecture } as
    // QStrings, at most ECUIO_J2534_MAX_VENDORS (core's bound). A registry
    // read with no session and no other I/O, so safe to call synchronously
    // from QML. dllPath is a display value (rule 2), the user's route to a
    // direct-load URI, not for parsing.
    Q_INVOKABLE QVariantList discoverJ2534Devices() const;
    // The machine's serial ports, for elm327:// (ADR-0026 section 7): one map
    // per port, { name, device } as QStrings, in core's order. name is
    // what follows elm327://; device is the driver's device on Windows
    // ("\Device\BthModem0"), "" elsewhere. A registry read or a /dev
    // listing; no port is opened, so it's safe to call from QML.
    Q_INVOKABLE QVariantList discoverSerialPorts() const;
    // The machine's CAN interfaces, for socketcan:// (ADR-0032 section 7), Linux
    // only: one map per interface, { name, kind, up, listenOnly, fd,
    // bitrate, state, modes, modesKnown, modesSupported }, kind "can" or
    // "vcan", bitrate in bit/s (0 for vcan), state and the modes in ip
    // link's words (core's; state "" when none is reported), the driver's
    // modes only with modesKnown (a kernel from 6.0). A netlink read;
    // nothing is opened. Empty elsewhere.
    Q_INVOKABLE QVariantList discoverCanInterfaces() const;
    // The newest recordings first, at most limit, for the Recordings page
    // to open or replay by a tap: one map per folder holding a signals.csv
    // or a frames.csv (what the viewer opens; a CSV import holds values
    // alone), { title, uri, folder, source, profile } as QStrings and
    // frames, true when it holds a frames.csv (what Replay plays). title is
    // the folder's date and label ("2026-10-28 14:30:12 · Daily driver"),
    // uri its replay:// URI (replayUriFor), folder its path, source the
    // recorded URI as the pill shows it (uriLabelFor), "" when events.log
    // doesn't name one. Reads the earlier product name's folder too, when
    // it's another (ADR-0001's appendix of 2026-10-28), newest first across
    // both. A directory listing and one short read per folder.
    Q_INVOKABLE QVariantList recentRecordings(int limit) const;
    // One recording's row, as recentRecordings gives it, for a folder
    // that holds a signals.csv or a frames.csv (what the viewer opens);
    // an empty map when it's gone. folder comes back as given, so it
    // matches the path it was stored as (a vehicle's baseline).
    Q_INVOKABLE QVariantMap recordingRow(const QString& folder) const;
    int linkErrorCode() const { return m_linkErrorCode; }
    int errorEpisodes() const { return static_cast<int>(m_errorEpisodes); }
    bool logPaused() const { return m_logPaused; }
    void setLogPaused(bool paused);
    bool keepAwake() const { return m_keepAwake.load(); }
    void setKeepAwake(bool on);
    bool recording() const { return m_recording; }
    void setRecording(bool on);
    QString recordingFolder() const;
    QUrl recordingFolderUrl() const;
    bool recordingActive() const { return m_recorder != nullptr; }
    int markCount() const { return m_markCount; }
    // A marker in the recording under way (ADR-0049 section 4: the app's
    // Mark, as a line typed into havne-cli stream --record): label, or
    // "Mark N" for an empty one, N counting this recording's marks. False
    // with no recording, or one core refused it (a write failed).
    Q_INVOKABLE bool mark(const QString& label = QString());
    bool logActive() const { return m_logActive; }
    bool replayFinished() const { return m_replayFinished; }
    qint64 logElapsedMs() const;
    qint64 logBytes() const { return m_logBytes; }
    QString logNotice() const { return m_logNotice; }
    Q_INVOKABLE void clearLogNotice() { setLogNotice(QString()); }
    // Start log: the plotted rows' ids (SignalListModel::plottedIds) as
    // the session's focus, then a recorder opened as Record sessions'
    // (the same root, label and caller lines) and attached to the running
    // session. False, with the reason in the activity log and nothing
    // opened, while not connected, on a replay, while a recorder is live,
    // with no row plotted, or when core refuses the focus. Ticks on Live
    // while it runs change the chart, never the focus.
    Q_INVOKABLE bool startLog();
    // Stop log: the recording closed, the whole set asked again, then
    // logStopped; nothing with no log running, or while disconnecting
    // (the teardown's end closes it)
    Q_INVOKABLE void stopLog();
    bool armedActive() const { return m_armedActive; }
    bool armedRecording() const { return m_armedInWindow; }
    int armedWindows() const { return m_armedWindows; }
    QString armedFolder() const { return m_armedFolder; }
    QString armedWhy() const { return m_armedWhy; }
    // Start armed: a recorder opened with ECUIO_RECORD_ARMED through the
    // log's openRecorder and attached to the running session, which
    // records no focus: the whole session is the pre-roll. False, with the
    // reason in the activity log and the Dashboard's notice and nothing
    // opened, for any armedWhy, or when core refuses the attach (its detail)
    Q_INVOKABLE bool startArmed();
    // Stop armed: the recorder closed, a window under way ended with it;
    // nothing while not armed or disconnecting (the teardown's end closes it)
    Q_INVOKABLE void stopArmed();
    QString vehicleLabel() const { return m_vehicleLabel; }
    void setVehicleLabel(const QString& label);
    // The VIN (ADR-0027 section 3) stays in the bridge: QML gets a salted
    // SHA-256 of it, as lowercase hex, and "" before vehicleIdentified()
    // or with no session. newSalt() is 16 random bytes as hex.
    Q_INVOKABLE QString vinKey(const QString& salt) const;
    Q_INVOKABLE QString newSalt() const;
    // What a VIN says without asking anyone (ADR-0043): the model year,
    // the make and the check digit in one line, as the activity log's
    // "VIN:" line gives them for the connected car. describeVin() is
    // Settings' lookup of a typed VIN (spaces dropped, upper-cased, never
    // kept; "" when empty); vinSummary() takes the latest model year
    // (this year plus one in both), so tests pin it. vinSource() names
    // the vPIC release the makes come from.
    Q_INVOKABLE QString describeVin(const QString& typed) const;
    Q_INVOKABLE QString vinSource() const;
    // A typed trouble code's title (ADR-0044), Diagnostics' lookup:
    // "P0301: Cylinder 1 Misfire Detected", spaces dropped and upper-cased,
    // "" when empty. The list's source and license are the packages'
    // notices and the release notes', not the page's.
    Q_INVOKABLE QString describeDtc(const QString& typed) const;
    // A line in the activity log (and a recording's events.log), for the
    // app's own events: the vehicle lines of ADR-0027 section 4
    Q_INVOKABLE void logEvent(const QString& line) { emit logMessage(line); }
    int unitSystem() const { return static_cast<int>(m_signalModel->unitSystem()); }
    void setUnitSystem(int units);   // anything but 0 or 1 is ignored
    int minSweepMs() const { return m_minSweepMs; }
    void setMinSweepMs(int ms); // anything but a choice is ignored
    QVariantList pickGauges() const { return m_pickGauges; }
    void setPickGauges(const QVariantList& cells);
    QVariantList minSweepChoices() const;
    // A quantity in a decoded unit, as the current unit system shows it:
    // the Dashboard's scale ends and zones are written once, in decoded
    // units, and converted here rather than in QML (rule 2).
    Q_INVOKABLE double displayValue(const QString& unit, double value) const;
    Q_INVOKABLE QString displayUnit(const QString& unit) const;
    // displayValue's inverse, for a level typed in the display unit: every
    // conversion is affine (SignalsTest.EveryImperialConversionIsAffine),
    // so two points give it; a unit left as it is passes through
    Q_INVOKABLE double decodedValue(const QString& unit, double shown) const;
    QVariantList diagnostics() const { return m_diagnostics; }
    QStringList clearTargets() const;
    bool clearFindsNothing() const;
    QVariantMap inspection() const;
    // Read: each module in turn, its standard codes then its enhanced ones
    // (UDS 0x19), a result per module as its pass ends; nothing without a
    // session. readDiagnostics is the same pass.
    Q_INVOKABLE void readDiagnostics();
    Q_INVOKABLE void readModuleCodes();
    // Clears the codes of every ECU whose read has completed (ADR-0038):
    // saves those reports first, to diagnostics-<stamp>[ <vehicle>].jsonl
    // in the recordings folder (diagnosticsFileText; the vehicle for an
    // adapter's session only, as a recording's folder), then asks core to
    // clear each at the generation saved, at any engine speed
    // (ECUIO_CLEAR_ALLOW_RUNNING: the user's word, given in the page's
    // confirmation). A failed save sends nothing. Returns the saved file
    // for display (relative to Documents), or "" when nothing was sent;
    // each ECU's outcome comes in its report and the activity log
    // (channelClearText), and a moving vehicle's in clearMoving
    Q_INVOKABLE QString clearDiagnostics();
    // The user's word on a moving vehicle too: clears each clearMoving
    // ECU at the generation its clear ended on, so a report read again
    // since (not the one the saved file holds) is refused unsent. No
    // second file: nothing was sent, and the first holds their reports
    Q_INVOKABLE void clearDiagnosticsMoving();
    // No word on a moving vehicle: clearMoving empties, nothing more sent
    Q_INVOKABLE void cancelClearMoving();
    QStringList clearMoving() const;
    QString clearMovingSpeed() const;
    QString refusedUnnamed() const { return m_refusedUnnamed; }
    // Before every connect, on the GUI thread: readies the link an app-fed
    // stream stands behind (ADR-0028; main() hands it the Bluetooth
    // singleton's prepare) and returns why it can't, "" when it can or
    // the URI names none. A refusal fails the connect with its text,
    // before core is asked.
    void setStreamPreparer(std::function<QString(const QString& uri)> preparer)
    {
        m_streamPreparer = std::move(preparer);
    }

public slots:
    // Both connects take the URI as typed and trim the spaces around it
    Q_INVOKABLE QString connectSession(const QString& uri);
    // Async disconnect: the GUI-facing state (connected, link state,
    // URI) drops at once, and the teardown (joining the worker, the
    // vendor's disconnect and close) runs on a worker thread, so a
    // wedged adapter can't freeze the UI. `disconnecting` stays true
    // until teardown ends; no connect starts before then, because the
    // same adapter may refuse a second open until its close returns.
    Q_INVOKABLE void disconnectSession();
    Q_INVOKABLE void clearFrames();
    // Async connect: the slow core work (create and register, the DLL
    // load and PassThruOpen included) runs on a worker thread; starting
    // the session, GUI state application and the result signal follow
    // queued on the GUI thread. Never blocks the UI: a J2534 handshake
    // can legally take seconds (the surrogate allows 8 s per stage).
    Q_INVOKABLE void connectSessionAsync(const QString& uri);

signals:
    void connectedChanged(bool);
    void frameCountChanged();
    void framesReset();
    void droppedFramesChanged(int);
    void overflowFramesChanged();
    void notifyCountChanged(int newCount);
    void signalBatchChanged();
    void currentUriChanged();
    void connectingChanged();
    void disconnectingChanged();
    void connectResult(const QString& uri, const QString& error);
    void logMessage(const QString& line);
    // The first channel's identity named a VIN this session hadn't
    // (ADR-0027 section 3): vinKey() has an answer now
    void vehicleIdentified();
    void vehicleLabelChanged();
    void linkStateChanged();
    void replayFinishedChanged();
    void logPausedChanged();
    void keepAwakeChanged();
    void recordingChanged();
    void recordingStateChanged();
    void markCountChanged();
    void logStateChanged();
    void logProgressChanged();
    void logNoticeChanged();
    void armedStateChanged();
    void armedWhyChanged();
    // A log ended: byUser for Stop log, false for the session's end or a
    // failed write (reported as a Record sessions recording's end is).
    // The folder is absolute, as recordingRow takes it
    void logStopped(const QString& folder, bool byUser);
    void unitSystemChanged();
    void conditionsChanged();
    void alertLevelsChanged();
    void minSweepMsChanged();
    void pickGaugesChanged();
    void diagnosticsChanged();
    void clearMovingChanged();
    void refusedUnnamedChanged();
    void readingOnceChanged();
    void readOnceResult(const QVariantMap& result);

private:
    static void onFrame(const ecuio_frame_t* frame, void* user_data);
    void handleFrameInternal(const ecuio_frame_t* frame,
                             HostClock::time_point arrived);
    // Core's values callback (ADR-0024): WORKER thread, stages a frame's
    // values under m_valuesMutex
    static void onValues(const ecuio_value_batch_t* batch, void* user_data);
    // GUI thread, before the session starts: sizes the staging for set
    void prepareValueStaging(const ecuio_signal_set_t* set);
    // Core's conditions callback (ADR-0063 section 6): WORKER thread, each
    // event into its condition's slot under m_conditionsMutex
    static void onConditions(const ecuio_condition_event_t* events,
                             uint32_t count, void* user_data);
    // GUI thread: no slots for any attach, every event passed by
    void clearConditionStaging();
    // GUI thread: set (nullptr: none) attached to session, judged with its
    // signal file, and the model's rows; false with *why core's words,
    // nothing changed. Core copies the set. The set before keeps its slots
    // and its rows' copy, so its firings that arrive later are still lines.
    // The caller flushes the window first (applySavedConditions).
    bool attachConditions(ecuio_session_t* session,
                          const ecuio_condition_set_t* set, QString* why);
    // GUI thread, before the start: the replay's own copy, else the saved set
    void attachSessionConditions(ecuio_session_t* session, const QString& uri);
    // Why an edit saved now waits ("applies when this recording ends"),
    // "" when it can be judged at once
    QString conditionsWait() const;
    // The saved set judged now: attached to the session, or shown offline
    void applySavedConditions();
    // A session's end (or a start that failed): nothing judged, no edit
    // waiting, the saved set's rows shown offline
    void endSessionConditions();
    // The activity lines of the connect's attach, written after "Session:"
    QStringList m_conditionsLines;
    // The saved file read again: conditionsProblem and the offline rows
    void loadSavedConditions();
    // The model's rows of the saved set with no session
    void showSavedConditions();
    // The tick's half: the window's events into the model and the
    // activity log, then the states core keeps
    void flushConditions(ecuio_session_t* session);
    // alertLevels from the model; emits on a change
    void judgeAlertLevels();
    // A replay's recording's conditions.json when its hash is the
    // header's; nullptr otherwise, with *unused why when the header names
    // one ("doesn't match its hash")
    ConditionSetPtr recordingConditions(const QString& uri, QString* unused);
    // GUI thread: swaps the staging out and drains it, each sample's wait
    // into the delivery figures and each signal's window into the model's
    // batch
    void drainValues();
    // GUI thread: drainValues, then the batch into the model (the tick,
    // and once more at disconnect)
    void flushValues();
    void applyFrameLoggingEnv();
    // Where recordings go: the test root, else Documents/Havne/recordings;
    // "" with no Documents folder
    QString recordingsRoot() const;
    // The earlier product name's recordings, read but never written: the
    // test root, else Documents/<earlierProductName()>/recordings; "" when
    // that's recordingsRoot() itself or there's no Documents folder
    QString earlierRecordingsRoot() const;
    // Before the session starts: the recorder opened on recordingsRoot()
    // and attached, when the switch is on and the URI isn't a replay's
    void startRecording(ecuio_session_t* session, const QString& uri);
    // A recorder opened on recordingsRoot() with the app's caller lines
    // and the session's signal file, and attached to it; nullptr, with
    // the reason logged after `failed` ("Recording failed"), and given
    // to `reason` when the caller wants it, when it can't be
    ecuio_recorder_t* openRecorder(ecuio_session_t* session, const QString& uri,
                                   const QString& failed,
                                   QString* reason = nullptr,
                                   uint32_t flags = 0);
    // armedWhy, judged again; armedWhyChanged when it differs
    void judgeArmedWhy();
    QString armedRefusal() const;
    // An armed recorder's windows against the last poll: the opened and
    // closed lines, armedStateChanged on a change
    void pollArmed(const ecuio_recorder_state_t& st);
    // The armed recorder's end: its last window's line and a line saying
    // what it wrote
    void endArmed();
    // logNotice, with its change signal when it changes
    void setLogNotice(const QString& text);
    // m_recorder from here, its folder shown and its marks from 0
    void adoptRecorder(ecuio_recorder_t* recorder);
    // Its last notes, then closed with why; "" with nothing open. A log
    // ends with it: the whole set asked again while the session runs,
    // its readout's last count, logStopped (byUser: Stop log's). An end
    // not byUser is told on the Dashboard: told, or "Log ended: <why>"
    void stopRecording(const QString& why, bool byUser = false,
                       const QString& told = QString());
    // The log's folder size, its files' sum (a handful, stat'd once a
    // second on the GUI thread)
    void countLogBytes();
    // The tick: a write that failed stops the recording, once
    void pollRecording();
    // An activity line core's recording writes in its own form (a link
    // transition, a sleep): shown, never noted
    void logUnnoted(const QString& line);
    // The connect thread's take for a created session, and the apply's
    // half: the refusal's line, and a release if the switch went off
    // meanwhile (bridge_awake.cpp)
    void holdAwake(ecuio_session_t* session);
    void applyAwake(ecuio_session_t* session);
    void writePickLine();
    void notifyDropped();   // GUI thread: emit droppedFramesChanged if the count moved
    void notifyFrameCounts();   // GUI thread: frameCount and guiNotifyCount, if they moved
    void destroySessionLocked();  // the destructor's teardown; disconnectSession tears down on its own thread

    FrameListModel* m_frameLog = nullptr;
    FrameIdModel* m_frameIds = nullptr;
    RecordingImport* m_recordingImport = nullptr;

    QTimer* m_coalesceTimer = nullptr;
    // flushConditions' parts: the set before's firings with each key's tone
    // asked, a line for each firing, and the states core keeps into the model
    void
    gatherFiringsBefore(const std::vector<ConditionSlot>* before,
                        std::vector<ConditionFiring>& fired,
                        const std::function<void(uint32_t, uint64_t*)>& ask);
    QStringList firingLines(const std::vector<ConditionFiring>& fired,
                            const std::vector<ConditionSlot>* full,
                            const std::vector<ConditionSlot>* before) const;
    void applyConditionStates(ecuio_session_t* session);
    SignalListModel* m_signalModel = nullptr;
    LiveSignalModel* m_liveSignalModel = nullptr;
    QString connectCoreOnly(const QString& uri, ecuio_session_t** out_session);
    // A replay:// URI selecting the profile its recording names (or that
    // copy's own name, rec.<16 hex>), with the recording's signals.json
    // there and matching its hash: the URI that decodes with those bytes
    // (ADR-0051 section 4): the copy's own, loaded and registered as
    // rec.<16 hex> for the bridge's life. Any other URI,
    // or a copy missing, changed or refused, comes back as given, with a
    // log line saying why. GUI thread, before the connect.
    QString replayUriWithOwnSignals(const QString& uri);
    // The log line naming the profile a replayed recording was made under,
    // and saying so when it holds no signal values (never its path)
    void logReplayMadeUnder(const QString& uri);
    QString startSession(ecuio_session_t* session, const QString& uri);
    // The async connect's end on the GUI thread: the pending session
    // started and applied (err "" so far), connecting cleared, the result
    void finishConnect(const QString& uri, QString err, const ConsentNeed& need);
    void applyConnectedState(ecuio_session_t* session, const QString& uri);
    void pollLinkHealth();
    // What each channel's ECU said it supports, logged once per update
    void pollChannelSupport();
    // The signals core reports its frames come without (ADR-0039 section 6):
    // read each tick into m_absentRead (sized at connect), handed to the
    // model when they differ from m_absentIds
    void pollAbsentSignals();
    // Sets the default pick (cli/default_pick.h), with the ticks once the
    // model holds the set; false when a log or no asking leaves it alone
    bool applyDefaultPick(ecuio_session_t* session, bool withTicks, bool again);
    // Each tick: a pick owed is set, and one whose requests the walk
    // dropped since is set again, so core judges if it still asks anything
    void pollDefaultPick();
    // Live's "not asked" rows, read each tick into m_unaskedRead (sized at
    // connect), handed to the model when the list changes
    void pollUnaskedSignals();
    // What each channel's ECU said it is (ADR-0021), logged once complete
    void pollChannelIdentity();
    // Channel 0's VIN into m_vin once core has it in (ECUIO_IDENTITY_VIN)
    void takeVin(const ecuio_channel_identity_t& s);
    void pollReplayEnd();   // the tick: "Replay finished" once, at the end of the file
    void pollSleep();       // the tick: "Asleep for N s" after a sleep
    // What each channel's ECU reported of its diagnostics (ADR-0037),
    // logged once per completed read
    void pollChannelDiagnostics();
    void rebuildDiagnostics();
    void pollModuleCodes();
    void resetModuleCodes();
    QByteArray clearFileText(const ecuio_channel_diagnostics_t* reports,
                             const uint32_t* channels, uint32_t n) const;
    bool sessionOffered() const;
    void advanceCodesVisit();
    QVariantMap previewObdbBytes(const QByteArray& bytes,
                                 const QString& refusal, int modelYear,
                                 const QString& name);
    ecuio_signal_file_t* mergeTogether(const PluginStore::Parts& parts,
                                       const QString& own, QVariantMap* answer);
    const ecuio_signal_file_t*
    togetherFileOf(const ecuio_signal_set_t* set) const;
    void releaseTogether(bool all);
    void holdUriSet(const QString& uri);
    void endTeardown();
    // A signal file into the plugin store, as a new plugin or a variant
    QVariantMap keepFile(const QByteArray& file, const QString& into,
                         const QString& variant, const QString& vehicle);
};
