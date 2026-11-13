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
#include "signalstore.h"
#include "framefilter.h"
#include "recordingimport.h"
#include "signalfilter.h"
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
#include "latencystats.h"

// ecuio_clock_us as a chrono clock: the bridge stamps a frame's and a
// value's arrival with it, for the delivery figures
struct HostClock {
    using duration = std::chrono::microseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<HostClock>;
    static constexpr bool is_steady = true;
    static time_point now() noexcept
    {
        return time_point(duration(static_cast<rep>(ecuio_clock_us())));
    }
};

// The Log page's frames, newest first by default. The last MaxFrames
// in a fixed ring of plain rows: each frame overwrites the oldest once
// it's full, so a frame costs the same at the cap as below it, and the
// hex is formatted only for the rows a view asks for. Row order is
// arrival, never the frame's timestamp, which comes from the producer's
// clock (on J2534, the adapter's deltas after the first frame).
// Rows can be selected for copying: the selection holds frames, not row
// numbers, so it stays on them as rows arrive, the order flips or the
// ring wraps, and a frame leaving the ring leaves the selection.
// A filter (FrameFilter) shows only the frames it passes: the rows are
// then those frames' arrival numbers, kept as frames come and go, so a
// frame costs the same filtered as not; count is the rows shown and
// heldCount the frames the ring holds.
class FrameListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int heldCount READ heldCount NOTIFY countChanged)
    Q_PROPERTY(int filterTerms READ filterTerms NOTIFY filterChanged)
    Q_PROPERTY(bool newestFirst READ newestFirst WRITE setNewestFirst NOTIFY newestFirstChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)

