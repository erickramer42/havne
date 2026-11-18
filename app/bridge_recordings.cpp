// File/layer: app/bridge_recordings.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "sessionbridge.h"
#include "recording_signals.h" /* cli/: a replay's own signal file */
#include "signal_file_lines.h" /* cli/: the profile a URI selects */

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>

QString SessionBridge::replayUriFor(const QString& folder,
                                    const QString& recordedUri)
{
    const qsizetype q = recordedUri.indexOf(QLatin1Char('?'));
    QString profile;
    QStringList dids;
    if (q >= 0) {
        // Core's grammar: key=value pairs split on &, the last profile=
        // winning
        const QStringList pairs =
            recordedUri.mid(q + 1).split(QLatin1Char('&'));
        for (const QString& pair : pairs) {
            if (pair.startsWith(QLatin1String("profile="))) {
                profile = pair.mid(8);
            } else if (pair.startsWith(QLatin1String("did="))) {
                dids << pair;
            }
        }
    }
    if (profile.isEmpty()) {
        // Core's defaults by scheme (ADR-0013); a replay's is raw, which
        // can't decode a recording wrongly, and so is an unknown source's.
        // Not profileOfUri's table: a recording made on another platform
        // names j2534:// or another case, and its frames still decode
        profile = recordedUri.startsWith(QLatin1String("mock://"),
                                         Qt::CaseInsensitive)
                      ? QStringLiteral("mock")
                  : recordedUri.startsWith(QLatin1String("j2534://"),
                                           Qt::CaseInsensitive) ||
                          recordedUri.startsWith(QLatin1String("elm327://"),
                                                 Qt::CaseInsensitive)
                      ? QStringLiteral("obd2")
                      : QStringLiteral("raw");
    }
    QString uri = QStringLiteral("replay://") +
                  QDir::fromNativeSeparators(folder) +
                  QStringLiteral("?profile=") + profile;
    // did= belongs to uds-generic alone; core refuses it under any other
    if (profile == QLatin1String("uds-generic")) {
        for (const QString& did : dids) {
            uri += QLatin1Char('&') + did;
        }
    }
    return uri;
}

namespace {

// A recording's events.log header, as cli::RecordedSignals reads it;
// false when there's no events.log to read
bool readRecordedHeader(const QString& folder, cli::RecordedSignals* recorded)
{
    QFile events(folder + QStringLiteral("/events.log"));
    if (!events.open(QIODevice::ReadOnly)) {
        return false;
    }
    // The header alone: its lines up to the blank one (the enum labels'
    // lines make it longer than a few)
    while (!events.atEnd()) {
        QByteArray line = events.readLine(64 * 1024);
        if (line.endsWith('\n')) {
            line.chop(1);
        }
        if (!recorded->take(line.toStdString())) {
            break;
        }
    }
    return true;
}

} // namespace

QString SessionBridge::replayUriWithOwnSignals(const QString& uri)
{
    const std::string uri8 = uri.toStdString();
    std::string folder8;
    if (!cli::replay_folder(uri8, &folder8)) {
        return uri;
    }
    const QString folder = QString::fromStdString(folder8);
    cli::RecordedSignals recorded;
    if (!readRecordedHeader(folder, &recorded)) {
        return uri;
    }
    if (!recorded.names_file()) {
        return uri;
    }
    // A URI with no profile= replays raw (ADR-0013), which nothing decodes
    // wrongly: the copy is for the profile the recording names
    std::string selected;
    if (!cli::uri_profile(uri8, &selected)) {
        return uri;
    }
    const QString profile = QString::fromStdString(recorded.profile);
    const QString own =
        QString::fromStdString(cli::own_profile(recorded.sha256));
    const QString chosen = QString::fromStdString(selected);
    if (chosen != profile && chosen != own) {
        return uri;
    }
    const auto as_loaded = [&](const QString& why) {
        emit logMessage(QStringLiteral("Replay: the recording's signals.json "
                                       "%1; decoding with %2 as loaded now")
                            .arg(why, chosen));
        return uri;
    };
    QFile copy(folder + QStringLiteral("/signals.json"));
    if (!copy.open(QIODevice::ReadOnly)) {
        return as_loaded(QStringLiteral("is missing"));
    }
    const QByteArray bytes = copy.read(ECUIO_SIGNAL_SET_MAX_BYTES + 1);
    const QString sha = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    if (sha.toStdString() != recorded.sha256) {
        return as_loaded(QStringLiteral("doesn't match its hash"));
    }
    const auto selecting = [&](const QString& p) {
        return chosen == p ? uri
                           : QString::fromStdString(
                                 cli::with_profile(uri8, p.toStdString()));
    };
    auto held = m_replayFiles.find(sha);
    if (held == m_replayFiles.end()) {
        ecuio_signal_file_t* file = nullptr;
        if (ecuio_signal_file_load_with(
                bytes.constData(), static_cast<size_t>(bytes.size()),
                ECUIO_SIGNAL_FILE_LOAD_SET, &file) != ECUIO_OK) {
            return as_loaded(
                QStringLiteral("doesn't load (%1)")
                    .arg(QString::fromUtf8(ecuio_last_error_detail())));
        }
        const QByteArray name8 = own.toUtf8();
        const ecuio_status_t rc = ecuio_profile_register(
            name8.constData(), ecuio_signal_file_set(file), nullptr);
        if (rc != ECUIO_OK) {
            ecuio_signal_file_free(file);
            return as_loaded(
                QStringLiteral("can't be registered (%1)")
                    .arg(QString::fromUtf8(ecuio_status_string(rc))));
        }
        held = m_replayFiles
                   .emplace(sha, ReplayFile{file,
                                            QString::fromUtf8(
                                                ecuio_signal_file_name(file)),
                                            own})
                   .first;
    }
    emit logMessage(QStringLiteral("Replay: decoding with the recording's own "
                                   "signal file, %1 (%2)")
                        .arg(held->second.name, own));
    return selecting(own);
}

