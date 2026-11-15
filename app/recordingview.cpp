// File/layer: app/recordingview.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: RecordingViewTest (test_bridge)

#include "recordingview.h"
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
#include <new>
#include <system_error>

#include "ecuio/core.h"
#include "ecuio/profile.h"

#include "recording_signals.h" /* cli/: events.log's profile and file lines */
#include "view_frames.h"       /* cli/: which set decodes the frames */

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

// A time from QML that may be before the start, to the microsecond; false
// for NaN or past a few hundred years either way
bool signedUs(double ms, int64_t* out)
{
    const double us = std::round(ms * 1000.0);
    if (!(std::fabs(us) < 9e15)) {
        return false;
    }
    *out = static_cast<int64_t>(us);
    return true;
}

// Why a folder of values alone (a CSV import) has no check or decode
constexpr const char* kNoFrames = "the recording has no frames";

QString detailOr(ecuio_status_t rc)
{
    const char* detail = ecuio_last_error_detail();
    if (detail != nullptr && detail[0] != '\0') {
        return QString::fromUtf8(detail);
    }
    return QString::fromUtf8(ecuio_status_string(rc));
}

} // namespace

// A set copied on the GUI thread for the open's thread: the registry's
// can be unregistered, and a plugin's freed, while the open runs
struct RecordingView::FrameSet {
    std::vector<ecuio_signal_def_t> defs;
    std::vector<ecuio_channel_decl_t> channels;
    std::vector<ecuio_enum_entry_t> enums;
    std::vector<ecuio_computed_t> computed;
    ecuio_signal_set_t set{};
    QString decodedWith;

    explicit FrameSet(const ecuio_signal_set_t& s)
        : defs(s.defs, s.defs + (s.defs != nullptr ? s.count : 0)),
          channels(s.channels,
                   s.channels + (s.channels != nullptr ? s.channel_count : 0)),
          enums(s.enums, s.enums + (s.enums != nullptr ? s.enum_count : 0)),
          computed(s.computed,
                   s.computed + (s.computed != nullptr ? s.computed_count : 0))
    {
        set = s;
        set.defs = defs.empty() ? nullptr : defs.data();
        set.channels = channels.empty() ? nullptr : channels.data();
        set.enums = enums.empty() ? nullptr : enums.data();
        set.computed = computed.empty() ? nullptr : computed.data();
    }
};

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
    m_reportStop = true;
    stop();
    // Its end, posted to this object, is dropped with it
    if (m_reportWorker.joinable()) {
        m_reportWorker.join();
    }
}

void RecordingView::run(const std::shared_ptr<Result>& res,
                        std::atomic<bool>* cancel, RecordingView* view,
                        quint64 generation, const QString& folder,
                        const std::shared_ptr<FrameSet>& frames)
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
    res->rc =
        frames ? ecuio_recording_open_frames(path.constData(), &frames->set,
                                             onProgress, &opening, &res->reader)
               : ecuio_recording_open(path.constData(), onProgress, &opening,
                                      &res->reader);
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
    std::shared_ptr<FrameSet> frames;
    if (source.own_file) {
        QFile file(QDir(folder).filePath(QStringLiteral("signals.json")));
        const QByteArray bytes = file.open(QIODevice::ReadOnly)
                                     ? file.read(ECUIO_SIGNAL_FILE_MAX_BYTES)
                                     : QByteArray();
        ecuio_signal_file_t* loaded = nullptr;
        if (ecuio_signal_file_load(bytes.constData(),
                                   static_cast<size_t>(bytes.size()),
                                   &loaded) != ECUIO_OK) {
            fail(folder,
                 QStringLiteral("its signals.json doesn't load: %1")
                     .arg(QString::fromUtf8(ecuio_last_error_detail())));
            return;
        }
        frames = std::make_shared<FrameSet>(*ecuio_signal_file_set(loaded));
        ecuio_signal_file_free(loaded);
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
        frames = std::make_shared<FrameSet>(*set);
        frames->decodedWith = name;
    }
    start(folder, frames);
}

void RecordingView::start(const QString& folder,
                          const std::shared_ptr<FrameSet>& frames)
{
    m_folder = folder;
    m_error.clear();
    m_decodedWith = frames ? frames->decodedWith : QString();
    m_state = Opening;
    m_progress = 0;
    const quint64 generation = ++m_generation;
    auto res = std::make_shared<Result>();
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
            own.size() <= static_cast<qint64>(ECUIO_SIGNAL_FILE_MAX_BYTES)) {
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
    m_check.clear();
    if (m_stale) {
        m_stale = false;
        emit staleChanged();
    }
}