public:
    explicit FrameListModel(QObject* parent = nullptr);
    static constexpr int MaxFrames = 10000;

    enum FrameRoles {
        TimestampRole = Qt::UserRole + 1,
        DlcRole,
        ExtendedRole,
        PayloadRole,
        ArbitrationIdRole,  /* appended, so existing role numbers are unchanged */
        SelectedRole,
        FlagsRole,          /* the frame's ECUIO_FRAME_FLAG_* */
        FlagsTextRole       /* flagWords(): "FD reassembled", "" for none */
    };

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    // len is the frame's data_length, already clamped to its buffer
    void appendFrame(const ecuio_frame_t& frame, uint16_t len);
    void clear();
    // The rows shown: every frame held, or those the filter passes
    int count() const;
    int heldCount() const { return m_count; }
    bool newestFirst() const { return m_newestFirst; }
    void setNewestFirst(bool newestFirst);

    // The filter (framefilter.h's syntax): the ID and data fields' text,
    // FrameFilter::Direction, FrameFilter::Flag, and the length and time
    // fields' text. A field that doesn't parse keeps its part of the
    // filter in force as it was; the answer is { ids, data, lengths,
    // times }, each "" or the words for its field. A change shows the
    // rows anew and lets the selection go
    Q_INVOKABLE QVariantMap setFilter(const QString& ids, const QString& data,
                                      int direction, int flag,
                                      const QString& lengths = QString(),
                                      const QString& times = QString());
    int filterTerms() const { return m_filter.terms(); }
    // The flags a row shows, in words, the ID's width left to its column:
    // "FD BRS", "reassembled", "truncated", "after a loss", "error",
    // "sent", "remote"; "" for none
    static QString flagWords(uint32_t flags);

    // The frames the filter passes, oldest first, as the By ID view and an
    // export read them
    struct Shown {
        uint32_t id;
        uint32_t flags;
        quint64 timestampUs;
        const uint8_t* data;
        int length;
    };
    template <typename F> void forEachShown(F&& f) const
    {
        for (int age = 0; age < m_count; ++age) {
            const Row& r = m_ring[(m_oldest + static_cast<size_t>(age)) % m_ring.size()];
            if (!m_filter.active() || shows(r)) {
                f(Shown{r.arbId, r.flags, r.timestampUs, r.data(), r.length});
            }
        }
    }
    // The newest frame's time, 0 with none held
    quint64 newestTimestampUs() const;

    // The text and hex defs of each session's set, copied and kept, so an
    // export after the session ends still leaves their messages out
    void noteTextDefs(const ecuio_signal_set_t* set);
    // Export CSV (ADR-0054's appendix "frames in memory"): a copy of the
    // frames held through ecuio_scrub_frames, then the kept ones the
    // filter shows, oldest first. One comment line of counts, then
    // time_s,id,ext,dir,dlc,flags,data. A failed check is ok false with
    // core's words, never a value, and no bytes
    struct ScrubbedCsv {
        bool ok = false;
        QString words;
        QByteArray bytes;
        int written = 0;
        quint64 identification = 0;
        quint64 textValues = 0;
        quint64 unattributed = 0;
    };
    ScrubbedCsv scrubbedCsv() const;

    // The selection, by view row. A tap toggles a row and makes it the
    // anchor; a shift-click selects every row from the anchor to its own
    // (a toggle when the anchor has left the ring)
    int selectedCount() const { return static_cast<int>(m_selected.size()); }
    // A view row's arrival number; -1 for a row that isn't there. A table
    // keeps its place by it: rows come and go while it's hidden, and a
    // frame's time can step back (a vendor's stamp that wraps)
    Q_INVOKABLE double seqAt(int row) const;
    // The view row of that arrival, or with it gone or filtered out the
    // next row in the view's order (the first to arrive after it oldest
    // first, before it newest first), the last row past them all; -1 with
    // no rows
    Q_INVOKABLE int rowAtSeq(double seq) const;
    Q_INVOKABLE void toggleSelected(int row);
    Q_INVOKABLE void selectThrough(int row);
    Q_INVOKABLE void clearSelection();
    // The selected rows in view order, a header line first, one line
    // each, tab-separated: time in seconds to the microsecond, DLC, "E"
    // for an extended ID or "-", the ID in hex and the payload. Empty
    // with nothing selected. The page puts it on the clipboard
    Q_INVOKABLE QString selectedText() const;

    // A payload up to this many bytes lives in its row: every reply the
    // first vehicle gave (27 bytes at most) and every mock frame
    static constexpr int InlinePayload = 64;

    // Test seams: the ring's slots, and the heap bytes the rows hold
    size_t slotsForTesting() const { return m_ring.size(); }
    size_t spilledBytesForTesting() const;

signals:
    void countChanged(int newCount);
    void newestFirstChanged();
    void selectionChanged();
    void filterChanged();

private:
    struct Row {
        quint64 seq = 0;   // arrival number: the ring's rows run consecutively
        quint64 timestampUs = 0;
        quint32 arbId = 0;
        quint32 flags = 0; // ECUIO_FRAME_FLAG_*
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
    // The view row of the frame that arrived seq-th; -1 when it's gone
    // or the filter doesn't show it
    int rowOfSeq(quint64 seq) const;
    void notifySelected(int first, int last);
    bool shows(const Row& r) const
    {
        return m_filter.matches(r.arbId, r.flags, r.timestampUs, r.data(),
                                r.length);
    }
    // The ring's row for an arrival number it holds
    const Row& rowOfArrival(quint64 seq) const
    {
        const quint64 oldestSeq = m_nextSeq - static_cast<quint64>(m_count);
        return m_ring[(m_oldest + static_cast<size_t>(seq - oldestSeq)) % m_ring.size()];
    }

    // Grows to MaxFrames and keeps its slots across clear(), so the ring
    // stops allocating once it has gone round
    std::vector<Row> m_ring;
    size_t m_oldest = 0;       // the oldest row's slot
    int m_count = 0;           // rows shown, at most m_ring.size()
    bool m_newestFirst = true;
    quint64 m_nextSeq = 0;     // the next frame's arrival number
    std::set<quint64> m_selected;   // arrival numbers; ordered, so a
                                    // frame leaving the ring is the first
    quint64 m_anchorSeq = 0;
    bool m_hasAnchor = false;
    FrameFilter m_filter;
    // While the filter is active, the arrival numbers of the frames it
    // shows, oldest first
    std::deque<quint64> m_shown;
    // noteTextDefs's copies, each once
    std::vector<ecuio_signal_def_t> m_textDefs;
};

