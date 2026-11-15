// File/layer: app/recordingexport.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: RecordingExportTest (test_bridge); QML: QmlTest.recordings,
//          QmlTest.phone; MANUAL SMOKE: the test phone (a provider's folder)

#include "recordingexport.h"
#include "urlwrite.h"

#include "healthreport.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>

#include <chrono>
#include <new>
#include <system_error>

#include "ecuio/core.h"
#include "ecuio/recording.h"

static_assert(RecordingExport::Folder == ECUIO_EXPORT_FOLDER &&
                  RecordingExport::Csv == ECUIO_EXPORT_CSV &&
                  RecordingExport::Pcapng == ECUIO_EXPORT_PCAPNG,
              "RecordingExport::Format is core's");

namespace {

constexpr int kMaxIntervals = 64;
// A temporary folder's age past which no export still runs in it
constexpr qint64 kStaleSeconds = 24 * 60 * 60;

using Hold = RecordingExport::HoldForTesting;

// A worker's stop: the object's flag or the worker's own, asked by core
// (ecuio_export_with) and by the copy between chunks
struct Stop {
    const std::atomic<bool>* stopping = nullptr;
    const std::atomic<bool>* canceled = nullptr;
    Hold hold = Hold::None;
    int holdMs = 0;
    std::atomic<int>* held = nullptr;
    bool asked = false;

    bool stopped() const { return stopping->load() || canceled->load(); }
    // A test's hold: the stop, or its time, ends it
    void holdAt(Hold where) const
    {
        if (hold != where) {
            return;
        }
        held->fetch_add(1);
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(holdMs);
        while (!stopped() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

int stopAsked(void* user)
{
    auto* s = static_cast<Stop*>(user);
    if (!s->asked) {
        s->asked = true;
        s->holdAt(Hold::FirstAsk);
    }
    return s->stopped() ? 1 : 0;
}

// What a worker that ran out of memory says: it can't build its own answer
const auto kOutOfMemory = QStringLiteral("out of memory");

struct Job {
    QByteArray folder;
    QString stem;
    int format = 0;
    bool longLayout = false;
    quint32 rowMs = 0;
    quint32 units = 0;
    QList<QByteArray> columns;
    bool raw = false;
    QUrl target;
    bool throughProvider = false;
    bool failCopy = false; // setFailCopyForTesting
    bool failCopyAfterBytes = false; // setFailCopyAfterBytesForTesting
};

std::vector<const char*> pointers(const QList<QByteArray>& names)
{
    std::vector<const char*> out;
    out.reserve(static_cast<size_t>(names.size()));
    for (const QByteArray& n : names) {
        out.push_back(n.constData());
    }
    return out;
}

// Core's words for a refused call, read on the thread that made it
QString coreWords(ecuio_status_t status)
{
    const QString detail = QString::fromUtf8(ecuio_last_error_detail());
    return !detail.isEmpty()
               ? detail
               : QStringLiteral("the export failed (status %1)").arg(status);
}

ecuio_status_t exportTo(const Job& job, const QString& out,
                        ecuio_export_report_t* report, Stop& stop)
{
    const QByteArray outUtf8 = out.toUtf8();
    std::vector<const char*> names = pointers(job.columns);
    ecuio_export_options_t o{};
    o.format = static_cast<uint32_t>(job.format);
    o.flags = job.raw ? ECUIO_EXPORT_RAW : 0u;
    o.folder_utf8 = job.folder.constData();
    o.out_path_utf8 = outUtf8.constData();
    if (job.format == ECUIO_EXPORT_CSV) {
        o.layout = job.longLayout ? ECUIO_CSV_LONG : ECUIO_CSV_WIDE;
        if (!job.longLayout) {
            o.row_ms = job.rowMs;
            o.units = job.units;
            o.column_count = static_cast<uint32_t>(names.size());
            o.columns_utf8 = names.empty() ? nullptr : names.data();
        }
    }
    return ecuio_export_with(&o, stopAsked, &stop, report);
}

// One count with its noun's number
QString counted(quint64 n, const char* one, const char* many)
{
    return QStringLiteral("%1 %2").arg(n).arg(
        QLatin1String(n == 1 ? one : many));
}

QString writtenWords(const Job& job, const ecuio_export_report_t& r)
{
    if (job.format == ECUIO_EXPORT_PCAPNG) {
        return counted(r.frames, "frame", "frames");
    }
    if (job.format == ECUIO_EXPORT_FOLDER) {
        return counted(r.files, "file", "files");
    }
    const QString signalWords = counted(r.signal_count, "signal", "signals");
    return (job.longLayout ? counted(r.values, "value", "values")
                           : counted(r.rows, "row", "rows")) +
           QStringLiteral(" of ") + signalWords;
}

QString leftOutWords(const Job& job, const ecuio_export_report_t& r)
{
    if (job.raw) {
        return QStringLiteral("not scrubbed");
    }
    QStringList parts;
    const auto add = [&parts](quint64 n, const char* one, const char* many) {
        if (n > 0) {
            parts << counted(n, one, many);
        }
    };
    add(r.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION], "identification item",
        "identification items");
    add(r.left_out[ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE],
        "unattributable segment", "unattributable segments");
    add(r.left_out[ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES], "text value",
        "text values");
    add(r.left_out[ECUIO_EXPORT_LEFT_OUT_ACTIVITY], "activity line",
        "activity lines");
    add(r.files_left_out, "other file", "other files");
    return parts.isEmpty() ? QStringLiteral("nothing left out")
                           : QStringLiteral("left out: ") +
                                 parts.join(QStringLiteral(", "));
}

QString formatWords(const Job& job)
{
    if (job.format == ECUIO_EXPORT_PCAPNG) {
        return QStringLiteral("pcapng");
    }
    if (job.format == ECUIO_EXPORT_FOLDER) {
        return QStringLiteral("a folder copy");
    }
    return job.longLayout ? QStringLiteral("long CSV")
                          : QStringLiteral("wide CSV");
}

QVariantMap failedIn(const QByteArray& folder, const QString& words)
{
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("words"), words},
            {QStringLiteral("folder"), QString::fromUtf8(folder)},
            {QStringLiteral("line"),
             QStringLiteral("Export failed: %1").arg(words)}};
}

QVariantMap failed(const Job& job, const QString& words)
{
    return failedIn(job.folder, words);
}

// An export cancel() stopped: no failure, nothing written
QVariantMap canceledIn(const QByteArray& folder)
{
    return {
        {QStringLiteral("ok"), false},
        {QStringLiteral("canceled"), true},
        {QStringLiteral("words"), QStringLiteral("The export was canceled")},
        {QStringLiteral("folder"), QString::fromUtf8(folder)},
        {QStringLiteral("line"), QStringLiteral("Export canceled")}};
}

QVariantMap written(const Job& job, const ecuio_export_report_t& r,
                    const QString& name)
{
    const QString summary =
        writtenWords(job, r) + QStringLiteral(", ") + leftOutWords(job, r);
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("folder"), QString::fromUtf8(job.folder)},
            {QStringLiteral("name"), name},
            {QStringLiteral("summary"), summary},
            {QStringLiteral("line"),
             QStringLiteral("Exported %1: %2").arg(formatWords(job), summary)},
            {QStringLiteral("frames"), static_cast<double>(r.frames)},
            {QStringLiteral("values"), static_cast<double>(r.values)},
            {QStringLiteral("signals"), static_cast<double>(r.signal_count)},
            {QStringLiteral("rows"), static_cast<double>(r.rows)},
            {QStringLiteral("files"), static_cast<double>(r.files)}};
}

