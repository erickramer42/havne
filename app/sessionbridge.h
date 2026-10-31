// File/layer: app/sessionbridge.h | Qt6 Core, app layer, C++17
// Testing: test_bridge (Qt-linked target): the frame log's ring, order
//          and payload storage, SignalListModel's history ring and display units,
//          discoverJ2534Devices' and discoverSerialPorts' marshaling
//          shape, recentRecordings and replayUriFor
//          (BridgeTest.RecentRecordings*, .ReplayUriFor*), the delivery figures
//          (BridgeTest.DeliveryText*, .DisconnectLogsTheDelivery), the
//          vehicle's VIN key, label and plotted names (ADR-0027;
//          BridgeTest.VinFromIdentity*, .UnplottedNames*, …)

#pragma once

#include <QObject>
#include <QString>
#include <QAbstractListModel>
#include <QVariantList>
#include <QVariantMap>
#include <QTimer>
#include <QHash>
#include <QUrl>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "latencystats.h"
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

// QElapsedTimer's restart() and elapsed() on core's clock (ecuio_clock_us),
// which counts the time the machine sleeps (ADR-0033): a value read before
// a sleep is as old as it is at the wake, and dims then
class HostElapsedTimer
{
public:
    void restart() { m_startUs = ecuio_clock_us(); }
    qint64 elapsed() const
    {
        return static_cast<qint64>((ecuio_clock_us() - m_startUs) / 1000);
    }

private:
    uint64_t m_startUs = ecuio_clock_us();
};

class SignalListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int subscribedCount READ subscribedCount NOTIFY subscribedCountChanged)

public:
    explicit SignalListModel(QObject* parent = nullptr);

    // Rolling history depth per signal: strip chart window. A window
    // adds one to three samples (its lowest, highest and newest value,
    // ADR-0005's appendix), so this is about a 12 s trace at 10 Hz.
    // Kept constexpr so tests can assert exact ring behavior without a
    // config surface.
    static constexpr int HistoryCapacity = 360;

    // One signal's coalesce window, as the bridge's staging hands it
    // over: its newest value and its lowest and highest, each at its
    // frame's session time in ms
    struct WindowValue {
        uint16_t id = 0;
        double last = 0.0;
        qint64 lastMs = 0;
        double min = 0.0;
        qint64 minMs = 0;
        double max = 0.0;
        qint64 maxMs = 0;
    };

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
        IsTextRole,     /* the def's kind is a text kind: LiveView shows
                         * TextRole and never plots the row. Fixed per
                         * signal set. */
        WindowHighRole, /* the last window's highest and lowest value,
                         * displayed like ValueRole: the Dashboard's peak
                         * and low hold read them, so a peak that isn't
                         * a window's newest value still counts.
                         * Notified with ValueRole. */
        WindowLowRole
    };

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setSignalSet(const ecuio_signal_set_t* set);
    // The bridge's form: each signal's window. The row shows the newest
    // value, and the window's highest and lowest beside it; the history
    // takes the distinct points among the three, in time order, so a
    // peak inside a window stays on the chart at its own time.
    // Every row updates, subscribed or not: a subscription only chooses
    // what chartData() plots, and the Dashboard reads every signal.
    void updateWindow(const std::vector<WindowValue>& window);
    // The one-point form: each value its window's lowest, highest and
    // newest at timestampMs (< 0: the model's clock). Tests use it.
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
    // A vehicle's plotted signals (ADR-0027 §1): the names of the
    // numeric rows the chart leaves out, and setting them, which ticks
    // every other numeric row. Text rows are never plotted or listed.
    Q_INVOKABLE QStringList unplottedNames() const;
    Q_INVOKABLE void setUnplotted(const QStringList& names);

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
     * the internal clock, but dataChanged only fires on updateValues:
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
        double windowHigh = 0.0;  // the last window's highest and lowest
        double windowLow = 0.0;
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
        // When the last value arrived, on m_clock: ageMs counts from
        // here, since sample times are session time and can run faster
        // than the wall clock (a replay at 10x). -1 until one does.
        qint64 arrivedMs = -1;
    };

    static void pushSample(Entry& e, qint64 stamp, double value);
    static void pushWindow(Entry& e, const WindowValue& w);
    // A row's value as displayed in the current unit system
    ecuio_display_value_t shown(const Entry& e, double value) const;

    std::vector<Entry> m_entries;
    // Each signal id's row, so a batch finds its rows without a search.
    // A set's ids are unique (the validator refuses a duplicate).
    QHash<uint16_t, int> m_rowOfId;
    int m_subscribedCount = 0;
    uint32_t m_units = ECUIO_UNITS_METRIC;
    HostElapsedTimer m_clock;   // fallback stamps; restarted in setSignalSet
};

class SessionBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString coreVersion READ coreVersion CONSTANT)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY connectingChanged)
    Q_PROPERTY(bool disconnecting READ disconnecting NOTIFY disconnectingChanged)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY frameCountChanged)
    Q_PROPERTY(int droppedFrames READ droppedFrames NOTIFY droppedFramesChanged)
    // This session's frames that came after a loss at the adapter
    // (ECUIO_FRAME_FLAG_OVERFLOW: its buffer overflowed, frames were lost)
    Q_PROPERTY(int overflowFrames READ overflowFrames NOTIFY overflowFramesChanged)
    Q_PROPERTY(FrameListModel* model READ model CONSTANT)
    Q_PROPERTY(int guiNotifyCount READ guiNotifyCount NOTIFY notifyCountChanged)
    Q_PROPERTY(int signalBatchCount READ signalBatchCount NOTIFY signalBatchChanged)
    Q_PROPERTY(SignalListModel* signalModel READ signalModel CONSTANT)
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
    // session.
    Q_PROPERTY(bool recording READ recording WRITE setRecording NOTIFY recordingChanged)
    // The current or last recording, for display: relative to Documents
    // when it's there, so no account name shows. Empty before the first.
    Q_PROPERTY(QString recordingFolder READ recordingFolder NOTIFY recordingStateChanged)
    Q_PROPERTY(QUrl recordingFolderUrl READ recordingFolderUrl NOTIFY recordingStateChanged)
    Q_PROPERTY(bool recordingActive READ recordingActive NOTIFY recordingStateChanged)
    // The picked vehicle's label (ADR-0027 §2), "" for none: an adapter
    // session's recording folder is named "<stamp> <label>". Settings
    // sets it before each connect.
    Q_PROPERTY(QString vehicleLabel READ vehicleLabel WRITE setVehicleLabel NOTIFY vehicleLabelChanged)
    // Display units (ADR-0022): ECUIO_UNITS_METRIC (0) or _IMPERIAL (1).
    // Applies to what the UI shows at once, connected or not; recordings
    // keep the decoded values. Main.qml persists it.
    Q_PROPERTY(int unitSystem READ unitSystem WRITE setUnitSystem NOTIFY unitSystemChanged)
    // What each channel's ECU has reported of its diagnostics (ADR-0037),
    // one map per ECU (diagnosticsEntry), for the Diagnostics page:
    // refreshed on the tick as a read settles each item, and when the
    // unit system changes; emptied at connect, kept after disconnect
    Q_PROPERTY(QVariantList diagnostics READ diagnostics NOTIFY diagnosticsChanged)

