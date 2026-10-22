// File/layer: app/qttunebridge.h | Qt6 Core, app layer, C++17
// Testing: SignalListModel history ring covered by test_bridge (Qt-linked target);
//          discoverJ2534Devices marshaling shape covered by test_bridge

#pragma once

#include <QObject>
#include <QString>
#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QVariantList>
#include <QVariantMap>
#include <QTimer>
#include <QElapsedTimer>

#include <atomic>
#include <thread>
#include <utility>
#include <vector>

#include "qttune/core.h"
#include "qttune/signals.h"   

class FrameListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

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
    void appendFrame(const QVariantMap& frame);
    void clear();
    int count() const { return static_cast<int>(m_frames.size()); }

signals:
    void countChanged(int newCount);

private:
    std::vector<QVariantMap> m_frames;
};

class FrameSortProxy : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(bool newestFirst READ newestFirst WRITE setNewestFirst NOTIFY newestFirstChanged)

public:
    explicit FrameSortProxy(QObject* parent = nullptr);
    bool newestFirst() const;
    void setNewestFirst(bool newestFirst);

protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

signals:
    void newestFirstChanged();

private:
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
        DecimalsRole    /* the def's decimal_places: the one source of
                         * display precision for LiveView and the
                         * Dashboard. Fixed per signal set. */
    };

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setSignalSet(const QttuneSignalSet* set);
    // timestampMs < 0  => stamp from the internal clock (production path:
    // coalesce-drain time, ~100 ms resolution).
    // timestampMs >= 0  => use as-is (tests inject deterministic values; this
    // is also the seam for per-sample frame timestamps once decode moves to
    // the worker side under J2534).
    // Every row updates, subscribed or not: a subscription only chooses
    // what chartData() plots, and the Dashboard reads every signal.
    void updateValues(const std::vector<std::pair<uint16_t, float>>& values,
                      qint64 timestampMs = -1);
    int subscribedCount() const { return static_cast<int>(m_subscribedCount); }

    Q_INVOKABLE void toggleSubscribed(int row);

    // Strip-chart pull API: one map per SUBSCRIBED signal with non-empty
    // history: { id, row, name, unit, decimals, value,
    // samples: [ts, val, ts, val, ...] }
    // `row` is the signal's model row, so the trace colour can match the
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
        float value;
    };

    struct Entry {
        uint16_t id;
        char name[32];
        char unit[16];
        uint8_t decimals = 0;
        float value = 0.0f;
        // Whether the strip chart plots this signal. Values and history
        // update regardless: the Dashboard reads every row, and a signal
        // plotted again shows the history it kept meanwhile.
        bool subscribed = true;
        // GUI-thread-only rolling ring (written exclusively by updateValues).
        // Fixed capacity, evict-oldest — mirrors the FrameListModel ring
        // philosophy: bounded memory, stream never freezes.
        Sample history[HistoryCapacity];
        uint32_t histHead = 0;   // next write slot
        uint32_t histCount = 0; // valid samples, saturates at HistoryCapacity
    };

    std::vector<Entry> m_entries;
    int m_subscribedCount = 0;
    QElapsedTimer m_clock;   // fallback timestamp source; started in setSignalSet
};

class QtTuneBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString coreVersion READ coreVersion CONSTANT)
    Q_PROPERTY(bool coreInitialized READ coreInitialized NOTIFY coreInitializedChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY connectingChanged)
    Q_PROPERTY(bool disconnecting READ disconnecting NOTIFY disconnectingChanged)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY frameCountChanged)
    Q_PROPERTY(int droppedFrames READ droppedFrames NOTIFY droppedFramesChanged)
    Q_PROPERTY(FrameSortProxy* model READ model CONSTANT)
    Q_PROPERTY(int guiNotifyCount READ guiNotifyCount NOTIFY notifyCountChanged)
    Q_PROPERTY(int signalBatchCount READ signalBatchCount NOTIFY signalBatchChanged)
    Q_PROPERTY(SignalListModel* signalModel READ signalModel CONSTANT)
    Q_PROPERTY(QString currentUri READ currentUri NOTIFY currentUriChanged)
    Q_PROPERTY(QString linkState READ linkState NOTIFY linkStateChanged)
    Q_PROPERTY(int linkErrorCode READ linkErrorCode NOTIFY linkStateChanged)
    Q_PROPERTY(int errorEpisodes READ errorEpisodes NOTIFY linkStateChanged)
    // Log page Pause: frames still count and decode, but none are added
    // to the frame log. Every session starts unpaused.
    Q_PROPERTY(bool logPaused READ logPaused WRITE setLogPaused NOTIFY logPausedChanged)

