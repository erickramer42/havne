// File/layer: app/qttunebridge.h | Qt6 Core, app layer, C++17
// Testing: test_bridge (Qt-linked target): the frame log's ring, order
//          and payload storage, SignalListModel's history ring and display units,
//          discoverJ2534Devices' marshaling shape

#pragma once

#include <QObject>
#include <QString>
#include <QAbstractListModel>
#include <QVariantList>
#include <QVariantMap>
#include <QTimer>
#include <QElapsedTimer>
#include <QUrl>

#include <atomic>
#include <thread>
#include <utility>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/signals.h"
#include "sessionrecorder.h"

// The Log page's frames, newest first by default. The last MaxFrames
// in a fixed ring of plain rows: each frame overwrites the oldest once
// it's full, so a frame costs the same at the cap as below it, and the
// hex is formatted only for the rows a view asks for. Row order is
// arrival, never the frame's timestamp, which comes from the producer's
// clock (on J2534, the adapter's deltas after the first frame).
class FrameListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool newestFirst READ newestFirst WRITE setNewestFirst NOTIFY newestFirstChanged)

public:
    explicit FrameListModel(QObject* parent = nullptr);
    static constexpr int MaxFrames = 10000;

    enum FrameRoles {
        TimestampRole = Qt::UserRole + 1,
        DlcRole,
        ExtendedRole,
        PayloadRole,
        ArbitrationIdRole   /* appended, so existing role numbers are unchanged */
    };

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    // len is the frame's data_length, already clamped to its buffer
    void appendFrame(const ecuio_frame_t& frame, uint16_t len);
    void clear();
    int count() const { return m_count; }
    bool newestFirst() const { return m_newestFirst; }
    void setNewestFirst(bool newestFirst);

    // A payload up to this many bytes lives in its row: every reply the
    // first vehicle gave (27 bytes at most) and every mock frame
    static constexpr int InlinePayload = 64;

    // Test seams: the ring's slots, and the heap bytes the rows hold
    size_t slotsForTesting() const { return m_ring.size(); }
    size_t spilledBytesForTesting() const;

signals:
    void countChanged(int newCount);
    void newestFirstChanged();

private:
    struct Row {
        quint64 timestampUs = 0;
        quint32 arbId = 0;
        quint16 length = 0;
        bool extended = false;
        uint8_t inlineData[InlinePayload] = {};
        // Only a payload past InlinePayload; a short frame taking the
        // slot frees it, so long replies hold memory only while shown
        std::vector<uint8_t> spill;
        const uint8_t* data() const
        {
            return spill.empty() ? inlineData : spill.data();
        }
    };

    const Row& rowAt(int row) const;

    // Grows to MaxFrames and keeps its slots across clear(), so the ring
    // stops allocating once it has gone round
    std::vector<Row> m_ring;
    size_t m_oldest = 0;       // the oldest row's slot
    int m_count = 0;           // rows shown, at most m_ring.size()
    bool m_newestFirst = true;
};

class SignalListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int subscribedCount READ subscribedCount NOTIFY subscribedCountChanged)

