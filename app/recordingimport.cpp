// File/layer: app/recordingimport.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest.Import* (test_bridge); QML: QmlTest.recordings

#include "recordingimport.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <chrono>
#include <system_error>

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

// An import the user stopped: nothing was made
QVariantMap canceled()
{
    return {
        {QStringLiteral("ok"), false},
        {QStringLiteral("canceled"), true},
        {QStringLiteral("words"), QStringLiteral("the import was canceled")}};
}

// The file's bytes into path, a chunk at a time: a log can be larger than
// the memory a phone gives the app. stop, when given, ends it between
// chunks
bool copyTo(const QUrl& url, const QString& path, bool* readable,
            const std::atomic<bool>* stop = nullptr)
{
    QFile in(openable(url));
    *readable = in.open(QIODevice::ReadOnly);
    QFile out(path);
    if (!*readable || !out.open(QIODevice::WriteOnly)) {
        return false;
    }
    QByteArray chunk(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        if (stop != nullptr && stop->load()) {
            return false;
        }
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
    QByteArray mapText;
    bool hasMapText = false;
    QString interfaceName;
    QString root;
    QString stamp;
    QString label;
    bool copyLocal = false;
    int holdMsForTesting = 0;
    std::atomic<bool>* stop = nullptr;
};

// ecuio_import_with's cancel: the importer's cancel() or its destruction
int stopAsked(void* user)
{
    return static_cast<const std::atomic<bool>*>(user)->load() ? 1 : 0;
}

QVariantMap run(const Job& job)
{
    const bool csv = job.kind == RecordingImport::Csv;
    QByteArray map;
    if (csv && job.hasMapText) {
        map = job.mapText;
        if (map.size() > RecordingImport::MapMaxBytes) {
            return refused(
                QStringLiteral("the column map is larger than 1 MiB"));
        }
    } else if (csv) {
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
        !job.label.isEmpty()
            ? job.label
            : csv ? QStringLiteral("CSV import")
                  : QStringLiteral("candump import");
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
    // removeStaleCopies finds this folder by its name
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
        if (!copyTo(job.file, in, &readable, job.stop)) {
            if (job.stop->load()) {
                return canceled(); // the copy stopped, not a failed read
            }
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
    for (int waited = 0; waited < job.holdMsForTesting && !job.stop->load();
         waited += 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const ecuio_status_t status =
        ecuio_import_with(&o, stopAsked, job.stop, &r);
    if (status == ECUIO_ERR_CANCELED) {
        return canceled();
    }
    if (status != ECUIO_OK) {
        const QString detail = QString::fromUtf8(ecuio_last_error_detail());
        return refused(
            !detail.isEmpty()
                ? detail
                : QStringLiteral("the import failed (status %1)").arg(status));
    }
    // A CSV that carries dates is stamped with its first row's time, not the
    // import's: the row then says when the drive was
    if (csv && r.utc_start_us > 0) {
        const QString stamp =
            QDateTime::fromMSecsSinceEpoch(
                static_cast<qint64>(r.utc_start_us / 1000))
                .toString(QStringLiteral("yyyyMMdd-HHmmss"));
        for (int n = 1; n <= 999; ++n) {
            const QString name =
                stamp + (n > 1 ? QStringLiteral("-%1").arg(n) : QString()) +
                QLatin1Char(' ') + label;
            const QString target = job.root + QLatin1Char('/') + name;
            if (target == folder) {
                break;
            }
            if (!QFileInfo::exists(target) && QDir().rename(folder, target)) {
                folder = target;
                break;
            }
        }
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("folder"), folder},
            {QStringLiteral("frames"), static_cast<double>(r.frames)},
            {QStringLiteral("values"), static_cast<double>(r.values)},
            {QStringLiteral("columnsIgnored"),
             static_cast<double>(r.columns_ignored)},
            {QStringLiteral("rowsNoTime"), static_cast<double>(r.rows_no_time)},
            {QStringLiteral("durationUs"), static_cast<double>(r.duration_us)}};
}

// The guess reads no more of a file than core's does: a longer file is
// copied to this many bytes, and core cuts it at its last line feed
constexpr qint64 kGuessCopyBytes = 1280 * 1024;

// The first bytes of the file into path
bool copyHeadTo(const QUrl& url, const QString& path, bool* readable)
{
    QFile in(openable(url));
    *readable = in.open(QIODevice::ReadOnly);
    QFile out(path);
    if (!*readable || !out.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray head = in.read(kGuessCopyBytes);
    if (in.error() != QFileDevice::NoError) {
        *readable = false;
        return false;
    }
    return out.write(head) == head.size() && out.flush();
}

// What the preview shows of a file: its header row's cells and the first
// rows below. Display only: core's reader is the import's, and nothing the
// import does follows from what this one splits.
struct Shown {
    QList<QStringList> rows;
};

constexpr int kShownRows = 5;
constexpr int kShownBytes = 64 * 1024;
constexpr int kShownCellChars = 32;

bool isPadding(char c)
{
    return c == ' ' || c == '\t';
}

// Spaces and tabs off both ends, as the import trims an unquoted cell
QByteArray trimPadding(const QByteArray& text)
{
    qsizetype a = 0;
    qsizetype b = text.size();
    while (a < b && isPadding(text[a])) {
        ++a;
    }
    while (b > a && isPadding(text[b - 1])) {
        --b;
    }
    return text.mid(a, b - a);
}

// RFC 4180 records of the text's first bytes (quotes, doubled quotes, CRLF
// or LF), each cell as the import reads it: an unquoted one trimmed of
// padding, a quoted one exact; a record the read cut short is left out
QList<QStringList> splitRecords(const QByteArray& bytes, char delimiter,
                                bool cut, int wanted)
{
    QList<QStringList> records;
    QStringList row;
    QByteArray cell;
    bool quoted = false;
    bool wasQuoted = false; // the cell under way opened with a quote
    bool any = false;       // the record under way has a byte
    auto endCell = [&] {
        row << QString::fromUtf8(wasQuoted ? cell : trimPadding(cell));
        cell.clear();
        wasQuoted = false;
    };
    auto endRecord = [&] {
        if (any) {
            endCell();
            records << row;
        }
        row.clear();
        any = false;
    };
    for (qsizetype i = 0; i < bytes.size() && records.size() < wanted; ++i) {
        const char c = bytes[i];
        if (quoted) {
            if (c == '"' && i + 1 < bytes.size() && bytes[i + 1] == '"') {
                cell += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                cell += c;
            }
        } else if (c == '"' && !wasQuoted && trimPadding(cell).isEmpty()) {
            cell.clear();
            quoted = true;
            wasQuoted = true;
            any = true;
        } else if (c == delimiter) {
            // Before the padding test, as core's reader has it: a tab
            // delimiter after a closing quote ends the cell
            endCell();
            any = true;
        } else if (wasQuoted && isPadding(c)) {
            any = true; // padding after the closing quote
        } else if (c == '\n') {
            endRecord();
        } else if (c != '\r') {
            cell += c;
            any = true;
        }
    }
    if (!cut && !quoted && any && records.size() < wanted) {
        endRecord();
    }
    return records;
}

// The rows below the header row, as they split
Shown shownOf(const QString& path, char delimiter, int headerRow)
{
    Shown shown;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return shown;
    }
    QByteArray head = f.read(kShownBytes);
    const bool cut = !f.atEnd();
    if (head.startsWith("\xEF\xBB\xBF")) {
        head.remove(0, 3);
    }
    // header_row counts lines; the preamble is skipped by lines too
    qsizetype at = 0;
    for (int line = 1; line <= headerRow && at >= 0; ++line) {
        at = head.indexOf('\n', at);
        at = at < 0 ? -1 : at + 1;
    }
    if (at < 0) {
        return shown;
    }
    shown.rows = splitRecords(head.mid(at), delimiter, cut, kShownRows);
    return shown;
}

QString shownCell(const QStringList& row, qsizetype column)
{
    if (column >= row.size()) {
        return QStringLiteral("-");
    }
    QString text = row[column];
    if (text.size() > kShownCellChars) {
        text.truncate(kShownCellChars - 3);
        // A cut between the halves of a surrogate pair leaves half a character
        if (text.back().isHighSurrogate()) {
            text.chop(1);
        }
        text += QStringLiteral("...");
    }
    return text;
}

// A name of at most 31 bytes, cut on a character (never between the halves
// of a surrogate pair)
QString nameFrom(const QString& header)
{
    QString text = header.trimmed();
    while (text.toUtf8().size() > 31) {
        text.chop(1);
    }
    if (!text.isEmpty() && text.back().isHighSurrogate()) {
        text.chop(1);
    }
    return text;
}

QByteArray guessText(const QString& path, const QByteArray& unit,
                     ecuio_csv_guess_t* report, QString* words)
{
    const QByteArray in = path.toUtf8();
    const char* u = unit.isEmpty() ? nullptr : unit.constData();
    // The text's size, then the text; asked again if the file grew
    // between the two calls
    for (int attempt = 0; attempt < 3; ++attempt) {
        size_t needed = 0;
        ecuio_status_t rc = ecuio_import_csv_guess(in.constData(), u, nullptr,
                                                   0, &needed, report);
        if (rc == ECUIO_OK) {
            QByteArray text(static_cast<qsizetype>(needed), '\0');
            rc = ecuio_import_csv_guess(in.constData(), u, text.data(),
                                        static_cast<size_t>(text.size()),
                                        &needed, report);
            if (rc == ECUIO_OK && needed <= static_cast<size_t>(text.size())) {
                text.truncate(static_cast<qsizetype>(needed) - 1);
                return text;
            }
        }
        if (rc != ECUIO_OK) {
            const QString detail = QString::fromUtf8(ecuio_last_error_detail());
            *words =
                !detail.isEmpty()
                    ? detail
                    : QStringLiteral("the guess failed (status %1)").arg(rc);
            return {};
        }
    }
    *words = QStringLiteral("the file changed while it was read");
    return {};
}

QVariantMap runGuess(const QUrl& file, const QString& timeUnit, bool copyLocal)
{
    QString in;
    // removeStaleCopies finds this folder by its name
    QTemporaryDir copy(QDir::tempPath() +
                       QStringLiteral("/havne-import-XXXXXX"));
    if (file.isLocalFile() && !copyLocal) {
        in = file.toLocalFile();
    } else {
        if (!copy.isValid()) {
            return refused(QStringLiteral(
                "there's no room to copy the file for the import"));
        }
        in = copy.filePath(QStringLiteral("input"));
        bool readable = false;
        if (!copyHeadTo(file, in, &readable)) {
            return refused(
                readable
                    ? QStringLiteral(
                          "there's no room to copy the file for the import")
                    : QStringLiteral("the file can't be read"));
        }
    }
    ecuio_csv_guess_t report{};
    QString words;
    const QByteArray text = guessText(in, timeUnit.toUtf8(), &report, &words);
    if (text.isEmpty()) {
        return refused(words);
    }
    const QJsonObject root = QJsonDocument::fromJson(text).object();
    const QJsonObject map = root.value(QStringLiteral("map")).toObject();
    if (map.isEmpty()) {
        return refused(QStringLiteral("the guess can't be read"));
    }
    const QByteArray mapBytes =
        text.mid(static_cast<qsizetype>(report.map_offset),
                 static_cast<qsizetype>(report.map_length));

    const QString delimiter =
        map.value(QStringLiteral("delimiter")).toString(QStringLiteral(","));
    const int headerRow = map.value(QStringLiteral("header_row")).toInt(1);
    const Shown shown = shownOf(
        in, delimiter.isEmpty() ? ',' : delimiter.toLatin1().at(0), headerRow);

    const QJsonObject time = map.value(QStringLiteral("time")).toObject();
    const QString timeColumn = time.value(QStringLiteral("column")).toString();
    QHash<QString, QJsonObject> signalOf;
    for (const QJsonValue v : map.value(QStringLiteral("signals")).toArray()) {
        const QJsonObject o = v.toObject();
        signalOf.insert(o.value(QStringLiteral("column")).toString(), o);
    }

    QVariantList columns;
    const QJsonArray listed = root.value(QStringLiteral("columns")).toArray();
    // A header text the file has twice can't be named by a map, so such a
    // column is never put back
    QHash<QString, int> headerCount;
    for (const QJsonValue v : listed) {
        ++headerCount[v.toObject().value(QStringLiteral("column")).toString()];
    }
    for (qsizetype i = 0; i < listed.size(); ++i) {
        const QJsonObject c = listed[i].toObject();
        const QString header = c.value(QStringLiteral("column")).toString();
        const bool used = c.value(QStringLiteral("used")).toBool();
        const bool isTime = used && header == timeColumn;
        const QJsonObject sig = signalOf.value(header);
        QStringList samples;
        for (const QStringList& row : shown.rows) {
            samples << shownCell(row, i);
        }
        QVariantMap col;
        col.insert(QStringLiteral("index"), static_cast<int>(i));
        col.insert(QStringLiteral("column"), header);
        col.insert(QStringLiteral("label"),
                   QString::fromUtf8(trimPadding(header.toUtf8())));
        col.insert(QStringLiteral("used"), used);
        col.insert(QStringLiteral("why"),
                   c.value(QStringLiteral("why")).toString());
        col.insert(QStringLiteral("isTime"), isTime);
        // A column with an empty header (spaces alone too), or a header the
        // file has twice, can't be named in a map
        col.insert(QStringLiteral("canPutBack"),
                   !used && !trimPadding(header.toUtf8()).isEmpty() &&
                       headerCount.value(header) == 1);
        const bool signal = used && !isTime;
        col.insert(QStringLiteral("name"),
                   signal ? sig.value(QStringLiteral("name")).toString()
                          : nameFrom(header));
        col.insert(QStringLiteral("unit"),
                   sig.value(QStringLiteral("unit")).toString());
        col.insert(QStringLiteral("kind"),
                   signal ? sig.value(QStringLiteral("kind"))
                                .toString(QStringLiteral("number"))
                          : QStringLiteral("text"));
        col.insert(QStringLiteral("samples"), samples);
        columns << col;
    }

    QStringList notes;
    for (const QJsonValue n : root.value(QStringLiteral("notes")).toArray()) {
        notes << n.toString();
    }
    const quint32 flags = report.flags;
    return {
        {QStringLiteral("ok"), true},
        {QStringLiteral("words"), QString()},
        {QStringLiteral("mapText"), QString::fromUtf8(mapBytes)},
        {QStringLiteral("unitKnown"),
         (flags & ECUIO_CSV_GUESS_TIME_UNIT_KNOWN) != 0},
        {QStringLiteral("timeNumbers"),
         (flags & ECUIO_CSV_GUESS_TIME_NUMBERS) != 0},
        {QStringLiteral("decimalComma"),
         (flags & ECUIO_CSV_GUESS_DECIMAL_COMMA) != 0},
        {QStringLiteral("preamble"), (flags & ECUIO_CSV_GUESS_PREAMBLE) != 0},
        {QStringLiteral("delimiter"), delimiter},
        {QStringLiteral("headerRow"), headerRow},
        {QStringLiteral("timeColumn"), timeColumn},
        {QStringLiteral("timeUnit"),
         time.value(QStringLiteral("unit")).toString()},
        {QStringLiteral("columns"), columns},
        {QStringLiteral("notes"), notes}};
}

// A saved map's file name from what was typed: ASCII letters, digits, - and
// _ ; anything else (a space too) becomes -
QString safeName(const QString& typed)
{
    QString out;
    for (const QChar ch : typed.trimmed()) {
        const ushort u = ch.unicode();
        const bool plain = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                           (u >= '0' && u <= '9') || u == '-' || u == '_';
        out += plain ? ch : QLatin1Char('-');
    }
    return out.left(64);
}

} // namespace

RecordingImport::RecordingImport(std::function<QString()> root, QObject* parent)
    : QObject(parent)
    , m_root(std::move(root))
{
}

RecordingImport::~RecordingImport()
{
    m_stop = true;
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void RecordingImport::setLabel(const QString& label)
{
    // Core's recorder rule (reduce_label): ASCII letters and digits, space,
    // '-', '_', '.' and every character past ASCII; runs of spaces made
    // one; at most 40 characters, counted whole (never half a surrogate
    // pair); no dot or space at the end (Windows refuses them)
    QList<char32_t> kept;
    for (const char32_t c : label.toUcs4()) {
        const bool ok = c > 127 || (c >= '0' && c <= '9') ||
                        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        c == '-' || c == '_' || c == '.' || c == ' ';
        if (!ok || (c == ' ' && (kept.isEmpty() || kept.back() == ' '))) {
            continue;
        }
        kept.append(c);
        if (kept.size() == 40) {
            break;
        }
    }
    while (!kept.isEmpty() && (kept.back() == ' ' || kept.back() == '.')) {
        kept.removeLast();
    }
    const QString out = QString::fromUcs4(kept.constData(), kept.size());
    if (out == m_label) {
        return;
    }
    m_label = out;
    emit labelChanged();
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
    if (kind != Csv && kind != Candump) {
        return false;
    }
    return beginImport(file, kind, map, {}, false, interfaceName);
}

bool RecordingImport::startWithMap(const QUrl& file, const QString& mapText)
{
    return beginImport(file, Csv, {}, mapText.toUtf8(), true, {});
}

bool RecordingImport::beginImport(const QUrl& file, int kind, const QUrl& map,
                                  const QByteArray& mapText, bool hasMapText,
                                  const QString& interfaceName)
{
    if (m_busy) {
        return false;
    }
    Job job;
    job.file = file;
    job.kind = kind;
    job.map = map;
    job.mapText = mapText;
    job.hasMapText = hasMapText;
    job.interfaceName = interfaceName;
    job.root = m_root();
    job.label = m_label;
    job.stamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyyMMdd-HHmmss"));
    job.copyLocal = m_copyLocal;
    job.holdMsForTesting = m_holdMsForTesting;
    job.stop = &m_stop;
    return launch(false, [job] { return run(job); });
}

void RecordingImport::cancel()
{
    m_stop = true; // an import's next look ends it; a guess never looks
}

bool RecordingImport::guess(const QUrl& file, const QString& timeUnit)
{
    if (m_busy) {
        return false;
    }
    const bool copyLocal = m_copyLocal;
    return launch(true, [file, timeUnit, copyLocal] {
        return runGuess(file, timeUnit, copyLocal);
    });
}

bool RecordingImport::launch(bool isGuess, std::function<QVariantMap()> work)
{
    if (m_worker.joinable()) {
        m_worker.join(); // ended: its post is what cleared m_busy
    }
    try {
        if (m_failThreadForTesting) {
            throw std::system_error(std::make_error_code(
                std::errc::resource_unavailable_try_again));
        }
        m_stop = false; // an earlier cancel was the earlier worker's
        m_worker = std::thread([this, isGuess, work = std::move(work)] {
            const QVariantMap result = work();
            QMetaObject::invokeMethod(
                this, [this, result, isGuess] { done(result, isGuess); },
                Qt::QueuedConnection);
        });
    } catch (const std::system_error&) {
        return false;
    }
    m_busy = true;
    emit busyChanged();
    return true;
}

void RecordingImport::done(const QVariantMap& result, bool isGuess)
{
    if (m_worker.joinable()) {
        m_worker.join(); // it posted this as its last act
    }
    m_busy = false;
    emit busyChanged();
    if (isGuess) {
        emit guessFinished(result);
    } else {
        emit finished(result);
    }
}

QString RecordingImport::composeMap(const QString& mapText,
                                    const QVariantList& edits,
                                    const QString& timeUnit) const
{
    const QJsonDocument base = QJsonDocument::fromJson(mapText.toUtf8());
    if (!base.isObject()) {
        return {};
    }
    QJsonObject map = base.object();
    QHash<QString, QJsonObject> had;
    for (const QJsonValue v : map.value(QStringLiteral("signals")).toArray()) {
        const QJsonObject o = v.toObject();
        had.insert(o.value(QStringLiteral("column")).toString(), o);
    }
    const QString timeColumn = map.value(QStringLiteral("time"))
                                   .toObject()
                                   .value(QStringLiteral("column"))
                                   .toString();
    QJsonArray signalList;
    for (const QVariant& e : edits) {
        const QVariantMap edit = e.toMap();
        if (!edit.value(QStringLiteral("include")).toBool()) {
            continue;
        }
        const QString column = edit.value(QStringLiteral("column")).toString();
        if (column == timeColumn) {
            // The time column is the map's time, never also a signal
            continue;
        }
        QJsonObject sig = had.value(column);
        sig.insert(QStringLiteral("column"), column);
        sig.insert(QStringLiteral("name"),
                   edit.value(QStringLiteral("name")).toString());
        const QString unit = edit.value(QStringLiteral("unit")).toString();
        if (unit.isEmpty()) {
            sig.remove(QStringLiteral("unit"));
        } else {
            sig.insert(QStringLiteral("unit"), unit);
        }
        const bool text = edit.value(QStringLiteral("kind")).toString() ==
                          QLatin1String("text");
        sig.insert(QStringLiteral("kind"),
                   text ? QStringLiteral("text") : QStringLiteral("number"));
        if (text) {
            // A text shows no decimals and plots no rank
            sig.remove(QStringLiteral("decimals"));
            sig.remove(QStringLiteral("rank"));
        }
        signalList.append(sig);
    }
    map.insert(QStringLiteral("signals"), signalList);
    if (!timeUnit.isEmpty()) {
        QJsonObject time = map.value(QStringLiteral("time")).toObject();
        time.insert(QStringLiteral("unit"), timeUnit);
        map.insert(QStringLiteral("time"), time);
    }
    return QString::fromUtf8(
        QJsonDocument(map).toJson(QJsonDocument::Indented));
}

QString RecordingImport::mapsRoot() const
{
    if (!m_mapsRoot.isEmpty()) {
        return m_mapsRoot;
    }
    // The plugin store's rule: HAVNE_SETTINGS_DIR, else the app's data folder
    const QString dir = qEnvironmentVariable("HAVNE_SETTINGS_DIR");
    const QString base =
        dir.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            : dir;
    return base + QStringLiteral("/maps");
}

QVariantMap RecordingImport::saveMap(const QString& name,
                                     const QString& mapText)
{
    const QString safe = safeName(name);
    if (safe.isEmpty() || safe.count(QLatin1Char('-')) == safe.size()) {
        return refused(QStringLiteral("give the map a name"));
    }
    const QString root = mapsRoot();
    if (!QDir().mkpath(root)) {
        return refused(QStringLiteral("the maps folder can't be made"));
    }
    const QString path =
        root + QLatin1Char('/') + safe + QStringLiteral(".json");
    if (QFileInfo::exists(path)) {
        return refused(
            QStringLiteral("a map named %1 is saved already").arg(safe));
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(mapText.toUtf8()) < 0 ||
        !f.commit()) {
        return refused(QStringLiteral("the map can't be written"));
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("name"), safe},
            {QStringLiteral("url"), QUrl::fromLocalFile(path)}};
}

QVariantList RecordingImport::savedMaps() const
{
    QVariantList list;
    const QDir dir(mapsRoot());
    for (const QString& file :
         dir.entryList({QStringLiteral("*.json")}, QDir::Files,
                       QDir::Name | QDir::IgnoreCase)) {
        list << QVariantMap{
            {QStringLiteral("name"), QFileInfo(file).completeBaseName()},
            {QStringLiteral("url"), QUrl::fromLocalFile(dir.filePath(file))}};
    }
    return list;
}
