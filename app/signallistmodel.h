// File/layer: app/signallistmodel.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge); QML: QmlTest.liveview{,_pause}

#pragma once

#include <QObject>
#include <QString>
#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QVariantList>
#include <QHash>
#include <QByteArray>
#include <QStringList>
#include <QtGlobal>

#include <utility>
#include <vector>

#include "signalfilter.h"
#include "hostclock.h"
#include "ecuio/signals.h"
#include "ecuio/recording.h"

// A value, and a def's range, as displayed in a unit system (ADR-0022):
// the signal model's roles and SessionBridge::profileSignals share them
ecuio_display_value_t displayed(const char* unit, uint8_t decimals,
                                double value, uint32_t units);
std::pair<double, double> displayedRange(const char* unit,
                                         uint8_t decimals, double lo,
                                         double hi, uint32_t units);

class SignalListModel : public QAbstractListModel
{
    Q_OBJECT
    // The plotted rows Live shows: hidden rows aren't counted
    Q_PROPERTY(int subscribedCount READ subscribedCount NOTIFY subscribedCountChanged)
    // The ticked rows Live shows, text rows among them: what Start log asks
    Q_PROPERTY(int tickedCount READ tickedCount NOTIFY tickedCountChanged)
    // The rows Live leaves out: unsupported or absent (rejudgeHidden)
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY hiddenCountChanged)
    // The rows Live shows that the focus leaves unasked (applyUnasked)
    Q_PROPERTY(int unaskedCount READ unaskedCount NOTIFY unaskedCountChanged)

public:
    explicit SignalListModel(QObject* parent = nullptr);

    // Rolling history depth per signal: room for the longest window the
    // strip chart and the Dashboard's traces draw, 120 s. A 100 ms window
    // adds one to three samples (its lowest, highest and newest value,
    // ADR-0005's appendix), so 120 s of any signal fits. Kept constexpr so
    // tests can assert exact ring behavior without a config surface.
    static constexpr int HistoryCapacity = 3600;

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
        IsEnumRole, /* the def's kind is ENUM. Fixed per signal set. */
        UnaskedRole, /* the session's focus leaves it unasked
                      * (applyUnasked, ecuio_session_get_unasked_signals):
                      * Live and the Dashboard say "not asked" in place of
                      * a value */
        HiddenWhyRole, /* why a hidden row is hidden: "unsupported" (its
                        * ECU's supported-PID list leaves it out) or
                        * "absent" (its replies come without the field),
                        * unsupported first when both hold; "" for a shown
                        * row. Notified with HiddenRole, including when the
                        * reason changes under a row that stays hidden. */
        KeyRole        /* the key the signal is kept under (ADR-0065
                        * section 9, ecuio_signal_set_keys with the set's
                        * file): what a vehicle's ticks and a recording
                        * name it by. Fixed per signal set. */
    };

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    // file: the signal file whose set it is (SessionBridge::fileOfSet),
    // for the rows' refs; nullptr for a built-in set or none
    // pickOnly: the session asks only its picks, so no row is plotted
    // until ticked (a set that ranks none otherwise plots every row)
    void setSignalSet(const ecuio_signal_set_t* set,
                      const ecuio_signal_file_t* file = nullptr,
                      bool pickOnly = false);
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
    int tickedCount() const { return m_tickedCount; }
    int hiddenCount() const { return m_hiddenCount; }
    // Whether any row has taken a value since the set loaded
    bool anyValue() const;
    // The ids of the ticked rows Live shows, in the model's order: the
    // rows the chart plots (subscribedCount of them) and the ticked text
    // rows, the focus Start log asks
    std::vector<uint16_t> plottedIds() const;
    // The rows the user ticked past their default, text rows among them,
    // in the model's order: the default pick adds them to the ranked ones
    std::vector<uint16_t> tickedIds() const;
    // Each number's and enumeration's newest value as core decoded it
    // (the def's unit, an enumeration's raw number; never the display's
    // conversion), aged by the model's clock from the window that brought
    // it, for the health report: core keeps none. Rows with no value yet
    // and text rows are left out
    std::vector<ecuio_health_value_t> heldValues() const;
    // When def id's row last took a value, on the model's steady clock in
    // ms; -1 for no row or no value yet
    qint64 readAtMs(uint32_t id) const;
    // What turns readAtMs into the wall clock's ms since the epoch, now
    qint64 wallOffsetMs() const;
    // readAtMs on the wall clock; -1 for no row or no value yet
    qint64 lastReadMs(uint32_t id) const;

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
    // The signals the session's focus leaves unasked
    // (ecuio_session_get_unasked_signals): those rows say "not asked".
    // Each call judges every row afresh; a new set leaves none unasked.
    void applyUnasked(const std::vector<uint16_t>& ids);
    // The unasked rows Live shows, for its "N signals not asked" line
    int unaskedCount() const { return m_unaskedCount; }

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
    // clears the row's choice, so the row follows its default again. A
    // text row's tick asks it (the default pick takes it) and never plots.
    Q_INVOKABLE void toggleSubscribed(int row);
    // The hidden rows as { name, why } in set order, for Live's list of
    // them; a binding that reads hiddenCount re-runs it on every judgment
    Q_INVOKABLE QVariantList hiddenSignals() const;
    // A vehicle's ticked signals (ADR-0027 section 1), by key (ADR-0065
    // section 9): the rows the user ticked past the default and the
    // numeric rows unticked from it, and setting both, which leaves every
    // other row to its default. A row in both lists is unplotted; a text
    // row is never unplotted, its default being unticked.
    // setPlotChoices takes each text as a key, else as a name one row has
    // (ChoiceRows), so lists kept by name tick the same rows. A text no
    // row answers, or one whose row has no key (core refused the keys),
    // is carried: both lists give it back after the rows'
    // keys, at most CarriedMax in each (the newest), so a tick outlives
    // a session under a set without its signal. A new set carries none.
    static constexpr int CarriedMax = 1024;
    Q_INVOKABLE QStringList plottedKeys() const;
    Q_INVOKABLE QStringList unplottedKeys() const;
    Q_INVOKABLE void setPlotChoices(const QStringList& plotted, const QStringList& unplotted);
    // Forgets the carried texts, which were another vehicle's: the rows'
    // own choices stay. No signal.
    Q_INVOKABLE void dropCarriedChoices();

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

    // One signal's trace for the Dashboard's sparkline and Number tile:
    // [ts, val, ...] as chartData's samples, for the row of def `id`
    // whether or not Live plots it, over the last windowMs before that
    // row's newest sample, sliced as chartData slices (from time 0, so a
    // sample keeps its slice as the window moves). A break is a point whose
    // value is NaN: a stall in the ring past the stale time
    // (ECUIO_VALUE_STALE_US), or a value that isn't a number, which also
    // starts a new slice. [] for no such row, a text row or one with no
    // history. The trace reads the model's ring, so it outlives the page
    // that shows it.
    Q_INVOKABLE QList<double> trace(int id, qint64 windowMs, int columns) const;

    /* Periodic age refresh: ageMs is computed from the clock at data(), but
     * dataChanged fires only on updateValues, so a stalled row would never
     * dim. Emits dataChanged over every row for AgeMsRole alone (no value
     * or hasValue churn, no reset). */
    void refreshAges();

