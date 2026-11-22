// File/layer: app/recordingview.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: RecordingViewTest (test_bridge)

#include "recordingview.h"
#include "recordingview_internal.h"

#include "recording_frameset.h"
#include "urlwrite.h"

#include "healthreport.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMetaObject>
#include <QThreadPool>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ecuio/conditions.h"
#include "ecuio/core.h"
#include "ecuio/profile.h"

#include "recording_signals.h" /* cli/: events.log's profile and file lines */
#include "view_frames.h"       /* cli/: which set decodes the frames */

using namespace recordingview_detail;

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

void RecordingView::run(const std::shared_ptr<Result>& res,
                        std::atomic<bool>* cancel, RecordingView* view,
                        quint64 generation, const QString& folder,
                        const std::shared_ptr<RecordingFrameSet>& frames)
{
    const QDir dir(folder);
    const double frameBytes = static_cast<double>(
        QFileInfo(dir.filePath(QStringLiteral("frames.csv"))).size());
    /* The open reads signals.csv, or frames.csv a first time */
    const double signalBytes =
        frames ? frameBytes
               : static_cast<double>(
                     QFileInfo(dir.filePath(QStringLiteral("signals.csv")))
                         .size());
    const double whole = signalBytes + frameBytes;
    const double openShare = whole > 0 ? signalBytes / whole : 1.0;
    auto post = [](RecordingView* v, double p, quint64 g) {
        QMetaObject::invokeMethod(
            v, [v, p, g] { v->setProgress(p, g); }, Qt::QueuedConnection);
    };
    const QByteArray path = folder.toUtf8();

    Pass opening{cancel,    view, generation, 0.0,
                 openShare, post, nullptr,    nullptr};
    if (frames) {
        frames->loadText();
    }
    res->rc = frames
                  ? ecuio_recording_open_frames_with(
                        path.constData(), frames->openSet(), frames->file.get(),
                        onProgress, &opening, &res->reader)
                  : ecuio_recording_open(path.constData(), onProgress, &opening,
                                         &res->reader);
    if (res->rc == ECUIO_OK) {
        pairConditions(*res, folder, cancel);
    }
    if (res->rc != ECUIO_OK) {
        res->detail = detailOr(res->rc);
    } else if (!dir.exists(QStringLiteral("frames.csv"))) {
        /* Values alone (a CSV import): nothing for the check to read */
        res->checkDetail = QString::fromUtf8(kNoFrames);
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
    const QDir dir(folder);
    if (!dir.exists(QStringLiteral("signals.csv")) &&
        dir.exists(QStringLiteral("frames.csv"))) {
        openFrames(folder, QString());
        return;
    }
    stop();
    readHeader(folder);
    start(folder, nullptr);
}

void RecordingView::openFrames(const QString& folder, const QString& profile)
{
    stop();
    readHeader(folder);
    if (!m_hasFrames) {
        fail(folder, QString::fromUtf8(kNoFrames));
        return;
    }
    const cli::FramesSource source = cli::frames_source(
        profile == QStringLiteral("signals.json") ? std::string()
                                                  : profile.toStdString(),
        cli::RecordedSignals{m_recordedProfile.toStdString(),
                             m_recordedSha.toStdString(), true},
        m_ownSignalFile);
    if (profile == QStringLiteral("signals.json") && !m_ownSignalFile) {
        fail(folder,
             QStringLiteral(
                 "its signals.json isn't the file it was recorded with"));
        return;
    }
    std::shared_ptr<RecordingFrameSet> frames;
    if (source.own_file) {
        QFile file(QDir(folder).filePath(QStringLiteral("signals.json")));
        // A byte past the bound, so a longer copy is refused, never cut
        const QByteArray bytes = file.open(QIODevice::ReadOnly)
                                     ? file.read(ECUIO_SIGNAL_SET_MAX_BYTES + 1)
                                     : QByteArray();
        ecuio_signal_file_t* loaded = nullptr;
        if (ecuio_signal_file_load_with(
                bytes.constData(), static_cast<size_t>(bytes.size()),
                ECUIO_SIGNAL_FILE_LOAD_SET, &loaded) != ECUIO_OK) {
            fail(folder,
                 QStringLiteral("its signals.json doesn't load: %1")
                     .arg(QString::fromUtf8(ecuio_last_error_detail())));
            return;
        }
        frames = std::make_shared<RecordingFrameSet>(loaded);
        frames->decodedWith = QStringLiteral("signals.json");
    } else if (source.profile.empty()) {
        fail(
            folder,
            m_recordedProfile.isEmpty()
                ? QStringLiteral(
                      "it names no profile: pick one to decode its frames with")
            : m_recordedProfile == QStringLiteral("raw")
                ? QStringLiteral(
                      "it was recorded under raw, which decodes nothing: pick "
                      "a profile to decode its frames with")
                : QStringLiteral(
                      "uds-generic's signals come from its session's DIDs: "
                      "pick a signal file to decode its frames with"));
        return;
    } else {
        const ecuio_signal_set_t* set = nullptr;
        const bool registered =
            ecuio_profile_lookup(source.profile.c_str(), &set) == ECUIO_OK;
        const QString name = QString::fromStdString(source.profile);
        if (!registered) {
            fail(folder,
                 QStringLiteral("the profile %1 isn't loaded").arg(name));
            return;
        }
        if (set == nullptr || set->count == 0) {
            fail(folder,
                 QStringLiteral("the profile %1 has no signals to decode with")
                     .arg(name));
            return;
        }
        frames = std::make_shared<RecordingFrameSet>(*set);
        frames->decodedWith = name;
        frames->fileText = RecordingFrameSet::signalFileText(set);
    }
    start(folder, frames);
}

void RecordingView::start(const QString& folder,
                          const std::shared_ptr<RecordingFrameSet>& frames)
{
    m_folder = folder;
    m_error.clear();
    m_decodedWith = frames ? frames->decodedWith : QString();
    m_state = Opening;
    m_progress = 0;
    const quint64 generation = ++m_generation;
    auto res = std::make_shared<Result>();
    res->cutForTesting = m_conditionsCutForTesting;
    m_stop = std::make_shared<Stop>();
    m_worker =
        std::thread(run, res, &m_cancel, this, generation, folder, frames);
    emit progressChanged();
    emit stateChanged();
    emit signalListChanged();
}

void RecordingView::fail(const QString& folder, const QString& words)
{
    m_folder = folder;
    m_error = words;
    m_decodedWith.clear();
    m_state = Failed;
    m_progress = 0;
    emit progressChanged();
    emit stateChanged();
    emit signalListChanged();
}

// events.log's profile and signal file lines, and whether the folder's
// signals.json still hashes to the one named: what openFrames("") picks
// from, and what the page offers
void RecordingView::readHeader(const QString& folder)
{
    cli::RecordedSignals recorded;
    QFile log(QDir(folder).filePath(QStringLiteral("events.log")));
    if (log.open(QIODevice::ReadOnly)) {
        /* the header is the lines before the first blank one */
        for (int n = 0; n < 4096 && !log.atEnd(); ++n) {
            QByteArray line = log.readLine(4096);
            while (line.endsWith('\n') || line.endsWith('\r')) {
                line.chop(1);
            }
            if (!recorded.take(line.toStdString())) {
                break;
            }
        }
    }
    m_recordedProfile = QString::fromStdString(recorded.profile);
    m_recordedSha = QString::fromStdString(recorded.sha256);
    m_hasFrames = QDir(folder).exists(QStringLiteral("frames.csv"));
    m_ownSignalFile = false;
    if (recorded.names_file()) {
        QFile own(QDir(folder).filePath(QStringLiteral("signals.json")));
        if (own.open(QIODevice::ReadOnly) &&
            own.size() <= static_cast<qint64>(ECUIO_SIGNAL_SET_MAX_BYTES)) {
            m_ownSignalFile = QCryptographicHash::hash(
                                  own.readAll(), QCryptographicHash::Sha256)
                                  .toHex() == m_recordedSha.toLatin1();
        }
    }
}

void RecordingView::close()
{
    if (m_state == Closed) {
        return;
    }
    stop();
    m_folder.clear();
    m_error.clear();
    m_decodedWith.clear();
    m_recordedProfile.clear();
    m_recordedSha.clear();
    m_ownSignalFile = false;
    m_hasFrames = false;
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
    if (m_stop) {
        m_stop->stopped = true;
    }
    if (m_worker.joinable()) {
        m_cancel = true;
        m_worker.join();
        m_cancel = false;
    }
    if (m_asks) {
        bool answering = false;
        {
            const std::lock_guard<std::mutex> lock(m_asks->m);
            answering = m_asks->answering;
            m_asks->stats.reset();
            m_asks->step.reset();
            m_asks->means.reset();
            m_asks->meansStale = true;
        }
        if (answering) {
            m_asks.reset(); // the task's own until it ends
        }
    }
    // Closed here, or when a pass or an ask still reading lets go of it
    m_readerHold.reset();
    m_reader = nullptr;
    m_info = {};
    m_signals.clear();
    m_markers.clear();
    m_findings.clear();
    m_conditionSpans.clear();
    m_conditionSpanCount = 0;
    m_conditionPartsCut = false;
    m_conditionsCut = false;
    m_conditionEventsLost = 0;
    m_check.clear();
    if (m_stale) {
        m_stale = false;
        emit staleChanged();
    }
}

bool RecordingView::pullsLackSignals() const
{
    if (m_state != Open || m_reader == nullptr) {
        return false;
    }
    for (const uint32_t role : {ECUIO_RECORDING_ROLE_THROTTLE,
                                ECUIO_RECORDING_ROLE_ENGINE_LOAD}) {
        uint32_t index = UINT32_MAX;
        if (ecuio_recording_role_signal(m_reader, role, &index) != ECUIO_OK ||
            index != UINT32_MAX) {
            return false;
        }
    }
    return true;
}

bool RecordingView::saveHealthReport(const QUrl& url, int filterIndex,
                                     const QString& baselineFolder)
{
    if (m_state != Open || m_reporting) {
        // The picker made the document the report would have filled
        if (m_throughProvider || !url.isLocalFile()) {
            removeIfRemovable(pathOfUrl(url));
        }
        return false;
    }
    if (m_reportWorker.joinable()) {
        m_reportWorker.join(); // ended: its post is what cleared m_reporting
    }
    ecuio_health_options_t o{};
    o.format = healthFormatFor(url, filterIndex);
    o.units = m_units;
    o.latest_model_year = healthLatestModelYear();
    o.utc_us = healthUtcNow();
    const quint64 generation = m_generation;
    const QByteArray folder = m_folder.toUtf8();
    const QByteArray baseline = healthBaselineArgument(baselineFolder);
    const bool provider = m_throughProvider;
    try {
        if (m_failThreadForTesting) {
            throw std::system_error(std::make_error_code(
                std::errc::resource_unavailable_try_again));
        }
        m_reportWorker = std::thread(
            [this, o, folder, baseline, url, generation, provider]() mutable {
                o.folder_utf8 = folder.constData();
                o.baseline_folder_utf8 =
                    baseline.isEmpty() ? nullptr : baseline.constData();
                const QVariantMap result =
                    writeHealthReport(o, url, &m_reportStop, provider);
                QMetaObject::invokeMethod(
                    this,
                    [this, result, generation] {
                        reportFinished(result, generation);
                    },
                    Qt::QueuedConnection);
            });
    } catch (const std::system_error&) {
        // The picker made the document the report would have filled
        if (provider || !url.isLocalFile()) {
            removeIfRemovable(pathOfUrl(url));
        }
        return false;
    }
    m_reporting = true;
    emit reportingChanged();
    return true;
}

void RecordingView::reportFinished(const QVariantMap& result,
                                   quint64 generation)
{
    if (m_reportWorker.joinable()) {
        m_reportWorker.join(); // it posted this as its last act
    }
    m_reporting = false;
    emit reportingChanged();
    if (generation == m_generation) {
        emit reportSaved(result);
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
    m_readerHold.reset(res->reader, ecuio_recording_close);
    m_reader = m_readerHold.get();
    res->reader = nullptr;
    ecuio_recording_get_info(m_reader, &m_info);

    bool ranked = false;
    m_signals.resize(m_info.signal_count);
    for (uint32_t i = 0; i < m_info.signal_count; ++i) {
        ecuio_recording_get_signal(m_reader, i, &m_signals[i].pub);
        ranked = ranked || m_signals[i].pub.plot_rank != 0;
    }
    // At most ViewerPlotted numbers plot on open, so a phone's chart stays
    // readable: a ranked recording's lowest ranks (the signals Live plotted),
    // else the first in the recording's order. A signal with no samples gives
    // its place up; an unranked one in a ranked recording never plots.
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < m_info.signal_count; ++i) {
        const ecuio_recording_signal_t& s = m_signals[i].pub;
        const bool number =
            (s.flags & ECUIO_RECORDING_SIGNAL_TEXT) == 0 && s.samples > 0;
        if (number && (!ranked || s.plot_rank != 0)) {
            order.push_back(i);
        }
    }
    if (ranked) {
        std::stable_sort(
            order.begin(), order.end(), [this](uint32_t a, uint32_t b) {
                return m_signals[a].pub.plot_rank < m_signals[b].pub.plot_rank;
            });
    }
    if (order.size() > static_cast<size_t>(ViewerPlotted)) {
        order.resize(ViewerPlotted);
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
    for (const ConditionSpan& s : res->conditionSpans) {
        const Result::ConditionKey& k = res->conditionKeys[s.condition];
        QVariantList stale;
        for (const auto& [from, to] : s.stale) {
            stale.append(QVariantMap{{QStringLiteral("fromMs"), toMs(from)},
                                     {QStringLiteral("toMs"), toMs(to)}});
        }
        m_conditionSpans.append(QVariantMap{
            {QStringLiteral("key"), QString::fromStdString(k.key)},
            {QStringLiteral("name"), k.name},
            {QStringLiteral("level"), k.level},
            {QStringLiteral("on"), s.on},
            {QStringLiteral("fromMs"), toMs(s.from_us)},
            {QStringLiteral("toMs"), toMs(s.to_us)},
            {QStringLiteral("heldStart"), s.heldStart},
            {QStringLiteral("openEnd"), s.openEnd},
            {QStringLiteral("stale"), stale},
        });
    }
    m_conditionSpanCount = static_cast<double>(res->conditionSpanCount);
    m_conditionPartsCut = res->conditionPartsCut;
    m_conditionsCut =
        res->cutForTesting ||
        (m_info.flags & ECUIO_RECORDING_INFO_CONDITIONS_CUT) != 0;
    ecuio_recording_conditions_t conditions{};
    ecuio_recording_get_conditions(m_reader, &conditions);
    m_conditionEventsLost = static_cast<double>(conditions.events_lost);

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
        {QStringLiteral("conditionEvents"),
         static_cast<double>(m_info.condition_events)},
        {QStringLiteral("conditionsCut"), m_conditionsCut},
        {QStringLiteral("conditionSpanCount"), m_conditionSpanCount},
        {QStringLiteral("conditionSpansCut"),
         m_conditionSpanCount > ViewerConditionSpans},
        {QStringLiteral("conditionPartsCut"), m_conditionPartsCut},
        {QStringLiteral("conditionBound"), ViewerConditionSpans},
        {QStringLiteral("conditionEventsLost"), m_conditionEventsLost},
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
            {QStringLiteral("key"), QString::fromUtf8(s.pub.key)},
            {QStringLiteral("name"), QString::fromUtf8(s.pub.name)},
            {QStringLiteral("unit"), unit},
            {QStringLiteral("decimals"), decimals},
            {QStringLiteral("isText"), text},
            {QStringLiteral("plotRank"), static_cast<int>(s.pub.plot_rank)},
            {QStringLiteral("group"), static_cast<int>(s.pub.group)},
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
