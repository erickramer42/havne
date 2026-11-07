// File/layer: app/recordingimport.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest.Import* (test_bridge); QML: QmlTest.recordings

#include "recordingimport.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

#include "ecuio/core.h"
#include "ecuio/recording.h"

namespace {

// A picked file opens through QFile as it is: a local one by its path,
// Android's content:// URI through its provider
QString openable(const QUrl& url)
{
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
}

QVariantMap refused(const QString& words)
{
    return {{QStringLiteral("ok"), false}, {QStringLiteral("words"), words}};
}

// The file's bytes into path, a chunk at a time: a log can be larger than
// the memory a phone gives the app
bool copyTo(const QUrl& url, const QString& path, bool* readable)
{
    QFile in(openable(url));
    *readable = in.open(QIODevice::ReadOnly);
    QFile out(path);
    if (!*readable || !out.open(QIODevice::WriteOnly)) {
        return false;
    }
    QByteArray chunk(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        const qint64 n = in.read(chunk.data(), chunk.size());
        if (n < 0) {
            *readable = false;
            return false;
        }
        if (n == 0) {
            return out.flush();
        }
        if (out.write(chunk.constData(), n) != n) {
            return false;
        }
    }
}

struct Job {
    QUrl file;
    int kind = 0;
    QUrl map;
    QString interfaceName;
    QString root;
    QString stamp;
    bool copyLocal = false;
};

QVariantMap run(const Job& job)
{
    const bool csv = job.kind == RecordingImport::Csv;
    QByteArray map;
    if (csv) {
        if (job.map.isEmpty()) {
            return refused(
                QStringLiteral("choose the logger's column map first"));
        }
        QFile f(openable(job.map));
        if (!f.open(QIODevice::ReadOnly)) {
            return refused(QStringLiteral("the column map can't be read"));
        }
        map = f.read(RecordingImport::MapMaxBytes + 1);
        if (map.size() > RecordingImport::MapMaxBytes) {
            return refused(
                QStringLiteral("the column map is larger than 1 MiB"));
        }
    }
    if (job.root.isEmpty()) {
        return refused(QStringLiteral(
            "there's no Documents folder to keep recordings in"));
    }
    if (!QDir().mkpath(job.root)) {
        return refused(QStringLiteral("the recordings folder can't be made"));
    }
    // Named and sorted as a recording is ("<stamp>[-n] <label>"), never by
    // the file's own name, which can hold a VIN or a person's name
    const QString label =
        csv ? QStringLiteral("CSV import") : QStringLiteral("candump import");
    QString folder;
    for (int n = 1; n <= 999 && folder.isEmpty(); ++n) {
        const QString name =
            job.stamp + (n > 1 ? QStringLiteral("-%1").arg(n) : QString()) +
            QLatin1Char(' ') + label;
        if (!QFileInfo::exists(job.root + QLatin1Char('/') + name)) {
            folder = job.root + QLatin1Char('/') + name;
        }
    }
    if (folder.isEmpty()) {
        return refused(
            QStringLiteral("every recording's name for this second is taken"));
    }

    // Core reads a path: a file a provider hands out is copied first, into
    // a folder removed when the import is done
    QString in;
    QTemporaryDir copy(QDir::tempPath() +
                       QStringLiteral("/havne-import-XXXXXX"));
    if (job.file.isLocalFile() && !job.copyLocal) {
        in = job.file.toLocalFile();
    } else {
        if (!copy.isValid()) {
            return refused(QStringLiteral(
                "there's no room to copy the file for the import"));
        }
        in = copy.filePath(QStringLiteral("input"));
        bool readable = false;
        if (!copyTo(job.file, in, &readable)) {
            return refused(
                readable
                    ? QStringLiteral(
                          "there's no room to copy the file for the import")
                    : QStringLiteral("the file can't be read"));
        }
    }

    const QByteArray inUtf8 = in.toUtf8();
    const QByteArray outUtf8 = folder.toUtf8();
    const QByteArray iface = job.interfaceName.trimmed().toUtf8();
    ecuio_import_options_t o{};
    o.format = csv ? ECUIO_IMPORT_CSV : ECUIO_IMPORT_CANDUMP;
    o.in_path_utf8 = inUtf8.constData();
    o.out_folder_utf8 = outUtf8.constData();
    o.interface_utf8 = !csv && !iface.isEmpty() ? iface.constData() : nullptr;
    o.column_map_utf8 = csv ? map.constData() : nullptr;
    ecuio_import_report_t r{};
    const ecuio_status_t status = ecuio_import(&o, &r);
    if (status != ECUIO_OK) {
        const QString detail = QString::fromUtf8(ecuio_last_error_detail());
        return refused(
            !detail.isEmpty()
                ? detail
                : QStringLiteral("the import failed (status %1)").arg(status));
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("folder"), folder},
            {QStringLiteral("frames"), static_cast<double>(r.frames)},
            {QStringLiteral("values"), static_cast<double>(r.values)},
            {QStringLiteral("columnsIgnored"),
             static_cast<double>(r.columns_ignored)},
            {QStringLiteral("durationUs"), static_cast<double>(r.duration_us)}};
}

} // namespace

RecordingImport::RecordingImport(std::function<QString()> root, QObject* parent)
    : QObject(parent)
    , m_root(std::move(root))
{
}

RecordingImport::~RecordingImport()
{
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

int RecordingImport::guessKind(const QUrl& file) const
{
    static const QRegularExpression candump(QStringLiteral(
        "^\\([0-9]+(\\.[0-9]+)?\\)\\s+\\S+\\s+[0-9A-Fa-f]{1,8}#"));
    QFile f(openable(file));
    if (f.open(QIODevice::ReadOnly)) {
        // The first line that isn't blank, within the file's first 64 KiB
        const QByteArray head = f.read(64 * 1024);
        for (const QByteArray& line : head.split('\n')) {
            const QString text = QString::fromUtf8(line).trimmed();
            if (!text.isEmpty()) {
                return candump.match(text).hasMatch() ? Candump : Csv;
            }
        }
    }
    return Csv;
}

bool RecordingImport::start(const QUrl& file, int kind, const QUrl& map,
                            const QString& interfaceName)
{
    if (m_busy || (kind != Csv && kind != Candump)) {
        return false;
    }
    if (m_worker.joinable()) {
        m_worker.join(); // ended: its post is what cleared m_busy
    }
    Job job;
    job.file = file;
    job.kind = kind;
    job.map = map;
    job.interfaceName = interfaceName;
    job.root = m_root();
    job.stamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyyMMdd-HHmmss"));
    job.copyLocal = m_copyLocal;
    m_busy = true;
    m_worker = std::thread([this, job] {
        const QVariantMap result = run(job);
        QMetaObject::invokeMethod(
            this, [this, result] { done(result); }, Qt::QueuedConnection);
    });
    emit busyChanged();
    return true;
}

void RecordingImport::done(const QVariantMap& result)
{
    if (m_worker.joinable()) {
        m_worker.join(); // it posted this as its last act
    }
    m_busy = false;
    emit busyChanged();
    emit finished(result);
}
