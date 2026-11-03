// File/layer: app/recordingview.cpp | Qt6 Core, app layer, C++17
// Testing: UNIT: test_bridge (tests/test_recording_view.cpp,
//          RecordingViewTest.*) on tests/fixtures/recording/

#include "recordingview.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

#include "ecuio/core.h"

namespace {

// The most columns one query fills: well past any screen's width
constexpr int kMaxColumns = 65536;
// A text_at copy: a text signal's value is cut past this
constexpr uint32_t kTextCapacity = 1024;

double toMs(uint64_t us)
{
    return static_cast<double>(us) / 1000.0;
}

// -1 for "none", as QML reads it
double toMsOrNone(uint64_t us)
{
    return us == ECUIO_RECORDING_NONE ? -1.0 : toMs(us);
}

// A time from QML: negative and NaN are 0, the rest to the microsecond
uint64_t fromMs(double ms)
{
    if (!(ms > 0)) {
        return 0;
    }
    const double us = std::round(ms * 1000.0);
    if (us >= 1.8e19) {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(us);
}

QString detailOr(ecuio_status_t rc)
{
    const char* detail = ecuio_last_error_detail();
    if (detail != nullptr && detail[0] != '\0') {
        return QString::fromUtf8(detail);
    }
    return QString::fromUtf8(ecuio_status_string(rc));
}

} // namespace

// What the thread hands the GUI thread. A result dropped unread (a later
// open, the view destroyed) closes its reader.
struct RecordingView::Result {
    ecuio_recording_t* reader = nullptr;
    ecuio_status_t rc = ECUIO_OK;
    QString detail;
    bool checked = false;
    ecuio_status_t checkRc = ECUIO_OK;
    QString checkDetail;
    ecuio_check_summary_t summary{};
    std::vector<ecuio_check_finding_t> findings;
    bool findingsLost = false;