bool RecordingView::saveHealthReport(const QUrl& url, int filterIndex,
                                     const QString& baselineFolder)
{
    if (m_state != Open || m_reporting) {
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
            removeIfEmpty(pathOfUrl(url));
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
    if (!query(ids, t0Ms, t1Ms, static_cast<uint32_t>(n), cols)) {
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

// The reader takes no time before the start, so a window from before it
// is split as havne-cli view splits one (cli/view_lines.h, window_of):
// the columns wholly before the start stay empty, the one across it is a
// query of its own from 0, and the rest another. Columns of a second or
// more place whole seconds, each in the column it starts in, the first
// column taking one that starts before it: there the column across the
// start ends at its last whole second, and the second across the split
// goes to the next column, so none is counted twice.
bool RecordingView::query(const std::vector<uint32_t>& ids, double t0Ms,
                          double t1Ms, uint32_t n,
                          std::vector<ecuio_recording_column_t>& cols)
{
    int64_t t0 = 0;
    int64_t t1 = 0;
    if (!signedUs(t0Ms, &t0) || !signedUs(t1Ms, &t1) || t1 <= t0) {
        return false;
    }
    const auto nsig = static_cast<uint32_t>(ids.size());
    std::vector<ecuio_recording_column_t> got;
    // count columns over [from, to) into columns [first, first + count)
    const auto into = [&](uint64_t from, uint64_t to, uint32_t first,
                          uint32_t count) {
        got.assign(static_cast<size_t>(nsig) * count, {});
        const ecuio_status_t rc = ecuio_recording_query(
            m_reader, ids.data(), nsig, from, to, got.data(), count);
        if (rc == ECUIO_ERR_RECORDING_CHANGED) {
            markStale();
        }
        if (rc != ECUIO_OK) {
            return false;
        }
        for (size_t k = 0; k < nsig; ++k) {
            std::copy(
                got.begin() + static_cast<std::ptrdiff_t>(k * count),
                got.begin() + static_cast<std::ptrdiff_t>((k + 1) * count),
                cols.begin() + static_cast<std::ptrdiff_t>(k * n + first));
        }
        return true;
    };
    if (t0 >= 0) {
        return into(static_cast<uint64_t>(t0), static_cast<uint64_t>(t1), 0, n);
    }
    if (t1 <= 0) {
        return true; // all before the start
    }
    const int64_t width = (t1 - t0 + n - 1) / n;
    // The first column starting at the start or later: t0 + n x width
    // reaches t1, past 0, so it's within the n
    const auto skip = static_cast<uint32_t>((-t0 + width - 1) / width);
    const uint32_t rest = n - skip;
    const auto restT0 = static_cast<uint64_t>(t0 + skip * width);
    if (skip > 0 && restT0 > 0) {
        uint64_t end = restT0;
        if (rest > 0 && width >= 1000000) {
            end = restT0 / 1000000 * 1000000;
        }
        if (end > 0 && !into(0, end, skip - 1, 1)) {
            return false;
        }
    }
    return rest == 0 ||
           into(restT0, static_cast<uint64_t>(t0 + n * width), skip, rest);
}

double RecordingView::found(const ecuio_recording_find_t& what)
{
    uint64_t at = ECUIO_RECORDING_NONE;
    const ecuio_status_t rc = ecuio_recording_find(m_reader, &what, 0, &at);
    if (rc == ECUIO_ERR_RECORDING_CHANGED) {
        markStale();
    }
    return rc == ECUIO_OK ? toMsOrNone(at) : -1.0;
}

double RecordingView::findThreshold(int signal, double level, double hysteresis,
                                    bool rising)
{
    uint32_t i = 0;
    if (m_reader == nullptr || !signalIndex(signal, &i) ||
        (m_signals[i].pub.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0 ||
        !std::isfinite(level) || !(hysteresis >= 0)) {
        return -1.0;
    }
    // The level as recorded: every display conversion is a rising line
    // (signalList's ranges rest on it too), so two points of it invert it
    const Signal& s = m_signals[i];
    const double at0 = shown(s, 0).value;
    const double slope = shown(s, 1).value - at0;
    if (!(slope > 0)) {
        return -1.0;
    }
    ecuio_recording_find_t what{};
    what.kind = ECUIO_RECORDING_FIND_THRESHOLD;
    what.signal = i;
    what.direction =
        rising ? ECUIO_RECORDING_FIND_RISING : ECUIO_RECORDING_FIND_FALLING;
    what.level = (level - at0) / slope;
    what.hysteresis = hysteresis / slope;
    return found(what);
}

double RecordingView::findMarker(const QString& label)
{
    if (m_reader == nullptr) {
        return -1.0;
    }
    const QByteArray utf8 = label.toUtf8();
    ecuio_recording_find_t what{};
    what.kind = ECUIO_RECORDING_FIND_MARKER;
    what.label_utf8 = utf8.constData();
    return found(what);
}

double RecordingView::findEngineStart()
{
    if (m_reader == nullptr) {
        return -1.0;
    }
    ecuio_recording_find_t what{};
    what.kind = ECUIO_RECORDING_FIND_ENGINE_START;
    return found(what);
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
        QString label;
        const bool number = (s.pub.flags & ECUIO_RECORDING_SIGNAL_TEXT) == 0;
        if (number && at != ECUIO_RECORDING_NONE) {
            // signals.csv's numbers are C-locale, as QByteArray reads them
            bool ok = false;
            const double value = QByteArray(text).toDouble(&ok);
            if (ok) {
                const ecuio_display_value_t d = shown(s, value);
                shownText = QString::number(d.value, 'f', d.decimal_places);
                unit = QString::fromUtf8(d.unit);
                // An enum's raw number and its label (format 3's
                // events.log): the word shows, the number plots
                if (value >= 0.0 && value <= 4294967295.0 &&
                    value == std::floor(value)) {
                    const char* l = ecuio_recording_enum_label(
                        m_reader, i, static_cast<uint32_t>(value));
                    if (l != nullptr) {
                        label = QString::fromUtf8(l);
                    }
                }
            }
        } else if (number) {
            unit = QString::fromUtf8(shown(s, 0).unit);
        }
        out.append(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(i)},
            {QStringLiteral("text"), shownText},
            {QStringLiteral("unit"), unit},
            {QStringLiteral("label"), label},
            {QStringLiteral("atMs"), toMsOrNone(at)},
        });
    }
    return out;
}

bool RecordingView::haveAsks()
{
    if (!m_asks) {
        try {
            m_asks = std::make_shared<Asks>();
        } catch (const std::bad_alloc&) {
            return false;
        }
    }
    return true;
}

bool RecordingView::startAsks()
{
    {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->holdMs = m_asksHoldMs;
        m_asks->yieldHoldMs = m_yieldHoldMs;
        m_asks->loseMeans = m_loseMeansForTesting;
        if (m_asks->answering) {
            return true; // it takes the ask before it ends
        }
        m_asks->answering = true;
    }
    try {
        if (m_failThreadForTesting) {
            throw std::bad_alloc();
        }
        // A task of the global pool with its share of the reader and the
        // asks: a close leaves it to finish, as a stretches pass
        QThreadPool::globalInstance()->start(
            [asks = m_asks, reader = m_readerHold, stop = m_stop,
             view = QPointer<RecordingView>(this), generation = m_generation] {
                answerAsks(asks, reader, stop, view, generation);
            });
    } catch (const std::bad_alloc&) {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->answering = false;
        return false;
    }
    return true;
}

namespace {

// Each id's mean in each of the columns over [t0, t1) of the recording's
// time in microseconds, a row of `columns` per id in out, NaN for none:
// every column from the statistics over its own microseconds. Ends early,
// with ECUIO_ERR_CANCELED, once stale or the open's stopped is set. Starts
// at column `next` (0: out is made) and, with `waiting` set, stops before
// a column after the first with `yielded` and `next` the column it left
// off at, so a call again resumes there
ecuio_status_t ownColumnMeans(ecuio_recording_t* reader,
                              const std::vector<uint32_t>& ids, int64_t t0,
                              int64_t t1, uint32_t columns,
                              const std::atomic<bool>& stale,
                              const std::atomic<bool>& stopped,
                              const std::atomic<bool>& waiting, uint32_t& next,
                              bool& yielded, std::vector<double>& out)
{
    yielded = false;
    if (next == 0) {
        out.assign(ids.size() * columns, NAN);
    }
    std::vector<ecuio_recording_stats_t> stats(ids.size());
    const double step = static_cast<double>(t1 - t0) / columns;
    const uint32_t first = next;
    for (uint32_t c = first; c < columns; ++c) {
        if (stale.load() || stopped.load()) {
            return ECUIO_ERR_CANCELED;
        }
        if (c > first && waiting.load()) {
            next = c;
            yielded = true;
            return ECUIO_OK;
        }
        // The column's first microsecond and the next column's
        const int64_t from = t0 + std::llround(c * step);
        const int64_t past =
            c + 1 == columns ? t1 : t0 + std::llround((c + 1) * step);
        // Before the recording, or too narrow for a closed span
        if (past - 1 <= (std::max<int64_t>)(from, 0)) {
            continue;
        }
        const ecuio_status_t rc = ecuio_recording_stats(
            reader, ids.data(), static_cast<uint32_t>(ids.size()),
            static_cast<uint64_t>((std::max<int64_t>)(from, 0)),
            static_cast<uint64_t>(past - 1), stats.data());
        if (rc != ECUIO_OK) {
            return rc;
        }
        for (size_t k = 0; k < ids.size(); ++k) {
            if (stats[k].samples > 0) {
                out[k * columns + c] = stats[k].mean;
            }
        }
    }
    next = columns;
    return ECUIO_OK;
}

} // namespace

void RecordingView::answerAsks(const std::shared_ptr<Asks>& asks,
                               const std::shared_ptr<ecuio_recording_t>& reader,
                               const std::shared_ptr<Stop>& stop,
                               const QPointer<RecordingView>& view,
                               quint64 generation)
{
    // A closed open's call isn't made: one call is at most a few hundred
    // ms, which only the application's end waits for
    const auto stopped = [&stop] {
        if (!stop->stopped.load()) {
            return false;
        }
        ++stop->readsStopped;
        return true;
    };
    // The means pass under way, kept across turns of the loop: it gives way
    // to a statistics or step ask put in (`waiting`) and resumes at the next
    // column after it, so a cursor ask never waits for a whole pass
    std::optional<MeansAsk> pass;
    std::vector<double> passOut;
    uint32_t passAt = 0;
    for (;;) {
        std::optional<StatsAsk> stats;
        std::optional<StepAsk> step;
        int holdMs = 0;
        int yieldHoldMs = 0;
        bool loseMeans = false;
        {
            // Ending and an ask's arrival are decided under one lock, so an
            // ask put in while this runs is never left behind
            const std::lock_guard<std::mutex> lock(asks->m);
            if (!asks->stats && !asks->step && !asks->means && !pass) {
                asks->answering = false;
                return;
            }
            stats.swap(asks->stats);
            step.swap(asks->step);
            asks->waiting = false;
            bool took = stats || step;
            if (asks->means) {
                // A newer ask replaces a pass under way: its answer is none
                if (pass) {
                    ++asks->stopped;
                }
                pass = std::move(asks->means);
                asks->means.reset();
                passOut.clear();
                passAt = 0;
                asks->meansStale = false;
                took = true;
            }
            holdMs = took ? asks->holdMs : 0;
            yieldHoldMs = asks->yieldHoldMs;
            loseMeans = asks->loseMeans;
        }
        if (holdMs > 0) {
            ++asks->held;
            std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
        }
        // An application ending reads null here, as a stretches pass does
        QCoreApplication* app = QCoreApplication::instance();
        if (app == nullptr) {
            const std::lock_guard<std::mutex> lock(asks->m);
            asks->answering = false;
            return;
        }
        // A copy that can't be allocated leaves its ask unanswered: an
        // exception out of this task would end the process
        try {
            if (step && !stopped()) {
                uint64_t at = ECUIO_RECORDING_NONE;
                const ecuio_status_t rc = ecuio_recording_step(
                    reader.get(), step->signal, step->t, step->direction, &at);
                QMetaObject::invokeMethod(
                    app,
                    [view, request = step->request, rc, at, generation] {
                        if (view) {
                            view->stepFinished(request, rc, at, generation);
                        }
                    },
                    Qt::QueuedConnection);
            }
        } catch (const std::bad_alloc&) {
            ++asks->lost;
        }
        try {
            if (stats && !stopped()) {
                std::vector<ecuio_recording_stats_t> got(stats->ids.size());
                const ecuio_status_t rc = ecuio_recording_stats(
                    reader.get(), stats->ids.data(),
                    static_cast<uint32_t>(stats->ids.size()), stats->t0,
                    stats->t1, got.data());
                QMetaObject::invokeMethod(
                    app,
                    [view, ask = *stats, rc, got, generation] {
                        if (view) {
                            view->statsFinished(ask, rc, got, generation);
                        }
                    },
                    Qt::QueuedConnection);
            }
        } catch (const std::bad_alloc&) {
            ++asks->lost;
        }
        try {
            if (pass) {
                if (loseMeans) {
                    throw std::bad_alloc();
                }
                bool yielded = false;
                const ecuio_status_t rc = ownColumnMeans(
                    reader.get(), pass->ids, pass->t0, pass->t1, pass->columns,
                    asks->meansStale, stop->stopped, asks->waiting, passAt,
                    yielded, passOut);
                if (yielded) {
                    ++asks->yields; // the pass stays, resumed after the ask
                    if (yieldHoldMs > 0) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(yieldHoldMs));
                    }
                } else if (rc == ECUIO_ERR_CANCELED) {
                    // A pass a newer means ask or a close ended has no answer
                    ++asks->stopped;
                    if (stop->stopped.load()) {
                        ++stop->readsStopped;
                    }
                    pass.reset();
                } else {
                    QMetaObject::invokeMethod(
                        app,
                        [view, ask = *pass, rc, got = std::move(passOut),
                         generation] {
                            if (view) {
                                view->meansFinished(ask, rc, got, generation);
                            }
                        },
                        Qt::QueuedConnection);
                    pass.reset();
                    passOut.clear();
                }
            }
        } catch (const std::bad_alloc&) {
            ++asks->lost;
            // A page pairs two views' means and waits for both: this one
            // says it has none, where that much can still be posted
            const std::optional<MeansAsk> lost = std::move(pass);
            pass.reset();
            passOut.clear();
            try {
                if (lost) {
                    QMetaObject::invokeMethod(
                        app,
                        [view, request = lost->request, generation] {
                            if (view) {
                                view->meansLost(request, generation);
                            }
                        },
                        Qt::QueuedConnection);
                }
            } catch (const std::bad_alloc&) {
                ++asks->lost; // not even that: the ask stays unanswered
            }
        }
    }
}