namespace {

// A folder the viewer opens: one holding a signals.csv (values, a CSV
// import's alone) or a frames.csv (frames, decoded through a profile)
bool holdsRecording(const QString& folder)
{
    return QFileInfo::exists(folder + QStringLiteral("/signals.csv")) ||
           QFileInfo::exists(folder + QStringLiteral("/frames.csv"));
}

// A recording folder's row for the Recordings page: { title, uri, folder,
// source, profile, frames, durationMs, signals, log, marks } (recentRecordings
// says what each holds)
QVariantMap recordingRowOf(const QFileInfo& info)
{
    // "<yyyyMMdd-HHmmss>[-n][ <label>]" (core's recorder)
    static const QRegularExpression kName(
        QStringLiteral("^(\\d{4})(\\d{2})(\\d{2})-(\\d{2})(\\d{2})(\\d{2})(?:-"
                       "\\d+)?(?: (.+))?$"));
    const QString folder = info.absoluteFilePath();

    // events.log's header, up to its "columns" line: the "uri", each "signal"
    // and the "focus" that only a log of chosen signals has
    QString recorded;
    int signalCount = 0;
    bool isLog = false;
    qint64 durationMs = -1;
    QFile events(folder + QStringLiteral("/events.log"));
    if (events.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // A header holds a line for each signal and each enum label, at most
        // about 140 bytes each (every name byte a quote); a file with no
        // "columns" line is read no further than that many lines at 150
        constexpr qint64 kHeaderBytes =
            static_cast<qint64>(ECUIO_SET_MAX_TABLE +
                                ECUIO_SIGNAL_SET_MAX_SIGNALS) *
            150;
        // A line longer than a read (a "focus" line names every pick) comes in
        // parts, and only the part that starts it is judged
        constexpr qint64 kLineBytes = 1100;
        qint64 read = 0;
        bool startsLine = true;
        while (read < kHeaderBytes && !events.atEnd()) {
            const QByteArray raw = events.readLine(kLineBytes);
            if (raw.isEmpty()) {
                break; // a read that failed before the end
            }
            read += raw.size();
            const bool judged = startsLine;
            startsLine = raw.endsWith('\n');
            if (!judged) {
                continue;
            }
            const QString line = QString::fromUtf8(raw).trimmed();
            if (line.startsWith(QLatin1String("columns"))) {
                break;
            }
            if (line.startsWith(QLatin1String("uri ")) && recorded.isEmpty()) {
                recorded = line.mid(4).trimmed();
            } else if (line.startsWith(QLatin1String("signal  "))) {
                ++signalCount;
            } else if (line.startsWith(QLatin1String("focus "))) {
                isLog = true;
            }
        }
        // The last line's "+seconds" column is the recording's length
        static const QRegularExpression kAt(
            QStringLiteral("^\\S+\\s+\\+(\\d+(?:\\.\\d+)?)\\s"));
        constexpr qint64 kTail = 4096;
        events.seek(qMax<qint64>(0, events.size() - kTail));
        const QStringList tail = QString::fromUtf8(events.readAll())
                                     .split(QLatin1Char('\n'),
                                            Qt::SkipEmptyParts);
        for (auto it = tail.crbegin(); it != tail.crend(); ++it) {
            const QRegularExpressionMatch at = kAt.match(*it);
            if (at.hasMatch()) {
                durationMs = qRound64(at.captured(1).toDouble() * 1000.0);
                break;
            }
        }
    }
    // markers.csv: a header row, then one row each
    int marks = 0;
    QFile markers(folder + QStringLiteral("/markers.csv"));
    if (markers.open(QIODevice::ReadOnly)) {
        while (!markers.atEnd()) {
            marks += markers.readLine().trimmed().isEmpty() ? 0 : 1;
        }
        marks = qMax(0, marks - 1);
    }

    const QRegularExpressionMatch m = kName.match(info.fileName());
    QString title = info.fileName();
    if (m.hasMatch()) {
        title = QStringLiteral("%1-%2-%3 %4:%5:%6")
                    .arg(m.captured(1), m.captured(2), m.captured(3),
                         m.captured(4), m.captured(5), m.captured(6));
        if (m.hasCaptured(7)) {
            title += QStringLiteral(" · ") + m.captured(7);
        }
    }
    const QString uri = SessionBridge::replayUriFor(folder, recorded);
    QVariantMap row;
    row["title"] = title;
    row["uri"] = uri;
    row["folder"] = folder;
    row["source"] = SessionBridge::uriLabelFor(recorded);
    // After the query's "?profile=", the last ? in it
    row["profile"] = uri.mid(uri.lastIndexOf(QLatin1Char('?')) + 9)
                         .section(QLatin1Char('&'), 0, 0);
    // Replay plays frames.csv: a folder of values alone has none to play
    row["frames"] = QFileInfo::exists(folder + QStringLiteral("/frames.csv"));
    row["durationMs"] = durationMs;
    row["signals"] = signalCount;
    row["log"] = isLog;
    row["marks"] = marks;
    return row;
}

} // namespace