    Result() = default;
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    ~Result() { ecuio_recording_close(reader); }
};

namespace {

// The callbacks' context, one for both of the check's: the pass's share
// of the whole, and where its findings go (the open has none)
struct Pass {
    std::atomic<bool>* cancel;
    RecordingView* view;
    quint64 generation;
    double base;
    double weight;
    void (*post)(RecordingView*, double, quint64);
    std::vector<ecuio_check_finding_t>* findings;
    bool* lost;
};

int onProgress(uint64_t read, uint64_t total, void* user)
{
    auto* p = static_cast<Pass*>(user);
    const double share =
        total > 0 ? static_cast<double>(read) / static_cast<double>(total)
                  : 1.0;
    p->post(p->view, p->base + p->weight * std::min(share, 1.0), p->generation);
    return p->cancel->load() ? 1 : 0;
}

int onFinding(const ecuio_check_finding_t* f, void* user)
{
    auto* sink = static_cast<Pass*>(user);
    if (!*sink->lost) {
        try {
            sink->findings->push_back(*f);
        } catch (const std::bad_alloc&) {
            *sink->lost = true;
        }
    }
    return sink->cancel->load() ? 1 : 0;
}

} // namespace

RecordingView::RecordingView(QObject* parent) : QObject(parent) {}

RecordingView::~RecordingView()
{
    stop();
}

void RecordingView::run(const std::shared_ptr<Result>& res,
                        std::atomic<bool>* cancel, RecordingView* view,
                        quint64 generation, const QString& folder)
{
    const QDir dir(folder);
    const double signalBytes = static_cast<double>(
        QFileInfo(dir.filePath(QStringLiteral("signals.csv"))).size());
    const double frameBytes = static_cast<double>(
        QFileInfo(dir.filePath(QStringLiteral("frames.csv"))).size());
    const double whole = signalBytes + frameBytes;
    const double openShare = whole > 0 ? signalBytes / whole : 1.0;
    auto post = [](RecordingView* v, double p, quint64 g) {
        QMetaObject::invokeMethod(
            v, [v, p, g] { v->setProgress(p, g); }, Qt::QueuedConnection);
    };
    const QByteArray path = folder.toUtf8();

    Pass opening{cancel,    view, generation, 0.0,
                 openShare, post, nullptr,    nullptr};
    res->rc = ecuio_recording_open(path.constData(), onProgress, &opening,
                                   &res->reader);
    if (res->rc != ECUIO_OK) {
        res->detail = detailOr(res->rc);
    } else {
        Pass checking{
            cancel,          view, generation,     openShare,
            1.0 - openShare, post, &res->findings, &res->findingsLost};
        res->checked = true;
        res->checkRc =
            ecuio_recording_check(path.constData(), nullptr, onProgress,
                                  onFinding, &checking, &res->summary);
        if (res->checkRc != ECUIO_OK) {
            res->checkDetail = detailOr(res->checkRc);
        }
    }
    QMetaObject::invokeMethod(
        view, [view, res, generation] { view->finish(res, generation); },
        Qt::QueuedConnection);
}

void RecordingView::open(const QString& folder)
{
    stop();
    m_folder = folder;
    m_error.clear();
    m_state = Opening;
    m_progress = 0;
    const quint64 generation = ++m_generation;
    auto res = std::make_shared<Result>();
    m_worker = std::thread(run, res, &m_cancel, this, generation, folder);
    emit progressChanged();
    emit stateChanged();
    emit signalListChanged();
}

void RecordingView::close()
{
    if (m_state == Closed) {
        return;
    }
    stop();
    m_folder.clear();
    m_error.clear();
    m_state = Closed;
    m_progress = 0;
    emit progressChanged();
    emit stateChanged();
    emit signalListChanged();
}

void RecordingView::stop()
{
    // A pass under way stops at its next callback; whatever it posts
    // carries the old generation and is dropped
    ++m_generation;
    if (m_worker.joinable()) {
        m_cancel = true;
        m_worker.join();
        m_cancel = false;
    }
    ecuio_recording_close(m_reader);
    m_reader = nullptr;
    m_info = {};
    m_signals.clear();
    m_markers.clear();
    m_findings.clear();
    m_check.clear();
    if (m_stale) {
        m_stale = false;
        emit staleChanged();
    }
}

void RecordingView::setProgress(double p, quint64 generation)
{
    if (generation != m_generation || m_state != Opening || p == m_progress) {
        return;
    }
    m_progress = p;
    emit progressChanged();
}

void RecordingView::finish(const std::shared_ptr<Result>& res,
                           quint64 generation)
{
    if (generation != m_generation) {
        return;
    }
    if (m_worker.joinable()) {
        m_worker.join(); // it posted this as its last act
    }
    m_progress = 1;
    if (res->rc != ECUIO_OK) {
        m_state = Failed;
        m_error = res->detail;
        emit progressChanged();
        emit stateChanged();
        return;
    }
    m_reader = res->reader;
    res->reader = nullptr;
    ecuio_recording_get_info(m_reader, &m_info);

    bool ranked = false;
    m_signals.resize(m_info.signal_count);
    for (uint32_t i = 0; i < m_info.signal_count; ++i) {
        ecuio_recording_get_signal(m_reader, i, &m_signals[i].pub);
        ranked = ranked || m_signals[i].pub.plot_rank != 0;
    }
    // Live's default (SignalListModel::judgePlotted): every number, or in a
    // ranked recording the DefaultPlotted of the lowest rank, a tie in the
    // recording's order; a signal with no samples gives its place up
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < m_info.signal_count; ++i) {
        const ecuio_recording_signal_t& s = m_signals[i].pub;
        const bool number =
            (s.flags & ECUIO_RECORDING_SIGNAL_TEXT) == 0 && s.samples > 0;
        if (!number) {
            continue;
        }
        if (!ranked) {
            m_signals[i].plotted = true;
        } else if (s.plot_rank != 0) {
            order.push_back(i);
        }
    }
    std::stable_sort(
        order.begin(), order.end(), [this](uint32_t a, uint32_t b) {
            return m_signals[a].pub.plot_rank < m_signals[b].pub.plot_rank;
        });
    if (order.size() > static_cast<size_t>(DefaultPlotted)) {
        order.resize(DefaultPlotted);
    }
    for (uint32_t i : order) {
        m_signals[i].plotted = true;
    }

    for (uint32_t i = 0; i < m_info.marker_count; ++i) {
        ecuio_recording_marker_t m{};
        if (ecuio_recording_get_marker(m_reader, i, &m) != ECUIO_OK) {
            continue;
        }
        m_markers.append(QVariantMap{
            {QStringLiteral("timeMs"), toMs(m.host_us)},
            {QStringLiteral("label"),
             QString::fromUtf8(m.label_utf8,
                               static_cast<qsizetype>(m.label_length))},
        });
    }

