// File/layer: app/bridgestate.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "alerttone.h"
#include "conditionmodel.h"
#include "conditionstore.h"
#include "consentflow.h"
#include "hostclock.h"
#include "latencystats.h"
#include "pluginstore.h"
#include "readonce.h"
#include "signallistmodel.h"

#include "ecuio/conditions.h"
#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

// SessionBridge's data members and the helper types they use, apart from
// the few its constructor initializes. No moc: nothing here is a signal,
// slot or property. The bridge inherits it protected, so only the bridge
// and the test seams (bridgeseams.h) see a member. Members stay grouped by
// topic, not packed.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct BridgeState
{
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
    ConditionStore m_conditionStore;
    ConditionModel* m_conditionModel = nullptr;
    ConsentFlow* m_consent = nullptr; // ADR-0066: consent and the unlock
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

    int m_guiNotifyCount = 0;

    std::atomic<int> m_inFlightFrames{0};   // posted-but-not-yet-processed count
    std::atomic<bool> m_dropNotifyPending{false};   // at most one queued drop notification
    static constexpr int MaxInFlightFrames = 1000; // queue backlog bound

    // Async connect support. m_connecting is written only on the GUI thread
    // (in the async entry and the queued completion), so a plain bool will do.
    std::thread m_connectThread;
    bool m_connecting = false;
    // The created, registered, not started session the connect thread
    // hands back. The queued completion takes it; if the bridge dies
    // first, the completion is dropped and the destructor destroys it.
    // Atomic only for the test seam: both owners read it after joining
    // the connect thread.
    std::atomic<ecuio_session_t*> m_pendingSession{nullptr};
    // Keep-awake: the switch, read by the connect thread; and that
    // thread's refusal line, read by the completion after the join
    std::atomic<bool> m_keepAwake{true};
    QString m_awakeRefusal;
    // Async disconnect support, the same shape as connect: the thread
    // owns the session it tears down, and m_disconnecting is written
    // only on the GUI thread (in disconnectSession and its completion).
    std::thread m_disconnectThread;
    bool m_disconnecting = false;
    int m_linkErrorCode = ECUIO_OK;
    uint32_t m_errorEpisodes = 0;
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
    uint32_t m_identityGeneration[ECUIO_MAX_CHANNELS] = {};
    uint32_t m_diagnosticsGeneration[ECUIO_MAX_CHANNELS] = {};
    // A clear asked of each channel, its outcome not yet logged, and the
    // generation it was asked at (cli::clear_settled)
    bool m_clearAsked[ECUIO_MAX_CHANNELS] = {};
    uint32_t m_clearAskedGeneration[ECUIO_MAX_CHANNELS] = {};
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