// The Log page's By ID view: one row per ID (and extended or not) over
// the frames FrameListModel shows: how many the table holds, the rate
// over its last 10 s, and the newest payload. refresh() reads the frames
// again, which the page does twice a second while the view shows; rows
// keep their places, an ID that comes or goes inserted or removed, so
// the view doesn't jump.
class FrameIdModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit FrameIdModel(const FrameListModel* frames, QObject* parent = nullptr);

    // The window the rate counts over
    static constexpr quint64 RateWindowUs = 10000000;

    enum IdRoles {
        ArbIdRole = Qt::UserRole + 1,
        ExtendedRole,
        CountRole,
        RateRole,    // frames a second, a double
        PayloadRole, // the newest payload, spaced hex
        ChangedRole  // the newest payload's byte positions that differ from
                     // the frame before it (a list of ints; past the
                     // shorter payload, every byte); none with one frame
    };

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return static_cast<int>(m_rows.size()); }
    Q_INVOKABLE void refresh();

signals:
    void countChanged();

private:
    struct IdRow {
        quint64 key = 0;    // extended in bit 32, then the ID: the sort order
        int count = 0;
        double rate = 0.0;
        QByteArray payload; // the newest
        QVariantList changed;
    };
    const FrameListModel* m_frames;
    std::vector<IdRow> m_rows;
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
    // The plotted rows Live shows: hidden rows aren't counted
    Q_PROPERTY(int subscribedCount READ subscribedCount NOTIFY subscribedCountChanged)
    // The rows Live leaves out: unsupported or absent (rejudgeHidden)
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY hiddenCountChanged)