namespace {

enum class RuleUnit { Percent, Seconds, Rpm, Speed };

// One level of ecuio_stretch_rules_t as a page sets it: its key in the
// rules map, its name in a refusal's words, core's default (%, seconds,
// rpm or km/h) and where a value in core's unit (%, us, rpm, km/h) goes
struct StretchRule {
    const char* key;
    const char* name;
    RuleUnit unit;
    double fallback;
    void (*set)(ecuio_stretch_rules_t&, double);
};

uint64_t wholeUs(double us)
{
    return static_cast<uint64_t>(std::llround(us));
}

const StretchRule kStretchRules[] = {
    {"pullLevel", "Pull level", RuleUnit::Percent,
     ECUIO_STRETCH_DEFAULT_PULL_LEVEL,
     [](ecuio_stretch_rules_t& r, double v) { r.pull_level = v; }},
    {"pullLoadLevel", "Pull load level", RuleUnit::Percent,
     ECUIO_STRETCH_DEFAULT_PULL_LOAD_LEVEL,
     [](ecuio_stretch_rules_t& r, double v) { r.pull_load_level = v; }},
    {"pullHoldS", "Pull hold", RuleUnit::Seconds,
     ECUIO_STRETCH_DEFAULT_PULL_HOLD_US / 1e6,
     [](ecuio_stretch_rules_t& r, double v) { r.pull_hold_us = wholeUs(v); }},
    {"pullGapS", "Pull gap", RuleUnit::Seconds,
     ECUIO_STRETCH_DEFAULT_PULL_GAP_US / 1e6,
     [](ecuio_stretch_rules_t& r, double v) { r.pull_gap_us = wholeUs(v); }},
    {"pullRise", "Pull rise", RuleUnit::Rpm, ECUIO_STRETCH_DEFAULT_PULL_RISE,
     [](ecuio_stretch_rules_t& r, double v) { r.pull_min_rise = v; }},
    {"pullGain", "Pull gain", RuleUnit::Speed, ECUIO_STRETCH_DEFAULT_PULL_GAIN,
     [](ecuio_stretch_rules_t& r, double v) { r.pull_min_gain = v; }},
    {"cruiseHoldS", "Cruise hold", RuleUnit::Seconds,
     ECUIO_STRETCH_DEFAULT_CRUISE_HOLD_US / 1e6,
     [](ecuio_stretch_rules_t& r, double v) { r.cruise_hold_us = wholeUs(v); }},
    {"cruiseSpeedBand", "Cruise speed band", RuleUnit::Speed,
     ECUIO_STRETCH_DEFAULT_CRUISE_SPEED_BAND,
     [](ecuio_stretch_rules_t& r, double v) { r.cruise_speed_band = v; }},
    {"cruiseThrottleBand", "Cruise throttle band", RuleUnit::Percent,
     ECUIO_STRETCH_DEFAULT_CRUISE_THROTTLE_BAND,
     [](ecuio_stretch_rules_t& r, double v) { r.cruise_throttle_band = v; }},
    {"cruiseFloor", "Cruise floor", RuleUnit::Speed,
     ECUIO_STRETCH_DEFAULT_CRUISE_FLOOR,
     [](ecuio_stretch_rules_t& r, double v) { r.cruise_floor = v; }},
};

// The most a rule takes: a percentage, an hour, and a speed in km/h
constexpr double kPercentMost = 100.0;
constexpr double kSecondsMost = 3600.0;
constexpr double kSpeedMostKmh = 1000.0;

// 1 km/h in the unit system's speed unit, by core's display table
double speedPerKmh(uint32_t units)
{
    ecuio_display_value_t one{};
    return ecuio_display_convert("km/h", 0, 1.0, units, &one) ==
                   ECUIO_DISPLAY_OK
               ? one.value
               : 1.0;
}

bool isNumber(const QVariant& v)
{
    switch (v.typeId()) {
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return true;
    default:
        return false;
    }
}

// A page's rules map as core's struct, its speeds from the unit system's
// unit to km/h; false with the refusal's words, naming the rule
bool stretchRulesOf(const QVariantMap& map, uint32_t units,
                    ecuio_stretch_rules_t* out, QString* problem)
{
    const double perKmh = speedPerKmh(units);
    const bool imperial = units == ECUIO_UNITS_IMPERIAL;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        const StretchRule* rule = nullptr;
        for (const StretchRule& r : kStretchRules) {
            if (it.key() == QLatin1String(r.key)) {
                rule = &r;
            }
        }
        if (rule == nullptr) {
            *problem =
                QStringLiteral("'%1' isn't a stretch rule").arg(it.key());
            return false;
        }
        const double shown = isNumber(it.value()) ? it.value().toDouble() : NAN;
        double value = shown; // in core's unit
        double most = INFINITY;
        QString range = QStringLiteral(", 0 or more");
        switch (rule->unit) {
        case RuleUnit::Percent:
            most = kPercentMost;
            range = QStringLiteral(" from 0 to 100");
            break;
        case RuleUnit::Seconds:
            most = kSecondsMost;
            range = QStringLiteral(" from 0 to 3600 s");
            value = shown * 1e6;
            break;
        case RuleUnit::Speed:
            most = kSpeedMostKmh * perKmh;
            range = QStringLiteral(" from 0 to %1 %2")
                        .arg(static_cast<int>(most))
                        .arg(imperial ? QStringLiteral("mph")
                                      : QStringLiteral("km/h"));
            value = shown / perKmh;
            break;
        case RuleUnit::Rpm:
            break;
        }
        if (!std::isfinite(shown) || shown < 0.0 || shown > most) {
            *problem = QStringLiteral("%1 must be a number%2")
                           .arg(QLatin1String(rule->name), range);
            return false;
        }
        rule->set(*out, value);
    }
    return true;
}

} // namespace