QVariantList SessionBridge::recentRecordings(int limit) const
{
    QVariantList out;
    if (limit <= 0) {
        return out;
    }
    // Both roots' folders, newest first across them (the names start with
    // the stamp, so by name, last first, is newest first)
    QFileInfoList folders;
    for (const QString& root : {recordingsRoot(), earlierRecordingsRoot()}) {
        if (!root.isEmpty()) {
            folders +=
                QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        }
    }
    std::sort(folders.begin(), folders.end(),
              [](const QFileInfo& a, const QFileInfo& b) {
                  return a.fileName() > b.fileName();
              });
    for (const QFileInfo& info : folders) {
        if (out.size() >= limit) {
            break;
        }
        if (holdsRecording(info.absoluteFilePath())) {
            out.append(recordingRowOf(info));
        }
    }
    return out;
}

QVariantMap SessionBridge::recordingRow(const QString& folder) const
{
    if (folder.isEmpty() || !holdsRecording(folder)) {
        return {};
    }
    QVariantMap row = recordingRowOf(QFileInfo(folder));
    row["folder"] = folder;
    return row;
}

ConditionSetPtr SessionBridge::recordingConditions(const QString& uri,
                                                   QString* unused)
{
    std::string folder8;
    if (!uri.startsWith(QLatin1String("replay://"), Qt::CaseInsensitive) ||
        !cli::replay_folder(uri.toStdString(), &folder8)) {
        return nullptr;
    }
    const QString folder = QString::fromStdString(folder8);
    cli::RecordedSignals recorded;
    if (!readRecordedHeader(folder, &recorded) ||
        recorded.conditions_sha256.empty()) {
        return nullptr;
    }
    QFile copy(folder + QStringLiteral("/conditions.json"));
    if (!copy.open(QIODevice::ReadOnly)) {
        *unused = QStringLiteral("is missing");
        return nullptr;
    }
    const QByteArray bytes = copy.read(ECUIO_CONDITION_FILE_MAX_BYTES + 1);
    const std::string sha =
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
            .toHex()
            .toStdString();
    if (sha != recorded.conditions_sha256) {
        *unused = QStringLiteral("doesn't match its hash");
        return nullptr;
    }
    QString words;
    ConditionSetPtr set = loadConditionSet(bytes, &words);
    if (!set) {
        *unused = QStringLiteral("doesn't load (%1)").arg(words);
    }
    return set;
}