public:
    explicit SignalListModel(QObject* parent = nullptr);

    // Rolling history depth per signal: room for the strip chart's
    // longest window, 120 s. A 100 ms window adds one to three samples
    // (its lowest, highest and newest value, ADR-0005's appendix), and a
    // signal refreshing at 20 Hz or less adds at most two, so 120 s of it
    // fits; a faster one keeps a shorter span. Kept constexpr so tests
    // can assert exact ring behavior without a config surface.
    static constexpr int HistoryCapacity = 2400;

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
                         * show "—" instead of a fake 0 for a PID
                         * not yet answered or refused. Notified per batch;
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
        WindowLowRole,
        RowRole,         /* the row in this model, which toggleSubscribed
                         * takes and chartData's `row` names, whatever
                         * row a filtered view shows it at */
        HiddenRole,      /* core reports it unsupported or absent
                         * (applyUnsupported, applyAbsent): Live
                         * leaves the row out */
        GroupRole,       /* the def's group (ECUIO_SIGNAL_GROUP_*,
                         * ADR-0050), for Live's filter. Fixed per
                         * signal set. */
        LabelRole,       /* an enum row's label for its newest raw value
                         * (ADR-0051), from the set's enums; "" for a
                         * value with none, which shows its number, and
                         * for any other kind. Notified with ValueRole. */
        RefusedRole,     /* the words of its ECU's negative reply to the
                         * request that reads it ("Refused:
                         * requestOutOfRange (7F 22 31)", applyRefused,
                         * ADR-0051 section 3); "" while none refused it */
        RefusedCodeRole, /* that reply's bytes alone ("7F 22 31"), for a
                         * row too narrow for the words; notified with
                         * RefusedRole */
        RefRole,         /* the def's reference across sets (ADR-0057):
                         * "id:<n>" for a def of the mock or obd2 set
                         * or of a file's extends base, "key:<k>" for a
                         * file's own def, "" for any other set's
                         * (uds-generic's). Fixed per signal set. */
        RangeLowRole,    /* the lowest and highest value the def's field
                         * decodes to (ecuio_def_range), displayed like
                         * ValueRole. NaN means no range (core gives none:
                         * a float, text or computed def, a range of no
                         * width or a non-finite end); QML tests it with
                         * isNaN. NaN isn't JSON (QJsonValue writes null,
                         * which reads back as 0), so a layout file never
                         * stores an end as is. Notified with UnitRole. */
        RangeHighRole,
        IsEnumRole /* the def's kind is ENUM. Fixed per signal set. */
    };

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    // file: the signal file whose set it is (SessionBridge::fileOfSet),
    // for the rows' refs; nullptr for a built-in set or none
    void setSignalSet(const ecuio_signal_set_t* set,
                      const ecuio_signal_file_t* file = nullptr);
    // A def's fixed facts beyond the def itself, the same for a row and
    // for SessionBridge::profileSignals: its ref (RefRole), whether it
    // is an enum, and its decoded range in the def's unit, NaN for none
    struct DefFacts {
        QString ref;
        double rangeLow = 0.0;
        double rangeHigh = 0.0;
        bool isEnum = false;
    };
    // builtIn: set is the registered mock or obd2 set (isBuiltIn),
    // found once per set by the caller
    static DefFacts factsOf(const ecuio_signal_file_t* file, bool builtIn,
                            const ecuio_signal_def_t& def);
    static bool isBuiltIn(const ecuio_signal_set_t* set);
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
    int hiddenCount() const { return m_hiddenCount; }
    // The ids of the rows the chart plots and Live shows, in the model's
    // order: subscribedCount of them, the focus Start log asks
    std::vector<uint16_t> plottedIds() const;
    // Each number's and enumeration's newest value as core decoded it
    // (the def's unit, an enumeration's raw number; never the display's
    // conversion), aged by the model's clock from the window that brought
    // it, for the health report: core keeps none. Rows with no value yet
    // and text rows are left out
    std::vector<ecuio_health_value_t> heldValues() const;

    // The signals core reports their ECU doesn't support
    // (ecuio_session_get_unsupported_signals, ADR-0039 section 6): those
    // rows are hidden. Each call judges every row afresh, so a rescan
    // that finds a PID shows its row again. A new set shows all.
    void applyUnsupported(const std::vector<uint16_t>& ids);
    // The signals core reports its frames come without
    // (ecuio_session_get_absent_signals): those rows are hidden too, and
    // the rest are judged by their support alone. A new set shows all.
    void applyAbsent(const std::vector<uint16_t>& ids);
    // The signals their ECU refused with a negative reply, each with the
    // refusal's words (cli/refused_signals.h, from the channels' support
    // reports): Live shows the words in place of a value, never a blank.
    // Each call judges every row afresh; a new set refuses none. A
    // refused row gives its place in the default plot to the next, as a
    // hidden one does: it never has a value to draw.
    struct Refusal {
        uint16_t id = 0;
        QString words; // "Refused: requestOutOfRange (7F 22 31)"
        QString code;  // "7F 22 31"
    };
    void applyRefused(const std::vector<Refusal>& refused);

    // Display units (ADR-0022): rows and history keep the decoded value;
    // value, unit, decimals and chartData() convert on the way out
    // through ecuio_display_convert, so a change redraws everything at
    // once and nothing recorded changes. ECUIO_UNITS_METRIC or _IMPERIAL.
    uint32_t unitSystem() const { return m_units; }
    void setUnitSystem(uint32_t units);

    // How many rows a ranked set plots by default (ADR-0039 section 8):
    // the shown numeric rows with the lowest plot_rank. A set that ranks
    // none plots every numeric row.
    static constexpr int DefaultPlotted = 6;

    // A tick or untick on Live. One that matches the row's default
    // clears the row's choice, so the row follows its default again.
    Q_INVOKABLE void toggleSubscribed(int row);
    // A vehicle's plotted signals (ADR-0027 section 1): the names of the
    // numeric rows the user ticked past the default and unticked from
    // it, and setting both, which leaves every other numeric row to its
    // default. A name in both lists is unplotted. Text rows are never
    // plotted or listed.
    Q_INVOKABLE QStringList plottedNames() const;
    Q_INVOKABLE QStringList unplottedNames() const;
    Q_INVOKABLE void setPlotChoices(const QStringList& plotted, const QStringList& unplotted);

    // Strip-chart pull API: one map per subscribed signal with history,
    // { id, row, name, unit, decimals, value, samples: [ts, val, ...] },
    // samples flattened doubles, oldest first. windowMs > 0 keeps the last
    // windowMs before the newest plotted sample; columns > 0 keeps each of
    // that many equal slices' lowest and highest (and always the newest),
    // so a long window hands the chart at most what it can draw. `row` is
    // the model row, so the trace's color matches the list's swatch and
    // stays put as others are unsubscribed. The QML Canvas calls it once
    // per valuesUpdated batch (~10 Hz), one call per repaint rather than
    // role lookups per row.
    Q_INVOKABLE QVariantList chartData(qint64 windowMs = 0, int columns = 0) const;

    /* Periodic age refresh: ageMs is computed from the clock at data(), but
     * dataChanged fires only on updateValues, so a stalled row would never
     * dim. Emits dataChanged over every row for AgeMsRole alone (no value
     * or hasValue churn, no reset). */
    void refreshAges();