// A chunk at a time: a recording can be larger than the memory a phone
// gives the app. A stop ends it as a failure does
bool copyChunks(QFile& in, QIODevice& out, const Stop& stop)
{
    QByteArray chunk(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        if (stop.stopped()) {
            return false;
        }
        const qint64 n = in.read(chunk.data(), chunk.size());
        if (n <= 0) {
            return n == 0;
        }
        if (out.write(chunk.constData(), n) != n) {
            return false;
        }
    }
}

// Where a copy goes. InTree is a new file in a provider's folder, which Qt's
// content engine makes: the content resolver opens existing documents only
enum class CopyTo { Local, Picked, InTree };

// A local file is written whole or not at all; a provider's can't be renamed
// into place
bool copyFile(const QString& from, const QString& to, CopyTo where,
              const Stop& stop)
{
    QFile in(from);
    if (!in.open(QIODevice::ReadOnly)) {
        return false;
    }
    if (where == CopyTo::Local) {
        QSaveFile out(to);
        return out.open(QIODevice::WriteOnly) && copyChunks(in, out, stop) &&
               out.commit();
    }
    if (where == CopyTo::InTree) {
        QFile out(to);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            !copyChunks(in, out, stop) || !out.flush()) {
            return false;
        }
        out.close();
        return out.error() == QFileDevice::NoError;
    }
    PickedFile out;
    return openPicked(out, to, QIODevice::WriteOnly | QIODevice::Truncate) &&
           copyChunks(in, out, stop) && out.flush() && closePicked(out);
}

