// File/layer: app/recordingview.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: RecordingViewTest (test_bridge)

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "ecuio/recording.h"
#include "ecuio/signals.h"

// One recording opened for the viewer (ADR-0049 step 5): core's reader
// and the frames check, run once on a thread of its own so a long
// recording's pass never holds the GUI thread, then everything the page
// draws. Times cross to QML as milliseconds since the recording started
// (host_us / 1000, a double), values in the display units unitSystem
// names (ADR-0022), as Live's model shows them. The reader answers the
// queries on the GUI thread: a column of a second or more comes from
// its per-second summaries. A span's statistics, a cursor's step and the
// recording's stretches are answered by tasks of the global thread pool
// that share the reader, so a close never waits for a pass over a long
// recording: the pass ends on its own and its answer is dropped. Only the
// application's own end waits for one.
class RecordingView : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString folder READ folder NOTIFY stateChanged)
    // The open's failure, in core's words; empty otherwise
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    // 0 to 1 while opening: signals.csv's pass, then frames.csv's, by size
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int unitSystem READ unitSystem WRITE setUnitSystem NOTIFY
                   unitSystemChanged)
    Q_PROPERTY(double durationMs READ durationMs NOTIFY stateChanged)
    // { format, utcStartMs (0 for none), noSummary, rowsTorn,
    //   recordsDropped, notesDropped }
    Q_PROPERTY(QVariantMap info READ info NOTIFY stateChanged)
    // One map per signal, in the reader's order: { index, name, unit,
    // decimals, isText, plotRank, plotted (the default), samples,
    // lowest, highest, firstMs, lastMs (-1 with no samples) }. Not named
    // "signals", which Qt's macro takes.
    Q_PROPERTY(QVariantList signalList READ signalList NOTIFY signalListChanged)
    // { timeMs, label }, in time order
    Q_PROPERTY(QVariantList markers READ markers NOTIFY stateChanged)
    // The check's findings in the order it settled them: { kind
    // (ECUIO_CHECK_*), id (-1 for the whole bus), extended, atMs,
    // lengthMs, count, code }
    Q_PROPERTY(QVariantList findings READ findings NOTIFY stateChanged)
    // The check's outcome: { ran, error (why it didn't: "the recording
    // has no frames", a row it refused), frames, ids, isotpSkipped, isotpFlagsUnknown,
    // isotpNoWire, tornRows }
    Q_PROPERTY(QVariantMap check READ check NOTIFY stateChanged)
    // A query found the file changed since the open (a row that no
    // longer parses, the file gone): what's shown is the open's
    Q_PROPERTY(bool stale READ stale NOTIFY staleChanged)
    // Where the values come from (ADR-0049's appendix of 2026-11-06): ""
    // for signals.csv, else the profile frames.csv decoded through, or
    // "signals.json" for the recording's own signal file
    Q_PROPERTY(QString decodedWith READ decodedWith NOTIFY stateChanged)
    // The profile events.log's header names ("" for none), and whether
    // the folder's signals.json is the file it names, its bytes unchanged
    Q_PROPERTY(QString recordedProfile READ recordedProfile NOTIFY stateChanged)
    Q_PROPERTY(bool ownSignalFile READ ownSignalFile NOTIFY stateChanged)
    // Whether the folder holds a frames.csv: a CSV import holds values
    // alone, so it has no frames to check, decode or replay
    Q_PROPERTY(bool hasFrames READ hasFrames NOTIFY stateChanged)
    // A health report being written (saveHealthReport), until its thread
    // ends, whether or not the viewer closed meanwhile
    Q_PROPERTY(bool reporting READ reporting NOTIFY reportingChanged)
    // The levels stretches() takes as its rules, each core's default
    // (ECUIO_STRETCH_DEFAULT_*) in display units: { pullLevel,
    // pullLoadLevel, cruiseThrottleBand (%), pullHoldS, pullGapS,
    // cruiseHoldS (seconds), pullRise (rpm), pullGain, cruiseSpeedBand,
    // cruiseFloor (km/h, or mph under imperial units) }
    Q_PROPERTY(QVariantMap stretchDefaults READ stretchDefaults NOTIFY
                   unitSystemChanged)
    // Why the last stretches() was refused, naming the rule ("Pull level
    // must be a number from 0 to 100"); empty once an ask is taken
    Q_PROPERTY(QString stretchesProblem READ stretchesProblem NOTIFY
                   stretchesProblemChanged)