public:
    explicit SignalListModel(QObject* parent = nullptr);

    // Rolling history depth per signal — strip chart window.
    // At 10 Hz coalesced batches this is a ~12 s trace. Kept constexpr so
    // tests can assert exact ring behavior without a config surface.
    static constexpr int HistoryCapacity = 120;

    enum SignalRoles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        UnitRole,
        ValueRole,
        SubscribedRole,
        HasValueRole,   /* false until the first sample, so QML can
                         * show "—" instead of a fake 0 for an
                         * unsupported PID. Notified per batch;
                         * LiveView binds it. */
        AgeMsRole,      /* -1 = never arrived; else ms since the last
                         * sample, for dimming stale values. Notified
                         * per batch and on every coalesce tick via
                         * refreshAges(); LiveView binds it. */
        DecimalsRole,   /* the def's decimal_places: the one source of
                         * display precision for LiveView and the
                         * Dashboard. Fixed per signal set. */
        TextRole,       /* a text signal's latest value: TEXT shows the
                         * bytes as ASCII, HEX as hex pairs. "" for a
                         * numeric signal. Notified per batch. */
        IsTextRole      /* the def's kind is a text kind: LiveView shows
                         * TextRole and never plots the row. Fixed per
                         * signal set. */
    };

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setSignalSet(const ecuio_signal_set_t* set);
    // timestampMs < 0  => stamp from the internal clock (production path:
    // coalesce-drain time, ~100 ms resolution).
    // timestampMs >= 0  => use as-is (tests inject deterministic values; this
    // is also the seam for per-sample frame timestamps once decode moves to
    // the worker side under J2534).
    // Every row updates, subscribed or not: a subscription only chooses
    // what chartData() plots, and the Dashboard reads every signal.
    void updateValues(const std::vector<std::pair<uint16_t, double>>& values,
                      qint64 timestampMs = -1);
    // The text rows' counterpart: each id's latest text. The sample's
    // time goes into the row's history for hasValue and ageMs; the
    // chart never plots a text row.
    void updateTexts(const std::vector<std::pair<uint16_t, QString>>& texts,
                     qint64 timestampMs = -1);
    int subscribedCount() const { return static_cast<int>(m_subscribedCount); }

    // Display units (ADR-0022): rows and history keep the decoded value;
    // value, unit, decimals and chartData() convert on the way out
    // through ecuio_display_convert, so a change redraws everything at
    // once and nothing recorded changes. ECUIO_UNITS_METRIC or _IMPERIAL.
    uint32_t unitSystem() const { return m_units; }
    void setUnitSystem(uint32_t units);

    Q_INVOKABLE void toggleSubscribed(int row);

    // Strip-chart pull API: one map per SUBSCRIBED signal with non-empty
    // history: { id, row, name, unit, decimals, value,
    // samples: [ts, val, ts, val, ...] }
    // `row` is the signal's model row, so the trace color can match the
    // list row's swatch and stays put when other signals are unsubscribed.
    // Samples are flattened doubles, oldest-first. Called by the QML
    // Canvas once per valuesUpdated batch (~10 Hz). One call per repaint
    // instead of per-row role lookups keeps the QML side free of
    // role-number hacks.
    Q_INVOKABLE QVariantList chartData() const;

    /* Periodic age refresh: ageMs is computed lazily from
     * the internal clock, but dataChanged only fires on updateValues —
     * so a stalled signal's row never re-evaluates and never dims, the
     * exact row dimming exists for. Emits dataChanged over ALL rows for
     * AgeMsRole ONLY: views re-query data(), which recomputes elapsed
     * time. No value/hasValue churn, no reset. */
    void refreshAges();

signals:
    void valuesUpdated();
    void subscribedCountChanged();

private:
    struct Sample {
        qint64 timestampMs;
        double value;
    };

    struct Entry {
        uint16_t id;
        char name[32];
        char unit[16];
        uint8_t decimals = 0;
        double value = 0.0;
        QString text;             // a text row's latest value
        bool isText = false;      // the def's kind is TEXT or HEX
        // Whether the strip chart plots this signal. Values and history
        // update regardless: the Dashboard reads every row, and a signal
        // plotted again shows the history it kept meanwhile.
        bool subscribed = true;
        // GUI-thread-only rolling ring, written only by pushSample (from
        // updateValues and updateTexts). Fixed capacity, evict-oldest:
        // bounded memory, and the stream never freezes.
        Sample history[HistoryCapacity];
        uint32_t histHead = 0;   // next write slot
        uint32_t histCount = 0; // valid samples, saturates at HistoryCapacity
    };

    static void pushSample(Entry& e, qint64 stamp, double value);
    // A row's value as displayed in the current unit system
    ecuio_display_value_t shown(const Entry& e, double value) const;

    std::vector<Entry> m_entries;
    int m_subscribedCount = 0;
    uint32_t m_units = ECUIO_UNITS_METRIC;
    QElapsedTimer m_clock;   // fallback timestamp source; started in setSignalSet
};

class QtTuneBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString coreVersion READ coreVersion CONSTANT)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY connectingChanged)
    Q_PROPERTY(bool disconnecting READ disconnecting NOTIFY disconnectingChanged)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY frameCountChanged)
    Q_PROPERTY(int droppedFrames READ droppedFrames NOTIFY droppedFramesChanged)
    Q_PROPERTY(FrameListModel* model READ model CONSTANT)
    Q_PROPERTY(int guiNotifyCount READ guiNotifyCount NOTIFY notifyCountChanged)
    Q_PROPERTY(int signalBatchCount READ signalBatchCount NOTIFY signalBatchChanged)
    Q_PROPERTY(SignalListModel* signalModel READ signalModel CONSTANT)
    Q_PROPERTY(QString currentUri READ currentUri NOTIFY currentUriChanged)
    // currentUri as the header pill and the Dashboard show it (uriLabelFor)
    Q_PROPERTY(QString uriLabel READ uriLabel NOTIFY currentUriChanged)
    Q_PROPERTY(QString linkState READ linkState NOTIFY linkStateChanged)
    Q_PROPERTY(int linkErrorCode READ linkErrorCode NOTIFY linkStateChanged)
    Q_PROPERTY(int errorEpisodes READ errorEpisodes NOTIFY linkStateChanged)
    // Log page Pause: frames still count and decode, but none are added
    // to the frame log. Every session starts unpaused.
    Q_PROPERTY(bool logPaused READ logPaused WRITE setLogPaused NOTIFY logPausedChanged)
    // Session recording (ADR-0017), off by default: the Settings switch.
    // It takes effect at the next connect and can't change while a
    // session is up or in flight, so a recording always covers a whole
    // session.
    Q_PROPERTY(bool recording READ recording WRITE setRecording NOTIFY recordingChanged)
    // The current or last recording, for display: relative to Documents
    // when it's there, so no account name shows. Empty before the first.
    Q_PROPERTY(QString recordingFolder READ recordingFolder NOTIFY recordingStateChanged)
    Q_PROPERTY(QUrl recordingFolderUrl READ recordingFolderUrl NOTIFY recordingStateChanged)
    Q_PROPERTY(bool recordingActive READ recordingActive NOTIFY recordingStateChanged)
    // Display units (ADR-0022): ECUIO_UNITS_METRIC (0) or _IMPERIAL (1).
    // Applies to what the UI shows at once, connected or not; recordings
    // keep the decoded values. Main.qml persists it.
    Q_PROPERTY(int unitSystem READ unitSystem WRITE setUnitSystem NOTIFY unitSystemChanged)

public:
    explicit QtTuneBridge(QObject* parent = nullptr);
    ~QtTuneBridge() override;

    QString coreVersion() const;
    bool connected() const { return m_connected.load(std::memory_order_acquire); }
    int frameCount() const { return m_currentFrameCount; }
    int droppedFrames() const { return m_droppedFrames.load(std::memory_order_relaxed); }
    FrameListModel* model() const { return m_frameLog; }
    int guiNotifyCount() const { return m_guiNotifyCount; }
    int signalBatchCount() const { return m_signalBatchCount; }
    SignalListModel* signalModel() const { return m_signalModel; }
    QString currentUri() const { return m_currentUri; }
    QString uriLabel() const { return uriLabelFor(m_currentUri); }
    bool connecting() const { return m_connecting; }
    bool disconnecting() const { return m_disconnecting; }
    void dispatchFrameForTesting(const ecuio_frame_t& frame);
    static constexpr int maxInFlightForTesting() { return MaxInFlightFrames; }
    ecuio_session_t* pendingSessionForTesting() const { return m_pendingSession.load(); }
    QString linkState() const;
    static QString linkStateForError(int32_t errorCode);  /* code → state string; static so tests need no session */
    static QString uriLabelFor(const QString& uri);  /* a direct-load DLL path cut to its file name */
    static QString sessionInfoText(const ecuio_session_info_t& info);  /* the activity log's "Session:" line */
    static QString channelSupportText(const ecuio_channel_support_t& support);  /* its "ECU 0x…:" lines */
    static QString channelIdentityText(const ecuio_channel_identity_t& identity);  /* its "ECU 0x… identity:" lines */

    Q_INVOKABLE QString statusString(int statusCode) const;
    Q_INVOKABLE int maxFrameCount() const { return FrameListModel::MaxFrames; }
    Q_INVOKABLE void forceFrameCountForTesting(int count);

    // J2534 device discovery (Windows; empty list elsewhere). One map per
    // installed Pass-Thru interface driver: { vendorName, dllPath,
    // architecture } — all QString. Registry read, no session attached; bounded at
    // ECUIO_J2534_MAX_VENDORS by core. Safe to call synchronously from
    // QML (few reg keys, no I/O beyond the registry). UI sees display
    // values only (rule 2) — dllPath is exposed because it is the user's
    // route to a direct-load URI, not for parsing.
    Q_INVOKABLE QVariantList discoverJ2534Devices() const;
    int linkErrorCode() const { return m_linkErrorCode; }
    int errorEpisodes() const { return m_errorEpisodes; }
    bool logPaused() const { return m_logPaused; }
    void setLogPaused(bool paused);
    bool recording() const { return m_recording; }
    void setRecording(bool on);
    QString recordingFolder() const;
    QUrl recordingFolderUrl() const;
    bool recordingActive() const { return m_recorder.active(); }
    int unitSystem() const { return static_cast<int>(m_signalModel->unitSystem()); }
    void setUnitSystem(int units);   // anything but 0 or 1 is ignored
    // A quantity in a decoded unit, as the current unit system shows it:
    // the Dashboard's scale ends and zones are written once, in decoded
    // units, and converted here rather than in QML (rule 2).
    Q_INVOKABLE double displayValue(const QString& unit, double value) const;
    Q_INVOKABLE QString displayUnit(const QString& unit) const;
    // Test seam: where recordings go instead of Documents/QtTune/recordings
    void setRecordingsRootForTesting(const QString& root) { m_recordingsRoot = root; }

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
    // queued on the GUI thread. Never blocks the UI — a J2534 handshake
    // can legally take seconds (the surrogate allows 8 s per stage).
    Q_INVOKABLE void connectSessionAsync(const QString& uri);