public:
    explicit QtTuneBridge(QObject* parent = nullptr);
    ~QtTuneBridge() override;

    QString coreVersion() const;
    bool coreInitialized() const;
    bool connected() const { return m_connected.load(std::memory_order_acquire); }
    int frameCount() const { return m_currentFrameCount; }
    int droppedFrames() const { return m_droppedFrames.load(std::memory_order_relaxed); }
    FrameSortProxy* model() const { return m_sortProxy; }
    int guiNotifyCount() const { return m_guiNotifyCount; }
    int signalBatchCount() const { return m_signalBatchCount; }
    SignalListModel* signalModel() const { return m_signalModel; }
    QString currentUri() const { return m_currentUri; }
    bool connecting() const { return m_connecting; }
    bool disconnecting() const { return m_disconnecting; }
    void dispatchFrameForTesting(const qttune_frame_t& frame);
    static constexpr int maxInFlightForTesting() { return MaxInFlightFrames; }
    qttune_session_t* pendingSessionForTesting() const { return m_pendingSession.load(); }
    QString linkState() const;
    static QString linkStateForError(int32_t errorCode);  /* code → state string; static so tests need no session */

    Q_INVOKABLE QString statusString(int statusCode) const;
    Q_INVOKABLE int maxFrameCount() const { return FrameListModel::MaxFrames; }
    Q_INVOKABLE void forceFrameCountForTesting(int count);

    // J2534 device discovery (Windows; empty list elsewhere). One map per
    // installed Pass-Thru interface driver: { vendorName, dllPath,
    // architecture } — all QString. Registry read, no session attached; bounded at
    // QTTUNE_J2534_MAX_VENDORS by core. Safe to call synchronously from
    // QML (few reg keys, no I/O beyond the registry). UI sees display
    // values only (rule 2) — dllPath is exposed because it is the user's
    // route to a direct-load URI, not for parsing.
    Q_INVOKABLE QVariantList discoverJ2534Devices() const;
    int linkErrorCode() const { return m_linkErrorCode; }
    int errorEpisodes() const { return m_errorEpisodes; }
    bool logPaused() const { return m_logPaused; }
    void setLogPaused(bool paused);

public slots:
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
    void coreInitializedChanged();
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

private:
    static void onFrame(const qttune_frame_t* frame, void* user_data);
    void handleFrameInternal(const qttune_frame_t* frame);
    void notifyDropped();   // GUI thread: emit droppedFramesChanged if the count moved
    void destroySessionLocked();  // the destructor's teardown; disconnectSession tears down on its own thread

    bool m_initialized = false;
    qttune_session_t* m_session = nullptr;
    QString m_currentUri;

    std::atomic<bool> m_connected{false};
    std::atomic<int> m_droppedFrames{0};
    int m_currentFrameCount = 0;      // GUI-thread-only (queued invocations)
    int m_lastReportedDropped = 0;     // GUI-thread-only companion to m_droppedFrames
    bool m_logPaused = false;          // GUI-thread-only

    FrameListModel* m_sourceModel = nullptr;
    FrameSortProxy* m_sortProxy = nullptr;

    QTimer* m_coalesceTimer = nullptr;
    // Latest value per signal ID, flushed by the 100 ms coalesce timer
    std::vector<std::pair<uint16_t, float>> m_stagingBuffer;
    int m_signalBatchCount = 0;

    SignalListModel* m_signalModel = nullptr;
    int m_guiNotifyCount = 0;

    std::atomic<int> m_inFlightFrames{0};   // posted-but-not-yet-processed count
    std::atomic<bool> m_dropNotifyPending{false};   // at most one queued drop notification
    static constexpr int MaxInFlightFrames = 1000; // queue backlog bound

    // Async connect support. m_connecting is written ONLY on the GUI
    // thread (in the async entry and the queued completion), so a plain
    // bool suffices despite the worker's existence.
    QString connectCoreOnly(const QString& uri, qttune_session_t** out_session);
    QString startSession(qttune_session_t* session);
    void applyConnectedState(qttune_session_t* session, const QString& uri);
    std::thread m_connectThread;
    bool m_connecting = false;
    // The created, registered, NOT started session the connect thread
    // hands back. The queued completion takes it; if the bridge dies
    // first, the completion is dropped and the destructor destroys it.
    // Atomic only for the test seam: both owners read it after joining
    // the connect thread.
    std::atomic<qttune_session_t*> m_pendingSession{nullptr};
    // Async disconnect support, the same shape as connect: the thread
    // owns the session it tears down, and m_disconnecting is written
    // only on the GUI thread (in disconnectSession and its completion).
    std::thread m_disconnectThread;
    bool m_disconnecting = false;
    void pollLinkHealth();
    int m_linkErrorCode = QTTUNE_OK;
    uint32_t m_errorEpisodes = 0;
};