    for (const ecuio_check_finding_t& f : res->findings) {
        const bool wholeBus = (f.flags & ECUIO_CHECK_FINDING_WHOLE_BUS) != 0;
        m_findings.append(QVariantMap{
            {QStringLiteral("kind"), static_cast<int>(f.kind)},
            {QStringLiteral("id"),
             wholeBus ? -1.0 : static_cast<double>(f.arbitration_id)},
            {QStringLiteral("extended"),
             (f.flags & ECUIO_CHECK_FINDING_EXTENDED) != 0},
            {QStringLiteral("atMs"), toMs(f.at_us)},
            {QStringLiteral("lengthMs"), toMs(f.length_us)},
            {QStringLiteral("count"), static_cast<double>(f.count)},
            {QStringLiteral("code"), f.code},
        });
    }
    const ecuio_check_summary_t& sum = res->summary;
    const bool ran =
        res->checked && res->checkRc == ECUIO_OK && !res->findingsLost;
    QString why = res->checkDetail;
    if (res->findingsLost) {
        why = QString::fromUtf8(ecuio_status_string(ECUIO_ERR_OUT_OF_MEMORY));
    }
    m_check = QVariantMap{
        {QStringLiteral("ran"), ran},
        {QStringLiteral("error"), ran ? QString() : why},
        {QStringLiteral("frames"), static_cast<double>(sum.frames)},
        {QStringLiteral("ids"), static_cast<int>(sum.ids)},
        {QStringLiteral("isotpSkipped"),
         (sum.flags & ECUIO_CHECK_SKIPPED_ISOTP) != 0},
        {QStringLiteral("isotpFlagsUnknown"),
         (sum.flags & ECUIO_CHECK_ISOTP_FLAGS_UNKNOWN) != 0},
        {QStringLiteral("isotpNoWire"),
         (sum.flags & ECUIO_CHECK_ISOTP_NO_WIRE) != 0},
        {QStringLiteral("tornRows"), static_cast<double>(sum.torn_rows)},
    };
    if (!ran) {
        m_findings.clear();
    }

    m_state = Open;
    emit progressChanged();
    emit stateChanged();
    emit signalListChanged();
}

void RecordingView::setUnitSystem(int units)
{
    const auto u = static_cast<uint32_t>(units);
    if ((u != ECUIO_UNITS_METRIC && u != ECUIO_UNITS_IMPERIAL) ||
        u == m_units) {
        return;
    }
    m_units = u;
    emit unitSystemChanged();
    emit signalListChanged();
}

double RecordingView::durationMs() const
{
    return toMs(m_info.duration_us);
}

QVariantMap RecordingView::info() const
{
    if (m_state != Open) {
        return {};
    }
    return QVariantMap{
        {QStringLiteral("format"), static_cast<int>(m_info.format)},
        {QStringLiteral("utcStartMs"), toMs(m_info.utc_start_us)},
        {QStringLiteral("noSummary"),
         (m_info.flags & ECUIO_RECORDING_INFO_NO_SUMMARY) != 0},
        {QStringLiteral("rowsTorn"), static_cast<double>(m_info.rows_torn)},
        {QStringLiteral("recordsDropped"),
         static_cast<double>(m_info.records_dropped)},
        {QStringLiteral("notesDropped"),
         static_cast<double>(m_info.notes_dropped)},
    };
}

ecuio_display_value_t RecordingView::shown(const Signal& s, double value) const
{
    ecuio_display_value_t out;
    if (ecuio_display_convert(s.pub.unit, s.pub.decimal_places, value, m_units,
                              &out) != ECUIO_DISPLAY_OK) {
        // m_units is only ever metric or imperial, so this is unreachable;
        // show the recorded value rather than nothing
        out = {};
        out.value = value;
        qstrncpy(out.unit, s.pub.unit, sizeof(out.unit));
        out.decimal_places = s.pub.decimal_places;
    }
    return out;
}

QVariantList RecordingView::signalList() const
{
    QVariantList list;
    for (size_t i = 0; i < m_signals.size(); ++i) {
        const Signal& s = m_signals[i];
        const bool text = (s.pub.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0;
        QString unit = QString::fromUtf8(s.pub.unit);
        int decimals = s.pub.decimal_places;
        double lowest = std::numeric_limits<double>::quiet_NaN();
        double highest = lowest;
        if (!text) {
            // Every conversion rises with its value, so a range stays one
            const ecuio_display_value_t lo = shown(s, s.pub.lowest);
            const ecuio_display_value_t hi = shown(s, s.pub.highest);
            unit = QString::fromUtf8(lo.unit);
            decimals = lo.decimal_places;
            if (s.pub.samples > 0) {
                lowest = lo.value;
                highest = hi.value;
            }
        }
        list.append(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(i)},
            {QStringLiteral("name"), QString::fromUtf8(s.pub.name)},
            {QStringLiteral("unit"), unit},
            {QStringLiteral("decimals"), decimals},
            {QStringLiteral("isText"), text},
            {QStringLiteral("plotRank"), static_cast<int>(s.pub.plot_rank)},
            {QStringLiteral("plotted"), s.plotted},
            {QStringLiteral("samples"), static_cast<double>(s.pub.samples)},
            {QStringLiteral("lowest"), lowest},
            {QStringLiteral("highest"), highest},
            {QStringLiteral("firstMs"), toMsOrNone(s.pub.first_us)},
            {QStringLiteral("lastMs"), toMsOrNone(s.pub.last_us)},
        });
    }
    return list;
}

