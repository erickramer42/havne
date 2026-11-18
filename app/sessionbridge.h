// File/layer: app/sessionbridge.h | Qt6 Core, app layer, C++17
// Testing: UNIT: CoreTest (test_core); BRIDGE: BridgeTest (test_bridge);
//          QML: QmlTest.dashboardbook

#pragma once

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

class QSettings;

class SessionBridge : public QObject
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
    // replies' and text signals' frames, words never naming the path. A
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
    // The profile a session on uri decodes through, as core picks it at
    // ecuio_session_create: the last profile= (1 to
    // ECUIO_PROFILE_NAME_MAX bytes), else the scheme's default. "" for
    // a scheme this platform's core doesn't take, a malformed query or
    // a bad profile= value. Static, so tests need no session.
    static QString profileOfUri(const QString& uri);
    // The signal file whose set set is: a plugin variant's or a replay's
    // own signals.json; nullptr for any other set
    const ecuio_signal_file_t* fileOfSet(const ecuio_signal_set_t* set) const;
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
    // The generation of the session's last attach, as core counts them
    uint32_t conditionsGenerationForTesting() const
    {
        return m_conditionsGeneration;
    }
    // The next n attaches refused where core's call is made; during, when
    // given, runs there first, as the worker's callback can meanwhile
    void failConditionsAttachesForTesting(int n,
                                          std::function<void()> during = {})
    {
        m_failAttaches = n;
        m_duringFailedAttach = std::move(during);
    }
    // The alert tones asked, and a later time for the tone's 5 s
    const AlertTone& alertToneForTesting() const { return m_tone; }
    void addToneTimeForTesting(uint64_t us) { m_toneTimeAddedForTesting += us; }

    ConditionModel* conditionModel() const { return m_conditionModel; }
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
    // The samples one window can hold, from the next connect
    void setValueSampleLimitForTesting(int samples) { m_valueSampleLimit = samples; }
    static constexpr int defaultValueSampleLimit() { return DefaultValueSampleLimit; }
    static constexpr int maxInFlightForTesting() { return MaxInFlightFrames; }
    ecuio_session_t* pendingSessionForTesting() const { return m_pendingSession.load(); }
    QString linkState() const;
    static QString linkStateForError(int32_t errorCode);  /* code -> state string; static so tests need no session */
    /* A direct-load DLL or replay path cut to its last name, a Bluetooth address to its last two octets */
    Q_INVOKABLE static QString uriLabelFor(const QString& uri);
    static QString sessionInfoText(const ecuio_session_info_t& info);  /* the activity log's "Session:" line */
    static QString versionsText(const ecuio_session_info_t& info);  /* its versions clause; "" with none read */
    static QString channelSupportText(const ecuio_channel_support_t& support);  /* its "ECU 0x…:" lines */
    // A module's session change (ADR-0060): "ECU 0x7E8: reading in the
    // extended session (50 03)" and the like; "" for none to tell
    static QString channelSessionText(const ecuio_channel_support_t& support);
    // At Disconnect, for a module that entered the extended session; ""
    // otherwise
    static QString sessionReturnedText(const ecuio_channel_support_t& support);
    // Live's "Refused: <name> (7F <service> <code>)"
    static QString refusalText(uint8_t service, uint8_t nrc);
    // The negative reply alone: "7F 22 31"
    static QString refusalCode(uint8_t service, uint8_t nrc);
    // refusedUnnamed's words for each ECU's ID and its refusals past the list
    static QString
    refusedUnnamedText(const std::vector<std::pair<uint32_t, uint32_t>>& more);
    static QString channelIdentityText(const ecuio_channel_identity_t& identity);  /* its "ECU 0x… identity:" lines */
    static QString channelDiagnosticsText(const ecuio_channel_diagnostics_t& diagnostics);  /* its "ECU 0x… diagnostics:" lines */
    // its "ECU 0x... clear:" lines (ADR-0038), speeds in the unit system
    static QString channelClearText(const ecuio_channel_diagnostics_t& diagnostics,
                                    uint32_t units);
    // A clear's saved reports, as havne-cli writes them (ADR-0031 section 3): a
    // start line, then one diagnostics line per report
    static QByteArray diagnosticsFileText(const ecuio_channel_diagnostics_t* reports,
                                          uint32_t count);

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
    /* The activity log's "Reply timing:" line at a disconnect (ADR-0042);
     * empty when no request was asked */
    static QString timingText(const ecuio_session_timing_t& timing,
                              const std::vector<ecuio_request_timing_t>& rows);

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
    // A recording's replay:// URI, decoding under the profile it was
    // recorded with: the recorded URI's profile= (with its did= under
    // uds-generic), else its scheme's default (mock:// mock, replay://
    // raw, an adapter obd2); raw when nothing was recorded. Static, so
    // tests need no files.
    static QString replayUriFor(const QString& folder,
                                const QString& recordedUri);
    int linkErrorCode() const { return m_linkErrorCode; }
    int errorEpisodes() const { return static_cast<int>(m_errorEpisodes); }
    bool logPaused() const { return m_logPaused; }
    void setLogPaused(bool paused);
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
    static QString vinSummary(const QString& vin, int latestModelYear);
    // A line in the activity log (and a recording's events.log), for the
    // app's own events: the vehicle lines of ADR-0027 section 4
    Q_INVOKABLE void logEvent(const QString& line) { emit logMessage(line); }
    // A label as a recording folder takes it: letters, digits, space,
    // -, _ and ., trimmed, at most 40 characters, no trailing dot
    static QString folderLabel(const QString& label);
    int unitSystem() const { return static_cast<int>(m_signalModel->unitSystem()); }
    void setUnitSystem(int units);   // anything but 0 or 1 is ignored
    int minSweepMs() const { return m_minSweepMs; }
    void setMinSweepMs(int ms); // anything but a choice is ignored
    QVariantList pickGauges() const { return m_pickGauges; }
    void setPickGauges(const QVariantList& cells);
    // Each cell's first ref the set answers to (a key def's "id:<n>" too),
    // in the cells' order, each once; file is the set's (fileOfSet)
    static std::vector<uint16_t> gaugeIds(const ecuio_signal_set_t* set,
                                          const ecuio_signal_file_t* file,
                                          const QVariantList& cells);
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
    // A report a clear goes to: its read complete, no clear under way
    static bool clearTarget(const ecuio_channel_diagnostics_t& report);
    bool clearFindsNothing() const;
    QVariantMap inspection() const;
    // ecuio_diag_inspect's verdict over reports for modelYear (0: none
    // known) as the page shows it: verdict, havne-cli's word ("ready",
    // "not_ready", "mil_on", "year_needed", "not_covered"), "" while a
    // read is under way or when no ECU answered PID 01; text, one
    // sentence naming the incomplete monitors; rule, the rule and its
    // reach. Static, so test_bridge pins it without a session.
    static QVariantMap
    inspectionEntry(const ecuio_channel_diagnostics_t* reports, uint32_t count,
                    uint32_t modelYear);
    // A report that answered there is nothing to erase: PID 01 with the
    // light off and no confirmed codes, the stored and pending lists
    // answered empty, no freeze frame. A refused or unanswered item is
    // not known to be empty, so it never counts as nothing.
    static bool nothingStored(const ecuio_channel_diagnostics_t& report);
    // Read again: every ECU's diagnostics asked once more
    // (ecuio_session_read_diagnostics); nothing without a session
    Q_INVOKABLE void readDiagnostics();
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
    // One ECU's report as the page shows it: ecu ("0x7E8"); asked,
    // complete; readiness (an outcome: "unasked", "answered", "refused",
    // "unanswered") and readinessNrc ("7F 01 12" when refused); mil,
    // dtcCount, compression; lists, three of {key ("stored", "pending",
    // "permanent"), outcome, nrc, codes (["P0301", ...]), more (codes past
    // the held ones)}; cycle, PID 41's outcome (those above, or
    // "unsupported": the ECU's bitmaps leave it out, so it wasn't asked);
    // monitors, [{id, name, complete, cycleEnabled, cycleComplete}], the
    // last two this drive cycle's (PID 41) once cycle is "answered";
    // freeze, {outcome, nrc, stored, code, values: [{name, value, unit,
    // decimals}]}, each value through the obd2 def that decoded it and the
    // unit system; clearing, and the last clear's clear ("none", "cleared",
    // "refused", "unanswered", "engineRunning", "noEngineSpeed",
    // "reportChanged"), clearNrc ("7F 04 22" when refused, "7F 01 12" when
    // the engine speed was) and clearRpm. Static, so test_bridge pins it
    // without a session.
    static QVariantMap diagnosticsEntry(const ecuio_channel_diagnostics_t& d,
                                        uint32_t units);
    // Test seam: where recordings go instead of Documents/Havne/recordings
    void setRecordingsRootForTesting(const QString& root) { m_recordingsRoot = root; }
    // Test seam: the earlier name's recordings, instead of
    // Documents/<earlierProductName()>/recordings
    void setEarlierRecordingsRootForTesting(const QString& root)
    {
        m_earlierRecordingsRoot = root;
    }
    // Before every connect, on the GUI thread: readies the link an app-fed
    // stream stands behind (ADR-0028; main() hands it the Bluetooth
    // singleton's prepare) and returns why it can't, "" when it can or
    // the URI names none. A refusal fails the connect with its text,
    // before core is asked.
    void setStreamPreparer(std::function<QString(const QString& uri)> preparer)
    {
        m_streamPreparer = std::move(preparer);
    }
    // Test seam: record a replay too, to compare it with its source. The
    // app never records one: it would copy the recording it plays
    void setRecordReplaysForTesting(bool on) { m_recordReplays = on; }
    // Armed no longer judges "no condition records" itself, so core's own
    // refusal at the attach reaches the user
    void skipArmedJudgmentForTesting(bool on)
    {
        m_skipArmedJudgmentForTesting = on;
    }
    // Test seam: the next load of the plugin copy with this hash fails
    void failPluginLoadForTesting(const QString& sha)
    {
        m_pluginStore.failLoadForTesting(sha);
    }
    // Test seam: the session's recorder, for core's internal.h seams
    // (a failed write); nullptr with none
    ecuio_recorder_t* recorderForTesting() const { return m_recorder; }
    // Test seam: the running session, for a recorder of the test's own
    // (an attach the session refuses); nullptr with none
    ecuio_session_t* sessionForTesting() const { return m_session; }
    // Test seam: how long Read once waits for its reply once the
    // session starts (3 s)
    void setReadOnceWaitForTesting(int ms) { m_readOnceWaitMs = ms; }
    // Test seam: a sleep of this long, added to what core's count says,
    // for the tick to find (no runner can sleep)
    void addSleptForTesting(uint64_t us) { m_sleptAddedForTesting += us; }
    // Test seam: core can't give a recorder's folder whole, so the bridge
    // reads it as empty
    void failRecorderFolderForTesting(bool on)
    {
        m_failRecorderFolderForTesting = on;
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
    void writePickLine();
    void notifyDropped();   // GUI thread: emit droppedFramesChanged if the count moved
    void notifyFrameCounts();   // GUI thread: frameCount and guiNotifyCount, if they moved
    void destroySessionLocked();  // the destructor's teardown; disconnectSession tears down on its own thread

    ecuio_session_t* m_session = nullptr;
    QString m_currentUri;
    QString m_adapterVersions;

    std::atomic<bool> m_connected{false};
    // onFrame's gate: set just before the session starts, so frames sent
    // before the connect is applied queue instead of vanishing (a replay's
    // first frame is due the moment it starts); the GUI-thread half still
    // waits for m_connected, which they reach after the apply
    std::atomic<bool> m_accepting{false};
    // Every frame onFrame saw, dropped ones too
    std::atomic<uint64_t> m_framesDelivered{0};
    std::atomic<int> m_droppedFrames{0};
    // Frames flagged ECUIO_FRAME_FLAG_OVERFLOW, counted in onFrame so one
    // the in-flight cap drops still counts; reported on the tick, the
    // first time with an activity-log line
    std::atomic<int> m_overflowFrames{0};
    int m_reportedOverflow = 0;        // GUI-thread-only
    int m_currentFrameCount = 0;      // GUI-thread-only (queued invocations)
    int m_reportedFrameCount = 0;      // what QML was last told, and below
    int m_reportedNotifyCount = 0;
    int m_lastReportedDropped = 0;     // GUI-thread-only companion to m_droppedFrames
    bool m_logPaused = false;          // GUI-thread-only
    // A replay without loop: its frames, and whether its end is logged
    uint32_t m_replayFrames = 0;
    bool m_replayEndLogged = true;
    // The tick after the end is seen: pollReplayEnd runs after flushValues,
    // so the last frames' values, queued behind their frames, are in by then
    bool m_replaySettling = false;
    bool m_replayFinished = false;
    // The machine's slept count at the last tick (ADR-0033 section 6), and the
    // test seam's addition to it
    uint64_t m_sleptUs = 0;
    uint64_t m_sleptAddedForTesting = 0;
    bool m_failRecorderFolderForTesting = false;
    uint64_t sleptUs() const
    {
        return ecuio_clock_slept_us() + m_sleptAddedForTesting;
    }

    FrameListModel* m_frameLog = nullptr;
    FrameIdModel* m_frameIds = nullptr;
    RecordingImport* m_recordingImport = nullptr;

    QTimer* m_coalesceTimer = nullptr;
    // The model's batch for one tick: each signal's window
    std::vector<SignalListModel::WindowValue> m_stagingBuffer;
    std::vector<std::pair<uint16_t, QString>> m_stagingTexts;   // the text rows'
    int m_signalBatchCount = 0;

    // One coalesce window of decoded values, written by onValues on the
    // worker. Sized at connect and never grown there: a sample past the
    // limit is counted and goes untimed (a numeric signal's newest,
    // lowest and highest are kept regardless; the recording is core's).
    struct ValueStaging {
        struct Sample {
            HostClock::time_point arrived;
            uint32_t def = 0;          // index into the session's set
            uint32_t textAt = 0;       // a text sample's bytes in text
            uint32_t textLen = 0;
            bool isText = false;
            double value = 0.0;
        };
        std::vector<Sample> samples;   // every value, in arrival order
        std::vector<char> text;        // the text samples' bytes
        std::vector<double> newest;    // per def: its newest numeric value
        // per def: the window's lowest and highest, and each value's
        // frame time in session us
        std::vector<double> lowest;
        std::vector<double> highest;
        std::vector<uint64_t> newestUs;
        std::vector<uint64_t> lowestUs;
        std::vector<uint64_t> highestUs;
        std::vector<int> newestText;   // per def: its newest text sample, or -1
        std::vector<uint8_t> has;      // per def: a value this window
        size_t sampleLimit = 0;
        size_t textLimit = 0;
        uint32_t dropped = 0;          // samples past a limit this window

        void prepare(size_t defs, size_t samplesMax, size_t textMax);
        void stage(uint32_t def, HostClock::time_point arrived,
                   uint64_t frameUs, const ecuio_value_t& v);
        void reset();
    };
    std::mutex m_valuesMutex;           // guards m_valueStaging and m_activeStaging
    ValueStaging m_valueStaging[2];
    int m_activeStaging = 0;            // the one onValues writes
    // The session's set and each signal id's def index: written before
    // the session starts, read by onValues on the worker
    const ecuio_signal_set_t* m_valueSet = nullptr;
    QHash<uint16_t, int> m_defIndexOfId;
    static constexpr int DefaultValueSampleLimit = 16384;   // per window
    static constexpr int ValueTextLimit = 256 * 1024;       // bytes per window
    int m_valueSampleLimit = DefaultValueSampleLimit;
    quint64 m_valuesDropped = 0;        // this session's, past the limits

    // The conditions. A slot is one condition's window of events: the
    // newest, and the firings with the first of them, so the worker writes
    // a fixed slot per condition whatever the bus rate (sized at attach,
    // never grown on the worker)
    struct ConditionSlot {
        bool has = false;
        uint64_t firings = 0;
        ecuio_condition_event_t newest{};
        ecuio_condition_event_t first{}; // the window's first firing
    };
    // One attach's slots, twice: the worker writes staging[active] and the
    // tick swaps them to read the other
    struct ConditionWindow {
        std::vector<ConditionSlot> staging[2];
        int active = 0;
        uint32_t generation = 0; // the attach whose events it keeps
    };
    std::mutex m_conditionsMutex; // guards the windows
    ConditionWindow m_conditionWindow;
    // The set before's: the worker's dispatch under way at an attach took
    // that set, and gives its events after core's call returns
    ConditionWindow m_conditionWindowBefore;
    // The one before that, held only while core's call runs: a refusal
    // makes it the set before's again
    ConditionWindow m_conditionWindowOlder;
    // One firing of a tick: when, its row, and which set's rows that is
    struct ConditionFiring {
        uint64_t at;
        int row;
        ConditionModel::Rows rows;
    };
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
    ConditionStore m_conditionStore;
    ConditionModel* m_conditionModel = nullptr;
    int m_failAttaches = 0; // failConditionsAttachesForTesting
    std::function<void()> m_duringFailedAttach;
    uint32_t m_conditionsGeneration =
        0; // this session's attaches, core's count
    std::vector<ecuio_condition_state_t> m_conditionStates; // the tick's read
    QString m_conditionsPending;
    QString m_conditionsProblem;
    bool m_conditionsFromRecording = false;
    QVariantMap m_alertLevels;
    // The tone (ADR-0063 section 9): each condition's last tone on the
    // host clock, sized at the attach like the slots and kept by key
    static constexpr uint64_t ToneGapUs = 5000000;
    static constexpr uint64_t NoTone = UINT64_MAX;
    AlertTone m_tone;
    std::vector<uint64_t> m_toneAtUs;
    // The set before's stamps, for a key the set attached dropped
    std::vector<uint64_t> m_toneBeforeAtUs;
    uint64_t m_toneTimeAddedForTesting = 0;

    SignalListModel* m_signalModel = nullptr;
    LiveSignalModel* m_liveSignalModel = nullptr;
    int m_guiNotifyCount = 0;

    std::atomic<int> m_inFlightFrames{0};   // posted-but-not-yet-processed count
    std::atomic<bool> m_dropNotifyPending{false};   // at most one queued drop notification
    static constexpr int MaxInFlightFrames = 1000; // queue backlog bound

    // Async connect support. m_connecting is written only on the GUI thread
    // (in the async entry and the queued completion), so a plain bool will do.
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
    void applyConnectedState(ecuio_session_t* session, const QString& uri);
    std::thread m_connectThread;
    bool m_connecting = false;
    // The created, registered, not started session the connect thread
    // hands back. The queued completion takes it; if the bridge dies
    // first, the completion is dropped and the destructor destroys it.
    // Atomic only for the test seam: both owners read it after joining
    // the connect thread.
    std::atomic<ecuio_session_t*> m_pendingSession{nullptr};
    // Async disconnect support, the same shape as connect: the thread
    // owns the session it tears down, and m_disconnecting is written
    // only on the GUI thread (in disconnectSession and its completion).
    std::thread m_disconnectThread;
    bool m_disconnecting = false;
    void pollLinkHealth();
    int m_linkErrorCode = ECUIO_OK;
    uint32_t m_errorEpisodes = 0;
    // What each channel's ECU said it supports, logged once per update
    void pollChannelSupport();
    uint32_t m_channelCount = 0;
    uint32_t m_supportGeneration[ECUIO_MAX_CHANNELS] = {};
    // Each channel's last report, for the signals their ECUs refused
    ecuio_channel_support_t m_supportReports[ECUIO_MAX_CHANNELS] = {};
    // What each channel's session line last said (cli::session_due's rule)
    struct SessionWritten {
        uint8_t state = 0;
        uint8_t nrc = 0;
        uint16_t entries = 0;
        uint16_t lost = 0;
        bool written = false;
    };
    SessionWritten m_sessionWritten[ECUIO_MAX_CHANNELS] = {};
    QString m_refusedUnnamed;
    // The signals core reports their ECU doesn't support, read into
    // m_unsupportedRead (sized at connect) when a report changes
    std::vector<uint16_t> m_unsupportedRead;
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
    QVariantList m_pickGauges;
    std::vector<uint16_t> m_pickIds; // the pick set last, [] none
    bool m_pickSet = false;          // a pick, or the whole set, applied
    int m_pickMinSweepMs = 0;        // the minimum it was set with
    bool m_pickPolled = false;       // the URI names poll=
    bool m_pickDirty = false;        // the ticks, gauges or minimum moved
    bool m_pickLinePending = false;  // startSession's line, not yet out
    QString m_pickLine;              // the activity line last written
    bool m_pickLineCore = false;     // core's recorder writes it itself
    uint64_t m_pickDropped = 0;      // the reports' drops at the last set
    std::vector<uint16_t> m_unaskedRead;
    std::vector<uint16_t> m_unaskedIds;
    std::vector<uint16_t> m_absentRead;
    std::vector<uint16_t> m_absentIds;
    // What each channel's ECU said it is (ADR-0021), logged once complete
    void pollChannelIdentity();
    // Channel 0's VIN into m_vin once core has it in (ECUIO_IDENTITY_VIN)
    void takeVin(const ecuio_channel_identity_t& s);
    void pollReplayEnd();   // the tick: "Replay finished" once, at the end of the file
    void pollSleep();       // the tick: "Asleep for N s" after a sleep
    uint32_t m_identityGeneration[ECUIO_MAX_CHANNELS] = {};
    // What each channel's ECU reported of its diagnostics (ADR-0037),
    // logged once per completed read
    void pollChannelDiagnostics();
    uint32_t m_diagnosticsGeneration[ECUIO_MAX_CHANNELS] = {};
    // A clear asked of each channel, its outcome not yet logged
    bool m_clearAsked[ECUIO_MAX_CHANNELS] = {};
    // A clear that stopped at the vehicle speed, waiting for the word on
    // a moving vehicle: the generation it ended on, the speed it read
    // (UINT32_MAX: none)
    bool m_clearWaiting[ECUIO_MAX_CHANNELS] = {};
    uint32_t m_clearWaitGeneration[ECUIO_MAX_CHANNELS] = {};
    uint32_t m_clearWaitKmh[ECUIO_MAX_CHANNELS] = {};
    // The file the last clear saved its reports to, as shown
    QString m_clearSavedTo;
    // The reports the page shows, as last read, and their entries
    ecuio_channel_diagnostics_t m_diagnosticsShown[ECUIO_MAX_CHANNELS] = {};
    uint32_t m_diagnosticsShownCount = 0;
    QVariantList m_diagnostics;
    void rebuildDiagnostics();
    QString m_vin;            // the first channel's, this session; never leaves the bridge
    QString m_vehicleLabel;   // the picked vehicle's, for the recording folder

    // This session's delivery figures (deliveryStats), GUI thread only
    LatencyStats m_frameLatency;
    LatencyStats m_valueLatency;
    quint64 m_valuesStaged = 0;

    bool m_recording = false;
    ecuio_recorder_t* m_recorder =
        nullptr;                       // the session's, until stopRecording
    QString m_recordingFolder;         // the current or last one, absolute
    bool m_noting = true;              // logUnnoted clears it for its line
    int m_markCount = 0;               // the recording's marks, from 0
    bool m_logActive = false;          // m_recorder is Start log's
    bool m_armedActive = false;        // m_recorder is Armed's
    bool m_armedInWindow = false;      // a window is writing (last poll)
    int m_armedWindows = 0;            // windows seen opened
    QString m_armedFolder;             // the newest window's folder name
    QString m_armedWhy;                // armedWhy's last answer
    QString m_armedFailure;            // a failed write's reason, for the end
    bool m_skipArmedJudgmentForTesting = false;
    HostElapsedTimer m_logClock;       // from Start log, sleeps counted
    qint64 m_logElapsedMs = 0;         // the last log's, once it ends
    qint64 m_logBytes = 0;             // countLogBytes' last sum
    QString m_logNotice;               // the header's line about the log
    int m_minSweepMs = 0;              // a log's and the default pick's
    QTimer* m_logTimer = nullptr;      // the readout's second
    QString m_recordingsRoot;          // empty: Documents/Havne/recordings
    QString m_earlierRecordingsRoot;   // empty: the earlier name's folder
    bool m_recordReplays = false;      // tests only
    bool m_setFrameLogging = false;    // the bridge, not the launch, set ECUIO_SUR_LOG_FRAMES
    std::function<QString(const QString&)>
        m_streamPreparer; // none: nothing to ready
    ReadOnce m_readOnce;
    QByteArray m_obdbPending; // previewObdb's converted file, until kept
    int m_obdbPendingYear = 0;  // the model year it was converted for
    QByteArray m_importPending; // readImport's file, until kept or dropped
    QString m_importKind;       // what it is: "signals" or "obdb"
    QVariantMap previewObdbBytes(const QByteArray& bytes,
                                 const QString& refusal, int modelYear,
                                 const QString& name);
    // A signal file into the plugin store, as a new plugin or a variant
    QVariantMap keepFile(const QByteArray& file, const QString& into,
                         const QString& variant, const QString& vehicle);
    PluginStore m_pluginStore;
    // Recordings' own signal files by their hash, each registered under
    // its rec.<16 hex> profile until the bridge closes, as the store keeps
    // its files: the registry keeps the set's pointer
    struct ReplayFile {
        ecuio_signal_file_t* file = nullptr;
        QString name;    // the file's "name"
        QString profile; // rec.<16 hex>
    };
    std::map<QString, ReplayFile> m_replayFiles;
    int m_readOnceWaitMs = 3000;
};