QVariantMap RecordingView::stretchDefaults() const
{
    const double perKmh = speedPerKmh(m_units);
    QVariantMap out;
    for (const StretchRule& r : kStretchRules) {
        out.insert(QLatin1String(r.key), r.unit == RuleUnit::Speed
                                             ? r.fallback * perKmh
                                             : r.fallback);
    }
    return out;
}

void RecordingView::setStretchesProblem(const QString& words)
{
    if (words != m_stretchesProblem) {
        m_stretchesProblem = words;
        emit stretchesProblemChanged();
    }
}

void RecordingView::findStretches(
    const std::shared_ptr<ecuio_recording_t>& reader,
    const std::shared_ptr<Stop>& stop, const QPointer<RecordingView>& view,
    int request, ecuio_stretch_rules_t rules, quint64 generation, int holdMs,
    int stopAt, int holdAt)
{
    struct Asked {
        Stop* stop;
        int request;
        int stopAt; // a test's
        int holdAt; // a test's
        int asks;
    } asked{stop.get(), request, stopAt, holdAt, 0};
    // A close or a newer stretches ask ends the pass at its next second
    const ecuio_cancel_t closed = [](void* user) {
        auto* a = static_cast<Asked*>(user);
        ++a->asks;
        if (a->asks == a->holdAt) {
            ++a->stop->stretchesHeld;
            const auto until =
                std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!a->stop->stopped.load() &&
                   a->stop->latestStretches.load() <= a->request &&
                   std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return a->stop->stopped.load() ||
                       a->stop->latestStretches.load() > a->request ||
                       (a->stopAt > 0 && a->asks >= a->stopAt)
                   ? 1
                   : 0;
    };
    if (holdMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    }
    ecuio_recording_info_t info{};
    ecuio_recording_get_info(reader.get(), &info);
    const uint64_t end = info.duration_us + 1;
    // One pass for a recording with no more stretches than this; a second
    // only for one with more
    const uint32_t first = 64;
    ecuio_status_t rc = ECUIO_ERR_OUT_OF_MEMORY;
    std::vector<ecuio_recording_stretch_t> got;
    try {
        got.resize(first);
        uint32_t count = 0;
        rc =
            ecuio_recording_stretches_with(reader.get(), &rules, 0, end, closed,
                                           &asked, got.data(), first, &count);
        if (rc == ECUIO_OK && count > first) {
            got.resize(count);
            rc = ecuio_recording_stretches_with(reader.get(), &rules, 0, end,
                                                closed, &asked, got.data(),
                                                count, &count);
        }
        got.resize(rc == ECUIO_OK ? (std::min<size_t>)(got.size(), count) : 0);
    } catch (const std::bad_alloc&) {
        got.clear();
        rc = ECUIO_ERR_OUT_OF_MEMORY;
    }
    stop->passAsks = asked.asks;
    if (rc == ECUIO_ERR_CANCELED) {
        // Closed or asked again: its answer would be dropped
        if (!stop->stopped.load() && stop->latestStretches.load() > request) {
            ++stop->stretchesStaled;
        } else {
            ++stop->readsStopped;
        }
        return;
    }
    // An application ending reads null here (Qt 6.12 clears instance(),
    // then waits for the pool); one read non-null lives until this ends
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr) {
        return;
    }
    QMetaObject::invokeMethod(
        app,
        [view, request, rc, got, generation] {
            if (view) {
                view->stretchesFinished(request, rc, got, generation);
            }
        },
        Qt::QueuedConnection);
}

