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
        ArbitrationIdRole   /* appended (F1): additive, existing QML unaffected */
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
        SubscribedRole
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
    void updateValues(const std::vector<std::pair<uint16_t, float>>& values,
                      qint64 timestampMs = -1);
    int subscribedCount() const { return static_cast<int>(m_subscribedCount); }

    Q_INVOKABLE void toggleSubscribed(int row);

    // Strip-chart pull API: one map per SUBSCRIBED signal with non-empty
    // history: { id, name, unit, value, samples: [ts, val, ts, val, ...] }
    // Flattened doubles, oldest-first. Called by the QML Canvas once per
    // valuesUpdated batch (~10 Hz). One call per repaint instead of per-row
    // role lookups keeps the QML side free of role-number hacks.
    Q_INVOKABLE QVariantList chartData() const;

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
        float value = 0.0f;
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
    Q_PROPERTY(int frameCount READ frameCount NOTIFY frameCountChanged)
    Q_PROPERTY(int droppedFrames READ droppedFrames NOTIFY droppedFramesChanged)
    Q_PROPERTY(FrameSortProxy* model READ model CONSTANT)
    Q_PROPERTY(int guiNotifyCount READ guiNotifyCount NOTIFY notifyCountChanged)
    Q_PROPERTY(int signalBatchCount READ signalBatchCount NOTIFY signalBatchChanged)
    Q_PROPERTY(SignalListModel* signalModel READ signalModel CONSTANT)
    Q_PROPERTY(QString currentUri READ currentUri NOTIFY currentUriChanged)

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
    void dispatchFrameForTesting(const qttune_frame_t& frame);
    static constexpr int maxInFlightForTesting() { return MaxInFlightFrames; }

    Q_INVOKABLE QString statusString(int statusCode) const;
    Q_INVOKABLE int maxFrameCount() const { return FrameListModel::MaxFrames; }
    Q_INVOKABLE void forceFrameCountForTesting(int count);

    // J2534 device discovery (Windows; empty list elsewhere). One map per
    // installed Pass-Thru interface driver: { vendorName, dllPath } — both
    // QString. Registry read, no session attached; bounded at
    // QTTUNE_J2534_MAX_VENDORS by core. Safe to call synchronously from
    // QML (few reg keys, no I/O beyond the registry). UI sees display
    // values only (rule 2) — dllPath is exposed because it is the user's
    // route to a direct-load URI, not for parsing.
    Q_INVOKABLE QVariantList discoverJ2534Devices() const;

public slots:
    Q_INVOKABLE QString connectSession(const QString& uri);
    Q_INVOKABLE void disconnectSession();
    Q_INVOKABLE void clearFrames();
    // Async connect: pure-core work (create + register + start, the
    // DLL load and PassThruOpen included) runs on a worker thread; GUI
    // state application and the result signal arrive queued on the
    // GUI thread. Never blocks the UI — a J2534 handshake can legally
    // take seconds (Issue #22).
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
    void connectResult(const QString& uri, const QString& error);
    void logMessage(const QString& line);

private:
    static void onFrame(const qttune_frame_t* frame, void* user_data);
    void handleFrameInternal(const qttune_frame_t* frame);
    void notifyDropped();   // FIX: consolidated drop reporting, callable from worker path
    void destroySessionLocked();  // close-then-destroy in one place (F1)

    bool m_initialized = false;
    qttune_session_t* m_session = nullptr;
    QString m_currentUri;

    std::atomic<bool> m_connected{false};
    std::atomic<int> m_droppedFrames{0};
    int m_currentFrameCount = 0;      // GUI-thread-only (queued invocations)
    int m_lastReportedDropped = 0;     // GUI-thread-only companion to m_droppedFrames

    FrameListModel* m_sourceModel = nullptr;
    FrameSortProxy* m_sortProxy = nullptr;

    QTimer* m_coalesceTimer = nullptr;
    // FIX: latest-value-per-signal-ID staging (merge, not clear-and-refill)
    std::vector<std::pair<uint16_t, float>> m_stagingBuffer;
    int m_signalBatchCount = 0;

    SignalListModel* m_signalModel = nullptr;
    int m_guiNotifyCount = 0;

    std::atomic<int> m_inFlightFrames{0};   // posted-but-not-yet-processed count
    std::atomic<bool> m_dropNotifyPending{false};   // FIX: one queued drop-notify at a time
    static constexpr int MaxInFlightFrames = 1000; // queue backlog bound

    // Async connect support. m_connecting is written ONLY on the GUI
    // thread (in the async entry and the queued completion), so a plain
    // bool suffices despite the worker's existence.
    QString connectCoreOnly(const QString& uri, qttune_session_t** out_session);
    void applyConnectedState(qttune_session_t* session, const QString& uri);
    std::thread m_connectThread;
    bool m_connecting = false;
};
