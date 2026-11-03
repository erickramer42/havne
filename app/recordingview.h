// File/layer: app/recordingview.h | Qt6 Core, app layer, C++17
// Testing: UNIT: test_bridge (tests/test_recording_view.cpp,
//          RecordingViewTest.*) on tests/fixtures/recording/: the open
//          and check off the GUI thread, a cancel, a refusal, the
//          signals in display units and the default plotted ones,
//          columns, the values at a time, markers and findings

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>
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
// its per-second summaries.
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
    // The check's outcome: { ran, error (why it didn't: no frames.csv,
    // a row it refused), frames, ids, isotpSkipped, isotpFlagsUnknown,
    // isotpNoWire, tornRows }
    Q_PROPERTY(QVariantMap check READ check NOTIFY stateChanged)
    // A query found the file changed since the open (a row that no
    // longer parses, the file gone): what's shown is the open's
    Q_PROPERTY(bool stale READ stale NOTIFY staleChanged)

public:
    enum State { Closed, Opening, Open, Failed };
    Q_ENUM(State)

    // A ranked recording's signals plotted by default: Live's count
    // (SignalListModel::DefaultPlotted)
    static constexpr int DefaultPlotted = 6;

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

    // Opens the recording in folder (the one holding signals.csv), closing
    // any open or opening one first. The state goes to Opening at once,
    // then Open or Failed on the GUI thread.
    Q_INVOKABLE void open(const QString& folder);
    // Stops an open under way (Failed, "canceled") or closes the one open
    Q_INVOKABLE void close();

    // columns columns over [t0Ms, t1Ms) for each signal index in
    // signalIndexes: one map each, { index, lows: [...], highs: [...],
    // samples: [...] }, a column with no sample NaN in lows and highs;
    // display units. Empty when not open, for a bad range or index, or
    // when the file changed (stale goes true).
    Q_INVOKABLE QVariantList columns(const QVariantList& signalIndexes,
                                     double t0Ms, double t1Ms, int columns);
    // Each signal's value in force at tMs, as the cursor shows it: one map
    // each, { index, text (a number in display units, its decimals; a text
    // signal's as recorded; "" before its first sample), unit, atMs (-1
    // before the first sample) }
    Q_INVOKABLE QVariantList valuesAt(const QVariantList& signalIndexes,
                                      double tMs);

signals:
    void stateChanged();
    void progressChanged();
    void unitSystemChanged();
    void signalListChanged();
    void staleChanged();

private:
    struct Result;
    struct Signal {
        ecuio_recording_signal_t pub;
        bool plotted = false;
    };

    static void run(const std::shared_ptr<Result>& res,
                    std::atomic<bool>* cancel, RecordingView* view,
                    quint64 generation, const QString& folder);
    void finish(const std::shared_ptr<Result>& res, quint64 generation);
    void stop();
    void setProgress(double p, quint64 generation);
    void markStale();
    // A recorded number as displayed: the value, its unit and decimals
    ecuio_display_value_t shown(const Signal& s, double value) const;
    bool signalIndex(const QVariant& v, uint32_t* out) const;

    int m_state = Closed;
    QString m_folder;
    QString m_error;
    double m_progress = 0;
    uint32_t m_units = ECUIO_UNITS_METRIC;
    bool m_stale = false;

    ecuio_recording_t* m_reader = nullptr;
    ecuio_recording_info_t m_info{};
    std::vector<Signal> m_signals;
    QVariantList m_markers;
    QVariantList m_findings;
    QVariantMap m_check;

    std::thread m_worker;
    std::atomic<bool> m_cancel{false};
    // Each open's number: a result or progress posted for an earlier one
    // is dropped
    quint64 m_generation = 0;
};