public:
    explicit SessionBridge(QObject* parent = nullptr);
    ~SessionBridge() override;

    QString coreVersion() const;
    bool connected() const { return m_connected.load(std::memory_order_acquire); }
    int frameCount() const { return m_currentFrameCount; }
    int droppedFrames() const { return m_droppedFrames.load(std::memory_order_relaxed); }
    int overflowFrames() const { return m_reportedOverflow; }
    FrameListModel* model() const { return m_frameLog; }
    int guiNotifyCount() const { return m_guiNotifyCount; }
    int signalBatchCount() const { return m_signalBatchCount; }
    SignalListModel* signalModel() const { return m_signalModel; }
    QString currentUri() const { return m_currentUri; }
    QString uriLabel() const { return uriLabelFor(m_currentUri); }
    QString adapterVersions() const { return m_adapterVersions; }
    bool connecting() const { return m_connecting; }
    bool disconnecting() const { return m_disconnecting; }
    void dispatchFrameForTesting(const ecuio_frame_t& frame);
    // onValues on the calling thread, as core's worker calls it
    void dispatchValuesForTesting(const ecuio_value_batch_t& batch);
    // The samples one window can hold, from the next connect
    void setValueSampleLimitForTesting(int samples) { m_valueSampleLimit = samples; }
    static constexpr int defaultValueSampleLimit() { return DefaultValueSampleLimit; }
    static constexpr int maxInFlightForTesting() { return MaxInFlightFrames; }
    ecuio_session_t* pendingSessionForTesting() const { return m_pendingSession.load(); }
    QString linkState() const;
    static QString linkStateForError(int32_t errorCode);  /* code → state string; static so tests need no session */
    static QString uriLabelFor(const QString& uri);  /* a direct-load DLL or replay path cut to its last name, a Bluetooth address to its last two octets */
    static QString sessionInfoText(const ecuio_session_info_t& info);  /* the activity log's "Session:" line */
    static QString versionsText(const ecuio_session_info_t& info);  /* its versions clause; "" with none read */
    static QString channelSupportText(const ecuio_channel_support_t& support);  /* its "ECU 0x…:" lines */
    static QString channelIdentityText(const ecuio_channel_identity_t& identity);  /* its "ECU 0x… identity:" lines */
    static QString channelDiagnosticsText(const ecuio_channel_diagnostics_t& diagnostics);  /* its "ECU 0x… diagnostics:" lines */

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

    // J2534 device discovery (Windows; empty list elsewhere). One map per
    // installed Pass-Thru interface driver: { vendorName, dllPath,
    // architecture }: all QString. Registry read, no session attached; bounded at
    // ECUIO_J2534_MAX_VENDORS by core. Safe to call synchronously from
    // QML (few reg keys, no I/O beyond the registry). UI sees display
    // values only (rule 2): dllPath is exposed because it is the user's
    // route to a direct-load URI, not for parsing.
    Q_INVOKABLE QVariantList discoverJ2534Devices() const;
    // The machine's serial ports, for elm327:// (ADR-0026 §7): one map
    // per port, { name, device } as QStrings, in core's order. name is
    // what follows elm327://; device is the driver's device on Windows
    // ("\Device\BthModem0"), "" elsewhere. A registry read or a /dev
    // listing; no port is opened, so it's safe to call from QML.
    Q_INVOKABLE QVariantList discoverSerialPorts() const;
    // The machine's CAN interfaces, for socketcan:// (ADR-0032 §7), Linux
    // only: one map per interface, { name, kind, up, listenOnly, fd,
    // bitrate }, kind "can" or "vcan", bitrate in bit/s (0 for vcan). A
    // netlink read; nothing is opened. Empty elsewhere.
    Q_INVOKABLE QVariantList discoverCanInterfaces() const;
    // The newest recordings first, at most limit, for Settings to replay
    // by a tap: one map per folder holding a frames.csv, { title, uri,
    // source, profile } as QStrings. title is the folder's date and
    // label ("2026-10-28 14:30:12 · Daily driver"), uri its replay://
    // URI (replayUriFor), source the recorded URI as the pill shows it
    // (uriLabelFor), "" when events.log doesn't name one. Reads the
    // earlier product name's folder too, when it's another (ADR-0001's
    // appendix of 2026-10-28), newest first across both. A directory
    // listing and one short read per folder.
    Q_INVOKABLE QVariantList recentRecordings(int limit) const;
    // A recording's replay:// URI, decoding under the profile it was
    // recorded with: the recorded URI's profile= (with its did= under
    // uds-generic), else its scheme's default (mock:// mock, replay://
    // raw, an adapter obd2); raw when nothing was recorded. Static, so
    // tests need no files.
    static QString replayUriFor(const QString& folder,
                                const QString& recordedUri);
    int linkErrorCode() const { return m_linkErrorCode; }
    int errorEpisodes() const { return m_errorEpisodes; }
    bool logPaused() const { return m_logPaused; }
    void setLogPaused(bool paused);
    bool recording() const { return m_recording; }
    void setRecording(bool on);
    QString recordingFolder() const;
    QUrl recordingFolderUrl() const;
    bool recordingActive() const { return m_recorder.active(); }
    QString vehicleLabel() const { return m_vehicleLabel; }
    void setVehicleLabel(const QString& label);
    // The VIN (ADR-0027 §3) stays in the bridge: QML gets a salted
    // SHA-256 of it, as lowercase hex, and "" before vehicleIdentified()
    // or with no session. newSalt() is 16 random bytes as hex.
    Q_INVOKABLE QString vinKey(const QString& salt) const;
    Q_INVOKABLE QString newSalt() const;
    // A line in the activity log (and a recording's events.log), for the
    // app's own events: the vehicle lines of ADR-0027 §4
    Q_INVOKABLE void logEvent(const QString& line) { emit logMessage(line); }
    // The VIN in an identity report: 09 02's text, or 22 F190's when 09
    // 02 wasn't answered, letters and digits only, upper case, accepted
    // at 17 characters; "" otherwise. Static, so tests need no session.
    static QString vinFromIdentity(const ecuio_channel_identity_t& identity);
    // A label as a recording folder takes it: letters, digits, space,
    // -, _ and ., trimmed, at most 40 characters, no trailing dot
    static QString folderLabel(const QString& label);
    int unitSystem() const { return static_cast<int>(m_signalModel->unitSystem()); }
    void setUnitSystem(int units);   // anything but 0 or 1 is ignored
    // A quantity in a decoded unit, as the current unit system shows it:
    // the Dashboard's scale ends and zones are written once, in decoded
    // units, and converted here rather than in QML (rule 2).
    Q_INVOKABLE double displayValue(const QString& unit, double value) const;
    Q_INVOKABLE QString displayUnit(const QString& unit) const;
    QVariantList diagnostics() const { return m_diagnostics; }
    // Read again: every ECU's diagnostics asked once more
    // (ecuio_session_read_diagnostics); nothing without a session
    Q_INVOKABLE void readDiagnostics();
    // One ECU's report as the page shows it: ecu ("0x7E8"); asked,
    // complete; readiness (an outcome: "unasked", "answered", "refused",
    // "unanswered") and readinessNrc ("7F 01 12" when refused); mil,
    // dtcCount, compression; lists, three of {key ("stored", "pending",
    // "permanent"), outcome, nrc, codes (["P0301", …]), more (codes past
    // the held ones)}; monitors, [{id, name, complete}]; freeze, {outcome,
    // nrc, stored, code, values: [{name, value, unit, decimals}]}, each
    // value through the obd2 def that decoded it and the unit system.
    // Static, so test_bridge pins it without a session.
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
    // Test seam: a sleep of this long, added to what core's count says,
    // for the tick to find (no runner can sleep)
    void addSleptForTesting(uint64_t us) { m_sleptAddedForTesting += us; }

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
    // (ADR-0027 §3): vinKey() has an answer now
    void vehicleIdentified();
    void vehicleLabelChanged();
    void linkStateChanged();
    void logPausedChanged();
    void recordingChanged();
    void recordingStateChanged();
    void unitSystemChanged();
    void diagnosticsChanged();