QString whereOf(const QUrl& url)
{
    return pathOfUrl(url);
}

QVariantMap runFile(const Job& job, Stop& stop)
{
    const bool provider = job.throughProvider || !job.target.isLocalFile();
    const QString target = whereOf(job.target);
    // A picker has already made the document it names: one that was empty
    // when this began goes if nothing is written, even part written; one
    // the user chose to replace stays
    const bool madeEmpty = provider && isEmptyDocument(target);
    const auto dropPicked = [&] {
        if (madeEmpty) {
            removePicked(target);
        }
    };
    // removeStaleCopies finds this folder by its name
    QTemporaryDir temp(QDir::tempPath() +
                       QStringLiteral("/havne-export-XXXXXX"));
    if (!temp.isValid()) {
        dropPicked();
        return failed(job, QStringLiteral("Couldn't make a temporary folder"));
    }
    const QString made = temp.filePath(QStringLiteral("export"));
    ecuio_export_report_t report{};
    const ecuio_status_t status = exportTo(job, made, &report, stop);
    if (status != ECUIO_OK) {
        const QString words = coreWords(status);
        dropPicked();
        return failed(job, words);
    }
    stop.holdAt(Hold::BeforeCopy);
    if (job.failCopyAfterBytes) {
        QFile half(target);
        if (half.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            half.write("half a document");
        }
    }
    if (job.failCopy || job.failCopyAfterBytes ||
        !copyFile(made, target, provider ? CopyTo::Picked : CopyTo::Local,
                  stop)) {
        dropPicked();
        return failed(job, QStringLiteral("Couldn't write the file"));
    }
    return written(job, report, QString());
}

// A name not yet used under parent: stem, stem-2 and on
QString freeName(const QString& parent, const QString& stem)
{
    for (int n = 1; n <= 999; ++n) {
        const QString name =
            n == 1 ? stem : stem + QStringLiteral("-%1").arg(n);
        if (!QFileInfo::exists(parent + QLatin1Char('/') + name)) {
            return name;
        }
    }
    return QString();
}

QVariantMap runFolder(const Job& job, Stop& stop)
{
    const bool provider = job.throughProvider || !job.target.isLocalFile();
    const QString parent = whereOf(job.target);
    const QString name = freeName(parent, job.stem);
    if (name.isEmpty()) {
        return failed(job, QStringLiteral("Every copy's name is taken there"));
    }
    const QString copy = parent + QLatin1Char('/') + name;
    ecuio_export_report_t report{};
    if (!provider) {
        // Core makes the folder whole or not at all, beside where it goes
        const ecuio_status_t status = exportTo(job, copy, &report, stop);
        return status == ECUIO_OK ? written(job, report, name)
                                  : failed(job, coreWords(status));
    }
    QTemporaryDir temp(QDir::tempPath() +
                       QStringLiteral("/havne-export-XXXXXX"));
    if (!temp.isValid()) {
        return failed(job, QStringLiteral("Couldn't make a temporary folder"));
    }
    const QString made = temp.filePath(QStringLiteral("copy"));
    const ecuio_status_t status = exportTo(job, made, &report, stop);
    if (status != ECUIO_OK) {
        return failed(job, coreWords(status));
    }
    stop.holdAt(Hold::BeforeCopy);
    if (!QDir(parent).mkdir(name)) {
        return failed(job, QStringLiteral("Couldn't make the folder there"));
    }
    const QStringList files = QDir(made).entryList(QDir::Files, QDir::Name);
    for (const QString& file : files) {
        if (job.failCopy ||
            !copyFile(made + QLatin1Char('/') + file,
                      copy + QLatin1Char('/') + file, CopyTo::InTree, stop)) {
            QDir(copy).removeRecursively();
            return failed(job, QStringLiteral("Couldn't write the folder"));
        }
    }
    return written(job, report, name);
}