signals:
    void valuesUpdated();
    void subscribedCountChanged();
    void tickedCountChanged();
    void hiddenCountChanged();
    void unaskedCountChanged();
    // A tick or untick, or a vehicle's choices set: the default pick
    // follows them
    void ticksChanged();

private:
    struct Sample {
        qint64 timestampMs;
        double value;
    };

    enum class PlotChoice : uint8_t { Default, Plotted, Unplotted };

    struct Entry {
        uint16_t id;
        char name[32];
        char key[ECUIO_SIGNAL_KEY_MAX + 1]; // "" when core gave none
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
        enum class Why : unsigned char { None, Unsupported, Absent };
        Why why = Why::None;      // the reason it's hidden, unsupported first
        bool unasked = false;     // core: the focus leaves it unasked
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
    // The unasked rows Live shows; notifies a change
    void recountUnasked();
    // Each row's default and so its subscribed state, and the plotted
    // count; notifies the rows that changed and the count. Run after
    // anything that moves the defaults: a new set, hidden or refused
    // rows, choices.
    void judgePlotted();
    static void pushSample(Entry& e, qint64 stamp, double value);
    static const char* whyWords(Entry::Why why);
    // An enum row's label for its newest value, "" when it has none
    static QString labelOf(const Entry& e);
    static void pushWindow(Entry& e, const WindowValue& w);
    // The ring's samples at or after `from`, oldest first: every one when
    // sliceMs <= 0, else each slice's lowest and highest and the newest.
    // breakMs > 0 adds a nullptr for each stall longer than it and gives a
    // value that isn't a number a point of its own (trace's breaks)
    static std::vector<const Sample*>
    sliced(const Entry& e, qint64 from, double sliceMs, qint64 breakMs = 0);
    // A row's value as displayed in the current unit system
    ecuio_display_value_t shown(const Entry& e, double value) const;
    // A row's range as displayed (displayedRange)
    std::pair<double, double> shownRange(const Entry& e) const;

    std::vector<Entry> m_entries;
    // Each signal id's row, so a batch finds its rows without a search.
    // A set's ids are unique (the validator refuses a duplicate).
    QHash<uint16_t, int> m_rowOfId;
    int m_subscribedCount = 0;
    int m_tickedCount = 0;
    int m_hiddenCount = 0;
    int m_unaskedCount = 0;
    bool m_ranked = false;      // the set ranks a def (plot_rank)
    bool m_pickOnly = false;    // the session is asked only its picks
    // The texts of the last setPlotChoices that no keyed row answered
    QStringList m_carriedPlotted;
    QStringList m_carriedUnplotted;
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