bool RecordingView::signalIndex(const QVariant& v, uint32_t* out) const
{
    bool ok = false;
    const int i = v.toInt(&ok);
    if (!ok || i < 0 || static_cast<size_t>(i) >= m_signals.size()) {
        return false;
    }
    *out = static_cast<uint32_t>(i);
    return true;
}

void RecordingView::markStale()
{
    if (m_stale) {
        return;
    }
    m_stale = true;
    emit staleChanged();
}

QVariantList RecordingView::columns(const QVariantList& signalIndexes,
                                    double t0Ms, double t1Ms, int columns)
{
    if (m_reader == nullptr || signalIndexes.isEmpty() || columns < 1 ||
        columns > kMaxColumns) {
        return {};
    }
    const uint64_t t0 = fromMs(t0Ms);
    const uint64_t t1 = fromMs(t1Ms);
    if (t1 <= t0) {
        return {};
    }
    std::vector<uint32_t> ids;
    for (const QVariant& v : signalIndexes) {
        uint32_t i = 0;
        if (!signalIndex(v, &i)) {
            return {};
        }
        ids.push_back(i);
    }
    const size_t n = static_cast<size_t>(columns);
    std::vector<ecuio_recording_column_t> cols(ids.size() * n);
    const ecuio_status_t rc = ecuio_recording_query(
        m_reader, ids.data(), static_cast<uint32_t>(ids.size()), t0, t1,
        cols.data(), static_cast<uint32_t>(columns));
    if (rc == ECUIO_ERR_RECORDING_CHANGED) {
        markStale();
    }
    if (rc != ECUIO_OK) {
        return {};
    }

    const double none = std::numeric_limits<double>::quiet_NaN();
    QVariantList out;
    for (size_t k = 0; k < ids.size(); ++k) {
        const Signal& s = m_signals[ids[k]];
        const bool text = (s.pub.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0;
        QList<double> lows(columns, none);
        QList<double> highs(columns, none);
        QList<double> samples(columns, 0.0);
        for (size_t c = 0; c < n; ++c) {
            const ecuio_recording_column_t& col = cols[k * n + c];
            samples[static_cast<qsizetype>(c)] = col.samples;
            if (col.samples == 0 || text) {
                continue;
            }
            lows[static_cast<qsizetype>(c)] = shown(s, col.lowest).value;
            highs[static_cast<qsizetype>(c)] = shown(s, col.highest).value;
        }
        out.append(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(ids[k])},
            {QStringLiteral("lows"), QVariant::fromValue(lows)},
            {QStringLiteral("highs"), QVariant::fromValue(highs)},
            {QStringLiteral("samples"), QVariant::fromValue(samples)},
        });
    }
    return out;
}

QVariantList RecordingView::valuesAt(const QVariantList& signalIndexes,
                                     double tMs)
{
    if (m_reader == nullptr) {
        return {};
    }
    const uint64_t t = fromMs(tMs);
    QVariantList out;
    char text[kTextCapacity];
    for (const QVariant& v : signalIndexes) {
        uint32_t i = 0;
        if (!signalIndex(v, &i)) {
            return {};
        }
        const Signal& s = m_signals[i];
        uint64_t at = ECUIO_RECORDING_NONE;
        const ecuio_status_t rc =
            ecuio_recording_text_at(m_reader, i, t, text, sizeof(text), &at);
        if (rc == ECUIO_ERR_RECORDING_CHANGED) {
            markStale();
        }
        if (rc != ECUIO_OK) {
            return {};
        }
        QString shownText = QString::fromUtf8(text);
        QString unit = QString::fromUtf8(s.pub.unit);
        const bool number = (s.pub.flags & ECUIO_RECORDING_SIGNAL_TEXT) == 0;
        if (number && at != ECUIO_RECORDING_NONE) {
            // signals.csv's numbers are C-locale, as QByteArray reads them
            bool ok = false;
            const double value = QByteArray(text).toDouble(&ok);
            if (ok) {
                const ecuio_display_value_t d = shown(s, value);
                shownText = QString::number(d.value, 'f', d.decimal_places);
                unit = QString::fromUtf8(d.unit);
            }
        } else if (number) {
            unit = QString::fromUtf8(shown(s, 0).unit);
        }
        out.append(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(i)},
            {QStringLiteral("text"), shownText},
            {QStringLiteral("unit"), unit},
            {QStringLiteral("atMs"), toMsOrNone(at)},
        });
    }
    return out;
}