private:
    static void onFrame(const ecuio_frame_t* frame, void* user_data);
    void handleFrameInternal(const ecuio_frame_t* frame, SessionRecorder::Clock::time_point arrived);
    // Core's values callback (ADR-0024): WORKER thread, stages a frame's
    // values under m_valuesMutex
    static void onValues(const ecuio_value_batch_t* batch, void* user_data);
    // GUI thread, before the session starts: sizes the staging for set
    void prepareValueStaging(const ecuio_signal_set_t* set);
    // GUI thread: swaps the staging out and drains it, every sample to
    // the recorder and each signal's newest into the model's batch
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
    void startRecording(const QString& uri, const ecuio_signal_set_t* set);
    void stopRecording(const QString& why);
    QString recordingSummary(const QString& why) const;
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
    // The machine's slept count at the last tick (ADR-0033 §6), and the
    // test seam's addition to it
    uint64_t m_sleptUs = 0;
    uint64_t m_sleptAddedForTesting = 0;
    uint64_t sleptUs() const
    {
        return ecuio_clock_slept_us() + m_sleptAddedForTesting;
    }

    FrameListModel* m_frameLog = nullptr;

    QTimer* m_coalesceTimer = nullptr;
    // The model's batch for one tick: each signal's window
    std::vector<SignalListModel::WindowValue> m_stagingBuffer;
    std::vector<std::pair<uint16_t, QString>> m_stagingTexts;   // the text rows'
    int m_signalBatchCount = 0;

    // One coalesce window of decoded values, written by onValues on the
    // worker. Sized at connect and never grown there: a sample past the
    // limit is counted, and only the recording misses it (a numeric
    // signal's newest value is kept regardless).
    struct ValueStaging {
        struct Sample {
            SessionRecorder::Clock::time_point arrived;
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
        // frame time in session µs
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
        void stage(uint32_t def, SessionRecorder::Clock::time_point arrived,
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
    void pollReplayEnd();   // the tick: "Replay finished" once, at the end of the file
    void pollSleep();       // the tick: "Asleep for N s" after a sleep
    uint32_t m_identityGeneration[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    // What each channel's ECU reported of its diagnostics (ADR-0037),
    // logged once per completed read
    void pollChannelDiagnostics();
    uint32_t m_diagnosticsGeneration[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    // The reports the page shows, as last read, and their entries
    ecuio_channel_diagnostics_t m_diagnosticsShown[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
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
    SessionRecorder m_recorder;
    QString m_recordingsRoot;          // empty: Documents/Havne/recordings
    QString m_earlierRecordingsRoot;   // empty: the earlier name's folder
    bool m_recordReplays = false;      // tests only
    bool m_setFrameLogging = false;    // the bridge, not the launch, set ECUIO_SUR_LOG_FRAMES
    std::function<QString(const QString&)>
        m_streamPreparer; // none: nothing to ready
};