signals:
    void connectedChanged(bool);
    void frameCountChanged();
    void framesReset();
    void droppedFramesChanged(int);
    void notifyCountChanged(int newCount);
    void signalBatchChanged();
    void currentUriChanged();
    void connectingChanged();
    void disconnectingChanged();
    void connectResult(const QString& uri, const QString& error);
    void logMessage(const QString& line);
    void linkStateChanged();
    void logPausedChanged();
    void recordingChanged();
    void recordingStateChanged();
    void unitSystemChanged();

private:
    static void onFrame(const ecuio_frame_t* frame, void* user_data);
    void handleFrameInternal(const ecuio_frame_t* frame, SessionRecorder::Clock::time_point arrived);
    void applyFrameLoggingEnv();
    void startRecording(const QString& uri, const ecuio_signal_set_t* set);
    void stopRecording(const QString& why);
    QString recordingSummary(const QString& why) const;
    void notifyDropped();   // GUI thread: emit droppedFramesChanged if the count moved
    void notifyFrameCounts();   // GUI thread: frameCount and guiNotifyCount, if they moved
    void destroySessionLocked();  // the destructor's teardown; disconnectSession tears down on its own thread

    ecuio_session_t* m_session = nullptr;
    QString m_currentUri;

    std::atomic<bool> m_connected{false};
    std::atomic<int> m_droppedFrames{0};
    int m_currentFrameCount = 0;      // GUI-thread-only (queued invocations)
    int m_reportedFrameCount = 0;      // what QML was last told, and below
    int m_reportedNotifyCount = 0;
    int m_lastReportedDropped = 0;     // GUI-thread-only companion to m_droppedFrames
    bool m_logPaused = false;          // GUI-thread-only

    FrameListModel* m_frameLog = nullptr;

    QTimer* m_coalesceTimer = nullptr;
    // Latest value per signal ID, flushed by the 100 ms coalesce timer
    std::vector<std::pair<uint16_t, double>> m_stagingBuffer;
    std::vector<std::pair<uint16_t, QString>> m_stagingTexts;   // the text rows'
    std::vector<char> m_textScratch;   // ECUIO_DECODE_TEXT_MAX, for ecuio_decode_text
    int m_signalBatchCount = 0;

    SignalListModel* m_signalModel = nullptr;
    int m_guiNotifyCount = 0;

    std::atomic<int> m_inFlightFrames{0};   // posted-but-not-yet-processed count
    std::atomic<bool> m_dropNotifyPending{false};   // at most one queued drop notification
    static constexpr int MaxInFlightFrames = 1000; // queue backlog bound

    // Async connect support. m_connecting is written ONLY on the GUI
    // thread (in the async entry and the queued completion), so a plain
    // bool suffices despite the worker's existence.
    QString connectCoreOnly(const QString& uri, ecuio_session_t** out_session);
    QString startSession(ecuio_session_t* session);
    void applyConnectedState(ecuio_session_t* session, const QString& uri);
    std::thread m_connectThread;
    bool m_connecting = false;
    // The created, registered, NOT started session the connect thread
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
    uint32_t m_supportGeneration[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    // What each channel's ECU said it is (ADR-0021), logged once complete
    void pollChannelIdentity();
    uint32_t m_identityGeneration[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};

    bool m_recording = false;
    SessionRecorder m_recorder;
    QString m_recordingsRoot;          // empty: Documents/QtTune/recordings
    bool m_setFrameLogging = false;    // the bridge, not the launch, set ECUIO_SUR_LOG_FRAMES
};