public:
    enum State { Closed, Opening, Open, Failed };
    Q_ENUM(State)

    // The most signals a recording plots on open
    static constexpr int ViewerPlotted = 4;

    explicit RecordingView(QObject* parent = nullptr);
    ~RecordingView() override;

    int state() const { return m_state; }
    QString folder() const { return m_folder; }
    QString error() const { return m_error; }
    double progress() const { return m_progress; }
    int unitSystem() const { return static_cast<int>(m_units); }
    void setUnitSystem(int units);
    double durationMs() const;
    QVariantMap info() const;
    QVariantList signalList() const;
    QVariantList markers() const { return m_markers; }
    QVariantList findings() const { return m_findings; }
    QVariantMap check() const { return m_check; }
    bool stale() const { return m_stale; }
    QString decodedWith() const { return m_decodedWith; }
    QString recordedProfile() const { return m_recordedProfile; }
    bool ownSignalFile() const { return m_ownSignalFile; }
    bool hasFrames() const { return m_hasFrames; }
    bool reporting() const { return m_reporting; }
    QVariantMap stretchDefaults() const;
    QString stretchesProblem() const { return m_stretchesProblem; }

    // The log viewer's Save report (ADR-0054's appendix "the health report
    // as built"): the whole recording's health from its folder (core's
    // rule for its set, whatever the viewer decodes with; never the view's
    // zoom), to the file at url in the format healthFormatFor picks. On a
    // thread of its own, since core reads all of frames.csv; reportSaved
    // with writeHealthReport's map when it ends, unless the viewer closed
    // or opened another recording meanwhile. False, nothing started, when
    // no recording is open or a report is being written already.
    // baselineFolder, the picked vehicle's baseline recording ("" none),
    // which the report names by its start, never its folder
    Q_INVOKABLE bool
    saveHealthReport(const QUrl& url, int filterIndex,
                     const QString& baselineFolder = QString());
    // A local target written as a content:// one is, so a test reaches the
    // picked document's removal
    void setThroughProviderForTesting(bool on) { m_throughProvider = on; }
    // The next report's thread, or the statistics and step asks' task,
    // can't be started
    void setFailThreadForTesting(bool on) { m_failThreadForTesting = on; }

    // Opens the recording in folder (the one holding signals.csv), closing
    // any open or opening one first. The state goes to Opening at once,
    // then Open or Failed on the GUI thread. A folder with frames.csv and
    // no signals.csv opens as openFrames(folder, "") does.
    Q_INVOKABLE void open(const QString& folder);
    // Opens the recording with its values decoded from its frames.csv
    // (ecuio_recording_open_frames) through profile: a registered one
    // ("obd2", a vehicle's signal file's, a plugin variant's), or
    // "signals.json" for the recording's own; "" picks as havne-cli view
    // --frames does (cli/view_frames.h): its own file while it matches,
    // else the profile it was recorded under. The set is copied here, on
    // the GUI thread where the stores change it, before the open's thread
    // starts. A profile that can't decode is Failed at once, with words.
    Q_INVOKABLE void openFrames(const QString& folder, const QString& profile);
    // Stops an open under way (Failed, "canceled") or closes the one open
    Q_INVOKABLE void close();

    // columns columns over [t0Ms, t1Ms) for each signal index in
    // signalIndexes: one map each, { index, lows: [...], highs: [...],
    // samples: [...] }, a column with no sample NaN in lows and highs;
    // display units. t0Ms may be before the start (a view aligned on
    // another recording): the columns before it are empty. Empty when not
    // open, for a bad range or index, or when the file changed (stale
    // goes true).
    Q_INVOKABLE QVariantList columns(const QVariantList& signalIndexes,
                                     double t0Ms, double t1Ms, int columns);
    // Where two recordings line up (ADR-0049 section 5, as havne-cli view
    // --align-on finds it): the first time a number signal reaches level
    // (display units) rising, or falling, after being hysteresis beyond
    // it, or the first marker labeled label. -1 for none, a text signal,
    // a bad index, or a recording not open.
    Q_INVOKABLE double findThreshold(int signal, double level,
                                     double hysteresis, bool rising);
    Q_INVOKABLE double findMarker(const QString& label);
    // The engine's first start (core's ECUIO_RECORDING_FIND_ENGINE_START,
    // as havne-cli view --align-on engine-start finds it): the rpm signal
    // reaching 500 after being below 100. -1 for none (no rpm signal, an
    // engine running from the first sample and never stopped) or a
    // recording not open.
    Q_INVOKABLE double findEngineStart();
    // Each signal's value in force at tMs, as the cursor shows it: one map
    // each, { index, text (a number in display units, its decimals; a text
    // signal's as recorded; "" before its first sample), unit, atMs (-1
    // before the first sample) }
    Q_INVOKABLE QVariantList valuesAt(const QVariantList& signalIndexes,
                                      double tMs);
    // The time of the signal's sample nearest tMs, the earlier of two
    // as near, so a tapped cursor lands where the line has a sample and
    // the value it reads is the one drawn there: its first sample for a
    // time before it, its last after it. -1 for a signal with no
    // samples, a bad index, or a recording not open.
    Q_INVOKABLE double nearestSampleMs(int signal, double tMs);
    // Each signal's statistics over the closed span [t0Ms, t1Ms]
    // (ecuio_recording_stats), read off the GUI thread: the ask's number,
    // or 0 with nothing asked (not open, a bad index, no signals, no span
    // after the start). statsReady answers with that number unless a later
    // ask replaced it: one map per signal in signalIndexes' order, { index,
    // samples, first, last, lowest, highest, mean, change (last - first),
    // rate (the change per second), lowestMs, highestMs, unit, decimals }
    // in display units, each number NaN and each time -1 with none. A time
    // before the start reads from the start. Empty on a failure.
    Q_INVOKABLE int stats(const QVariantList& signalIndexes, double t0Ms,
                          double t1Ms);
    // Each signal's mean in each of `columns` columns over [t0Ms, t1Ms),
    // every column over its own span of the recording's time
    // (ecuio_recording_stats, which reads a part of a second from the
    // rows), where columns() puts a whole second in the column it starts
    // in: what the difference of two recordings a part of a second apart
    // is taken from (ADR-0056's appendix of 2026-11-12). Read off the GUI
    // thread: the ask's number, or 0 with nothing asked (not open, a bad
    // index, no signals, no span, a column count columns() refuses).
    // columnMeansReady answers with that number unless a later ask
    // replaced it: one map per signal in signalIndexes' order, { index,
    // means (`columns` numbers in display units; NaN for an empty column,
    // one before the start and a text signal) }. Empty on a failure.
    Q_INVOKABLE int columnMeans(const QVariantList& signalIndexes, double t0Ms,
                                double t1Ms, int columns);
    // The signal's next sample after tMs, or its previous before it
    // (ecuio_recording_step), off the GUI thread: the ask's number, 0 with
    // nothing asked. stepReady answers with that number unless a later ask
    // replaced it: the sample's time, -1 at the ends or on a failure.
    Q_INVOKABLE int step(int signal, double tMs, bool forward);
    // The recording's pulls, its cruises or both (ecuio_recording_stretches
    // over the whole recording, core's own pick of each role's signal),
    // off the GUI thread: the ask's number, or 0 with nothing asked (not
    // open, neither kind, rules refused). rules holds stretchDefaults'
    // keys in display units, a key left out or 0 taking core's default. A
    // key that isn't one, a value that isn't a finite number of 0 or
    // more, a percentage past 100, a time past an hour or a speed past
    // 1000 km/h refuses the ask here, on the caller's thread, with
    // stretchesProblem saying which. stretchesReady answers with
    // that number unless a later ask replaced it or the recording closed:
    // one map per stretch in time order, { kind ("pull" or "cruise"),
    // fromMs, toMs, start, end (a pull's measured signal at its two ends,
    // a cruise's mean speed in both, in display units), unit, decimals,
    // signal (the measured one's index) }, and ok: false, the list empty,
    // when the pass failed (the file changed since the open, no memory).
    Q_INVOKABLE int stretches(bool pulls, bool cruises,
                              const QVariantMap& rules = {});
    // Each stretches pass waits this long before it reads, so a test can
    // close or destroy the view while one is under way
    void setStretchesHoldForTesting(int ms) { m_stretchesHoldMs = ms; }
    // The same for the statistics and step asks' task, before each read
    void setAsksHoldForTesting(int ms) { m_asksHoldMs = ms; }
    // The holds this recording's asks' task has come to with an ask taken:
    // a close before the first finds no read under way
    int asksHeldForTesting() const { return m_asks ? m_asks->held.load() : 0; }
    // The means passes a newer ask or a close ended early
    int meansStoppedForTesting() const
    {
        return m_asks ? m_asks->stopped.load() : 0;
    }