int RecordingView::stats(const QVariantList& signalIndexes, double t0Ms,
                         double t1Ms)
{
    if (m_reader == nullptr || signalIndexes.isEmpty()) {
        return 0;
    }
    StatsAsk ask;
    for (const QVariant& v : signalIndexes) {
        uint32_t i = 0;
        if (!signalIndex(v, &i)) {
            return 0;
        }
        ask.ids.push_back(i);
    }
    ask.t0 = fromMs(t0Ms);
    ask.t1 = fromMs(t1Ms);
    if (ask.t1 <= ask.t0) {
        return 0;
    }
    if (!haveAsks()) {
        return 0;
    }
    ask.request = ++m_lastAsk;
    m_lastStats = ask.request;
    {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->stats = std::move(ask);
        m_asks->waiting = true;
    }
    if (!startAsks()) {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->stats.reset();
        m_lastStats = 0;
        return 0;
    }
    return m_lastStats;
}

int RecordingView::columnMeans(const QVariantList& signalIndexes, double t0Ms,
                               double t1Ms, int columns)
{
    if (m_reader == nullptr || signalIndexes.isEmpty() || columns < 1 ||
        columns > kMaxColumns) {
        return 0;
    }
    MeansAsk ask;
    for (const QVariant& v : signalIndexes) {
        uint32_t i = 0;
        if (!signalIndex(v, &i)) {
            return 0;
        }
        ask.ids.push_back(i);
    }
    if (!signedUs(t0Ms, &ask.t0) || !signedUs(t1Ms, &ask.t1) ||
        ask.t1 <= ask.t0 || !haveAsks()) {
        return 0;
    }
    ask.columns = static_cast<uint32_t>(columns);
    ask.request = ++m_lastAsk;
    m_lastMeans = ask.request;
    {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->means = std::move(ask);
        m_asks->meansStale = true; // an older pass under way stops
    }
    if (!startAsks()) {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->means.reset();
        m_lastMeans = 0;
        return 0;
    }
    return m_lastMeans;
}