QVariantMap runEstimate(const QByteArray& folder, const QList<quint32>& rowMs,
                        quint32 units, const QList<QByteArray>& columns,
                        Stop& stop)
{
    std::vector<const char*> names = pointers(columns);
    ecuio_export_options_t o{};
    o.format = ECUIO_EXPORT_CSV;
    o.layout = ECUIO_CSV_WIDE;
    o.units = units;
    o.folder_utf8 = folder.constData();
    o.column_count = static_cast<uint32_t>(names.size());
    o.columns_utf8 = names.empty() ? nullptr : names.data();
    std::vector<ecuio_export_estimate_t> sized(
        static_cast<size_t>(rowMs.size()));
    const ecuio_status_t status = ecuio_export_estimate_with(
        &o, rowMs.constData(), static_cast<uint32_t>(rowMs.size()), stopAsked,
        &stop, sized.data());
    if (status != ECUIO_OK) {
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("words"), coreWords(status)}};
    }
    QVariantList sizes;
    for (size_t i = 0; i < sized.size(); ++i) {
        sizes << QVariantMap{
            {QStringLiteral("rowMs"),
             static_cast<double>(rowMs[static_cast<qsizetype>(i)])},
            {QStringLiteral("rows"), static_cast<double>(sized[i].rows)},
            {QStringLiteral("bytes"), static_cast<double>(sized[i].bytes)}};
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("columns"),
             sized.empty() ? 0 : static_cast<int>(sized[0].columns)},
            {QStringLiteral("sizes"), sizes}};
}

QList<QByteArray> utf8Of(const QStringList& names)
{
    QList<QByteArray> out;
    out.reserve(names.size());
    for (const QString& n : names) {
        out << n.toUtf8();
    }
    return out;
}

} // namespace

RecordingExport::RecordingExport(QObject* parent) : QObject(parent) {}

RecordingExport::~RecordingExport()
{
    // What they post is dropped with this object
    m_stopping.store(true);
    if (m_worker.joinable()) {
        m_worker.join();
    }
    if (m_estimateWorker.joinable()) {
        m_estimateWorker.join();
    }
}

QString RecordingExport::fileStem(const QString& folder)
{
    static const QRegularExpression stamp(
        QStringLiteral("^\\d{8}-\\d{6}(-\\d+)?"));
    const QRegularExpressionMatch m =
        stamp.match(QFileInfo(QDir::cleanPath(folder)).fileName());
    return m.hasMatch() ? m.captured(0) : QStringLiteral("recording");
}

bool RecordingExport::holdsValues(const QString& folder)
{
    return QFileInfo::exists(folder + QStringLiteral("/signals.csv"));
}

int RecordingExport::removeStaleCopies(const QString& tempPath,
                                       const QDateTime& now)
{
    // QTemporaryDir's name for the templates of an export's copy, an
    // import's and a health report's (each names itself in a comment)
    static const QRegularExpression name(
        QStringLiteral("^havne-(export|import|health)-[A-Za-z0-9]{6}$"));
    int removed = 0;
    const QFileInfoList found = QDir(tempPath).entryInfoList(
        {QStringLiteral("havne-*")},
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden);
    for (const QFileInfo& folder : found) {
        if (name.match(folder.fileName()).hasMatch() &&
            folder.lastModified().secsTo(now) > kStaleSeconds &&
            QDir(folder.absoluteFilePath()).removeRecursively()) {
            ++removed;
        }
    }
    return removed;
}

bool RecordingExport::estimate(const QString& folder, const QVariantList& rowMs,
                               int units, const QStringList& columns)
{
    if (folder.isEmpty() || rowMs.isEmpty() || rowMs.size() > kMaxIntervals) {
        return false;
    }
    EstimateJob job;
    job.folder = folder.toUtf8();
    job.units = static_cast<quint32>(units);
    job.columns = utf8Of(columns);
    job.rowMs.reserve(rowMs.size());
    for (const QVariant& ms : rowMs) {
        job.rowMs << ms.toUInt();
    }
    const quint64 generation = ++m_estimateGeneration;
    if (m_sizing && m_estimateWorker.joinable()) {
        // The sizing under way stops at core's next ask: this one follows
        // it
        m_cancelEstimate.store(true);
        m_waitingEstimate = job;
        m_waitingGeneration = generation;
        return true;
    }
    launchEstimate(job, generation);
    return true;
}

void RecordingExport::dropEstimate()
{
    ++m_estimateGeneration;
    m_waitingEstimate.reset();
    if (m_sizing) {
        m_cancelEstimate.store(true);
    }
}

void RecordingExport::launchEstimate(const EstimateJob& job, quint64 generation)
{
    if (m_estimateWorker.joinable()) {
        m_estimateWorker.join(); // ended: its post is what got here
    }
    const bool wasSizing = m_sizing;
    m_sizing = true;
    m_cancelEstimate.store(false);
    try {
        Stop stop{&m_stopping, &m_cancelEstimate, m_holdForTesting,
                  m_holdMsForTesting, &m_heldForTesting};
        m_estimateWorker =
            std::thread([this, job, generation, stop,
                         fail = m_failWorkerForTesting]() mutable {
                QVariantMap result;
                try {
                    if (fail) {
                        throw std::bad_alloc();
                    }
                    result = runEstimate(job.folder, job.rowMs, job.units,
                                         job.columns, stop);
                } catch (...) {
                    result.clear(); // estimateDone says so
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, result, generation] {
                        estimateDone(result, generation);
                    },
                    Qt::QueuedConnection);
            });
    } catch (const std::system_error&) {
        m_sizing = false;
        if (wasSizing) {
            emit sizingChanged();
        }
        if (generation == m_estimateGeneration) {
            emit estimated({{QStringLiteral("ok"), false},
                            {QStringLiteral("words"),
                             QStringLiteral("Couldn't start the sizing")}});
        }
        return;
    }
    if (!wasSizing) {
        emit sizingChanged();
    }
}