signals:
    void valuesUpdated();
    void subscribedCountChanged();
    void hiddenCountChanged();

private:
    struct Sample {
        qint64 timestampMs;
        double value;
    };

    enum class PlotChoice : uint8_t { Default, Plotted, Unplotted };

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
        DefFacts facts;           // ref, range and enum-ness (factsOf)
        // An ENUM def's labels by raw value, copied from the set when it
        // loads, so a row keeps them whatever happens to the set later
        std::vector<std::pair<uint32_t, QString>> labels;
        bool unsupported = false; // core: its ECU doesn't support it
        bool absent = false;      // core: its frames come without it
        bool hidden = false;      // either: Live leaves the row out
        QString refused;          // its ECU's refusal in words, or ""
        QString refusedCode;      // its reply's bytes, or ""
        uint8_t plotRank = 0;     // the def's plot_rank; 0 unranked
        uint8_t group = 0;        // the def's group (ADR-0050)
        // The user's tick or untick; Default follows plotDefault
        PlotChoice choice = PlotChoice::Default;
        bool plotDefault = false; // judgePlotted's default for the row
        // Whether the strip chart plots this signal: the choice, else the
        // default. Values and history update regardless: the Dashboard
        // reads every row, and a signal plotted again shows the history
        // it kept meanwhile.
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

    // Hidden is unsupported or absent: re-judges every row, and notifies
    // the rows that changed, the hidden count and the plotted count
    void rejudgeHidden();
    // Each row's default and so its subscribed state, and the plotted
    // count; notifies the rows that changed and the count. Run after
    // anything that moves the defaults: a new set, hidden or refused
    // rows, choices.
    void judgePlotted();
    static void pushSample(Entry& e, qint64 stamp, double value);
    // An enum row's label for its newest value, "" when it has none
    static QString labelOf(const Entry& e);
    static void pushWindow(Entry& e, const WindowValue& w);
    // A row's value as displayed in the current unit system
    ecuio_display_value_t shown(const Entry& e, double value) const;
    // A row's range as displayed (displayedRange)
    std::pair<double, double> shownRange(const Entry& e) const;

    std::vector<Entry> m_entries;
    // Each signal id's row, so a batch finds its rows without a search.
    // A set's ids are unique (the validator refuses a duplicate).
    QHash<uint16_t, int> m_rowOfId;
    int m_subscribedCount = 0;
    int m_hiddenCount = 0;
    bool m_ranked = false;      // the set ranks a def (plot_rank)
    uint32_t m_units = ECUIO_UNITS_METRIC;
    HostElapsedTimer m_clock;   // fallback stamps; restarted in setSignalSet
};