int RecordingView::step(int signal, double tMs, bool forward)
{
    uint32_t i = 0;
    if (m_reader == nullptr || !signalIndex(QVariant(signal), &i) ||
        !haveAsks()) {
        return 0;
    }
    StepAsk ask;
    ask.request = ++m_lastAsk;
    ask.signal = i;
    ask.t = fromMs(tMs);
    ask.direction =
        forward ? ECUIO_RECORDING_STEP_FORWARD : ECUIO_RECORDING_STEP_BACKWARD;
    m_lastStep = ask.request;
    {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->step = ask;
        m_asks->waiting = true;
    }
    if (!startAsks()) {
        const std::lock_guard<std::mutex> lock(m_asks->m);
        m_asks->step.reset();
        m_lastStep = 0;
        return 0;
    }
    return m_lastStep;
}

int RecordingView::stretches(bool pulls, bool cruises, const QVariantMap& rules)
{
    // Judged here, before any task: a refusal is the caller's at once
    ecuio_stretch_rules_t asked{};
    QString problem;
    const bool taken = stretchRulesOf(rules, m_units, &asked, &problem);
    setStretchesProblem(problem);
    if (!taken || m_reader == nullptr || (!pulls && !cruises)) {
        return 0;
    }
    asked.kinds = (pulls ? ECUIO_STRETCH_PULLS : 0u) |
                  (cruises ? ECUIO_STRETCH_CRUISES : 0u);
    asked.throttle = asked.engine_load = UINT32_MAX;
    asked.vehicle_speed = asked.engine_speed = UINT32_MAX;
    const int request = ++m_lastAsk;
    try {
        if (m_failThreadForTesting) {
            throw std::bad_alloc();
        }
        // A task of the global pool with its share of the reader: a close
        // or a newer stretches ask stops it at its next second, and
        // QCoreApplication's end waits for it
        QThreadPool::globalInstance()->start(
            [reader = m_readerHold, stop = m_stop,
             view = QPointer<RecordingView>(this), request, asked,
             generation = m_generation, holdMs = m_stretchesHoldMs,
             stopAt = m_stretchesStopAt, holdAt = m_stretchesHoldAt] {
                findStretches(reader, stop, view, request, asked, generation,
                              holdMs, stopAt, holdAt);
            });
    } catch (const std::bad_alloc&) {
        return 0;
    }
    // Only a pass that started ends an older one
    m_stop->latestStretches = request;
    m_lastStretches = request;
    return request;
}