signals:
    void stateChanged();
    void progressChanged();
    void unitSystemChanged();
    void signalListChanged();
    void staleChanged();
    void reportingChanged();
    void reportSaved(const QVariantMap& result);
    void statsReady(int request, const QVariantList& stats);
    void stepReady(int request, double atMs);
    void columnMeansReady(int request, const QVariantList& means);
    void stretchesReady(int request, const QVariantList& stretches, bool ok);
    void stretchesProblemChanged();

private:
    struct Result;
    struct StatsAsk {
        int request = 0;
        std::vector<uint32_t> ids;
        uint64_t t0 = 0;
        uint64_t t1 = 0;
    };
    struct StepAsk {
        int request = 0;
        uint32_t signal = 0;
        uint64_t t = 0;
        uint32_t direction = 0;
    };
    struct MeansAsk {
        int request = 0;
        std::vector<uint32_t> ids;
        int64_t t0 = 0; // signed: a window can start before the recording
        int64_t t1 = 0;
        uint32_t columns = 0;
    };
    // What a view shares with the task answering its statistics, step and
    // column means asks: one of each kind waits, a newer ask replacing it,
    // so a dragged cursor never builds a queue. A close empties it and
    // lets go of it, and the task ends after the read it is in
    struct Asks {
        std::mutex m;
        std::optional<StatsAsk> stats;
        std::optional<StepAsk> step;
        std::optional<MeansAsk> means;
        bool answering = false; // a task is at work
        // Set by a close and by a newer means ask: a means pass under way
        // stops at its next column
        std::atomic<bool> meansStale{false};
        // Asks left unanswered because a copy could not be allocated
        std::atomic<int> lost{0};
        int holdMs = 0;              // a test's wait before each read
        std::atomic<int> held{0};    // a test's holds begun
        std::atomic<int> stopped{0}; // means passes ended before their end
    };
    struct Signal {
        ecuio_recording_signal_t pub;
        bool plotted = false;
    };

    struct FrameSet;
    static void run(const std::shared_ptr<Result>& res,
                    std::atomic<bool>* cancel, RecordingView* view,
                    quint64 generation, const QString& folder,
                    const std::shared_ptr<FrameSet>& frames);
    void start(const QString& folder, const std::shared_ptr<FrameSet>& frames);
    void readHeader(const QString& folder);
    void fail(const QString& folder, const QString& words);
    void finish(const std::shared_ptr<Result>& res, quint64 generation);
    void stop();
    void setProgress(double p, quint64 generation);
    void reportFinished(const QVariantMap& result, quint64 generation);
    void markStale();
    // A recorded number as displayed: the value, its unit and decimals
    ecuio_display_value_t shown(const Signal& s, double value) const;
    bool signalIndex(const QVariant& v, uint32_t* out) const;
    // Fills cols (ids.size() x n, zeroed) over [t0Ms, t1Ms), the part
    // before the start left empty; false on a query's failure
    bool query(const std::vector<uint32_t>& ids, double t0Ms, double t1Ms,
               uint32_t n, std::vector<ecuio_recording_column_t>& cols);
    double found(const ecuio_recording_find_t& what);
    // False when the asks have no place to wait in (no memory for it)
    bool haveAsks();
    // False when the asks' task can't be started (no memory for it)
    bool startAsks();
    // The asks' task: answers until none waits, each answer posted as a
    // stretches pass posts its own
    static void answerAsks(const std::shared_ptr<Asks>& asks,
                           const std::shared_ptr<ecuio_recording_t>& reader,
                           const QPointer<RecordingView>& view,
                           quint64 generation);
    void statsFinished(const StatsAsk& ask, ecuio_status_t rc,
                       const std::vector<ecuio_recording_stats_t>& got,
                       quint64 generation);
    void stepFinished(int request, ecuio_status_t rc, uint64_t at,
                      quint64 generation);
    void meansFinished(const MeansAsk& ask, ecuio_status_t rc,
                       const std::vector<double>& got, quint64 generation);
    // One pass, a task of the global thread pool that holds the reader
    // open; its answer is posted to the application and reaches view only
    // if it still lives
    static void findStretches(const std::shared_ptr<ecuio_recording_t>& reader,
                              const QPointer<RecordingView>& view, int request,
                              ecuio_stretch_rules_t rules, quint64 generation,
                              int holdMs);
    void setStretchesProblem(const QString& words);
    void stretchesFinished(int request, ecuio_status_t rc,
                           const std::vector<ecuio_recording_stretch_t>& got,
                           quint64 generation);

    int m_state = Closed;
    QString m_folder;
    QString m_error;
    double m_progress = 0;
    uint32_t m_units = ECUIO_UNITS_METRIC;
    bool m_stale = false;
    QString m_decodedWith;
    QString m_recordedProfile;
    QString m_recordedSha;
    bool m_ownSignalFile = false;
    bool m_hasFrames = false;

    // The open reader: m_reader for every call on it, m_readerHold its
    // owner, shared with a stretches pass and the asks' task so a close
    // mid-pass leaves the reader open until the pass ends
    ecuio_recording_t* m_reader = nullptr;
    std::shared_ptr<ecuio_recording_t> m_readerHold;
    ecuio_recording_info_t m_info{};
    std::vector<Signal> m_signals;
    QVariantList m_markers;
    QVariantList m_findings;
    QVariantMap m_check;

    std::thread m_worker;
    std::atomic<bool> m_cancel{false};
    // The health report's: joined when it posts its end, or at
    // destruction, which sets m_reportStop first so core's export stops
    // at its next ask
    std::thread m_reportWorker;
    std::atomic<bool> m_reportStop{false};
    bool m_reporting = false;
    bool m_throughProvider = false;
    bool m_failThreadForTesting = false;
    // Made at the first ask after an open or a close that left a task behind
    std::shared_ptr<Asks> m_asks;
    int m_asksHoldMs = 0;
    int m_lastAsk = 0;
    int m_lastStats = 0;
    int m_lastStep = 0;
    int m_lastMeans = 0;
    int m_lastStretches = 0;
    int m_stretchesHoldMs = 0;
    QString m_stretchesProblem;
    // Each open's number: a result or progress posted for an earlier one
    // is dropped
    quint64 m_generation = 0;
};