void RecordingExport::estimateDone(const QVariantMap& answer,
                                   quint64 generation)
{
    const QVariantMap result =
        !answer.isEmpty()
            ? answer
            : QVariantMap{{QStringLiteral("ok"), false},
                          {QStringLiteral("words"), kOutOfMemory}};
    if (m_estimateWorker.joinable()) {
        m_estimateWorker.join(); // it posted this as its last act
    }
    if (m_waitingEstimate) {
        const EstimateJob next = *m_waitingEstimate;
        m_waitingEstimate.reset();
        launchEstimate(next, m_waitingGeneration);
        return;
    }
    m_sizing = false;
    emit sizingChanged();
    if (generation == m_estimateGeneration) {
        emit estimated(result);
    }
}

bool RecordingExport::start(const QVariantMap& options, const QUrl& target)
{
    const int format = options.value(QStringLiteral("format")).toInt();
    const QString folder = options.value(QStringLiteral("folder")).toString();
    // A picker has already made the document a file export would fill: a
    // start that refuses, or can't run, leaves it only when it holds bytes
    // (never a folder target)
    const auto dropPicked = [&] {
        if (format != Folder && !target.isEmpty() &&
            (m_throughProvider || !target.isLocalFile())) {
            removeIfEmpty(whereOf(target));
        }
    };
    if (m_busy || folder.isEmpty() || target.isEmpty() ||
        (format != Folder && format != Csv && format != Pcapng)) {
        dropPicked();
        return false;
    }
    if (m_worker.joinable()) {
        m_worker.join(); // ended: its post is what cleared m_busy
    }
    Job job;
    job.folder = folder.toUtf8();
    job.stem = fileStem(folder);
    job.format = format;
    job.longLayout = options.value(QStringLiteral("longLayout")).toBool();
    job.rowMs = options.value(QStringLiteral("rowMs")).toUInt();
    job.units = options.value(QStringLiteral("units")).toUInt();
    job.columns =
        utf8Of(options.value(QStringLiteral("columns")).toStringList());
    job.raw = options.value(QStringLiteral("raw")).toBool();
    job.target = target;
    job.throughProvider = m_throughProvider;
    job.failCopy = m_failCopyForTesting;
    job.failCopyAfterBytes = m_failCopyAfterBytesForTesting;
    m_cancelExport.store(false);
    try {
        if (m_failThreadForTesting) {
            throw std::system_error(std::make_error_code(
                std::errc::resource_unavailable_try_again));
        }
        Stop stop{&m_stopping, &m_cancelExport, m_holdForTesting,
                  m_holdMsForTesting, &m_heldForTesting};
        m_worker = std::thread([this, job, stop,
                                fail = m_failWorkerForTesting]() mutable {
            QVariantMap result;
            try {
                if (fail) {
                    throw std::bad_alloc();
                }
                result = job.format == ECUIO_EXPORT_FOLDER
                             ? runFolder(job, stop)
                             : runFile(job, stop);
                if (!result.value(QStringLiteral("ok")).toBool() &&
                    stop.stopped()) {
                    result = canceledIn(job.folder);
                }
            } catch (...) {
                result.clear(); // exportDone says so
            }
            QMetaObject::invokeMethod(
                this,
                [this, result, folder = job.folder] {
                    exportDone(result, folder);
                },
                Qt::QueuedConnection);
        });
    } catch (const std::system_error&) {
        dropPicked();
        return false;
    }
    m_busy = true;
    emit busyChanged();
    return true;
}

void RecordingExport::cancel()
{
    if (m_busy) {
        m_cancelExport.store(true);
    }
}

void RecordingExport::exportDone(const QVariantMap& answer,
                                 const QByteArray& folder)
{
    if (m_worker.joinable()) {
        m_worker.join(); // it posted this as its last act
    }
    m_busy = false;
    emit busyChanged();
    emit finished(!answer.isEmpty() ? answer : failedIn(folder, kOutOfMemory));
}