void RecordingView::stretchesFinished(
    int request, ecuio_status_t rc,
    const std::vector<ecuio_recording_stretch_t>& got, quint64 generation)
{
    if (generation != m_generation || request != m_lastStretches) {
        return;
    }
    if (rc == ECUIO_ERR_RECORDING_CHANGED) {
        markStale();
    }
    QVariantList out;
    for (size_t k = 0; rc == ECUIO_OK && k < got.size(); ++k) {
        const ecuio_recording_stretch_t& g = got[k];
        if (g.signal >= m_signals.size()) {
            continue;
        }
        const ecuio_display_value_t start = shown(m_signals[g.signal], g.start);
        out.append(QVariantMap{
            {QStringLiteral("kind"), g.kind == ECUIO_RECORDING_STRETCH_PULL
                                         ? QStringLiteral("pull")
                                         : QStringLiteral("cruise")},
            {QStringLiteral("fromMs"), toMs(g.from_us)},
            {QStringLiteral("toMs"), toMs(g.to_us)},
            {QStringLiteral("start"), start.value},
            {QStringLiteral("end"), shown(m_signals[g.signal], g.end).value},
            {QStringLiteral("unit"), QString::fromUtf8(start.unit)},
            {QStringLiteral("decimals"), start.decimal_places},
            {QStringLiteral("signal"), static_cast<int>(g.signal)},
        });
    }
    emit stretchesReady(request, out, rc == ECUIO_OK);
}