// Live's list: SignalListModel without its hidden rows (ADR-0039 section 6).
// The Dashboard reads the whole model, so a gauge keeps its row either
// way; a row keeps its model row (RowRole) for its plot box and color.
// Its filter (ADR-0050) narrows the rows further, a row shown or not
// by SignalFilter's rule; its group choices come from the rows
// core leaves in.
class LiveSignalModel : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(SignalFilter* filter READ filter CONSTANT)
    // The groups' codes among the rows core leaves in, for the choices
    Q_PROPERTY(QVariantList groups READ groups NOTIFY groupsChanged)
    // The rows core leaves in, before the filter: what "of N" counts
    Q_PROPERTY(int shownCount READ shownCount NOTIFY groupsChanged)

public:
    explicit LiveSignalModel(SignalListModel* source, QObject* parent = nullptr);

    SignalFilter* filter() const { return m_filter; }
    QVariantList groups() const { return m_groups; }
    int shownCount() const { return m_shownCount; }

signals:
    void groupsChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    void refilter();
    void judgeGroups();

    SignalFilter* m_filter;
    QVariantList m_groups;
    int m_shownCount = 0;
};

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
    // The picked vehicle's label (ADR-0027 section 2), "" for none: an adapter
    // session's recording folder is named "<stamp> <label>". Settings
    // sets it before each connect.
    Q_PROPERTY(QString vehicleLabel READ vehicleLabel WRITE setVehicleLabel NOTIFY vehicleLabelChanged)
    // Display units (ADR-0022): ECUIO_UNITS_METRIC (0) or _IMPERIAL (1).
    // Applies to what the UI shows at once, connected or not; recordings
    // keep the decoded values. Main.qml persists it.
    Q_PROPERTY(int unitSystem READ unitSystem WRITE setUnitSystem NOTIFY unitSystemChanged)
    // A log's minimum time between sweeps (ADR-0056 section 4), ms, 0 off;
    // Settings' Advanced page and Main.qml, which keeps it
    Q_PROPERTY(int minSweepMs READ minSweepMs WRITE setMinSweepMs NOTIFY
                   minSweepMsChanged)
    Q_PROPERTY(QVariantList minSweepChoices READ minSweepChoices CONSTANT)
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
    // The vehicles' signal files (ADR-0051, SignalStore). Import reads
    // the file at url (a local file, or Android's content:// through
    // QFile) and keeps a copy when core's loader reads it: { ok, words,
    // sha256, name, profile, extends, count, own, kept }, words never
    // naming the path.
    // It registers nothing; the vehicle book says whether the file may
    // join (VehicleBook.profileProblem), then useSignalFiles registers
    // the files the book references. Each logs one line.
    Q_INVOKABLE QVariantMap importSignalFile(const QUrl& url);
    // The form's file (ADR-0051's form): its text into the store as an
    // import's bytes, the same result; and a kept copy's text back, ""
    // for none. Settings composes the file, core's loader judges it
    Q_INVOKABLE QVariantMap importSignalText(const QString& text);
    Q_INVOKABLE QString signalFileText(const QString& sha) const;
    // A kept copy saved where the user chose (a local file, or Android's
    // content:// from its document picker, by the content resolver): the copy's
    // bytes as imported or as the form wrote them, nothing added, so
    // havne-cli --signals and another vehicle read the same file. { ok,
    // words }, words never naming either path; a copy changed since it
    // was kept is refused, not saved. Logs one line
    Q_INVOKABLE QVariantMap saveSignalFile(const QString& sha, const QUrl& url);
    // A dashboard layout file (ADR-0057's appendix on the layout file): the
    // text DashboardBook.exportText made, written where the user chose (as
    // saveSignalFile writes), and a picked file's text back. { ok, words }
    // and { ok, words, text }, words never naming a path. The size is
    // bounded at layoutFileMaxBytes either way, a text that isn't UTF-8 is
    // refused unread, and a leading UTF-8 BOM is skipped. A local file is
    // written whole or not at all and is never removed; a provider's document
    // is written in place (it can't be atomic) and a refused or failed save
    // removes it only if it was empty when the save began.
    // Each logs one line
    Q_INVOKABLE QVariantMap saveLayoutFile(const QString& text, const QUrl& url);
    Q_INVOKABLE QVariantMap layoutFileText(const QUrl& url);
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
    Q_INVOKABLE QVariantList useSignalFiles(const QStringList& shas);
    // Removes every copy keep doesn't name, each profile unregistered
    // first; the files themselves stay loaded until the bridge closes,
    // since a session may still decode with one
    Q_INVOKABLE int deleteUnusedSignalFiles(const QStringList& keep);
    // An OBDb set imported (ADR-0051's appendix of 2026-11-06,
    // ObdbImport): preview reads the file at url as importSignalFile
    // does and converts it for modelYear (0: none), named name, giving
    // ObdbImport::report's map; the converted text is held as the one
    // pending import, nothing kept. keep puts it in the store as
    // importSignalText does, the same map back; replacing names the
    // vehicle's earlier import it takes the place of, "" for none, and
    // is refused, nothing changed and the import still pending, while a
    // session decodes with that file. drop forgets the pending import.
    // Each of preview and keep logs one line, never naming the path
    Q_INVOKABLE QVariantMap previewObdb(const QUrl& url, int modelYear,
                                        const QString& name);
    Q_INVOKABLE QVariantMap keepObdb(const QString& replacing);
    Q_INVOKABLE void dropObdb() { m_obdbPending.clear(); }
    // A kept copy the OBDb import wrote (ObdbImport::isConversion): only
    // such a file is replaced by importing the set again
    Q_INVOKABLE bool isObdbImport(const QString& sha) const;
    // The model year of the typed VIN, or with "" the session's (which
    // stays here, ADR-0027 section 3), by ADR-0043's decode; 0 for none
    Q_INVOKABLE int vinModelYear(const QString& typedVin) const;
    // The app's plugins (ADR-0052, PluginStore): every kept one loaded
    // and its variants registered at the first call, then the list, one
    // entry a plugin { sha256, ok, words, id, name, version, variants,
    // profiles }. Import reads a packed plugin (<id>.havne-plugin.json)
    // at url as importSignalFile reads a file and keeps a copy when core
    // loads it (the entry, kept, replaced: the version it replaced, or
    // ""); Remove frees it and deletes its copy. Both refuse, with
    // words, an id a session decodes with. Each logs one line.
    Q_INVOKABLE QVariantList plugins();
    Q_INVOKABLE QVariantMap importPlugin(const QUrl& url);
    Q_INVOKABLE QVariantMap removePlugin(const QString& sha);
    // The variants the plugins propose for a car (ADR-0052 section 5):
    // the VIN typed, or with "" the session's, which stays here (ADR-0027
    // section 3); { sha256, id, name, variant, profile } each, none
    // without a VIN. A proposal only: the user picks
    Q_INVOKABLE QVariantList pluginMatches(const QString& typedVin) const;
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
    // The signal file whose set set is: a vehicle's (the signal store),
    // a plugin variant's or a replay's own signals.json; nullptr for
    // any other set
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
    QVariantList minSweepChoices() const;
    // A quantity in a decoded unit, as the current unit system shows it:
    // the Dashboard's scale ends and zones are written once, in decoded
    // units, and converted here rather than in QML (rule 2).
    Q_INVOKABLE double displayValue(const QString& unit, double value) const;
    Q_INVOKABLE QString displayUnit(const QString& unit) const;
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
    void logPausedChanged();
    void recordingChanged();
    void recordingStateChanged();
    void markCountChanged();
    void logStateChanged();
    void logProgressChanged();
    void logNoticeChanged();
    // A log ended: byUser for Stop log, false for the session's end or a
    // failed write (reported as a Record sessions recording's end is).
    // The folder is absolute, as recordingRow takes it
    void logStopped(const QString& folder, bool byUser);
    void unitSystemChanged();
    void minSweepMsChanged();
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
                                   QString* reason = nullptr);
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
    // (ADR-0051 section 4). The recorded profile when the signal store
    // registers the same bytes under it; otherwise the copy, loaded and
    // registered as rec.<16 hex> for the bridge's life. Any other URI,
    // or a copy missing, changed or refused, comes back as given, with a
    // log line saying why. GUI thread, before the connect.
    QString replayUriWithOwnSignals(const QString& uri);
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
    uint32_t m_supportGeneration[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    // Each channel's last report, for the signals their ECUs refused
    ecuio_channel_support_t m_supportReports[ECUIO_SESSION_INFO_MAX_CHANNELS] =
        {};
    // What each channel's session line last said (cli::session_due's rule)
    struct SessionWritten {
        uint8_t state = 0;
        uint8_t nrc = 0;
        uint16_t entries = 0;
        uint16_t lost = 0;
        bool written = false;
    };
    SessionWritten m_sessionWritten[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    QString m_refusedUnnamed;
    // The signals core reports their ECU doesn't support, read into
    // m_unsupportedRead (sized at connect) when a report changes
    std::vector<uint16_t> m_unsupportedRead;
    // The signals core reports its frames come without (ADR-0039 section 6):
    // read each tick into m_absentRead (sized at connect), handed to the
    // model when they differ from m_absentIds
    void pollAbsentSignals();
    std::vector<uint16_t> m_absentRead;
    std::vector<uint16_t> m_absentIds;
    // What each channel's ECU said it is (ADR-0021), logged once complete
    void pollChannelIdentity();
    void pollReplayEnd();   // the tick: "Replay finished" once, at the end of the file
    void pollSleep();       // the tick: "Asleep for N s" after a sleep
    uint32_t m_identityGeneration[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    // What each channel's ECU reported of its diagnostics (ADR-0037),
    // logged once per completed read
    void pollChannelDiagnostics();
    uint32_t m_diagnosticsGeneration[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    // A clear asked of each channel, its outcome not yet logged
    bool m_clearAsked[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    // A clear that stopped at the vehicle speed, waiting for the word on
    // a moving vehicle: the generation it ended on, the speed it read
    // (UINT32_MAX: none)
    bool m_clearWaiting[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    uint32_t m_clearWaitGeneration[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    uint32_t m_clearWaitKmh[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    // The file the last clear saved its reports to, as shown
    QString m_clearSavedTo;
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
    ecuio_recorder_t* m_recorder =
        nullptr;                       // the session's, until stopRecording
    QString m_recordingFolder;         // the current or last one, absolute
    bool m_noting = true;              // logUnnoted clears it for its line
    int m_markCount = 0;               // the recording's marks, from 0
    bool m_logActive = false;          // m_recorder is Start log's
    HostElapsedTimer m_logClock;       // from Start log, sleeps counted
    qint64 m_logElapsedMs = 0;         // the last log's, once it ends
    qint64 m_logBytes = 0;             // countLogBytes' last sum
    QString m_logNotice;               // the header's line about the log
    int m_minSweepMs = 0;              // read at each Start log
    QTimer* m_logTimer = nullptr;      // the readout's second
    QString m_recordingsRoot;          // empty: Documents/Havne/recordings
    QString m_earlierRecordingsRoot;   // empty: the earlier name's folder
    bool m_recordReplays = false;      // tests only
    bool m_setFrameLogging = false;    // the bridge, not the launch, set ECUIO_SUR_LOG_FRAMES
    std::function<QString(const QString&)>
        m_streamPreparer; // none: nothing to ready
    ReadOnce m_readOnce;
    SignalStore m_signalStore;
    QByteArray m_obdbPending; // previewObdb's converted file, until kept
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