void RecordingView::statsFinished(
    const StatsAsk& ask, ecuio_status_t rc,
    const std::vector<ecuio_recording_stats_t>& got, quint64 generation)
{
    if (generation != m_generation || ask.request != m_lastStats) {
        return;
    }
    if (rc == ECUIO_ERR_RECORDING_CHANGED) {
        markStale();
    }
    QVariantList out;
    if (rc == ECUIO_OK) {
        const double seconds = static_cast<double>(ask.t1 - ask.t0) / 1e6;
        for (size_t k = 0; k < ask.ids.size(); ++k) {
            const Signal& s = m_signals[ask.ids[k]];
            const ecuio_recording_stats_t& g = got[k];
            // Every conversion is a rising line, so a mean converts as a value
            const ecuio_display_value_t first = shown(s, g.first);
            const double last = shown(s, g.last).value;
            const double change = last - first.value;
            out.append(QVariantMap{
                {QStringLiteral("index"), static_cast<int>(ask.ids[k])},
                {QStringLiteral("samples"), static_cast<double>(g.samples)},
                {QStringLiteral("first"), first.value},
                {QStringLiteral("last"), last},
                {QStringLiteral("lowest"), shown(s, g.lowest).value},
                {QStringLiteral("highest"), shown(s, g.highest).value},
                {QStringLiteral("mean"), shown(s, g.mean).value},
                {QStringLiteral("change"), change},
                {QStringLiteral("rate"), change / seconds},
                {QStringLiteral("lowestMs"), toMsOrNone(g.lowest_us)},
                {QStringLiteral("highestMs"), toMsOrNone(g.highest_us)},
                {QStringLiteral("unit"), QString::fromUtf8(first.unit)},
                {QStringLiteral("decimals"), first.decimal_places},
            });
        }
    }
    emit statsReady(ask.request, out);
}

void RecordingView::meansLost(int request, quint64 generation)
{
    if (generation == m_generation && request == m_lastMeans) {
        emit columnMeansReady(request, {});
    }
}

void RecordingView::meansFinished(const MeansAsk& ask, ecuio_status_t rc,
                                  const std::vector<double>& got,
                                  quint64 generation)
{
    if (generation != m_generation || ask.request != m_lastMeans) {
        return;
    }
    if (rc == ECUIO_ERR_RECORDING_CHANGED) {
        markStale();
    }
    QVariantList out;
    const double none = std::numeric_limits<double>::quiet_NaN();
    for (size_t k = 0; rc == ECUIO_OK && k < ask.ids.size(); ++k) {
        const Signal& s = m_signals[ask.ids[k]];
        const bool text = (s.pub.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0;
        QList<double> means(static_cast<qsizetype>(ask.columns), none);
        for (uint32_t c = 0; !text && c < ask.columns; ++c) {
            const double mean = got[k * ask.columns + c];
            if (std::isfinite(mean)) {
                // Every conversion is a rising line: a mean converts as a value
                means[static_cast<qsizetype>(c)] = shown(s, mean).value;
            }
        }
        out.append(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(ask.ids[k])},
            {QStringLiteral("means"), QVariant::fromValue(means)},
        });
    }
    emit columnMeansReady(ask.request, out);
}

void RecordingView::stepFinished(int request, ecuio_status_t rc, uint64_t at,
                                 quint64 generation)
{
    if (generation != m_generation || request != m_lastStep) {
        return;
    }
    if (rc == ECUIO_ERR_RECORDING_CHANGED) {
        markStale();
    }
    emit stepReady(request, rc == ECUIO_OK ? toMsOrNone(at) : -1.0);
}

double RecordingView::nearestSampleMs(int signal, double tMs)
{
    uint32_t i = 0;
    if (m_reader == nullptr || !signalIndex(QVariant(signal), &i)) {
        return -1.0;
    }
    const Signal& s = m_signals[i];
    if (s.pub.first_us == ECUIO_RECORDING_NONE) {
        return -1.0;
    }
    const uint64_t t = fromMs(tMs);
    if (t <= s.pub.first_us) {
        return toMs(s.pub.first_us);
    }
    if (t >= s.pub.last_us) {
        return toMs(s.pub.last_us);
    }
    char text[kTextCapacity];
    // The sample at or before t (the reader's "in force")
    const auto sampleAtOrBefore = [&](uint64_t when, uint64_t* at) {
        const ecuio_status_t rc =
            ecuio_recording_text_at(m_reader, i, when, text, sizeof(text), at);
        if (rc == ECUIO_ERR_RECORDING_CHANGED) {
            markStale();
        }
        return rc == ECUIO_OK;
    };
    uint64_t before = ECUIO_RECORDING_NONE;
    if (!sampleAtOrBefore(t, &before) || before == ECUIO_RECORDING_NONE) {
        return -1.0;
    }
    // The first sample after t, looked for only as far past t as the one
    // before it is behind: the reader answers "at or before", so each
    // look past t that finds a sample there looks again just short of it
    constexpr int kMaxLooks = 16;
    uint64_t after = ECUIO_RECORDING_NONE;
    uint64_t reach = t + (t - before);
    for (int look = 0; look < kMaxLooks && reach > t; ++look) {
        uint64_t at = ECUIO_RECORDING_NONE;
        if (!sampleAtOrBefore(reach, &at)) {
            return -1.0;
        }
        if (at == ECUIO_RECORDING_NONE || at <= t) {
            break;
        }
        after = at;
        reach = at - 1;
    }
    return toMs(after != ECUIO_RECORDING_NONE && after - t < t - before
                    ? after
                    : before);
}
