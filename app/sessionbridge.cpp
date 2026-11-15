// File/layer: app/sessionbridge.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge); Manual smoke

#include "sessionbridge.h"
#include "bridgetext.h"
#include "carryover.h"
#include "healthreport.h"
#include "obdbimport.h"
#include "storemigration.h"
#include "diagnostics_line.h" /* cli/: havne-cli's diagnostics line */
#include "recording_signals.h" /* cli/: a replay's own signal file */
#include "signal_file_lines.h" /* cli/: the profile a URI selects */
#include "refused_signals.h" /* cli/: the signals an ECU refuses, as havne-cli says them */
#include "default_pick.h" /* cli/: the default pick, as havne-cli's --pick ranked */
#include "scan_line.h"       /* cli/: when a module's session earns a line */
#include "urlwrite.h"
#include "ecuio/version.h"
#include "ecuio/signals.h"
#include "ecuio/j2534.h"
#include "ecuio/obdb.h"
#include "ecuio/profile.h"
#include "ecuio/serial.h"
#include "ecuio/signal_sets.h"
#include "ecuio/socketcan.h"
#include "ecuio/vin.h"

#include <cmath>
#include <QMetaObject>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringConverter>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <iterator>
#include <limits>
#include <chrono>
#include <cstring>

using namespace bridgetext;

namespace {

// A clear's vehicle speed in the unit system, converted by core
// (ADR-0022): "48 km/h", "30 mph"; "" if core can't convert it.
QString clearSpeedText(uint32_t kmh, uint32_t units)
{
    ecuio_display_value_t out{};
    if (ecuio_display_convert("km/h", 0, kmh, units, &out) !=
        ECUIO_DISPLAY_OK) {
        return {};
    }
    return QStringLiteral("%1 %2")
        .arg(qRound(out.value))
        .arg(QString::fromUtf8(out.unit));
}

} // namespace

// ---------------- SessionBridge ----------------
SessionBridge::SessionBridge(QObject* parent)
    : QObject(parent)
    , m_frameLog(new FrameListModel(this))
    , m_frameIds(new FrameIdModel(m_frameLog, this))
    , m_recordingImport(
          new RecordingImport([this] { return recordingsRoot(); }, this))
    , m_coalesceTimer(new QTimer(this))
    , m_signalModel(new SignalListModel(this))
    , m_liveSignalModel(new LiveSignalModel(m_signalModel, this))
{
    (void)ecuio_init();   // optional and always ECUIO_OK (core.h), paired below
    m_pluginStore.setLog(
        [this](const QString& line) { emit logMessage(line); });
    m_conditionModel = new ConditionModel(this);
    loadSavedConditions();

    connect(m_coalesceTimer, &QTimer::timeout, this, [this]() {
        notifyFrameCounts();
        pollLinkHealth();   // every tick while connected: transitions only
        pollChannelSupport();
        pollAbsentSignals();
        pollDefaultPick(); // after the reports: their drops judge it
        pollUnaskedSignals();
        pollChannelIdentity();
        pollChannelDiagnostics();
        pollReplayEnd();
        pollSleep();
        // Fires even when nothing arrived this window: dimming exists for
        // rows that stopped updating. 100 ms is well inside the 3 s dim
        // threshold, and the emission is one signal for the whole model.
        m_signalModel->refreshAges();
        // Drop count at tick rate rather than per frame; onFrame also
        // queues a report as it drops (at most one pending).
        notifyDropped();
        flushValues();
        flushConditions(
            m_session); // after the values: a firing's value is shown
        pollRecording();
    });
    // A tick asks its row: the default pick follows on the next tick
    connect(m_signalModel, &SignalListModel::ticksChanged, this,
            [this]() { m_pickDirty = true; });

    // The log's readout: its time and its folder's size, once a second
    m_logTimer = new QTimer(this);
    m_logTimer->setInterval(1000);
    connect(m_logTimer, &QTimer::timeout, this, [this]() {
        countLogBytes();
        emit logProgressChanged();
    });

    // What Armed says about starting follows everything that decides it
    for (auto changed :
         {&SessionBridge::recordingStateChanged,
          &SessionBridge::logStateChanged, &SessionBridge::conditionsChanged,
          &SessionBridge::currentUriChanged,
          &SessionBridge::disconnectingChanged}) {
        connect(this, changed, this, [this]() { judgeArmedWhy(); });
    }
    connect(this, &SessionBridge::connectedChanged, this,
            [this]() { judgeArmedWhy(); });
    connect(m_conditionModel, &QAbstractItemModel::modelReset, this,
            [this]() { judgeArmedWhy(); });

    // The activity lines go into the recording's events.log as notes,
    // but for those core writes in its own form (logUnnoted)
    connect(this, &SessionBridge::logMessage, this,
            [this](const QString& line) {
                if (m_recorder != nullptr && m_noting) {
                    (void)ecuio_recorder_note(m_recorder,
                                              line.toUtf8().constData());
                }
            });
}


SessionBridge::~SessionBridge()
{
    // A Read once's session closes within its wait; its queued result is
    // dropped with `this`, and core must outlive it
    m_readOnce.join();
    // The async connect worker calls core and posts a queued completion
    // against `this`: join it before tearing the session or core down.
    // With a wedged vendor that waits up to the surrogate handshake
    // timeout (~8 s): fine at exit, not on the UI thread mid-session,
    // hence the async connect.
    if (m_connectThread.joinable()) {
        m_connectThread.join();
    }
    // An async disconnect still tearing down: wait for it, so core is
    // never shut down under it. Its queued completion is dropped with
    // `this`. Bounded like any teardown: about 6 s with a wedged
    // surrogate child, which the unresponsive latch caps.
    if (m_disconnectThread.joinable()) {
        m_disconnectThread.join();
    }
    // A connect whose queued completion never ran (it was dropped with
    // `this`) left its session pending. It was never started, so no
    // worker is calling onFrame; destroy it like any other.
    if (ecuio_session_t* pending = m_pendingSession.exchange(nullptr)) {
        ecuio_session_destroy(pending);
    }
    if (m_session != nullptr) {
        destroySessionLocked();
    }
    // Closed without stopRecording's signal: QML may already be gone
    if (m_recorder != nullptr) {
        (void)ecuio_recorder_close(m_recorder, "the app closed");
        m_recorder = nullptr;
    }
    // Only now: no session is left to decode with the files' sets
    m_signalStore.close();
    m_pluginStore.close();
    for (auto& held : m_replayFiles) {
        const QByteArray name = held.second.profile.toUtf8();
        (void)ecuio_profile_unregister(name.constData());
        ecuio_signal_file_free(held.second.file);
    }
    m_replayFiles.clear();
    ecuio_shutdown();
}

QVariantMap SessionBridge::importSignalFile(const QUrl& url)
{
    // A picked file opens through openPicked: Android's content:// by the content resolver
    QFile file;
    QVariantMap result;
    if (!openPicked(file, pathOfUrl(url), QIODevice::ReadOnly)) {
        result = {{QStringLiteral("ok"), false},
                  {QStringLiteral("words"),
                   QStringLiteral("Couldn't read the file")}};
    } else if (file.size() > static_cast<qint64>(ECUIO_SIGNAL_FILE_MAX_BYTES)) {
        result = {
            {QStringLiteral("ok"), false},
            {QStringLiteral("words"),
             QStringLiteral("The file is over %1 KiB, a signal file's most")
                 .arg(ECUIO_SIGNAL_FILE_MAX_BYTES / 1024)}};
    } else {
        result =
            m_signalStore.import(file.read(ECUIO_SIGNAL_FILE_MAX_BYTES + 1));
    }
    emit logMessage(
        result.value(QStringLiteral("ok")).toBool()
            ? QStringLiteral("Signal file: %1 (%2, %3)")
                  .arg(result.value(QStringLiteral("name")).toString(),
                       result.value(QStringLiteral("profile")).toString(),
                       signalCountText(
                           result.value(QStringLiteral("count")).toInt()))
            : QStringLiteral("Signal file refused: %1")
                  .arg(result.value(QStringLiteral("words")).toString()));
    return result;
}

QVariantMap SessionBridge::importSignalText(const QString& text)
{
    const QVariantMap result = m_signalStore.import(text.toUtf8());
    emit logMessage(
        result.value(QStringLiteral("ok")).toBool()
            ? QStringLiteral("Signal file saved: %1 (%2, %3)")
                  .arg(result.value(QStringLiteral("name")).toString(),
                       result.value(QStringLiteral("profile")).toString(),
                       signalCountText(
                           result.value(QStringLiteral("count")).toInt()))
            : QStringLiteral("Signal file not saved: %1")
                  .arg(result.value(QStringLiteral("words")).toString()));
    return result;
}

QString SessionBridge::signalFileText(const QString& sha) const
{
    return SignalStore::text(sha);
}

QVariantMap SessionBridge::saveSignalFile(const QString& sha, const QUrl& url)
{
    QString words;
    const QByteArray bytes = SignalStore::bytes(sha, &words);
    const bool removable = pickedDocumentRemovable(url);
    bool written = false;
    if (!bytes.isEmpty()) {
        written = writeToUrl(url, bytes);
        if (!written) {
            words = QStringLiteral("Couldn't write the file");
        }
    }
    if (!written) {
        dropPickedDocument(url, removable);
    }
    emit logMessage(
        written ? QStringLiteral("Signal file %1 saved to a file (%2 bytes)")
                      .arg(sha.left(12))
                      .arg(bytes.size())
                : QStringLiteral("Signal file %1 not saved: %2")
                      .arg(sha.left(12), words));
    if (!written) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("words"), words}};
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("words"), QString()}};
}

QVariantMap SessionBridge::saveLayoutFile(const QString& text, const QUrl& url)
{
    const QByteArray bytes = text.toUtf8();
    const bool removable = pickedDocumentRemovable(url);
    QString words;
    bool written = false;
    if (bytes.isEmpty()) {
        words = QStringLiteral("There is no layout to save");
    } else if (bytes.size() > layoutFileMaxBytes) {
        words = QStringLiteral("The layout is over %1 bytes, a layout file's most")
                    .arg(layoutFileMaxBytes);
    } else {
        written = writeToUrl(url, bytes);
        if (!written) {
            words = QStringLiteral("Couldn't write the file");
        }
    }
    if (!written) {
        dropPickedDocument(url, removable);
    }
    emit logMessage(written ? QStringLiteral("Layout saved to a file (%1 bytes)")
                                  .arg(bytes.size())
                            : QStringLiteral("Layout not saved: %1").arg(words));
    return {{QStringLiteral("ok"), written}, {QStringLiteral("words"), words}};
}

void SessionBridge::dropPicked(const QUrl& url)
{
    dropPickedDocument(url, pickedDocumentRemovable(url));
}

QVariantMap SessionBridge::layoutFileText(const QUrl& url)
{
    // A picked file opens through openPicked: Android's content:// by the content resolver
    QFile file;
    QString words;
    QString text;
    if (!openPicked(file, pathOfUrl(url), QIODevice::ReadOnly)) {
        words = QStringLiteral("Couldn't read the file");
    } else {
        QByteArray bytes = file.read(layoutFileMaxBytes + 1);
        if (bytes.size() > layoutFileMaxBytes) {
            words = QStringLiteral("The file is over %1 bytes, a layout file's most")
                        .arg(layoutFileMaxBytes);
        } else {
            if (bytes.startsWith("\xEF\xBB\xBF")) {
                bytes.remove(0, 3);
            }
            QStringDecoder utf8(QStringDecoder::Utf8,
                                QStringConverter::Flag::Stateless |
                                    QStringConverter::Flag::ConvertInitialBom);
            text = utf8(bytes);
            if (utf8.hasError()) {
                text.clear();
                words = QStringLiteral("The file isn't UTF-8 text");
            }
        }
    }
    emit logMessage(words.isEmpty()
                        ? QStringLiteral("Layout file read (%1 characters)")
                              .arg(text.size())
                        : QStringLiteral("Layout file not read: %1").arg(words));
    return {{QStringLiteral("ok"), words.isEmpty()},
            {QStringLiteral("words"), words},
            {QStringLiteral("text"), text}};
}

QVariantMap SessionBridge::exportFrames(const QUrl& url)
{
    const FrameListModel::ScrubbedCsv csv = m_frameLog->scrubbedCsv();
    QString words = csv.words;
    const bool removable = pickedDocumentRemovable(url);
    bool written = false;
    if (csv.ok) {
        written = writeToUrl(url, csv.bytes);
        if (!written) {
            words = QStringLiteral("Couldn't write the file");
        }
    }
    if (!written) {
        dropPickedDocument(url, removable);
    }
    const qulonglong leftOut = csv.identification + csv.textValues;
    emit logMessage(
        written ? QStringLiteral("Frames exported: %1 written, %2 left out "
                                 "(identification and text signals), %3 "
                                 "segments unattributed")
                      .arg(csv.written)
                      .arg(leftOut)
                      .arg(qulonglong{csv.unattributed})
                : QStringLiteral("Frames not exported: %1").arg(words));
    return {{QStringLiteral("ok"), written},
            {QStringLiteral("words"), written ? QString() : words},
            {QStringLiteral("written"), written ? csv.written : 0},
            {QStringLiteral("leftOut"), leftOut},
            {QStringLiteral("unattributed"), qulonglong{csv.unattributed}}};
}

QVariantMap SessionBridge::saveHealthReport(const QUrl& url, int filterIndex,
                                            const QString& baselineFolder)
{
    if (m_session == nullptr || !connected()) {
        const QString words = QStringLiteral("No session");
        dropPickedDocument(url, pickedDocumentRemovable(url));
        emit logMessage(
            QStringLiteral("Health report not saved: %1").arg(words));
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("words"), words}};
    }
    const std::vector<ecuio_health_value_t> held = m_signalModel->heldValues();
    ecuio_health_options_t o{};
    o.format = healthFormatFor(url, filterIndex);
    o.units = m_signalModel->unitSystem();
    o.latest_model_year = healthLatestModelYear();
    o.utc_us = healthUtcNow();
    o.session = m_session;
    o.value_count = static_cast<uint32_t>(held.size());
    o.values = held.empty() ? nullptr : held.data();
    const QByteArray baseline = healthBaselineArgument(baselineFolder);
    o.baseline_folder_utf8 =
        baseline.isEmpty() ? nullptr : baseline.constData();
    const QVariantMap r = writeHealthReport(o, url);
    emit logMessage(r.value(QStringLiteral("line")).toString());
    return r;
}

QVariantList SessionBridge::useSignalFiles(const QStringList& shas)
{
    const QVariantList used = m_signalStore.use(shas);
    for (const QVariant& v : used) {
        const QVariantMap e = v.toMap();
        if (!e.value(QStringLiteral("ok")).toBool()) {
            emit logMessage(
                QStringLiteral("Signal file %1 not offered: %2")
                    .arg(e.value(QStringLiteral("sha256")).toString().left(12),
                         e.value(QStringLiteral("words")).toString()));
        }
    }
    return used;
}

int SessionBridge::deleteUnusedSignalFiles(const QStringList& keep)
{
    return m_signalStore.deleteUnused(keep);
}

QVariantMap SessionBridge::previewObdb(const QUrl& url, int modelYear,
                                       const QString& name)
{
    // A picked file opens through openPicked: Android's content:// by the content resolver
    QFile file;
    if (!openPicked(file, pathOfUrl(url), QIODevice::ReadOnly)) {
        return previewObdbBytes({}, QStringLiteral("Couldn't read the file"),
                                modelYear, name);
    }
    if (file.size() > static_cast<qint64>(ECUIO_OBDB_MAX_BYTES)) {
        return previewObdbBytes(
            {},
            QStringLiteral("The file is over %1 MiB, an OBDb set's most")
                .arg(ECUIO_OBDB_MAX_BYTES / (1024 * 1024)),
            modelYear, name);
    }
    return previewObdbBytes(file.read(ECUIO_OBDB_MAX_BYTES + 1), QString(),
                            modelYear, name);
}

QVariantMap SessionBridge::previewObdbImport(int modelYear, const QString& name)
{
    const bool waiting = m_importKind == QLatin1String("obdb");
    return previewObdbBytes(
        waiting ? m_importPending : QByteArray(),
        waiting ? QString()
                : QStringLiteral("No OBDb set is waiting: import it again"),
        modelYear, name);
}

QVariantMap SessionBridge::previewObdbBytes(const QByteArray& bytes,
                                            const QString& refusal,
                                            int modelYear, const QString& name)
{
    m_obdbPending.clear();
    const uint32_t year = modelYear > 0 ? static_cast<uint32_t>(modelYear) : 0u;
    ObdbImport::Preview p;
    if (refusal.isEmpty()) {
        p = ObdbImport::read(bytes, year, name);
    } else {
        p.words = refusal;
    }
    if (p.ok) {
        m_obdbPending = p.json;
        m_obdbPendingYear = static_cast<int>(year);
    }
    emit logMessage(
        p.ok ? QStringLiteral("OBDb set read for %1: %2 converted, %3 fields "
                              "differ, %4 left out (model year %5)")
                   .arg(name)
                   .arg(p.converted)
                   .arg(p.changed.size())
                   .arg(p.skipped.size())
                   .arg(year == 0 ? QStringLiteral("none")
                                  : QString::number(year))
             : QStringLiteral("OBDb set refused: %1").arg(p.words));
    return ObdbImport::report(p, name, year);
}

QVariantMap SessionBridge::keepObdb(const QString& replacing)
{
    QVariantMap result;
    if (m_obdbPending.isEmpty()) {
        result = {{QStringLiteral("ok"), false},
                  {QStringLiteral("words"),
                   QStringLiteral("No OBDb set is waiting: import it again")}};
    } else if (!replacing.isEmpty() && m_session != nullptr &&
               m_signalStore.shaFor(ecuio_session_signal_set(m_session)) ==
                   replacing) {
        // The session decodes with the file this replaces: the import
        // waits, so Keep works once it has disconnected
        result = {{QStringLiteral("ok"), false},
                  {QStringLiteral("words"),
                   QStringLiteral("The session reads the earlier import: "
                                  "disconnect, then keep it")}};
    } else {
        result = m_signalStore.import(m_obdbPending);
        m_obdbPending.clear();
    }
    emit logMessage(
        result.value(QStringLiteral("ok")).toBool()
            ? QStringLiteral("Signal file from OBDb: %1 (%2, %3)%4")
                  .arg(result.value(QStringLiteral("name")).toString(),
                       result.value(QStringLiteral("profile")).toString(),
                       signalCountText(
                           result.value(QStringLiteral("count")).toInt()),
                       replacing.isEmpty()
                           ? QString()
                           : QStringLiteral(", replacing %1")
                                 .arg(replacing.left(12)))
            : QStringLiteral("OBDb set not kept: %1")
                  .arg(result.value(QStringLiteral("words")).toString()));
    return result;
}

bool SessionBridge::isObdbImport(const QString& sha) const
{
    QString words;
    return ObdbImport::isConversion(SignalStore::bytes(sha, &words));
}

int SessionBridge::vinModelYear(const QString& typedVin) const
{
    return ObdbImport::modelYear(
        typedVin.trimmed().isEmpty() ? m_vin : typedVin,
        QDate::currentDate().year() + 1);
}

QVariantList SessionBridge::plugins()
{
    return m_pluginStore.all();
}

QVariantList SessionBridge::decodeProfiles()
{
    QVariantList out;
    for (const char* builtIn : {"obd2", "mock"}) {
        out.append(QVariantMap{
            {QStringLiteral("profile"), QString::fromLatin1(builtIn)},
            {QStringLiteral("name"), QString()}});
    }
    for (const QVariant& v : m_signalStore.registered()) {
        const QVariantMap e = v.toMap();
        out.append(QVariantMap{
            {QStringLiteral("profile"), e.value(QStringLiteral("profile"))},
            {QStringLiteral("name"), e.value(QStringLiteral("name"))}});
    }
    for (const QVariant& v : m_pluginStore.all()) {
        const QVariantMap e = v.toMap();
        if (!e.value(QStringLiteral("ok")).toBool()) {
            continue;
        }
        const QStringList variants =
            e.value(QStringLiteral("variants")).toStringList();
        const QStringList profiles =
            e.value(QStringLiteral("profiles")).toStringList();
        for (qsizetype i = 0; i < profiles.size(); ++i) {
            out.append(QVariantMap{
                {QStringLiteral("profile"), profiles.at(i)},
                {QStringLiteral("name"),
                 e.value(QStringLiteral("name")).toString() +
                     (i < variants.size() ? QStringLiteral(" ") + variants.at(i)
                                          : QString())}});
        }
    }
    return out;
}

// The activity log's line for a package's import
static QString pluginImportLine(const QVariantMap& result)
{
    if (!result.value(QStringLiteral("ok")).toBool()) {
        return QStringLiteral("Plugin refused: %1")
            .arg(result.value(QStringLiteral("words")).toString());
    }
    const QString name = result.value(QStringLiteral("name")).toString();
    const QString version = result.value(QStringLiteral("version")).toString();
    const QString profiles = result.value(QStringLiteral("profiles"))
                                 .toStringList()
                                 .join(QStringLiteral(", "));
    const QString updated = result.value(QStringLiteral("updated")).toString();
    return updated.isEmpty()
               ? QStringLiteral("Plugin: %1 %2 (%3)")
                     .arg(name, version, profiles)
               : QStringLiteral("Plugin updated: %1 from %2 to %3 (%4)")
                     .arg(name, updated, version, profiles);
}

QVariantMap SessionBridge::importPlugin(const QUrl& url)
{
    // A picked file opens through openPicked: Android's content:// by the content resolver
    QFile file;
    QVariantMap result;
    if (!openPicked(file, pathOfUrl(url), QIODevice::ReadOnly)) {
        result = {{QStringLiteral("ok"), false},
                  {QStringLiteral("words"),
                   QStringLiteral("Couldn't read the file")}};
    } else if (file.size() > static_cast<qint64>(ECUIO_PLUGIN_PACKAGE_MAX)) {
        result = {{QStringLiteral("ok"), false},
                  {QStringLiteral("words"),
                   QStringLiteral("The file is over %1 MiB, a plugin's most")
                       .arg(ECUIO_PLUGIN_PACKAGE_MAX / (1024 * 1024))}};
    } else {
        result = m_pluginStore.import(file.read(ECUIO_PLUGIN_PACKAGE_MAX + 1));
    }
    emit logMessage(pluginImportLine(result));
    return result;
}

QVariantMap SessionBridge::removePlugin(const QString& sha)
{
    const QVariantMap result = m_pluginStore.remove(sha);
    emit logMessage(
        result.value(QStringLiteral("ok")).toBool()
            ? QStringLiteral("Plugin removed")
            : QStringLiteral("Plugin not removed: %1")
                  .arg(result.value(QStringLiteral("words")).toString()));
    return result;
}

QVariantList SessionBridge::pluginMatches(const QString& typedVin) const
{
    return m_pluginStore.matches(typedVin.trimmed().isEmpty() ? m_vin
                                                              : typedVin,
                                 QDate::currentDate().year() + 1);
}

// The first key of a JSON object's text, "" when it doesn't start as one
static QByteArray firstKeyOf(const QByteArray& bytes)
{
    qsizetype at = 0;
    const auto skipSpace = [&]() {
        while (at < bytes.size() &&
               (bytes.at(at) == ' ' || bytes.at(at) == '\t' ||
                bytes.at(at) == '\r' || bytes.at(at) == '\n')) {
            ++at;
        }
    };
    skipSpace();
    if (at >= bytes.size() || bytes.at(at) != '{') {
        return {};
    }
    ++at;
    skipSpace();
    if (at >= bytes.size() || bytes.at(at) != '"') {
        return {};
    }
    const qsizetype end = bytes.indexOf('"', at + 1);
    return end < 0 ? QByteArray() : bytes.mid(at + 1, end - at - 1);
}

// What an import is (ADR-0064): a package or a signal file by its first
// key, an OBDb set by its commands; "" for anything else
static QString importKindOf(const QByteArray& bytes)
{
    const QByteArray first = firstKeyOf(bytes);
    if (first == "havne_plugin") {
        return QStringLiteral("plugin");
    }
    if (first == "havne_signals") {
        return QStringLiteral("signals");
    }
    // A file past an OBDb set's bound isn't parsed: the word alone says
    // which bound it is refused by
    const bool commands =
        bytes.size() > static_cast<qsizetype>(ECUIO_OBDB_MAX_BYTES)
            ? bytes.contains("\"commands\"")
            : QJsonDocument::fromJson(bytes).object().contains(
                  QStringLiteral("commands"));
    return commands ? QStringLiteral("obdb") : QString();
}

QVariantMap SessionBridge::readImport(const QUrl& url, const QString& only)
{
    dropImport();
    const auto refused = [](const QString& words) {
        return QVariantMap{{QStringLiteral("ok"), false},
                           {QStringLiteral("words"), words}};
    };
    // Android's content:// URIs open through QFile as they are
    QFile file(url.isLocalFile() ? url.toLocalFile() : url.toString());
    QVariantMap result;
    QString kind;
    QString line;
    const QString tooLarge =
        QStringLiteral("The file is over %1 MiB, a plugin's most")
            .arg(ECUIO_PLUGIN_PACKAGE_MAX / (1024 * 1024));
    QByteArray bytes;
    if (file.open(QIODevice::ReadOnly)) {
        // The read's own bound: a provider may give no size to judge by
        bytes = file.read(ECUIO_PLUGIN_PACKAGE_MAX + 1);
    }
    if (!file.isOpen()) {
        result = refused(QStringLiteral("Couldn't read the file"));
    } else if (bytes.size() >
               static_cast<qsizetype>(ECUIO_PLUGIN_PACKAGE_MAX)) {
        result = refused(tooLarge);
    } else {
        // An editor's byte-order mark is no part of the text the loaders
        // read
        if (bytes.startsWith("\xEF\xBB\xBF")) {
            bytes.remove(0, 3);
        }
        kind = importKindOf(bytes);
        if (!only.isEmpty() && kind != only) {
            // Before anything is kept: a package would go in at once
            result = refused(QStringLiteral(
                "Not a signal file: only one takes a variant's place"));
        } else if (kind == QLatin1String("plugin")) {
            result = m_pluginStore.import(bytes);
            line = pluginImportLine(result);
        } else if (kind == QLatin1String("signals")) {
            result = bytes.size() >
                             static_cast<qsizetype>(ECUIO_SIGNAL_FILE_MAX_BYTES)
                         ? refused(QStringLiteral("The file is over %1 KiB, a "
                                                  "signal file's most")
                                       .arg(ECUIO_SIGNAL_FILE_MAX_BYTES / 1024))
                         : PluginStore::describe(bytes);
            if (result.value(QStringLiteral("ok")).toBool()) {
                m_importPending = bytes;
                m_importKind = kind;
                line = QStringLiteral("Signal file read: %1 (as a plugin, %2)")
                           .arg(result.value(QStringLiteral("name")).toString(),
                                result.value(QStringLiteral("id")).toString());
            } else {
                line =
                    QStringLiteral("Signal file refused: %1")
                        .arg(result.value(QStringLiteral("words")).toString());
            }
        } else if (kind == QLatin1String("obdb") &&
                   bytes.size() >
                       static_cast<qsizetype>(ECUIO_OBDB_MAX_BYTES)) {
            result = refused(
                QStringLiteral("The file is over %1 MiB, an OBDb set's most")
                    .arg(ECUIO_OBDB_MAX_BYTES / (1024 * 1024)));
            line = QStringLiteral("OBDb set refused: %1")
                       .arg(result.value(QStringLiteral("words")).toString());
        } else if (kind == QLatin1String("obdb")) {
            // Held, so the report converts these bytes: the file is read
            // once
            m_importPending = bytes;
            m_importKind = kind;
            result = {{QStringLiteral("ok"), true},
                      {QStringLiteral("words"), QString()}};
            line = QStringLiteral("OBDb set found: its report comes first");
        } else {
            result = refused(QStringLiteral(
                "The file is neither a plugin, a signal file nor an OBDb "
                "signal set"));
        }
    }
    result.insert(QStringLiteral("kind"), kind);
    emit logMessage(
        line.isEmpty()
            ? QStringLiteral("Import refused: %1")
                  .arg(result.value(QStringLiteral("words")).toString())
            : line);
    return result;
}

// The activity log's line for a wrap (into "") or a put: what the user
// is told beside the plugin is core's notes, then the store's words (an
// index it couldn't write, a file here already)
static QString keptLine(const QVariantMap& result, const QString& into)
{
    QString said;
    for (const char* key : {"notes", "words"}) {
        const QString text = result.value(QLatin1String(key)).toString();
        if (!text.isEmpty()) {
            said += QStringLiteral("; ") + text;
        }
    }
    const bool here = result.value(QStringLiteral("kept")).toBool();
    return result.value(QStringLiteral("ok")).toBool()
               ? QStringLiteral("%1: %2 %3 (%4)%5")
                     .arg(!into.isEmpty() ? QStringLiteral("Plugin changed")
                          : here ? QStringLiteral("Plugin already here")
                                 : QStringLiteral("Plugin made"),
                          result.value(QStringLiteral("name")).toString(),
                          result.value(QStringLiteral("version")).toString(),
                          result.value(QStringLiteral("profiles"))
                              .toStringList()
                              .join(QStringLiteral(", ")),
                          said)
               : QStringLiteral("%1: %2").arg(
                     into.isEmpty() ? QStringLiteral("Plugin not made")
                                    : QStringLiteral("Plugin not changed"),
                     result.value(QStringLiteral("words")).toString());
}

QVariantMap SessionBridge::keepFile(const QByteArray& file, const QString& into,
                                    const QString& variant,
                                    const QString& vehicle)
{
    const QVariantMap result =
        into.isEmpty() ? m_pluginStore.wrap(file, {vehicle, variant})
                       : m_pluginStore.put(into, variant, file);
    emit logMessage(keptLine(result, into));
    return result;
}

QVariantMap SessionBridge::keepSignals(const QString& into,
                                       const QString& variant,
                                       const QString& vehicle)
{
    if (m_importKind != QLatin1String("signals")) {
        const QString words =
            QStringLiteral("No signal file is waiting: import it again");
        emit logMessage(QStringLiteral("Plugin not made: %1").arg(words));
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("words"), words}};
    }
    const QVariantMap result =
        keepFile(m_importPending, into, variant, vehicle);
    if (result.value(QStringLiteral("ok")).toBool()) {
        dropImport();
    }
    return result;
}

QVariantMap SessionBridge::keepSignalText(const QString& text,
                                          const QString& into,
                                          const QString& variant,
                                          const QString& vehicle)
{
    return keepFile(text.toUtf8(), into, variant, vehicle);
}

QVariantMap SessionBridge::keepObdbPlugin(const QString& vehicle,
                                          bool yearTyped)
{
    QVariantMap result;
    if (m_obdbPending.isEmpty()) {
        result = {{QStringLiteral("ok"), false},
                  {QStringLiteral("words"),
                   QStringLiteral("No OBDb set is waiting: import it again")}};
    } else {
        // Only a year the user typed names the variant: one decoded from
        // a VIN stays out of anything a plugin carries
        const QString variant =
            yearTyped && m_obdbPendingYear > 0
                ? QStringLiteral("my%1").arg(m_obdbPendingYear)
                : QString();
        result = m_pluginStore.wrap(m_obdbPending, {vehicle, variant});
        if (result.value(QStringLiteral("ok")).toBool()) {
            dropImport();
        }
    }
    const QString said = result.value(QStringLiteral("words")).toString();
    emit logMessage(
        result.value(QStringLiteral("ok")).toBool()
            ? QStringLiteral("Plugin from OBDb: %1 %2 (%3)%4%5")
                  .arg(result.value(QStringLiteral("name")).toString(),
                       result.value(QStringLiteral("version")).toString(),
                       result.value(QStringLiteral("profiles"))
                           .toStringList()
                           .join(QStringLiteral(", ")),
                       result.value(QStringLiteral("merged")).toBool()
                           ? QStringLiteral(
                                 ", as a variant of the earlier import")
                       : result.value(QStringLiteral("replaced"))
                               .toString()
                               .isEmpty()
                           ? QString()
                           : QStringLiteral(", in the earlier import's place"),
                       said.isEmpty() ? QString() : QStringLiteral("; ") + said)
            : QStringLiteral("OBDb set not kept: %1")
                  .arg(result.value(QStringLiteral("words")).toString()));
    return result;
}

QVariantMap SessionBridge::dropPluginVariant(const QString& sha,
                                             const QString& variant)
{
    const QVariantMap result = m_pluginStore.drop(sha, variant);
    emit logMessage(
        result.value(QStringLiteral("ok")).toBool()
            ? QStringLiteral("Plugin changed: %1 %2 (%3)")
                  .arg(result.value(QStringLiteral("name")).toString(),
                       result.value(QStringLiteral("version")).toString(),
                       result.value(QStringLiteral("profiles"))
                           .toStringList()
                           .join(QStringLiteral(", ")))
            : QStringLiteral("Variant not removed: %1")
                  .arg(result.value(QStringLiteral("words")).toString()));
    return result;
}

QVariantMap SessionBridge::markPluginMine(const QString& sha)
{
    const QVariantMap result = m_pluginStore.markMine(sha);
    const QString said = result.value(QStringLiteral("words")).toString();
    emit logMessage(
        result.value(QStringLiteral("ok")).toBool()
            ? QStringLiteral("Plugin %1 marked as yours%2")
                  .arg(sha.left(12),
                       said.isEmpty() ? QString() : QStringLiteral("; ") + said)
            : QStringLiteral("Plugin not marked as yours: %1")
                  .arg(result.value(QStringLiteral("words")).toString()));
    return result;
}

// The activity log's line for forgotten vehicles' imports, "" for none
static QString forgottenLine(const QVariantMap& result, bool every)
{
    const int count = result.value(QStringLiteral("count")).toInt();
    const QString said = result.value(QStringLiteral("words")).toString();
    return count == 0
               ? QString()
               : (every ? QStringLiteral("Plugins no longer imported for the "
                                         "forgotten vehicles: %1%2")
                        : QStringLiteral("Plugins no longer imported for a "
                                         "forgotten vehicle: %1%2"))
                     .arg(count)
                     .arg(said.isEmpty() ? QString()
                                         : QStringLiteral("; ") + said);
}

QVariantMap SessionBridge::forgetVehicleImports(const QString& vehicle)
{
    const QVariantMap result = m_pluginStore.forgetVehicle(vehicle);
    const QString line = forgottenLine(result, false);
    if (!line.isEmpty()) {
        emit logMessage(line);
    }
    return result;
}

QVariantMap SessionBridge::forgetAllVehicleImports()
{
    const QVariantMap result = m_pluginStore.forgetVehicles();
    const QString line = forgottenLine(result, true);
    if (!line.isEmpty()) {
        emit logMessage(line);
    }
    return result;
}

QStringList SessionBridge::migrateStores(QSettings& prefs)
{
    return StoreMigration::run(prefs, m_pluginStore).lines;
}

QVariantMap SessionBridge::saveUnmigratedFile(const QString& sha,
                                              const QUrl& url)
{
    QString words;
    // Judged before the save: a save that fails removes only the document
    // its own pick made (healthreport.h)
    const bool removable = pickedDocumentRemovable(url);
    QByteArray bytes = StoreMigration::unmigratedBytes(sha, &words);
    if (bytes.isEmpty()) {
        bytes = StoreMigration::oldCopy(sha, &words);
    }
    bool written = false;
    if (!bytes.isEmpty()) {
        written = writeToUrl(url, bytes);
        if (!written) {
            words = QStringLiteral("Couldn't write the file");
        }
    }
    if (!written) {
        dropPickedDocument(url, removable);
    }
    emit logMessage(
        written ? QStringLiteral("Signal file %1 that wasn't moved saved to "
                                 "a file (%2 bytes)")
                      .arg(sha.left(12))
                      .arg(bytes.size())
                : QStringLiteral("Signal file %1 that wasn't moved not "
                                 "saved: %2")
                      .arg(sha.left(12), words));
    return {{QStringLiteral("ok"), written},
            {QStringLiteral("words"), written ? QString() : words}};
}

bool SessionBridge::removeUnmigratedFile(const QString& sha)
{
    return StoreMigration::removeUnmigrated(sha);
}

QVariantMap SessionBridge::exportPlugin(const QString& sha, const QUrl& url)
{
    (void)m_pluginStore.all();
    QString words;
    const bool removable = pickedDocumentRemovable(url);
    const QByteArray bytes = m_pluginStore.packed(sha, &words);
    bool written = false;
    if (!bytes.isEmpty()) {
        written = writeToUrl(url, bytes);
        if (!written) {
            words = QStringLiteral("Couldn't write the file");
        }
    }
    if (!written) {
        dropPickedDocument(url, removable);
    }
    emit logMessage(
        written ? QStringLiteral("Plugin %1 exported to a file (%2 bytes)")
                      .arg(sha.left(12))
                      .arg(bytes.size())
                : QStringLiteral("Plugin %1 not exported: %2")
                      .arg(sha.left(12), words));
    return {{QStringLiteral("ok"), written},
            {QStringLiteral("words"), written ? QString() : words}};
}

QVariantMap SessionBridge::pluginExportNote(const QString& sha)
{
    return m_pluginStore.exportNote(sha);
}

QVariantMap SessionBridge::pluginVariantText(const QString& sha,
                                             const QString& variant)
{
    return m_pluginStore.variantText(sha, variant);
}

QVariantMap SessionBridge::saveVariantFile(const QString& sha,
                                           const QString& variant,
                                           const QUrl& url)
{
    QString words;
    const bool removable = pickedDocumentRemovable(url);
    const QByteArray bytes = m_pluginStore.variantBytes(sha, variant, &words);
    bool written = false;
    if (!bytes.isEmpty()) {
        written = writeToUrl(url, bytes);
        if (!written) {
            words = QStringLiteral("Couldn't write the file");
        }
    }
    if (!written) {
        dropPickedDocument(url, removable);
    }
    emit logMessage(
        written ? QStringLiteral("Variant %1 of plugin %2 saved as a signal "
                                 "file (%3 bytes)")
                      .arg(variant, sha.left(12))
                      .arg(bytes.size())
                : QStringLiteral("Variant %1 of plugin %2 not saved: %3")
                      .arg(variant, sha.left(12), words));
    return {{QStringLiteral("ok"), written},
            {QStringLiteral("words"), written ? QString() : words}};
}

QVariantMap SessionBridge::copyVariant(const QString& sha,
                                       const QString& variant,
                                       const QString& into,
                                       const QString& newVariant)
{
    const QVariantMap result =
        m_pluginStore.copyVariant(sha, variant, into, newVariant);
    emit logMessage(keptLine(result, into));
    return result;
}

QVariantList SessionBridge::offeredVariants(const QString& vehicle,
                                            const QString& typedVin,
                                            const QStringList& picks)
{
    return m_pluginStore.offered(
        vehicle, typedVin.trimmed().isEmpty() ? m_vin : typedVin,
        QDate::currentDate().year() + 1, picks);
}

QVariantList SessionBridge::pickRow(const QStringList& picks, bool adapter)
{
    return m_pluginStore.pickRow(picks, adapter);
}

QVariantMap SessionBridge::pickVariant(const QStringList& picks,
                                       const QString& profile)
{
    return m_pluginStore.pick(picks, profile);
}

QStringList SessionBridge::unpickVariant(const QStringList& picks,
                                         const QString& profile)
{
    return PluginStore::unpick(picks, profile);
}

QVariantList SessionBridge::pickFiles(const QStringList& picks)
{
    return m_pluginStore.pickFiles(picks);
}

QString SessionBridge::readOnce(const QString& adapterUri,
                                const QVariantMap& signal,
                                const QVariantMap& module)
{
    // The adapter is the session's while one runs, and a second open
    // of it may be refused: Read once waits for none
    if (m_connected.load(std::memory_order_acquire) || m_connecting ||
        m_disconnecting) {
        return QStringLiteral(
            "Read once reads only while no session is connected");
    }
    if (m_readOnce.busy()) {
        return QStringLiteral("A read is running");
    }
    ReadOnce::Request request;
    QString why;
    if (!ReadOnce::requestFrom(signal, module, &request, &why)) {
        return why;
    }
    const QString uri = ReadOnce::uriFor(adapterUri, request, &why);
    if (uri.isEmpty()) {
        return why;
    }
    // An app-fed stream's link is readied here, as a connect's is
    if (m_streamPreparer) {
        why = m_streamPreparer(uri);
        if (!why.isEmpty()) {
            return why;
        }
    }
    applyFrameLoggingEnv();
    const bool started = m_readOnce.start(
        uri, request, m_readOnceWaitMs, [this, uri](const QVariantMap& result) {
            QMetaObject::invokeMethod(
                this,
                [this, uri, result]() {
                    m_readOnce.join();
                    QVariantMap shown = result;
                    shown.insert(QStringLiteral("uri"), uri);
                    emit readingOnceChanged();
                    emit logMessage(
                        QStringLiteral("Read once: %1 %2")
                            .arg(result.value(QStringLiteral("request"))
                                     .toString(),
                                 result.value(QStringLiteral("ok")).toBool()
                                     ? QStringLiteral("answered")
                                     : result.value(QStringLiteral("words"))
                                           .toString()));
                    emit readOnceResult(shown);
                },
                Qt::QueuedConnection);
        });
    if (!started) {
        return QStringLiteral("A read is running");
    }
    emit readingOnceChanged();
    return {};
}

void SessionBridge::destroySessionLocked()
{
    // The destructor's teardown. destroy() closes the session first if needed.
    if (m_session != nullptr) {
        ecuio_session_destroy(m_session);
        m_session = nullptr;
    }
}

QString SessionBridge::coreVersion() const
{
    return QString::fromUtf8(ecuio_version_string());
}

QVariantMap SessionBridge::gaugeSignals() const
{
    const auto ids = [](int mock, int obd2) {
        return QVariantList{QStringLiteral("id:%1").arg(mock),
                            QStringLiteral("id:%1").arg(obd2)};
    };
    return {
        {QStringLiteral("rpm"),
         ids(ECUIO_MOCK_SIG_RPM_ID, ECUIO_OBD2_SIG_RPM_ID)},
        {QStringLiteral("speed"),
         ids(ECUIO_MOCK_SIG_SPEED_ID, ECUIO_OBD2_SIG_SPEED_ID)},
        {QStringLiteral("coolant"),
         ids(ECUIO_MOCK_SIG_COOLANT_ID, ECUIO_OBD2_SIG_COOLANT_TEMP_ID)},
        {QStringLiteral("oil"),
         ids(ECUIO_MOCK_SIG_OIL_ID, ECUIO_OBD2_SIG_OIL_TEMP_ID)},
        {QStringLiteral("iat"),
         ids(ECUIO_MOCK_SIG_IAT_ID, ECUIO_OBD2_SIG_IAT_ID)},
        {QStringLiteral("maf"),
         ids(ECUIO_MOCK_SIG_MAF_ID, ECUIO_OBD2_SIG_MAF_ID)},
        {QStringLiteral("throttle"),
         ids(ECUIO_MOCK_SIG_THROTTLE_ID, ECUIO_OBD2_SIG_THROTTLE_ID)},
        {QStringLiteral("load"),
         ids(ECUIO_MOCK_SIG_LOAD_ID, ECUIO_OBD2_SIG_ENGINE_LOAD_ID)},
        {QStringLiteral("battery"),
         ids(ECUIO_MOCK_SIG_BATTERY_V_ID, ECUIO_OBD2_SIG_BATTERY_V_ID)},
        {QStringLiteral("lambda"),
         ids(ECUIO_MOCK_SIG_LAMBDA_ID, ECUIO_OBD2_PID_SIG_ID(0x44, 0))},
        {QStringLiteral("boost"),
         ids(ECUIO_MOCK_SIG_BOOST_ID, ECUIO_OBD2_SIG_BOOST_ID)},
    };
}

const ecuio_signal_file_t*
SessionBridge::fileOfSet(const ecuio_signal_set_t* set) const
{
    if (set == nullptr) {
        return nullptr;
    }
    const ecuio_signal_file_t* file = m_signalStore.fileFor(set);
    if (file == nullptr) {
        file = m_pluginStore.fileFor(set);
    }
    for (auto it = m_replayFiles.begin();
         file == nullptr && it != m_replayFiles.end(); ++it) {
        if (ecuio_signal_file_set(it->second.file) == set) {
            file = it->second.file;
        }
    }
    return file;
}

QString SessionBridge::profileOfUri(const QString& uri)
{
    // Core's scheme routing and defaults (core.cpp, ecuio_session_create):
    // case-sensitive prefixes; j2534:// on Windows alone, socketcan:// on
    // Linux but not Android (internal.h's ECUIO_HAVE_SOCKETCAN); mock://
    // decodes through mock, an adapter through obd2, a replay and a bus
    // through raw. replayUriFor has its own rule for a recorded URI
    static const struct {
        const char* scheme;
        const char* profile;
    } kSchemes[] = {
        {"mock://", "mock"},   // the mock's own set
        {"replay://", "raw"},  // a recording's frames
        {"elm327://", "obd2"}, // an adapter
#ifdef _WIN32
        {"j2534://", "obd2"}, // an adapter
#endif
#if defined(__linux__) && !defined(__ANDROID__)
        {"socketcan://", "raw"}, // a bus
#endif
    };
    const QByteArray u = uri.toUtf8();
    QString profile;
    for (const auto& s : kSchemes) {
        if (u.startsWith(s.scheme)) {
            profile = QString::fromLatin1(s.profile);
            break;
        }
    }
    if (profile.isEmpty()) {
        return {};
    }
    const qsizetype q = u.indexOf('?');
    if (q < 0) {
        return profile;
    }
    // The query's grammar (core/src/uri_query.h): key=value segments
    // joined by '&', none empty, each with an '='; every profile= value
    // 1 to ECUIO_PROFILE_NAME_MAX bytes, the last one winning. Other keys
    // and the segment count are core's to judge at the connect
    const QList<QByteArray> segments = u.mid(q + 1).split('&');
    for (const QByteArray& segment : segments) {
        const qsizetype eq = segment.indexOf('=');
        if (eq < 0) {
            return {};
        }
        if (segment.left(eq) == "profile") {
            const QByteArray value = segment.mid(eq + 1);
            if (value.isEmpty() || value.size() > ECUIO_PROFILE_NAME_MAX) {
                return {};
            }
            profile = QString::fromUtf8(value);
        }
    }
    return profile;
}

QVariantList SessionBridge::profileSignals(const QString& uri) const
{
    const QString profile = profileOfUri(uri);
    const ecuio_signal_set_t* set = nullptr;
    if (profile.isEmpty() ||
        ecuio_profile_lookup(profile.toUtf8().constData(), &set) != ECUIO_OK ||
        set == nullptr || set->defs == nullptr) {
        return {};
    }
    const ecuio_signal_file_t* file = fileOfSet(set);
    const bool builtIn = SignalListModel::isBuiltIn(set);
    const uint32_t units = m_signalModel->unitSystem();
    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(set->count));
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& def = set->defs[i];
        const SignalListModel::DefFacts f =
            SignalListModel::factsOf(file, builtIn, def);
        const ecuio_display_value_t shown =
            displayed(def.unit, def.decimal_places, 0.0, units);
        const std::pair<double, double> range = displayedRange(
            def.unit, def.decimal_places, f.rangeLow, f.rangeHigh, units);
        // The file's own gauge for this def, in its decoded unit (the unit
        // a cell stores); empty when the file gives none
        QVariantMap gauge;
        if (file != nullptr) {
            if (const ecuio_signal_gauge_t* g =
                    ecuio_signal_file_gauge(file, &def)) {
                gauge.insert(QStringLiteral("min"), g->min);
                gauge.insert(QStringLiteral("max"), g->max);
                if ((g->flags & ECUIO_GAUGE_WARN_LOW) != 0) {
                    gauge.insert(QStringLiteral("warnLow"), g->warn_low);
                }
                if ((g->flags & ECUIO_GAUGE_WARN_HIGH) != 0) {
                    gauge.insert(QStringLiteral("warnHigh"), g->warn_high);
                }
            }
        }
        rows.append(QVariantMap{
            {QStringLiteral("id"), static_cast<int>(def.id)},
            {QStringLiteral("ref"), f.ref},
            {QStringLiteral("name"), QString::fromUtf8(def.name)},
            {QStringLiteral("unit"), QString::fromUtf8(shown.unit)},
            {QStringLiteral("decimals"),
             static_cast<int>(shown.decimal_places)},
            {QStringLiteral("isText"),
             def.kind == ECUIO_VALUE_TEXT || def.kind == ECUIO_VALUE_HEX},
            {QStringLiteral("isEnum"), f.isEnum},
            {QStringLiteral("group"), static_cast<int>(def.group)},
            {QStringLiteral("rangeLow"), range.first},
            {QStringLiteral("rangeHigh"), range.second},
            {QStringLiteral("decodedUnit"), QString::fromUtf8(def.unit)},
            {QStringLiteral("decodedLow"), f.rangeLow},
            {QStringLiteral("decodedHigh"), f.rangeHigh},
            {QStringLiteral("gauge"), gauge},
        });
    }
    return rows;
}

QString SessionBridge::statusString(int statusCode) const
{
    return QString::fromUtf8(ecuio_status_string(static_cast<ecuio_status_t>(statusCode)));
}

QVariantList SessionBridge::discoverJ2534Devices() const
{
    QVariantList out;

    ecuio_j2534_device_t* devices = nullptr;
    uint32_t count = 0;
    if (ecuio_j2534_discover_devices(&devices, &count) != ECUIO_OK) {
        return out;   // includes non-Windows NOT_IMPLEMENTED -> empty list
    }
    for (uint32_t i = 0; i < count; ++i) {
        QVariantMap m;
        m["vendorName"] = QString::fromUtf8(devices[i].vendor_name);
        m["dllPath"] = QString::fromUtf8(devices[i].dll_path);
        /* reserved[0] is the loader's PE sniff: 1 = x86, 2 = x64,
         * 0 = unknown. Unknown is displayed honestly: the loader
         * sends it to the x64 surrogate, which is where it would
         * load, not a claim about the binary. */
        m["architecture"] = (devices[i].reserved[0] == 2)
                                ? QStringLiteral("x64")
                            : (devices[i].reserved[0] == 1)
                                ? QStringLiteral("x86")
                                : QStringLiteral("unknown");
        out.append(m);
    }
    ecuio_j2534_free_devices(devices);
    return out;
}

QVariantList SessionBridge::discoverSerialPorts() const
{
    QVariantList out;
    uint32_t count = 0;
    if (ecuio_serial_ports(nullptr, 0, &count) != ECUIO_OK || count == 0) {
        return out;
    }
    /* A port plugged in between the calls is left for the next refresh */
    std::vector<ecuio_serial_port_t> ports(count);
    if (ecuio_serial_ports(ports.data(), count, &count) != ECUIO_OK) {
        return out;
    }
    const uint32_t listed =
        std::min<uint32_t>(count, static_cast<uint32_t>(ports.size()));
    for (uint32_t i = 0; i < listed; ++i) {
        QVariantMap m;
        m["name"] = QString::fromUtf8(ports[i].name);
        m["device"] = QString::fromUtf8(ports[i].device);
        out.append(m);
    }
    return out;
}

namespace {
/* A CAN mode mask in core's words (ip link's), the named bits alone */
QStringList canModeWords(uint32_t modes)
{
    QStringList words;
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        const char* word = (modes & bit) != 0 ? ecuio_can_mode_name(bit) : "";
        if (*word != '\0') {
            words.append(QString::fromLatin1(word));
        }
    }
    return words;
}
} // namespace

QVariantList SessionBridge::discoverCanInterfaces() const
{
    QVariantList out;
    uint32_t count = 0;
    if (ecuio_can_interfaces(nullptr, 0, &count) != ECUIO_OK || count == 0) {
        return out;
    }
    /* An interface added between the calls is left for the next refresh */
    std::vector<ecuio_can_interface_t> list(count);
    if (ecuio_can_interfaces(list.data(), count, &count) != ECUIO_OK) {
        return out;
    }
    const uint32_t listed =
        std::min<uint32_t>(count, static_cast<uint32_t>(list.size()));
    for (uint32_t i = 0; i < listed; ++i) {
        QVariantMap m;
        m["name"] = QString::fromLatin1(list[i].name);
        m["kind"] = QString::fromLatin1(list[i].kind);
        m["up"] = (list[i].flags & ECUIO_CAN_UP) != 0;
        m["listenOnly"] = (list[i].flags & ECUIO_CAN_LISTEN_ONLY) != 0;
        m["fd"] = (list[i].flags & ECUIO_CAN_FD) != 0;
        m["bitrate"] = list[i].bitrate;
        m["state"] = QString::fromLatin1(ecuio_can_state_name(list[i].state));
        m["modes"] = canModeWords(list[i].modes);
        m["modesKnown"] = (list[i].flags & ECUIO_CAN_MODES_KNOWN) != 0;
        m["modesSupported"] = canModeWords(list[i].modes_supported);
        out.append(m);
    }
    return out;
}

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
    const QByteArray bytes = copy.read(ECUIO_SIGNAL_FILE_MAX_BYTES + 1);
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
    // The same bytes in the store: the vehicle's own file, under its name
    if (m_signalStore.profileOf(sha) == profile) {
        return selecting(profile);
    }
    auto held = m_replayFiles.find(sha);
    if (held == m_replayFiles.end()) {
        ecuio_signal_file_t* file = nullptr;
        if (ecuio_signal_file_load(bytes.constData(),
                                   static_cast<size_t>(bytes.size()),
                                   &file) != ECUIO_OK) {
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
                                ECUIO_SIGNAL_FILE_MAX_SIGNALS) *
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

// Pure-core half: safe on any thread. Creates and registers, the slow
// part (DLL load, PassThruOpen), but doesn't start: until the GUI thread
// has taken the session, no worker may call onFrame with `this`. On
// failure, cleans up fully (core API, mutex-safe off-thread) and returns
// the error string. On success the caller owns *out_session and must
// startSession() and applyConnectedState() on the GUI thread. Touches no
// Qt models, timers, or properties.
QString SessionBridge::connectCoreOnly(const QString& uri,
                                       ecuio_session_t** out_session)
{
    const QByteArray uri8 = uri.toUtf8();
    ecuio_session_t* session = nullptr;
    ecuio_status_t rc = ecuio_session_create(uri8.constData(), &session);
    if (rc != ECUIO_OK || session == nullptr) {
        return failureText(rc);
    }

    rc = ecuio_register_frame_callback(session, &SessionBridge::onFrame, this);
    if (rc == ECUIO_OK) {
        // ADR-0024: core decodes on the worker and hands each frame's
        // values to onValues, after onFrame
        rc = ecuio_register_values_callback(session, &SessionBridge::onValues, this);
    }
    if (rc == ECUIO_OK) {
        // ADR-0063: a frame's condition events, after its values
        rc = ecuio_register_conditions_callback(
            session, &SessionBridge::onConditions, this);
    }
    if (rc != ECUIO_OK) {
        ecuio_session_destroy(session);
        return QString::fromUtf8(ecuio_status_string(rc));
    }

    *out_session = session;
    return {};
}

// GUI thread: start the worker of a session connectCoreOnly produced.
// onFrame and onValues accept from here (m_accepting), so a frame sent
// at once, as a replay's first is, queues and reaches
// handleFrameInternal after applyConnectedState, and its values wait in
// the staging, sized here, for the first tick. On failure the session
// is destroyed.
namespace {

// Whether a URI's query names poll=: such a session asks the polled
// signals alone, whatever its pick
bool namesPoll(const QString& uri)
{
    const qsizetype q = uri.indexOf(QLatin1Char('?'));
    if (q < 0) {
        return false;
    }
    const QStringList segments = uri.mid(q + 1).split(QLatin1Char('&'));
    return std::any_of(segments.begin(), segments.end(), [](const QString& s) {
        return s.startsWith(QLatin1String("poll="));
    });
}

} // namespace

QString SessionBridge::startSession(ecuio_session_t* session,
                                    const QString& uri)
{
    // Zeroed before the worker starts, not at the apply: a replay at max
    // can pass the in-flight cap before then, and those drops count
    m_framesDelivered.store(0, std::memory_order_relaxed);
    m_droppedFrames.store(0, std::memory_order_relaxed);
    m_lastReportedDropped = 0;
    m_overflowFrames.store(0, std::memory_order_relaxed);
    if (m_reportedOverflow != 0) {
        m_reportedOverflow = 0;
        emit overflowFramesChanged();
    }
    m_frameLatency.clear();
    m_valueLatency.clear();
    m_valuesStaged = 0;
    prepareValueStaging(ecuio_session_signal_set(session));
    m_frameLog->noteTextDefs(ecuio_session_signal_set(session));
    // Before the start (no whole sweep first) and the recorder (its header
    // names the pick); the vehicle's ticks come after the connect
    m_pickSet = false;
    m_pickIds.clear();
    m_pickPolled = namesPoll(uri);
    m_pickLine.clear();
    m_pickLinePending = false;
    applyDefaultPick(session, false, true);
    // Before the recorder, which holds the set the session judges then
    // (ECUIO_ERR_IN_USE after), and before the start, so the first frame
    // is judged
    attachSessionConditions(session, uri);
    // Attached before the worker starts, so the recording holds every frame
    startRecording(session, uri);
    m_accepting.store(true, std::memory_order_release);
    const ecuio_status_t rc = ecuio_session_start(session);
    if (rc != ECUIO_OK) {
        m_accepting.store(false, std::memory_order_release);
        const QString err = failureText(rc);
        // The folder keeps its header and says why there are no frames
        stopRecording(QStringLiteral("the session didn't start: %1").arg(err));
        ecuio_unregister_frame_callback(session, &SessionBridge::onFrame, this);
        ecuio_unregister_values_callback(session, &SessionBridge::onValues, this);
        ecuio_unregister_conditions_callback(
            session, &SessionBridge::onConditions, this);
        ecuio_session_destroy(session);
        endSessionConditions();
        return err;
    }
    return {};
}

// GUI-thread half: the state a successful connect applies, ending with the
// "Connected:" and "Session:" log lines; the sync and async paths share it,
// so they apply the same state.
void SessionBridge::applyConnectedState(ecuio_session_t* session, const QString& uri)
{
    // Before any emit: each runs handlers synchronously, and a connect one
    // of them starts must be refused, not replace this session
    m_session = session;
    m_connected.store(true, std::memory_order_release);
    ecuio_session_info_t info{};
    const bool haveInfo = ecuio_session_get_info(session, &info) == ECUIO_OK;
    m_adapterVersions = haveInfo ? versionsText(info) : QString();
    m_vin.clear(); // this car's comes with its identity
    m_currentUri = uri;
    emit currentUriChanged();
    // The frame log belongs to a session, like its count: rows from two
    // connects would otherwise run together with nothing between them.
    m_frameLog->clear();
    m_currentFrameCount = 0;
    notifyFrameCounts();
    m_signalModel->setSignalSet(ecuio_session_signal_set(session),
                                fileOfSet(ecuio_session_signal_set(session)));
    m_stagingBuffer.clear();   // previous session's batch must never
    m_stagingTexts.clear();    // bleed into this one's first
    m_linkErrorCode = ECUIO_OK;
    m_errorEpisodes = 0;
    emit linkStateChanged();
    setLogPaused(false);       // a new session always logs
    // Precise: a coarse timer fires up to 5 % early, and the ring's 120 s
    // counts on ten windows a second
    m_coalesceTimer->setTimerType(Qt::PreciseTimer);
    m_coalesceTimer->start(100);
    emit connectedChanged(true);
    emit framesReset();

    emit logMessage(QStringLiteral("Connected: %1").arg(uri));
    m_channelCount = 0;
    if (haveInfo) {
        emit logMessage(sessionInfoText(info));
        m_channelCount = std::min<uint32_t>(info.channel_count,
                                            ECUIO_SESSION_INFO_MAX_CHANNELS);
    }
    if (m_pickLinePending) {
        m_pickLinePending = false;
        writePickLine();
    }
    for (const QString& line : std::exchange(m_conditionsLines, {})) {
        emit logMessage(line);
    }
    m_replayFrames = info.replay_frames;
    m_sleptUs = sleptUs();
    m_replayEndLogged = info.backend != ECUIO_BACKEND_REPLAY ||
                        (info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP) != 0;
    for (uint32_t& g : m_supportGeneration) {
        g = 0;
    }
    for (ecuio_channel_support_t& s : m_supportReports) {
        s = ecuio_channel_support_t{};
    }
    for (SessionWritten& w : m_sessionWritten) {
        w = SessionWritten{};
    }
    if (!m_refusedUnnamed.isEmpty()) {
        m_refusedUnnamed.clear();
        emit refusedUnnamedChanged();
    }
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    m_absentRead.assign((set != nullptr && set->defs != nullptr) ? set->count : 0u, 0);
    m_unsupportedRead.assign(m_absentRead.size(), 0);
    m_absentIds.clear();
    m_unaskedRead.assign(m_absentRead.size(), 0);
    m_unaskedIds.clear();
    for (uint32_t& g : m_identityGeneration) {
        g = 0;
    }
    for (uint32_t& g : m_diagnosticsGeneration) {
        g = 0;
    }
    for (bool& asked : m_clearAsked) {
        asked = false;
    }
    for (bool& waiting : m_clearWaiting) {
        waiting = false;
    }
    m_clearSavedTo.clear();
    emit clearMovingChanged();
    m_diagnosticsShownCount = 0;
    if (!m_diagnostics.isEmpty()) {
        m_diagnostics.clear();
        emit diagnosticsChanged();
    }
}

QString SessionBridge::connectSession(const QString& typed)
{
    // Spaces around a pasted URI are the field's, not the URI's: core
    // matches the scheme at the first character
    QString uri = typed.trimmed();
    if (m_disconnecting) {
        return QStringLiteral("Disconnect in progress");
    }
    if (m_connected.load(std::memory_order_acquire) || m_connecting) {
        return {};
    }
    if (m_readOnce.busy()) {
        return QStringLiteral("Read once is using the adapter");
    }
    uri = replayUriWithOwnSignals(uri);

    QString err = m_streamPreparer ? m_streamPreparer(uri) : QString();
    if (!err.isEmpty()) {
        emit logMessage(
            QStringLiteral("Connect failed: %1 (%2)").arg(uri, err));
        return err;
    }
    applyFrameLoggingEnv();
    ecuio_session_t* session = nullptr;
    err = connectCoreOnly(uri, &session);
    if (err.isEmpty()) {
        err = startSession(session, uri);
    }
    if (!err.isEmpty()) {
        emit logMessage(QStringLiteral("Connect failed: %1 (%2)").arg(uri, err));
        return err;
    }
    applyConnectedState(session, uri);
    return {};
}

void SessionBridge::connectSessionAsync(const QString& typed)
{
    QString uri = typed.trimmed(); // as connectSession does
    if (m_connected.load(std::memory_order_acquire) || m_connecting
        || m_disconnecting) {
        return;   // one attempt at a time, none during a teardown; the
                  // UI already reflects state
    }
    if (m_readOnce.busy()) {
        const QString err = QStringLiteral("Read once is using the adapter");
        emit logMessage(
            QStringLiteral("Connect failed: %1 (%2)").arg(uri, err));
        emit connectResult(uri, err);
        return;
    }
    uri = replayUriWithOwnSignals(uri);
    // An app-fed stream's link is readied here, on the GUI thread, before
    // core opens it on the connect thread
    const QString unprepared =
        m_streamPreparer ? m_streamPreparer(uri) : QString();
    if (!unprepared.isEmpty()) {
        emit logMessage(
            QStringLiteral("Connect failed: %1 (%2)").arg(uri, unprepared));
        emit connectResult(uri, unprepared);
        return;
    }
    m_connecting = true;
    emit connectingChanged();
    emit logMessage(QStringLiteral("Connecting: %1").arg(uri));
    applyFrameLoggingEnv();   // before the connect thread spawns any surrogate

    m_connectThread = std::thread([this, uri]() {
        ecuio_session_t* session = nullptr;
        const QString core_err = connectCoreOnly(uri, &session);
        // Handed over through the member, not the lambda: a completion
        // dropped with `this` would leak a lambda capture, while the
        // destructor can always find the member.
        m_pendingSession.store(session);

        QMetaObject::invokeMethod(this, [this, uri, core_err]() {
            // Join first, then clear the flag, then emit: assigning to a
            // joinable std::thread terminates the process, and a QML
            // handler can run synchronously inside connectingChanged.
            // connecting clears only once the session is applied, so a
            // handler always sees connecting or connected: a connect it
            // starts is refused, never a second session over this one.
            if (m_connectThread.joinable()) {
                m_connectThread.join();
            }
            ecuio_session_t* session = m_pendingSession.exchange(nullptr);
            QString err = core_err;
            if (err.isEmpty()) {
                err = startSession(session, uri);
            }
            if (err.isEmpty()) {
                applyConnectedState(session, uri);
            }
            m_connecting = false;
            emit connectingChanged();

            if (err.isEmpty()) {
                emit connectResult(uri, {});
            } else {
                emit logMessage(QStringLiteral("Connect failed: %1 (%2)")
                                    .arg(uri, err));
                emit connectResult(uri, err);
            }
        }, Qt::QueuedConnection);
    });
}

void SessionBridge::disconnectSession()
{
    if (!m_connected.load(std::memory_order_acquire) || m_disconnecting) return;

    const QString uri = m_currentUri;
    emit logMessage(QStringLiteral("Disconnecting: %1").arg(uri));
    // The close sends 10 01 after the worker's last report (ADR-0060)
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        const QString line = sessionReturnedText(m_supportReports[c]);
        if (!line.isEmpty()) {
            emit logMessage(line);
        }
    }

    // Every flag before any emit: a QML handler can run synchronously
    // inside one, and must already see disconnecting with connected
    // false, never a moment where Connect looks available. From here
    // onFrame and handleFrameInternal drop everything, and the GUI
    // thread never touches the session again.
    ecuio_session_t* session = std::exchange(m_session, nullptr);
    m_accepting.store(false, std::memory_order_release);
    m_connected.store(false, std::memory_order_release);
    m_disconnecting = true;
    m_coalesceTimer->stop();
    notifyFrameCounts();   // the last tick's worth
    flushValues();         // and its values
    flushConditions(session); // and its condition events
    endSessionConditions();
    emit logMessage(deliveryText(deliveryStats()));
    m_valueSet = nullptr;  // the drain's; onValues stops at m_accepting
    m_stagingBuffer.clear();
    m_stagingTexts.clear();
    m_currentUri.clear();
    m_adapterVersions.clear();
    m_vin.clear();
    m_linkErrorCode = ECUIO_OK;           // the properties must not
    m_errorEpisodes = 0;              // outlive the session

    emit disconnectingChanged();
    if (!uri.isEmpty()) {
        emit currentUriChanged();
    }
    emit connectedChanged(false);
    emit linkStateChanged();          // "fault"/"quiet" -> "disconnected"
    setLogPaused(false);

    // The teardown thread is the session's one caller from here: the
    // session API is single-caller by contract.
    m_disconnectThread = std::thread([this, session, uri]() {
        // Close joins the worker, whose last act publishes reply timing's
        // final rows (ADR-0042); they're read here because destroy frees
        // them
        ecuio_session_close(session);
        ecuio_session_timing_t timing{};
        std::vector<ecuio_request_timing_t> rows;
        uint32_t count = 0;
        if (ecuio_session_get_timing(session, &timing, nullptr, 0, &count) ==
                ECUIO_OK &&
            count > 0) {
            rows.resize(count);
            if (ecuio_session_get_timing(session, &timing, rows.data(), count,
                                         &count) != ECUIO_OK) {
                count = 0;
            }
            rows.resize(std::min<size_t>(count, rows.size()));
        }
        ecuio_session_destroy(session);

        QMetaObject::invokeMethod(
            this,
            [this, uri, timing, rows]() {
                // Join first, then close the recording, then clear the
                // flag and emit, as the connect completion does: a QML
                // handler inside disconnectingChanged may start the next
                // connect, which must find this session's recorder gone.
                if (m_disconnectThread.joinable()) {
                    m_disconnectThread.join();
                }
                const QString timingLine = timingText(timing, rows);
                if (!timingLine.isEmpty()) {
                    emit logMessage(timingLine);
                }
                emit logMessage(QStringLiteral("Disconnected: %1").arg(uri));
                // Kept open until here, so it brackets the whole teardown.
                // Only the user's Disconnect gets here: the Dashboard needs
                // no reason, the recording's end note keeps it
                stopRecording(QStringLiteral("disconnected"), false,
                              QStringLiteral("Log ended"));
                m_disconnecting = false;
                emit disconnectingChanged();
            },
            Qt::QueuedConnection);
    });
}

void SessionBridge::setRecording(bool on)
{
    if (m_connected.load(std::memory_order_acquire) || m_connecting || m_disconnecting) {
        emit recordingChanged();   // refused: puts the switch back
        return;
    }
    if (m_recording == on) return;
    m_recording = on;
    emit recordingChanged();
}

namespace {
// Settings' choices for a log's minimum time between sweeps, ms; 0 is off
constexpr int kMinSweepChoices[] = {0, 100, 250, 500, 1000};
} // namespace

QVariantList SessionBridge::minSweepChoices() const
{
    QVariantList choices;
    for (int ms : kMinSweepChoices) {
        choices.append(ms);
    }
    return choices;
}

void SessionBridge::setMinSweepMs(int ms)
{
    if (std::find(std::begin(kMinSweepChoices), std::end(kMinSweepChoices),
                  ms) == std::end(kMinSweepChoices) ||
        ms == m_minSweepMs) {
        return;
    }
    m_minSweepMs = ms;
    m_pickDirty = true; // the default pick takes it too
    emit minSweepMsChanged();
}

void SessionBridge::setUnitSystem(int units)
{
    if (units != static_cast<int>(ECUIO_UNITS_METRIC) &&
        units != static_cast<int>(ECUIO_UNITS_IMPERIAL)) {
        return;
    }
    if (units == unitSystem()) return;
    m_signalModel->setUnitSystem(static_cast<uint32_t>(units));
    m_conditionModel->setUnitSystem(static_cast<uint32_t>(units));
    emit unitSystemChanged();
    emit clearMovingChanged();
    if (m_diagnosticsShownCount > 0) {
        rebuildDiagnostics();
    }
}

double SessionBridge::displayValue(const QString& unit, double value) const
{
    ecuio_display_value_t out;
    const QByteArray u = unit.toUtf8();
    if (ecuio_display_convert(u.constData(), 0, value, m_signalModel->unitSystem(), &out) !=
        ECUIO_DISPLAY_OK) {
        return value;
    }
    return out.value;
}

double SessionBridge::decodedValue(const QString& unit, double shown) const
{
    ecuio_display_value_t at0;
    ecuio_display_value_t at1;
    const QByteArray u = unit.toUtf8();
    const uint32_t units = m_signalModel->unitSystem();
    if (ecuio_display_convert(u.constData(), 0, 0.0, units, &at0) != ECUIO_DISPLAY_OK ||
        ecuio_display_convert(u.constData(), 0, 1.0, units, &at1) != ECUIO_DISPLAY_OK ||
        at0.converted == 0 || at1.value == at0.value) {
        return shown;
    }
    return (shown - at0.value) / (at1.value - at0.value);
}

QString SessionBridge::displayUnit(const QString& unit) const
{
    ecuio_display_value_t out;
    const QByteArray u = unit.toUtf8();
    if (ecuio_display_convert(u.constData(), 0, 0.0, m_signalModel->unitSystem(), &out) !=
        ECUIO_DISPLAY_OK) {
        return unit;
    }
    return QString::fromUtf8(out.unit);
}

QString SessionBridge::recordingFolder() const
{
    if (m_recordingFolder.isEmpty()) {
        return {};
    }
    return documentsRelative(m_recordingFolder);
}

QUrl SessionBridge::recordingFolderUrl() const
{
    return m_recordingFolder.isEmpty() ? QUrl()
                                       : QUrl::fromLocalFile(m_recordingFolder);
}

// Recording turns on the surrogate's payload logging for the spawns it
// records, so the session log holds the requests and write statuses the
// recording can't see. A value set at launch is left alone; one the
// bridge set comes off again at the next connect without recording.
// Called on the GUI thread before the connect spawns anything; the child
// inherits the environment at spawn.
void SessionBridge::applyFrameLoggingEnv()
{
    static const char kVar[] = "ECUIO_SUR_LOG_FRAMES";
    if (m_recording) {
        if (!m_setFrameLogging && !qEnvironmentVariableIsSet(kVar)) {
            qputenv(kVar, "1");
            m_setFrameLogging = true;
        }
    } else if (m_setFrameLogging) {
        qunsetenv(kVar);
        m_setFrameLogging = false;
    }
}

QString SessionBridge::recordingsRoot() const
{
    if (!m_recordingsRoot.isEmpty()) {
        return m_recordingsRoot;
    }
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return docs.isEmpty() ? QString()
                          : docs + QStringLiteral("/Havne/recordings");
}

QString SessionBridge::earlierRecordingsRoot() const
{
    if (!m_earlierRecordingsRoot.isEmpty()) {
        return m_earlierRecordingsRoot;
    }
    if (!m_recordingsRoot.isEmpty()) {
        return QString(); // a test's own root: Documents is never read
    }
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString root = docs + QLatin1Char('/') + earlierProductName() +
                         QStringLiteral("/recordings");
    return docs.isEmpty() || root == recordingsRoot() ? QString() : root;
}

void SessionBridge::startRecording(ecuio_session_t* session, const QString& uri)
{
    if (!m_recording) return;
    // A replay would copy the recording it plays
    if (!m_recordReplays &&
        uri.startsWith(QLatin1String("replay://"), Qt::CaseInsensitive)) {
        emit logMessage(QStringLiteral(
            "Not recording: a replay plays a recording already"));
        return;
    }
    ecuio_recorder_t* recorder =
        openRecorder(session, uri, QStringLiteral("Recording failed"));
    if (recorder == nullptr) {
        return;
    }
    adoptRecorder(recorder);
    emit logMessage(QStringLiteral("Recording: %1").arg(recordingFolder()));
}

namespace {

// A recorder's folder, absolute; empty if core couldn't give it whole, so
// a caller never acts on a cut path
QString folderOf(ecuio_recorder_t* recorder)
{
    char folder[4096] = {};
    uint32_t length = 0;
    if (ecuio_recorder_get_folder(recorder, folder, sizeof folder, &length) !=
            ECUIO_OK ||
        length >= sizeof folder) {
        return QString();
    }
    return QString::fromUtf8(folder);
}

// Whether frames.csv holds a row past its header
bool holdsFrames(const QString& folder)
{
    QFile frames(QDir(folder).filePath(QStringLiteral("frames.csv")));
    if (!frames.open(QIODevice::ReadOnly)) {
        return false;
    }
    (void)frames.readLine();
    return !frames.readLine().trimmed().isEmpty();
}

} // namespace

ecuio_recorder_t* SessionBridge::openRecorder(ecuio_session_t* session,
                                              const QString& uri,
                                              const QString& failed,
                                              QString* reason, uint32_t flags)
{
    const auto fail = [this, &failed, reason](const QString& why) {
        if (reason != nullptr) {
            *reason = why;
        }
        emit logMessage(QStringLiteral("%1: %2").arg(failed, why));
        return nullptr;
    };
    const QString root = recordingsRoot();
    if (root.isEmpty()) {
        return fail(QStringLiteral("no Documents folder"));
    }

    // The picked vehicle names an adapter's session only (ADR-0027 section 1):
    // a mock or a replay isn't that car. Core reduces the label to what a
    // folder's name may hold (ADR-0049 section 2).
    const bool adapter =
        uri.startsWith(QStringLiteral("j2534://"), Qt::CaseInsensitive) ||
        uri.startsWith(QStringLiteral("elm327://"), Qt::CaseInsensitive);
    const QByteArray frameLogging = qgetenv("ECUIO_SUR_LOG_FRAMES");
    // The app's own header lines, which core writes under "caller"
    QList<QByteArray> lines = {
        QStringLiteral("Havne %1").arg(coreVersion()).toUtf8(),
        (m_setFrameLogging
             ? QStringLiteral(
                   "surrogate frame logging  on, set by this recording")
         : frameLogging.isNull()
             ? QStringLiteral("surrogate frame logging  off")
             : QStringLiteral("surrogate frame logging  "
                              "ECUIO_SUR_LOG_FRAMES=%1, set at launch")
                   .arg(QString::fromUtf8(frameLogging)))
            .toUtf8(),
    };
    if (adapter && !m_vehicleLabel.isEmpty()) {
        lines << QStringLiteral("vehicle  %1").arg(m_vehicleLabel).toUtf8();
    }
    std::vector<const char*> header;
    for (const QByteArray& l : lines) {
        header.push_back(l.constData());
    }
    const QByteArray root8 = root.toUtf8();
    const QByteArray label8 = adapter ? m_vehicleLabel.toUtf8() : QByteArray();
    ecuio_record_options_t o{};
    o.root_utf8 = root8.constData();
    o.label_utf8 = label8.constData();
    o.header_lines = header.data();
    o.header_line_count = static_cast<uint32_t>(header.size());
    o.flags = flags;
    ecuio_recorder_t* recorder = nullptr;
    ecuio_status_t rc = ecuio_recorder_open(&o, &recorder);
    if (rc != ECUIO_OK) {
        return fail(QString::fromUtf8(ecuio_last_error_detail()));
    }
    // The signal file decoding the session goes in as signals.json
    // (ADR-0051 section 4): a vehicle's, its hash the store copy's name,
    // or a plugin variant's files merged, with the plugin's line
    // (ADR-0052 section 4), as havne-cli stream --plugin --record does
    const ecuio_signal_file_t* file =
        fileOfSet(ecuio_session_signal_set(session));
    if (file != nullptr) {
        rc = ecuio_recorder_signal_file(recorder, file);
    }
    if (rc == ECUIO_OK) {
        rc = ecuio_session_record(session, recorder);
    }
    if (rc != ECUIO_OK) {
        // Core's words first: the close below can clear them
        const QString detail = detailOr(rc);
        // The folder is this call's own, made by the open above; a
        // session never attached wrote it no frame, so nothing is lost
        const QString folder =
            m_failRecorderFolderForTesting ? QString() : folderOf(recorder);
        (void)ecuio_recorder_close(recorder,
                                   "the session couldn't be recorded");
        if (!folder.isEmpty() && !holdsFrames(folder)) {
            (void)QDir(folder).removeRecursively();
        }
        return fail(detail);
    }
    return recorder;
}

void SessionBridge::adoptRecorder(ecuio_recorder_t* recorder)
{
    m_recorder = recorder;
    m_recordingFolder =
        m_failRecorderFolderForTesting ? QString() : folderOf(recorder);
    if (m_markCount != 0) {
        m_markCount = 0;
        emit markCountChanged();
    }
    emit recordingStateChanged();
}

bool SessionBridge::startLog()
{
    // The activity log's line, and the Dashboard header's notice
    const auto refuse = [this](const QString& why) {
        const QString line = QStringLiteral("Log not started: %1").arg(why);
        setLogNotice(line);
        emit logMessage(line);
        return false;
    };
    if (m_logActive) {
        return refuse(QStringLiteral("a log is running already"));
    }
    if (m_session == nullptr || !m_connected.load(std::memory_order_acquire)) {
        return refuse(QStringLiteral("not connected"));
    }
    if (m_armedActive) {
        return refuse(QStringLiteral("Armed is recording this session"));
    }
    // One recorder a session: Record sessions' is attached from the start
    if (m_recorder != nullptr) {
        return refuse(QStringLiteral("Recording this session already"));
    }
    if (!m_recordReplays && m_currentUri.startsWith(QLatin1String("replay://"),
                                                    Qt::CaseInsensitive)) {
        return refuse(QStringLiteral("a replay plays a recording already"));
    }
    const std::vector<uint16_t> ids = m_signalModel->plottedIds();
    if (ids.empty()) {
        return refuse(QStringLiteral("no signal is ticked on Live"));
    }
    // The focus first, so one core refuses (it would leave nothing to
    // ask) opens no folder
    const ecuio_status_t rc = ecuio_session_set_focus_with(
        m_session, ids.data(), static_cast<uint32_t>(ids.size()),
        static_cast<uint32_t>(m_minSweepMs), 0);
    if (rc != ECUIO_OK) {
        const QString detail = QString::fromUtf8(ecuio_last_error_detail());
        return refuse(detail.isEmpty() ? failureText(rc) : detail);
    }
    QString why;
    ecuio_recorder_t* recorder = openRecorder(
        m_session, m_currentUri, QStringLiteral("Log not started"), &why);
    if (recorder == nullptr) {
        if (!applyDefaultPick(m_session, true, true)) {
            (void)ecuio_session_set_focus(m_session, nullptr, 0);
        }
        // openRecorder wrote the activity log's line
        setLogNotice(QStringLiteral("Log not started: %1").arg(why));
        return false;
    }
    setLogNotice(QString());
    m_logActive = true;
    // The log owns the focus; its end sets the pick again, with its line
    m_pickLine.clear();
    m_logClock.restart();
    m_logElapsedMs = 0;
    m_logBytes = 0;
    adoptRecorder(recorder);
    m_logTimer->start();
    emit logStateChanged();
    emit logProgressChanged();
    /* A session that asks nothing takes the minimum and changes nothing */
    ecuio_session_info_t info{};
    const bool asks = ecuio_session_get_info(m_session, &info) == ECUIO_OK &&
                      (info.flags & ECUIO_SESSION_INFO_REQUESTS) != 0;
    const QString pace = m_minSweepMs > 0 && asks
                             ? QStringLiteral(", at least %1 ms between sweeps")
                                   .arg(m_minSweepMs)
                             : QString();
    emit logMessage(QStringLiteral("Log started: %1 (%2%3)")
                        .arg(recordingFolder(),
                             signalCountText(static_cast<int>(ids.size())),
                             pace));
    return true;
}

void SessionBridge::stopLog()
{
    // A teardown under way ends the log itself, once the session is gone
    if (!m_logActive || m_disconnecting) {
        return;
    }
    emit logMessage(QStringLiteral("Log stopped: %1").arg(recordingFolder()));
    stopRecording(QStringLiteral("Stop log"), true);
}

QString SessionBridge::armedRefusal() const
{
    if (m_session == nullptr || !m_connected.load(std::memory_order_acquire)) {
        return QStringLiteral("not connected");
    }
    if (m_disconnecting) {
        return QStringLiteral("disconnecting");
    }
    if (m_armedActive) {
        return QString();
    }
    if (m_logActive) {
        return QStringLiteral("a log is running");
    }
    if (m_recorder != nullptr) {
        return QStringLiteral("Recording this session already");
    }
    // A replay is judged by its recording's own conditions, which an
    // armed copy of it would only duplicate: refused as a log of one is
    if (!m_recordReplays && m_currentUri.startsWith(QLatin1String("replay://"),
                                                    Qt::CaseInsensitive)) {
        return QStringLiteral("a replay plays a recording already");
    }
    // The words core's attach refuses with (ADR-0063 decision D4)
    if (!m_skipArmedJudgmentForTesting &&
        (m_conditionsGeneration == 0 || !m_conditionModel->anyRecords())) {
        return QStringLiteral("no condition records");
    }
    return QString();
}

void SessionBridge::judgeArmedWhy()
{
    const QString why = armedRefusal();
    if (why != m_armedWhy) {
        m_armedWhy = why;
        emit armedWhyChanged();
    }
}

bool SessionBridge::startArmed()
{
    const auto refuse = [this](const QString& why) {
        const QString line = QStringLiteral("Armed not started: %1").arg(why);
        setLogNotice(line);
        emit logMessage(line);
        return false;
    };
    if (m_armedActive) {
        return refuse(QStringLiteral("armed already"));
    }
    const QString why = armedRefusal();
    if (!why.isEmpty()) {
        return refuse(why);
    }
    QString failed;
    ecuio_recorder_t* recorder = openRecorder(
        m_session, m_currentUri, QStringLiteral("Armed not started"), &failed,
        ECUIO_RECORD_ARMED);
    if (recorder == nullptr) {
        // openRecorder wrote the activity log's line
        setLogNotice(QStringLiteral("Armed not started: %1").arg(failed));
        return false;
    }
    setLogNotice(QString());
    m_armedActive = true;
    m_armedInWindow = false;
    m_armedWindows = 0;
    m_armedFolder.clear();
    adoptRecorder(recorder);
    emit armedStateChanged();
    emit logMessage(QStringLiteral(
        "Armed: nothing is written until a condition that records fires"));
    judgeArmedWhy();
    return true;
}

void SessionBridge::stopArmed()
{
    // A teardown under way ends it itself, once the session is gone
    if (!m_armedActive || m_disconnecting) {
        return;
    }
    stopRecording(QStringLiteral("Stop armed"), true);
}

void SessionBridge::pollArmed(const ecuio_recorder_state_t& st)
{
    const bool inWindow = st.state == ECUIO_RECORDING_ACTIVE;
    const bool rose = static_cast<int>(st.windows) > m_armedWindows;
    const bool wasIn = m_armedInWindow;
    if (!rose && inWindow == wasIn) {
        return;
    }
    if (wasIn && (rose || !inWindow)) {
        emit logMessage(
            QStringLiteral("Armed window closed: %1").arg(m_armedFolder));
        m_armedInWindow = false;
    }
    if (rose) {
        // Windows that opened and closed between two polls show as one
        const int opened = static_cast<int>(st.windows) - m_armedWindows;
        m_armedWindows = static_cast<int>(st.windows);
        m_recordingFolder = folderOf(m_recorder);
        m_armedFolder = QFileInfo(m_recordingFolder).fileName();
        emit logMessage(
            opened > 1
                ? QStringLiteral("Armed windows opened (%1), newest: %2")
                      .arg(opened)
                      .arg(m_armedFolder)
                : QStringLiteral("Armed window opened: %1").arg(m_armedFolder));
        m_armedInWindow = true;
        if (!inWindow) {
            emit logMessage(
                QStringLiteral("Armed window closed: %1").arg(m_armedFolder));
            m_armedInWindow = false;
        }
    }
    emit armedStateChanged();
    emit recordingStateChanged();
}

void SessionBridge::endArmed()
{
    if (m_armedInWindow) {
        emit logMessage(
            QStringLiteral("Armed window closed: %1").arg(m_armedFolder));
    }
    // A window opened by the close's last drain isn't seen here: the
    // line says what was seen
    if (!m_armedFailure.isEmpty()) {
        emit logMessage(QStringLiteral("Armed ended: %1").arg(m_armedFailure));
        m_armedFailure.clear();
    } else {
        emit logMessage(m_armedWindows == 0
                            ? QStringLiteral("Armed ended: no window opened")
                            : QStringLiteral("Armed ended: %1 window%2")
                                  .arg(m_armedWindows)
                                  .arg(m_armedWindows == 1
                                           ? QString()
                                           : QStringLiteral("s")));
    }
    m_armedActive = false;
    m_armedInWindow = false;
    emit armedStateChanged();
}

qint64 SessionBridge::logElapsedMs() const
{
    return m_logActive ? m_logClock.elapsed() : m_logElapsedMs;
}

void SessionBridge::countLogBytes()
{
    // QDir("") would list the working directory
    if (m_recordingFolder.isEmpty()) {
        m_logBytes = 0;
        return;
    }
    qint64 bytes = 0;
    const QFileInfoList files =
        QDir(m_recordingFolder).entryInfoList(QDir::Files | QDir::Hidden);
    for (const QFileInfo& f : files) {
        bytes += f.size();
    }
    m_logBytes = bytes;
}

void SessionBridge::stopRecording(const QString& why, bool byUser,
                                  const QString& told)
{
    if (m_recorder == nullptr) {
        return;
    }
    // The frames the Log page skipped are in the recording all the same
    const int skipped = m_droppedFrames.load(std::memory_order_relaxed);
    if (skipped > 0) {
        (void)ecuio_recorder_note(
            m_recorder,
            QStringLiteral(
                "the Log page skipped %1 frames; the recording has them")
                .arg(skipped)
                .toUtf8()
                .constData());
    }
    if (m_armedActive) {
        ecuio_recorder_state_t last{};
        if (ecuio_recorder_get_state(m_recorder, &last) == ECUIO_OK) {
            pollArmed(last);
        }
    }
    ecuio_recorder_t* recorder = m_recorder;
    m_recorder = nullptr;
    (void)ecuio_recorder_close(recorder, why.toUtf8().constData());
    emit recordingStateChanged();
    if (m_armedActive) {
        endArmed();
    }
    // The recording held its set: an edit saved meanwhile is judged now
    if (!m_conditionsPending.isEmpty() && conditionsWait().isEmpty()) {
        m_conditionsPending.clear();
        applySavedConditions();
        emit conditionsChanged();
    }
    if (!m_logActive) {
        return;
    }
    m_logTimer->stop();
    m_logElapsedMs = m_logClock.elapsed();
    m_logActive = false;
    // A session still running asks its default pick again (the whole set
    // on one that ranks none); one torn down took its focus with it
    if (m_session != nullptr && !applyDefaultPick(m_session, true, true)) {
        (void)ecuio_session_set_focus(m_session, nullptr, 0);
    }
    countLogBytes(); // closed: the folder's final size
    // An end the user didn't ask for is told on the Dashboard; Stop
    // log's isn't, the viewer opens on it
    if (!byUser) {
        setLogNotice(told.isEmpty() ? QStringLiteral("Log ended: %1").arg(why)
                                    : told);
    }
    emit logStateChanged();
    emit logProgressChanged();
    emit logStopped(m_recordingFolder, byUser);
}

void SessionBridge::setLogNotice(const QString& text)
{
    if (m_logNotice == text) {
        return;
    }
    m_logNotice = text;
    emit logNoticeChanged();
}

bool SessionBridge::mark(const QString& label)
{
    if (m_recorder == nullptr) {
        return false;
    }
    // Between windows a mark would go nowhere (recording.h)
    if (m_armedActive) {
        ecuio_recorder_state_t st{};
        if (ecuio_recorder_get_state(m_recorder, &st) != ECUIO_OK ||
            st.state != ECUIO_RECORDING_ACTIVE) {
            return false;
        }
    }
    const QString text = label.trimmed().isEmpty()
                             ? QStringLiteral("Mark %1").arg(m_markCount + 1)
                             : label.trimmed();
    if (ecuio_recorder_mark(m_recorder, text.toUtf8().constData()) !=
        ECUIO_OK) {
        return false;
    }
    ++m_markCount;
    emit markCountChanged();
    // markers.csv holds it: the activity log only, not a note as well
    logUnnoted(QStringLiteral("Marked: %1").arg(text));
    return true;
}

void SessionBridge::pollRecording()
{
    ecuio_recorder_state_t st{};
    if (m_recorder == nullptr ||
        ecuio_recorder_get_state(m_recorder, &st) != ECUIO_OK) {
        return;
    }
    if (m_armedActive) {
        pollArmed(st);
    }
    if (st.state != ECUIO_RECORDING_FAILED) {
        return;
    }
    const QString reason = QString::fromUtf8(st.reason);
    const bool log = m_logActive;
    const bool armed = m_armedActive;
    if (armed) {
        m_armedFailure = reason;
    }
    stopRecording(QStringLiteral("a write failed"));
    emit logMessage(QStringLiteral("Recording stopped: %1").arg(reason));
    // The Dashboard's notice gets the recorder's own words (the disk is
    // full), not the recording's end note
    if (log) {
        setLogNotice(QStringLiteral("Log ended: %1").arg(reason));
    }
    if (armed) {
        setLogNotice(QStringLiteral("Armed ended: %1").arg(reason));
    }
}

void SessionBridge::logUnnoted(const QString& line)
{
    m_noting = false;
    emit logMessage(line);
    m_noting = true;
}

void SessionBridge::writePickLine()
{
    // A pick core took is in the recording as core's own pick line; a
    // whole set's line says why, which core's line doesn't
    if (m_pickLineCore) {
        logUnnoted(m_pickLine);
    } else {
        emit logMessage(m_pickLine);
    }
}

void SessionBridge::setLogPaused(bool paused)
{
    if (m_logPaused == paused) return;
    m_logPaused = paused;
    emit logPausedChanged();
}

void SessionBridge::clearFrames()
{
    m_frameLog->clear();
    m_currentFrameCount = 0;
    notifyFrameCounts();
    emit framesReset();
}

void SessionBridge::onFrame(const ecuio_frame_t* frame, void* user_data)
{
    SessionBridge* self = static_cast<SessionBridge*>(user_data);
    if (!frame || !self) return;
    if (!self->m_accepting.load(std::memory_order_acquire)) return;
    self->m_framesDelivered.fetch_add(1, std::memory_order_relaxed);
    if (frame->flags & ECUIO_FRAME_FLAG_OVERFLOW) {
        self->m_overflowFrames.fetch_add(1, std::memory_order_relaxed);
    }

    // Backpressure: bound the QUEUED backlog, not the lifetime count.
    if (self->m_inFlightFrames.load(std::memory_order_relaxed) >= MaxInFlightFrames) {
        self->m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        // Report drops from here too: handleFrameInternal doesn't run while
        // the backlog is saturated, which is exactly when drops happen.
        // m_dropNotifyPending limits this to one queued notification.
        bool expected = false;
        if (self->m_dropNotifyPending.compare_exchange_strong(expected, true)) {
            QMetaObject::invokeMethod(self, [self]() {
                // Clear the flag first: drops arriving meanwhile may queue a
                // fresh notification that reads a newer value; reset-after-read
                // could suppress a drop report.
                self->m_dropNotifyPending.store(false, std::memory_order_release);
                self->notifyDropped();
            }, Qt::QueuedConnection);
        }
        return;
    }

    // ecuio_frame_t holds its payload inline (data[ECUIO_FRAME_DATA_MAX]),
    // so this struct copy is a deep copy: no dangling pointer is possible.
    const ecuio_frame_t copy = *frame;
    // Stamped here, as it arrives, not when the GUI thread gets to it:
    // the frame's wait for the GUI thread starts here
    const auto arrived = HostClock::now();
    self->m_inFlightFrames.fetch_add(1, std::memory_order_relaxed);

    // Lifetime contract: with `self` as the context object, the queued
    // lambda is dropped if the bridge is destroyed before it runs. Don't
    // change it to a context-free postEvent without revisiting the
    // destruction order.
    QMetaObject::invokeMethod(self, [self, copy, arrived]() {
        self->m_inFlightFrames.fetch_sub(1, std::memory_order_relaxed);
        self->handleFrameInternal(&copy, arrived);
    }, Qt::QueuedConnection);
}

void SessionBridge::handleFrameInternal(const ecuio_frame_t* frame,
                                        HostClock::time_point arrived)
{
    if (!m_connected.load(std::memory_order_acquire)) return;

    // Defensive clamp: a misbehaving transport must never make us read past
    // the inline buffer, even if data_length exceeds ECUIO_FRAME_DATA_MAX.
    const uint16_t len = (frame->data_length <= sizeof(frame->data))
                         ? frame->data_length
                         : static_cast<uint16_t>(sizeof(frame->data));

    m_frameLatency.add(std::chrono::duration_cast<std::chrono::microseconds>(
                           HostClock::now() - arrived)
                           .count());

    // Paused: counted, but kept out of the frame log. Its values come
    // from core's worker through onValues, paused or not (ADR-0024).
    if (!m_logPaused) {
        m_frameLog->appendFrame(*frame, len);
    }
    ++m_currentFrameCount;

    // Counted here, reported on the tick (notifyFrameCounts): a signal
    // per frame would re-evaluate every binding on them per frame
    ++m_guiNotifyCount;
}

// The frame counters reach QML at the coalesce tick's rate, and at
// connect, disconnect and clear, never per frame.
void SessionBridge::notifyFrameCounts()
{
    if (m_currentFrameCount != m_reportedFrameCount) {
        m_reportedFrameCount = m_currentFrameCount;
        emit frameCountChanged();
    }
    if (m_guiNotifyCount != m_reportedNotifyCount) {
        m_reportedNotifyCount = m_guiNotifyCount;
        emit notifyCountChanged(m_guiNotifyCount);
    }
    const int overflow = m_overflowFrames.load(std::memory_order_relaxed);
    if (overflow != m_reportedOverflow) {
        if (m_reportedOverflow == 0) {
            emit logMessage(QStringLiteral(
                "The adapter's buffer overflowed: frames were lost"));
        }
        m_reportedOverflow = overflow;
        emit overflowFramesChanged();
    }
}

void SessionBridge::notifyDropped()
{
    const int dropped = m_droppedFrames.load(std::memory_order_relaxed);
    if (dropped != m_lastReportedDropped) {
        m_lastReportedDropped = dropped;
        emit droppedFramesChanged(dropped);
    }
}

void SessionBridge::forceFrameCountForTesting(int count)
{
    if (count < 0) {
        return;
    }
    m_currentFrameCount = count;
    notifyFrameCounts();   // keep the QML binding in sync
}

void SessionBridge::dispatchFrameForTesting(const ecuio_frame_t& frame)
{
    onFrame(&frame, this);
}

void SessionBridge::dispatchValuesForTesting(const ecuio_value_batch_t& batch)
{
    onValues(&batch, this);
}

void SessionBridge::dispatchConditionsForTesting(
    const ecuio_condition_event_t* events, uint32_t count)
{
    onConditions(events, count, this);
}

// ---- Decoded values from core's worker (ADR-0024) ----

void SessionBridge::ValueStaging::prepare(size_t defs, size_t samplesMax,
                                          size_t textMax)
{
    samples.clear();
    samples.reserve(samplesMax);
    text.clear();
    text.reserve(textMax);
    newest.assign(defs, 0.0);
    lowest.assign(defs, 0.0);
    highest.assign(defs, 0.0);
    newestUs.assign(defs, 0);
    lowestUs.assign(defs, 0);
    highestUs.assign(defs, 0);
    newestText.assign(defs, -1);
    has.assign(defs, 0);
    sampleLimit = samplesMax;
    textLimit = textMax;
    dropped = 0;
}

// Keeps a numeric signal's window (newest, lowest, highest, each at its
// frame's time) whatever the limits; a sample past one is counted and
// goes untimed. Grows nothing: prepare reserved the limits.
void SessionBridge::ValueStaging::stage(uint32_t def,
                                        HostClock::time_point arrived,
                                        uint64_t frameUs,
                                        const ecuio_value_t& v)
{
    // By kind: an enum's value carries its label as text too, and is
    // still a number (ADR-0051)
    const bool isText = v.kind == ECUIO_VALUE_TEXT || v.kind == ECUIO_VALUE_HEX;
    if (!isText) {
        if (!has[def] || v.value < lowest[def]) {
            lowest[def] = v.value;
            lowestUs[def] = frameUs;
        }
        if (!has[def] || v.value > highest[def]) {
            highest[def] = v.value;
            highestUs[def] = frameUs;
        }
        newest[def] = v.value;
        newestUs[def] = frameUs;
        has[def] = 1;
    }
    if (samples.size() >= sampleLimit ||
        (isText && text.size() + v.text_len > textLimit)) {
        ++dropped;
        return;
    }
    Sample s;
    s.arrived = arrived;
    s.def = def;
    s.isText = isText;
    s.value = v.value;
    if (isText) {
        s.textAt = static_cast<uint32_t>(text.size());
        s.textLen = v.text_len;
        text.insert(text.end(), v.text, v.text + v.text_len);
        newestText[def] = static_cast<int>(samples.size());
        has[def] = 1;
    }
    samples.push_back(s);
}

void SessionBridge::ValueStaging::reset()
{
    samples.clear();
    text.clear();
    std::fill(newestText.begin(), newestText.end(), -1);
    std::fill(has.begin(), has.end(), 0);
    dropped = 0;
}

void SessionBridge::prepareValueStaging(const ecuio_signal_set_t* set)
{
    const bool decodes = set != nullptr && set->defs != nullptr && set->count > 0;
    const size_t defs = decodes ? set->count : 0;
    std::lock_guard<std::mutex> lock(m_valuesMutex);
    m_valueSet = decodes ? set : nullptr;
    m_defIndexOfId.clear();
    for (size_t i = 0; i < defs; ++i) {
        m_defIndexOfId.insert(set->defs[i].id, static_cast<int>(i));
    }
    for (ValueStaging& staging : m_valueStaging) {
        staging.prepare(defs, decodes ? static_cast<size_t>(m_valueSampleLimit) : 0,
                        decodes ? static_cast<size_t>(ValueTextLimit) : 0);
    }
    m_activeStaging = 0;
    m_valuesDropped = 0;
}

void SessionBridge::onValues(const ecuio_value_batch_t* batch, void* user_data)
{
    SessionBridge* self = static_cast<SessionBridge*>(user_data);
    if (!batch || !self) return;
    if (!self->m_accepting.load(std::memory_order_acquire)) return;
    const auto arrived = HostClock::now();
    std::lock_guard<std::mutex> lock(self->m_valuesMutex);
    ValueStaging& staging = self->m_valueStaging[self->m_activeStaging];
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        const int def = self->m_defIndexOfId.value(v.signal_id, -1);
        if (def < 0 || static_cast<size_t>(def) >= staging.has.size()) {
            continue;
        }
        staging.stage(static_cast<uint32_t>(def), arrived, batch->timestamp_us, v);
    }
}

void SessionBridge::drainValues()
{
    if (m_valueSet == nullptr) return;
    ValueStaging* full = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_valuesMutex);
        full = &m_valueStaging[m_activeStaging];
        m_activeStaging ^= 1;
    }
    // The worker writes the other one now, so this one is the GUI
    // thread's until the next swap. Its values reach the model with this
    // drain, so each one's wait ends here.
    const auto drained = HostClock::now();
    m_valuesStaged += full->samples.size() + full->dropped;
    for (const ValueStaging::Sample& s : full->samples) {
        m_valueLatency.add(std::chrono::duration_cast<std::chrono::microseconds>(
                               drained - s.arrived)
                               .count());
    }
    for (size_t i = 0; i < full->has.size(); ++i) {
        if (!full->has[i]) continue;
        const uint16_t id = m_valueSet->defs[i].id;
        const int at = full->newestText[i];
        if (at >= 0) {
            const ValueStaging::Sample& s = full->samples[static_cast<size_t>(at)];
            m_stagingTexts.emplace_back(
                id, QString::fromLatin1(full->text.data() + s.textAt,
                                        static_cast<qsizetype>(s.textLen)));
        } else {
            SignalListModel::WindowValue w;
            w.id = id;
            w.last = full->newest[i];
            w.lastMs = static_cast<qint64>(full->newestUs[i] / 1000);
            w.min = full->lowest[i];
            w.minMs = static_cast<qint64>(full->lowestUs[i] / 1000);
            w.max = full->highest[i];
            w.maxMs = static_cast<qint64>(full->highestUs[i] / 1000);
            m_stagingBuffer.push_back(w);
        }
    }
    // Past the limit a value goes untimed; the Delivery line counts it
    m_valuesDropped += full->dropped;
    full->reset();
}

// The tick's half: the window's values into the model, one entry per
// signal, whatever the frame rate (ADR-0024 section 7)
void SessionBridge::flushValues()
{
    drainValues();
    if (m_stagingBuffer.empty() && m_stagingTexts.empty()) return;
    if (!m_stagingBuffer.empty()) {
        m_signalModel->updateWindow(m_stagingBuffer);
        m_stagingBuffer.clear();
    }
    if (!m_stagingTexts.empty()) {
        m_signalModel->updateTexts(m_stagingTexts);
        m_stagingTexts.clear();
    }
    ++m_signalBatchCount;
    emit signalBatchChanged();
}

// ---- Conditions (ADR-0063 sections 6 and 9) ----

namespace {

// "1 condition", "2 conditions"
QString conditionCountText(uint32_t count)
{
    return count == 1 ? QStringLiteral("1 condition")
                      : QStringLiteral("%1 conditions").arg(count);
}

} // namespace

void SessionBridge::onConditions(const ecuio_condition_event_t* events,
                                 uint32_t count, void* user_data)
{
    SessionBridge* self = static_cast<SessionBridge*>(user_data);
    if (events == nullptr || self == nullptr) {
        return;
    }
    if (!self->m_accepting.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(self->m_conditionsMutex);
    std::vector<ConditionSlot>& window =
        self->m_conditionStaging[self->m_activeConditions];
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_condition_event_t& e = events[i];
        // A set since replaced: its index means another condition now
        if (e.generation != self->m_stagingGeneration ||
            e.condition >= window.size()) {
            continue;
        }
        ConditionSlot& slot = window[e.condition];
        if ((e.flags & ECUIO_CONDITION_FIRED) != 0 && slot.firings++ == 0) {
            slot.first = e;
        }
        slot.newest = e;
        slot.has = true;
    }
}

void SessionBridge::prepareConditionStaging(uint32_t count, uint32_t generation)
{
    std::lock_guard<std::mutex> lock(m_conditionsMutex);
    for (std::vector<ConditionSlot>& window : m_conditionStaging) {
        window.assign(count, ConditionSlot{});
    }
    m_activeConditions = 0;
    m_stagingGeneration = generation;
}

bool SessionBridge::attachConditions(ecuio_session_t* session,
                                     const ecuio_condition_set_t* set,
                                     QString* why)
{
    if (m_conditionsGeneration != 0) {
        flushConditions(session);
    }
    size_t staged = 0;
    {
        std::lock_guard<std::mutex> lock(m_conditionsMutex);
        staged = m_conditionStaging[0].size();
    }
    // The slots first, under the generation core is about to begin: a
    // first judgment can come before the call returns
    const uint32_t count = ecuio_condition_set_count(set);
    const uint32_t next = m_conditionsGeneration + 1;
    prepareConditionStaging(count, next);
    const ecuio_signal_set_t* signalSet = ecuio_session_signal_set(session);
    const ecuio_signal_file_t* file = fileOfSet(signalSet);
    const ecuio_status_t rc =
        m_failAttaches > 0 ? (--m_failAttaches, ECUIO_ERR_OUT_OF_MEMORY)
                           : ecuio_session_set_conditions(session, set, file);
    if (rc != ECUIO_OK) {
        *why = detailOr(rc);
        prepareConditionStaging(static_cast<uint32_t>(staged),
                                m_conditionsGeneration);
        return false;
    }
    m_conditionsGeneration = next;
    m_conditionStates.assign(count, ecuio_condition_state_t{});
    // A condition whose key the new set keeps keeps its last tone: an edit
    // of another doesn't let it sound again within its 5 s
    std::vector<std::pair<QString, uint64_t>> toned;
    for (size_t i = 0; i < m_toneAtUs.size(); ++i) {
        if (m_toneAtUs[i] != NoTone) {
            toned.emplace_back(m_conditionModel->key(static_cast<int>(i)),
                               m_toneAtUs[i]);
        }
    }
    m_conditionModel->setConditions(set, signalSet, file);
    m_toneAtUs.assign(count, NoTone);
    for (const auto& [key, at] : toned) {
        const int row = m_conditionModel->rowOfKey(key);
        if (row >= 0 && static_cast<size_t>(row) < m_toneAtUs.size()) {
            m_toneAtUs[static_cast<size_t>(row)] = at;
        }
    }
    if (m_conditionModel->sounds()) {
        m_tone.prepare();
    }
    judgeAlertLevels();
    return true;
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

void SessionBridge::attachSessionConditions(ecuio_session_t* session,
                                            const QString& uri)
{
    m_conditionsGeneration = 0;
    m_conditionsPending.clear();
    m_conditionsFromRecording = false;
    m_conditionsLines.clear();
    prepareConditionStaging(0, 0);
    // ADR-0063 decision 12: a replay is judged by its recording's own copy
    QString unused;
    const ConditionSetPtr own = recordingConditions(uri, &unused);
    QString why;
    if (own != nullptr) {
        if (attachConditions(session, own.get(), &why)) {
            m_conditionsFromRecording = true;
            m_conditionsLines
                << QStringLiteral("Alerts: judging the recording's own %1")
                       .arg(conditionCountText(
                           ecuio_condition_set_count(own.get())));
            emit conditionsChanged();
            return;
        }
        unused = QStringLiteral("isn't judged (%1)").arg(why);
    }
    const ecuio_condition_set_t* saved = m_conditionStore.set();
    if (!unused.isEmpty()) {
        const QString judged = saved != nullptr
                                   ? QStringLiteral("the saved conditions")
                                   : QStringLiteral("none");
        m_conditionsLines << QStringLiteral(
                                 "Replay: the recording's conditions.json %1; "
                                 "judging %2")
                                 .arg(unused, judged);
    }
    if (saved == nullptr) {
        emit conditionsChanged();
        return;
    }
    if (attachConditions(session, saved, &why)) {
        m_conditionsLines << QStringLiteral("Alerts: judging %1")
                                 .arg(conditionCountText(
                                     ecuio_condition_set_count(saved)));
    } else {
        m_conditionsLines << QStringLiteral("Alerts not judged: %1").arg(why);
        showSavedConditions();
    }
    emit conditionsChanged();
}

QString SessionBridge::conditionsWait() const
{
    if (m_session == nullptr || !connected()) {
        return {};
    }
    if (m_conditionsFromRecording) {
        return QStringLiteral("applies when this replay is disconnected");
    }
    if (m_recorder != nullptr) {
        return QStringLiteral("applies when this recording ends");
    }
    return {};
}

void SessionBridge::loadSavedConditions()
{
    m_conditionsProblem.clear();
    if (!m_conditionStore.load(&m_conditionsProblem)) {
        m_conditionsProblem =
            QStringLiteral("The saved conditions weren't read: %1")
                .arg(m_conditionsProblem);
    }
    showSavedConditions();
}

void SessionBridge::reloadSavedConditionsForTesting()
{
    loadSavedConditions();
    emit conditionsChanged();
}

void SessionBridge::showSavedConditions()
{
    m_conditionModel->setConditions(m_conditionStore.set(), nullptr, nullptr);
    judgeAlertLevels();
}

void SessionBridge::applySavedConditions()
{
    if (m_session == nullptr || !connected()) {
        showSavedConditions();
        return;
    }
    QString why;
    const ecuio_condition_set_t* set = m_conditionStore.set();
    const uint32_t count = ecuio_condition_set_count(set);
    if (attachConditions(m_session, set, &why)) {
        emit logMessage(QStringLiteral("Alerts: judging %1")
                            .arg(conditionCountText(count)));
    } else {
        emit logMessage(QStringLiteral("Alerts not judged: %1").arg(why));
    }
}

void SessionBridge::endSessionConditions()
{
    m_conditionsGeneration = 0;
    prepareConditionStaging(0, 0);
    m_conditionStates.clear();
    m_toneAtUs.clear();
    m_conditionsPending.clear();
    m_conditionsFromRecording = false;
    m_conditionsLines.clear();
    showSavedConditions();
    emit conditionsChanged();
}

QString SessionBridge::conditionsText() const
{
    return QString::fromUtf8(m_conditionStore.bytes().isEmpty()
                                 ? m_conditionStore.refused()
                                 : m_conditionStore.bytes());
}

QVariantMap SessionBridge::saveConditions(const QString& text)
{
    QVariantMap result = m_conditionStore.save(text.toUtf8());
    if (!result.value(QStringLiteral("ok")).toBool()) {
        result.insert(QStringLiteral("pending"), QString());
        return result;
    }
    m_conditionsProblem.clear();
    const QString wait = conditionsWait();
    const QString saved =
        QStringLiteral("Alerts saved: %1")
            .arg(conditionCountText(static_cast<uint32_t>(
                result.value(QStringLiteral("conditions")).toInt())));
    emit logMessage(wait.isEmpty()
                        ? saved
                        : QStringLiteral("%1; the change %2").arg(saved, wait));
    m_conditionsPending = wait;
    if (wait.isEmpty()) {
        applySavedConditions();
    }
    result.insert(QStringLiteral("pending"), wait);
    emit conditionsChanged();
    return result;
}

QVariantMap SessionBridge::importConditions(const QUrl& url)
{
    // A picked file opens through openPicked: Android's content:// by the content resolver
    QFile file;
    QString words;
    QString text;
    if (!openPicked(file, pathOfUrl(url), QIODevice::ReadOnly)) {
        words = QStringLiteral("Couldn't read the file");
    } else {
        const QByteArray bytes = file.read(static_cast<qint64>(ECUIO_CONDITION_FILE_MAX_BYTES) + 1);
        if (bytes.size() > static_cast<qint64>(ECUIO_CONDITION_FILE_MAX_BYTES)) {
            words = QStringLiteral("The file is over %1 KiB, a condition file's most")
                        .arg(ECUIO_CONDITION_FILE_MAX_BYTES / 1024);
        } else {
            // Without ConvertInitialBom the decoder skips a leading BOM
            QStringDecoder utf8(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
            text = utf8(bytes);
            if (utf8.hasError()) {
                words = QStringLiteral("The file isn't UTF-8 text");
            }
        }
    }
    if (words.isEmpty()) {
        QVariantMap result = saveConditions(text);
        if (!result.value(QStringLiteral("ok")).toBool()) {
            emit logMessage(QStringLiteral("Alerts not imported: %1")
                                .arg(result.value(QStringLiteral("words")).toString()));
        }
        return result;
    }
    emit logMessage(QStringLiteral("Alerts not imported: %1").arg(words));
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("words"), words},
            {QStringLiteral("pending"), QString()}};
}

QVariantMap SessionBridge::exportConditions(const QUrl& url)
{
    // A file core refused is exported as it is, to be mended elsewhere
    const QByteArray bytes = m_conditionStore.bytes().isEmpty() ? m_conditionStore.refused()
                                                                : m_conditionStore.bytes();
    const bool removable = pickedDocumentRemovable(url);
    QString words;
    bool written = false;
    if (bytes.isEmpty()) {
        words = QStringLiteral("There are no saved alerts to export");
    } else {
        written = writeToUrl(url, bytes);
        if (!written) {
            words = QStringLiteral("Couldn't write the file");
        }
    }
    if (!written) {
        dropPickedDocument(url, removable);
    }
    emit logMessage(written ? QStringLiteral("Alerts exported to a file (%1 bytes)")
                                  .arg(bytes.size())
                            : QStringLiteral("Alerts not exported: %1").arg(words));
    return {{QStringLiteral("ok"), written}, {QStringLiteral("words"), words}};
}

int SessionBridge::alertLevelOf(int defId) const
{
    return m_alertLevels.value(QString::number(defId)).toInt();
}

void SessionBridge::judgeAlertLevels()
{
    QVariantMap levels;
    for (const auto& [id, level] : m_conditionModel->alertLevels()) {
        levels.insert(QString::number(id), level);
    }
    if (levels != m_alertLevels) {
        m_alertLevels = levels;
        emit alertLevelsChanged();
    }
}

void SessionBridge::flushConditions(ecuio_session_t* session)
{
    if (session == nullptr || m_conditionsGeneration == 0) {
        return;
    }
    std::vector<ConditionSlot>* full = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_conditionsMutex);
        full = &m_conditionStaging[m_activeConditions];
        m_activeConditions ^= 1;
    }
    // The worker writes the other one now. Each firing is a line, in the
    // order they fired; several of one condition's in a window, one line
    struct Fired {
        uint64_t at;
        int row;
    };
    std::vector<Fired> fired;
    // One tone a tick, the loudest that may sound. A 5 s starts at the ask,
    // heard or not: one the player drops (stale, a failed generator) too
    const uint64_t now = ecuio_clock_us() + m_toneTimeAddedForTesting;
    uint32_t tone = ECUIO_ALERT_NONE;
    for (size_t i = 0; i < full->size(); ++i) {
        ConditionSlot& slot = (*full)[i];
        if (!slot.has) {
            continue;
        }
        m_conditionModel->applyTerm(static_cast<int>(i), slot.newest.term);
        if (slot.firings == 0) {
            continue;
        }
        fired.push_back({slot.first.timestamp_us, static_cast<int>(i)});
        const uint32_t level = m_conditionModel->toneLevel(static_cast<int>(i));
        if (level != ECUIO_ALERT_NONE && i < m_toneAtUs.size() &&
            (m_toneAtUs[i] == NoTone || now - m_toneAtUs[i] >= ToneGapUs)) {
            m_toneAtUs[i] = now;
            tone = std::max(tone, level);
        }
    }
    if (tone != ECUIO_ALERT_NONE) {
        m_tone.play(tone);
    }
    std::stable_sort(
        fired.begin(), fired.end(),
        [](const Fired& a, const Fired& b) { return a.at < b.at; });
    // A handler of the lines may attach again: they go out once the slots
    // and the model are done with
    QStringList lines;
    for (const Fired& f : fired) {
        const ConditionSlot& slot = (*full)[static_cast<size_t>(f.row)];
        lines << (slot.firings == 1
                      ? m_conditionModel->firingText(f.row, slot.first.value,
                                                     slot.first.term)
                      : m_conditionModel->firingsText(f.row, slot.firings));
    }
    for (ConditionSlot& slot : *full) {
        slot = ConditionSlot{};
    }
    // The states core keeps: UNAVAILABLE and NOT_RECEIVED are the getter's
    // alone, and can change after the attach
    uint32_t n = 0;
    const auto size = static_cast<uint32_t>(m_conditionStates.size());
    if (size > 0 &&
        ecuio_session_get_condition_states(session, m_conditionStates.data(),
                                           size, &n) == ECUIO_OK &&
        n == size &&
        m_conditionStates[0].generation == m_conditionsGeneration) {
        m_conditionModel->applyStates(m_conditionStates.data(), n);
    }
    m_conditionModel->judgeStaleSince(
        [this](uint32_t defId) { return m_signalModel->readAtMs(defId); },
        m_signalModel->wallOffsetMs());
    judgeAlertLevels();
    for (const QString& line : lines) {
        emit logMessage(line);
    }
}

SessionBridge::DeliveryStats SessionBridge::deliveryStats() const
{
    DeliveryStats s;
    s.frames = m_framesDelivered.load(std::memory_order_relaxed);
    s.dropped = static_cast<quint64>(m_droppedFrames.load(std::memory_order_relaxed));
    const int queued = m_inFlightFrames.load(std::memory_order_relaxed);
    s.queuedAtEnd = queued > 0 ? static_cast<quint64>(queued) : 0;
    s.values = m_valuesStaged;
    s.valuesMissed = m_valuesDropped;
    s.frameToGui = m_frameLatency;
    s.valueToModel = m_valueLatency;
    return s;
}

namespace {

// A latency for the activity log: us below a millisecond, then ms with
// one decimal below 10 ms, then whole ms
QString latencyText(uint64_t us)
{
    if (us < 1000) return QStringLiteral("%1 µs").arg(us);
    if (us < 10000) {
        return QStringLiteral("%1 ms").arg(static_cast<double>(us) / 1000.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 ms").arg((us + 500) / 1000);
}

QString quantilesText(const LatencyStats& l)
{
    return QStringLiteral("median %1, p99 %2, max %3")
        .arg(latencyText(l.quantileUs(0.5)), latencyText(l.quantileUs(0.99)),
             latencyText(l.maxUs()));
}

} // namespace

// "Delivery: 7074 frames, none dropped; to the GUI thread in median
// 31 us, p99 412 us, max 1.2 ms; 28296 values, to the model in median
// 51 ms, p99 99 ms, max 101 ms". A quantile is the top of its bucket,
// at most 12.5 % above the true value (LatencyStats).
QString SessionBridge::deliveryText(const DeliveryStats& s)
{
    if (s.frames == 0) {
        return QStringLiteral("Delivery: no frames");
    }
    QString line = QStringLiteral("Delivery: %1 frames, ").arg(s.frames);
    if (s.dropped == 0) {
        line += QStringLiteral("none dropped");
    } else {
        line += QStringLiteral("%1 dropped at the in-flight cap (%2 %)")
                    .arg(s.dropped)
                    .arg(100.0 * static_cast<double>(s.dropped) / static_cast<double>(s.frames),
                         0, 'f', 1);
    }
    if (s.queuedAtEnd > 0) {
        line += QStringLiteral(", %1 still queued at the end").arg(s.queuedAtEnd);
    }
    if (s.frameToGui.count() > 0) {
        line += QStringLiteral("; to the GUI thread in ") + quantilesText(s.frameToGui);
    }
    if (s.values > 0) {
        line += QStringLiteral("; %1 values").arg(s.values);
        if (s.valueToModel.count() > 0) {
            line += QStringLiteral(", to the model in ") + quantilesText(s.valueToModel);
        }
        if (s.valuesMissed > 0) {
            line += QStringLiteral(" (%1 past the staging's limit, not timed)")
                        .arg(s.valuesMissed);
        }
    }
    return line;
}

// "Reply timing: 23 requests, 4512 answered, none refused, 2 timed out;
// latency P50 4.2 ms to 5.3 ms; intervals on the adapter's clock; ISO-TP:
// 640 messages, 40 segmented, none dropped, none unusable". Each
// request's figures are in the recording's latency.csv.
QString
SessionBridge::timingText(const ecuio_session_timing_t& t,
                          const std::vector<ecuio_request_timing_t>& rows)
{
    uint32_t asked = 0;
    quint64 answered = 0;
    quint64 refused = 0;
    quint64 timeouts = 0;
    uint32_t low = UINT32_MAX;
    uint32_t high = 0;
    for (const ecuio_request_timing_t& r : rows) {
        if (r.answered + r.refused + r.timeouts == 0) {
            continue;
        }
        ++asked;
        answered += r.answered;
        refused += r.refused;
        timeouts += r.timeouts;
        if (r.answered + r.refused > 0) {
            low = std::min(low, r.latency_p50_us);
            high = std::max(high, r.latency_p50_us);
        }
    }
    if (asked == 0) {
        return {};
    }
    const auto count = [](quint64 n) {
        return n == 0 ? QStringLiteral("none") : QString::number(n);
    };
    QString line =
        QStringLiteral(
            "Reply timing: %1 requests, %2 answered, %3 refused, %4 timed out")
            .arg(asked)
            .arg(answered)
            .arg(count(refused), count(timeouts));
    if (low != UINT32_MAX) {
        line += low == high
                    ? QStringLiteral("; latency P50 %1").arg(latencyText(low))
                    : QStringLiteral("; latency P50 %1 to %2")
                          .arg(latencyText(low), latencyText(high));
    }
    switch (t.interval_clock) {
    case ECUIO_TIMING_CLOCK_ADAPTER:
        line += QStringLiteral("; intervals on the adapter's clock");
        break;
    case ECUIO_TIMING_CLOCK_KERNEL:
        line += QStringLiteral("; intervals on the kernel's receive times");
        break;
    case ECUIO_TIMING_CLOCK_HOST:
        line += QStringLiteral("; intervals on the host's clock");
        break;
    default:
        break;
    }
    if (t.flags & ECUIO_TIMING_ISOTP) {
        line +=
            QStringLiteral(
                "; ISO-TP: %1 messages, %2 segmented, %3 dropped, %4 unusable")
                .arg(t.isotp_completed)
                .arg(t.isotp_first_frames)
                .arg(count(t.isotp_dropped), count(t.isotp_malformed));
    }
    return line;
}

void SessionBridge::pollLinkHealth()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    int32_t last_error = ECUIO_OK;
    uint32_t episodes = 0;
    (void)ecuio_session_get_error_state(m_session, &last_error, &episodes);
    if (last_error == m_linkErrorCode && episodes == m_errorEpisodes) {
        return;   // silent on unchanged state: the atomics only move
                  // on transitions, so this is not a poll storm
    }
    const int32_t prev = m_linkErrorCode;
    m_linkErrorCode = last_error;
    m_errorEpisodes = episodes;

    /* Transitions land in the activity log with no QML work, which turns
     * "connected with frozen values" into a named fault there. */
    if (prev != ECUIO_OK && last_error == ECUIO_OK) {
        logUnnoted(QStringLiteral("Link: recovered"));
    } else if (last_error == ECUIO_ERR_VEHICLE_TIMEOUT) {
        logUnnoted(QStringLiteral("Link: vehicle not responding"));
    } else if (last_error == ECUIO_ERR_REQUEST_FAILED) {
        logUnnoted(QStringLiteral("Link: requests failing (%1 episode(s))")
                       .arg(m_errorEpisodes));
    } else if (last_error == ECUIO_ERR_TRANSPORT_HARD_FAILURE) {
        logUnnoted(QStringLiteral("Link: adapter fault (%1 episode(s))")
                       .arg(m_errorEpisodes));
    } else if (last_error == ECUIO_ERR_RECORDING_CHANGED) {
        // Stopped, not finished: pollReplayEnd stays quiet. The worker
        // counted every frame it sent before it published the code.
        m_replayEndLogged = true;
        emit logMessage(QStringLiteral("Replay stopped after %1 frames: the recording "
                                       "changed after it was opened")
                            .arg(m_framesDelivered.load(std::memory_order_relaxed)));
    }
    emit linkStateChanged();
}

/* What each channel's ECU said it supports (ADR-0015 section 4), in the
 * activity log once per update: the probe's walk ending, and each
 * request it drops. Generation 0 is nothing learned yet. */
void SessionBridge::pollChannelSupport()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    bool changed = false;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_support_t s{};
        if (ecuio_session_get_channel_support(m_session, c, &s) != ECUIO_OK) {
            continue;
        }
        if (s.generation == m_supportGeneration[c]) {
            continue;
        }
        m_supportGeneration[c] = s.generation;
        m_supportReports[c] = s;
        changed = true;
        // The session's change first (ADR-0060): it's why this report moved
        SessionWritten& w = m_sessionWritten[c];
        cli::SessionSeen seen{w.state, w.nrc, w.entries, w.lost, w.written};
        if (cli::session_due(s, seen)) {
            w = {seen.state, seen.nrc, seen.entries, seen.lost, seen.written};
            const QString line = channelSessionText(s);
            if (!line.isEmpty()) {
                emit logMessage(line);
            }
        }
        emit logMessage(channelSupportText(s));
    }
    // Live says which signals their ECU refused, and with what (ADR-0051
    // section 3): the drops a negative reply made, by the defs they read
    if (changed) {
        std::vector<SignalListModel::Refusal> refused;
        for (const cli::RefusedSignal& r :
             cli::refused_signals(ecuio_session_signal_set(m_session),
                                  m_supportReports, m_channelCount)) {
            refused.push_back({r.id, refusalText(r.request[0], r.nrc),
                               refusalCode(r.request[0], r.nrc)});
        }
        m_signalModel->applyRefused(refused);
        std::vector<std::pair<uint32_t, uint32_t>> more;
        for (const cli::RefusedMore& m :
             cli::refused_unlisted(ecuio_session_signal_set(m_session),
                                   m_supportReports, m_channelCount)) {
            more.emplace_back(m.rx_id, m.count);
        }
        const QString unnamed = refusedUnnamedText(more);
        if (unnamed != m_refusedUnnamed) {
            m_refusedUnnamed = unnamed;
            emit refusedUnnamedChanged();
        }
    }
    // Live hides the rows core judges unsupported from the reports
    // (ADR-0039 section 6)
    uint32_t count = 0;
    if (changed && ecuio_session_get_unsupported_signals(
                       m_session, m_unsupportedRead.data(),
                       static_cast<uint32_t>(m_unsupportedRead.size()),
                       &count) == ECUIO_OK) {
        count = std::min<uint32_t>(
            count, static_cast<uint32_t>(m_unsupportedRead.size()));
        m_signalModel->applyUnsupported(std::vector<uint16_t>(
            m_unsupportedRead.begin(), m_unsupportedRead.begin() + count));
    }
}

// Live hides the signals core reports its frames come without, as it
// hides an unsupported PID's (ADR-0039 section 6): the fact is core's, the
// hiding the model's
void SessionBridge::pollAbsentSignals()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr ||
        m_absentRead.empty()) {
        return;
    }
    uint32_t count = 0;
    if (ecuio_session_get_absent_signals(m_session, m_absentRead.data(),
                                         static_cast<uint32_t>(m_absentRead.size()),
                                         &count) != ECUIO_OK) {
        return;
    }
    count = std::min<uint32_t>(count, static_cast<uint32_t>(m_absentRead.size()));
    if (count == m_absentIds.size() &&
        std::equal(m_absentIds.begin(), m_absentIds.end(), m_absentRead.begin())) {
        return;
    }
    m_absentIds.assign(m_absentRead.begin(), m_absentRead.begin() + count);
    m_signalModel->applyAbsent(m_absentIds);
}

void SessionBridge::pollUnaskedSignals()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr ||
        m_unaskedRead.empty()) {
        return;
    }
    uint32_t count = 0;
    if (ecuio_session_get_unasked_signals(
            m_session, m_unaskedRead.data(),
            static_cast<uint32_t>(m_unaskedRead.size()), &count) != ECUIO_OK) {
        return;
    }
    count =
        std::min<uint32_t>(count, static_cast<uint32_t>(m_unaskedRead.size()));
    if (count == m_unaskedIds.size() &&
        std::equal(m_unaskedIds.begin(), m_unaskedIds.end(),
                   m_unaskedRead.begin())) {
        return;
    }
    m_unaskedIds.assign(m_unaskedRead.begin(), m_unaskedRead.begin() + count);
    m_signalModel->applyUnasked(m_unaskedIds);
}

void SessionBridge::setPickGauges(const QVariantList& cells)
{
    if (cells == m_pickGauges) {
        return;
    }
    m_pickGauges = cells;
    m_pickDirty = true;
    emit pickGaugesChanged();
}

std::vector<uint16_t> SessionBridge::gaugeIds(const ecuio_signal_set_t* set,
                                              const ecuio_signal_file_t* file,
                                              const QVariantList& cells)
{
    std::vector<uint16_t> ids;
    if (set == nullptr || set->defs == nullptr) {
        return ids;
    }
    // Each def's refs as SignalHub keys them
    QHash<QString, uint16_t> byRef;
    const bool builtIn = SignalListModel::isBuiltIn(set);
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        const QString ref = SignalListModel::factsOf(file, builtIn, d).ref;
        if (ref.isEmpty()) {
            continue;
        }
        byRef.insert(ref, d.id);
        if (ref.startsWith(QLatin1String("key:")) &&
            d.id < ECUIO_USER_SIG_ID_BASE) {
            byRef.insert(QStringLiteral("id:%1").arg(d.id), d.id);
        }
    }
    for (const QVariant& cell : cells) {
        for (const QString& ref : cell.toStringList()) {
            const auto it = byRef.constFind(ref);
            if (it != byRef.constEnd()) {
                if (std::find(ids.begin(), ids.end(), *it) == ids.end()) {
                    ids.push_back(*it);
                }
                break;
            }
        }
    }
    return ids;
}

namespace {

// The default pick's activity line: its names, at most a dozen
QString pickText(const ecuio_signal_set_t* set,
                 const std::vector<uint16_t>& ids, int minSweepMs, bool polled)
{
    constexpr size_t kNamed = 12;
    QStringList names;
    for (uint16_t id : ids) {
        if (static_cast<size_t>(names.size()) == kNamed) {
            break;
        }
        for (uint32_t i = 0; i < set->count; ++i) {
            if (set->defs[i].id == id) {
                names << QString::fromUtf8(
                    set->defs[i].name,
                    static_cast<qsizetype>(
                        strnlen(set->defs[i].name, sizeof set->defs[i].name)));
                break;
            }
        }
    }
    QString list = names.join(QStringLiteral(", "));
    if (ids.size() > static_cast<size_t>(names.size())) {
        list += QStringLiteral(" and %1 more").arg(ids.size() - names.size());
    }
    const QString pace =
        minSweepMs > 0
            ? QStringLiteral(" (at least %1 ms between sweeps)").arg(minSweepMs)
            : QString();
    const QString count = signalCountText(static_cast<int>(ids.size()));
    return polled ? QStringLiteral("Picked %1%2; ?poll= asks only the polled "
                                   "ones among them: %3")
                        .arg(count, pace, list)
                  : QStringLiteral("Asking %1%2: %3").arg(count, pace, list);
}

} // namespace

bool SessionBridge::applyDefaultPick(ecuio_session_t* session, bool withTicks,
                                     bool again)
{
    m_pickDirty = false;
    if (session == nullptr || m_logActive) {
        return false;
    }
    ecuio_session_info_t info{};
    if (ecuio_session_get_info(session, &info) != ECUIO_OK ||
        (info.flags & ECUIO_SESSION_INFO_REQUESTS) == 0) {
        return false;
    }
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    std::vector<uint16_t> added = gaugeIds(set, fileOfSet(set), m_pickGauges);
    if (withTicks) {
        const std::vector<uint16_t> ticked = m_signalModel->tickedIds();
        added.insert(added.end(), ticked.begin(), ticked.end());
    }
    const std::vector<uint16_t> ids = cli::default_pick(set, added);
    if (m_pickSet && !again && ids == m_pickIds &&
        m_minSweepMs == m_pickMinSweepMs) {
        return true;
    }
    m_pickIds = ids;
    m_pickMinSweepMs = m_minSweepMs;
    m_pickSet = true;
    m_pickDropped = 0;
    if (session == m_session) {
        for (uint32_t c = 0; c < m_channelCount; ++c) {
            m_pickDropped += m_supportReports[c].dropped;
        }
    }
    QString line;
    bool picked = false;
    const QString whole = m_pickPolled
                              ? QStringLiteral("Asking every polled signal: ")
                              : QStringLiteral("Asking every signal: ");
    // Core takes a minimum only with a focus, so the whole set is unpaced
    const QString unpaced =
        m_minSweepMs > 0
            ? QStringLiteral("; no minimum time between sweeps applies to it")
            : QString();
    if (ids.empty()) {
        (void)ecuio_session_set_focus(session, nullptr, 0);
        line = whole + QStringLiteral("the profile ranks none") + unpaced;
    } else {
        const ecuio_status_t rc = ecuio_session_set_focus_with(
            session, ids.data(), static_cast<uint32_t>(ids.size()),
            static_cast<uint32_t>(m_minSweepMs), ECUIO_FOCUS_DEFAULT_PICK);
        if (rc == ECUIO_OK) {
            line = pickText(set, ids, m_minSweepMs, m_pickPolled);
            picked = true;
        } else {
            const QString detail = QString::fromUtf8(ecuio_last_error_detail());
            (void)ecuio_session_set_focus(session, nullptr, 0);
            line =
                whole + (detail.isEmpty() ? failureText(rc) : detail) + unpaced;
        }
    }
    if (line == m_pickLine) {
        return true;
    }
    m_pickLine = line;
    m_pickLineCore = picked;
    // Before the apply, the line waits for the session's own lines
    if (session == m_session) {
        writePickLine();
    } else {
        m_pickLinePending = true;
    }
    return true;
}

void SessionBridge::pollDefaultPick()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr ||
        m_logActive) {
        return;
    }
    if (m_pickDirty) {
        applyDefaultPick(m_session, true, false);
        return;
    }
    if (!m_pickSet || m_pickIds.empty()) {
        return;
    }
    uint64_t dropped = 0;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        dropped += m_supportReports[c].dropped;
    }
    if (dropped > m_pickDropped) {
        applyDefaultPick(m_session, true, true);
    }
}

/* One channel's "ECU 0x…:" line: what its supported-PID bitmaps list,
 * which requests left the rotation, and why, and whether its Service 01
 * requests carry several PIDs (ADR-0039 Appendix A): packing starting or
 * ending brings a new line, which must not read like the last. Static, like
 * sessionInfoText, so test_bridge pins the wording without an adapter. */
QString SessionBridge::refusalCode(uint8_t service, uint8_t nrc)
{
    return QStringLiteral("7F %1 %2").arg(hexByte(service), hexByte(nrc));
}

QString SessionBridge::refusalText(uint8_t service, uint8_t nrc)
{
    const QString reply = refusalCode(service, nrc);
    const QString name = QString::fromLatin1(cli::nrc_name(nrc));
    return name.isEmpty() ? QStringLiteral("Refused: %1").arg(reply)
                          : QStringLiteral("Refused: %1 (%2)").arg(name, reply);
}

QString SessionBridge::refusedUnnamedText(
    const std::vector<std::pair<uint32_t, uint32_t>>& more)
{
    QStringList parts;
    for (const auto& m : more) {
        parts << QStringLiteral("ECU %1 refused %2 more request%3")
                     .arg(hexId(m.first))
                     .arg(m.second)
                     .arg(m.second == 1 ? QString() : QStringLiteral("s"));
    }
    return parts.join(QStringLiteral("; "));
}

QString SessionBridge::channelSupportText(const ecuio_channel_support_t& s)
{

    QStringList parts;
    if ((s.flags & ECUIO_SUPPORT_PROBED) != 0) {
        // core's count of the data PIDs, and the last its bitmaps cover
        if ((s.flags & ECUIO_SUPPORT_COMPLETE) != 0) {
            parts << QStringLiteral("supports %1 PIDs").arg(s.pids_supported);
        } else if (s.ranges != 0) {
            parts << QStringLiteral(
                         "supports %1 PIDs up to %2, no answer past that")
                         .arg(s.pids_supported)
                         .arg(hexId(s.pids_through));
        } else {
            parts << QStringLiteral("no answer to the supported-PID query");
        }
    }

    QString requests = QStringLiteral("%1 request%2").arg(s.requests)
                           .arg(s.requests == 1 ? QString() : QStringLiteral("s"));
    if (s.dropped == 0) {
        requests += QStringLiteral(", none dropped");
    } else {
        QStringList gone;
        const uint32_t listed = std::min<uint32_t>(s.dropped, ECUIO_SUPPORT_MAX_DROPPED);
        for (uint32_t i = 0; i < listed; ++i) {
            const ecuio_dropped_request_t& d = s.dropped_list[i];
            QStringList key;
            for (uint8_t k = 0; k < d.length && k < sizeof(d.request); ++k) {
                key << hexByte(d.request[k]);
            }
            const QString nrc = QStringLiteral("7F %1 %2").arg(hexByte(d.request[0]),
                                                               hexByte(d.nrc));
            QString why;
            switch (d.reason) {
                case ECUIO_DROP_BITMAP:
                    why = QStringLiteral("not in its supported PIDs");
                    break;
                case ECUIO_DROP_NOT_SUPPORTED:
                    why = QStringLiteral("not supported: ") + nrc;
                    break;
                case ECUIO_DROP_SERVICE:
                    why = QStringLiteral("service not supported: ") + nrc;
                    break;
                case ECUIO_DROP_ACCESS:
                    // in the extended session, "another session" is wrong
                    why = (s.session == cli::kExtendedSession
                               ? QStringLiteral("needs security access, or a "
                                                "session Havne doesn't enter: ")
                               : QStringLiteral("needs security access or "
                                                "another session: ")) +
                          nrc;
                    break;
                default:
                    why = QStringLiteral("reason %1").arg(static_cast<uint>(d.reason));
                    break;
            }
            gone << key.join(QLatin1Char(' ')) + QStringLiteral(" (") + why + QLatin1Char(')');
        }
        if (s.dropped > listed) {
            gone << QStringLiteral("and %1 more").arg(s.dropped - listed);
        }
        requests += QStringLiteral(", %1 dropped: ").arg(s.dropped) +
                    gone.join(QStringLiteral(", "));
    }
    parts << requests;

    if ((s.flags & ECUIO_SUPPORT_UNPACKED) != 0) {
        parts << QStringLiteral("one PID per request, several went unanswered");
    } else if ((s.flags & ECUIO_SUPPORT_PACKED) != 0) {
        parts << QStringLiteral("several PIDs per request");
    }

    /* The DID scan (ADR-0020): how many DIDs it covers and what each
     * answer was; found ones are polled unless the table was full. */
    if (s.scan_total > 0) {
        QString scan = QStringLiteral("scan: %1 DID%2, %3 found")
                           .arg(s.scan_total)
                           .arg(s.scan_total == 1 ? QString() : QStringLiteral("s"))
                           .arg(s.scan_found);
        if (s.scan_polled < s.scan_found) {
            scan += QStringLiteral(" (%1 polled)").arg(s.scan_polled);
        }
        scan += QStringLiteral(", %1 refused").arg(s.scan_refused);
        if (s.scan_done < s.scan_total) {
            scan += QStringLiteral(", %1 unanswered").arg(s.scan_total - s.scan_done);
        }
        parts << scan;
    }
    if (s.session_asked != 0) {
        parts << (s.session == cli::kExtendedSession
                      ? QStringLiteral("extended session")
                      : QStringLiteral("default session"));
    }
    return QStringLiteral("ECU %1: ").arg(hexId(s.rx_id)) + parts.join(QStringLiteral("; "));
}

/* A module's session change (ADR-0060), one line per state the CLI's
 * session line reports; the refusal's code by name, as Live's refusals
 * are. Static, so test_bridge pins every state's words. */
QString SessionBridge::channelSessionText(const ecuio_channel_support_t& s)
{
    const QString ecu = QStringLiteral("ECU %1: ").arg(hexId(s.rx_id));
    const QString name = QString::fromLatin1(cli::nrc_name(s.session_nrc));
    switch (s.session_asked != 0 ? s.session_state
                                 : ECUIO_SESSION_STATE_NOT_ASKED) {
    case ECUIO_SESSION_STATE_ENTERED:
        if (s.session_lost == 0) {
            return ecu +
                   QStringLiteral("reading in the extended session (50 03)");
        }
        return ecu +
               QStringLiteral("left the extended session and entered it again "
                              "(%1 %2 so far)")
                   .arg(s.session_lost)
                   .arg(s.session_lost == 1 ? QStringLiteral("time")
                                            : QStringLiteral("times"));
    case ECUIO_SESSION_STATE_REFUSED: {
        const QString code = refusalCode(0x10, s.session_nrc);
        return ecu + QStringLiteral(
                         "refused the extended session (%1): reading in the "
                         "default session")
                         .arg(name.isEmpty()
                                  ? code
                                  : name + QStringLiteral(", ") + code);
    }
    case ECUIO_SESSION_STATE_UNANSWERED:
        return ecu + QStringLiteral(
                         "no answer to 10 03: reading in the default session, "
                         "asked again when the link recovers");
    case ECUIO_SESSION_STATE_ABANDONED: {
        const QString code =
            QStringLiteral("code %1").arg(hexByte(s.session_nrc));
        return ecu +
               QStringLiteral("kept leaving the extended session (%1, %2 times "
                              "within %3 s): reading in the default session")
                   .arg(name.isEmpty() ? code
                                       : name + QStringLiteral(", ") + code)
                   .arg(ECUIO_SESSION_LOSSES_MAX)
                   .arg(ECUIO_SESSION_LOSS_WINDOW_MS / 1000);
    }
    default:
        return QString();
    }
}

QString SessionBridge::sessionReturnedText(const ecuio_channel_support_t& s)
{
    if (s.session_asked == 0 || s.session_entries == 0) {
        return QString();
    }
    return QStringLiteral("ECU %1: returning to the default session (10 01)")
        .arg(hexId(s.rx_id));
}

/* What each channel's ECU said it is (ADR-0021), in the activity log
 * once the whole list has an outcome, and again when a recovery re-asks
 * the unanswered items and they settle. One line per ECU, so the VIN
 * and the calibration IDs land in the log and in a recording's
 * events.log together. The vehicle's VIN is taken as soon as it is in,
 * whatever else is still out. */
void SessionBridge::pollChannelIdentity()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_identity_t s{};
        if (ecuio_session_get_channel_identity(m_session, c, &s) != ECUIO_OK) {
            continue;
        }
        // The vehicle is the first channel's ECU's VIN (ADR-0027 section 3);
        // a recovery's re-asks settle on the same one and say nothing
        if (c == 0 && (s.flags & ECUIO_IDENTITY_VIN) != 0) {
            takeVin(s);
        }
        if ((s.flags & ECUIO_IDENTITY_COMPLETE) == 0 ||
            s.generation == m_identityGeneration[c]) {
            continue;
        }
        m_identityGeneration[c] = s.generation;
        emit logMessage(channelIdentityText(s));
    }
}

void SessionBridge::takeVin(const ecuio_channel_identity_t& s)
{
    char text[ECUIO_VIN_TEXT_LEN];
    const QString vin = ecuio_identity_vin(&s, text, sizeof text) == ECUIO_OK
                            ? QString::fromLatin1(text)
                            : QString();
    if (vin.isEmpty() || vin == m_vin) {
        return;
    }
    m_vin = vin;
    // What the VIN says of the car (ADR-0043)
    emit logMessage(QStringLiteral("VIN: ") +
                    vinSummary(vin, QDate::currentDate().year() + 1));
    emit vehicleIdentified();
    // the inspection's model year (ADR-0045)
    emit diagnosticsChanged();
}

/* Each channel's diagnostics report (ADR-0037), logged once per read
 * that completes: at connect, then once per Read again, which moves the
 * generation on. One line per ECU, so a recording's events.log keeps
 * the codes the car had when it was made. A clear (ADR-0038) moves the
 * generation with the codes unchanged, while it runs and when it ends
 * unsent or refused: that logs its own line, once per clear asked, and
 * no diagnostics line; after a 44 or no answer the read that follows
 * logs one. */
void SessionBridge::pollChannelDiagnostics()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return;
    }
    bool changed = m_diagnosticsShownCount != m_channelCount;
    bool clear_ended = false;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_diagnostics_t s{};
        if (ecuio_session_get_channel_diagnostics(m_session, c, &s) !=
            ECUIO_OK) {
            continue;
        }
        // The struct has no padding (core.h pins it); its doubles compare
        // by their bits, which is what a change test wants
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
        if (memcmp(&s, &m_diagnosticsShown[c], sizeof(s)) != 0) {
            m_diagnosticsShown[c] = s;
            changed = true;
        }
        bool clear_only = (s.flags & ECUIO_DIAG_CLEARING) != 0;
        if (m_clearAsked[c] && !clear_only && s.clear != ECUIO_CLEAR_NONE) {
            m_clearAsked[c] = false;
            clear_ended = true;
            emit logMessage(channelClearText(s, m_signalModel->unitSystem()));
            if (s.clear == ECUIO_CLEAR_MOVING ||
                s.clear == ECUIO_CLEAR_NO_VEHICLE_SPEED) {
                m_clearWaiting[c] = true;
                m_clearWaitGeneration[c] = s.generation;
                m_clearWaitKmh[c] = (s.flags & ECUIO_DIAG_CLEAR_KMH) != 0
                                        ? s.clear_kmh
                                        : UINT32_MAX;
            }
            clear_only = s.clear != ECUIO_CLEAR_CLEARED &&
                         s.clear != ECUIO_CLEAR_UNANSWERED;
        }
        if ((s.flags & ECUIO_DIAG_COMPLETE) == 0 ||
            s.generation == m_diagnosticsGeneration[c]) {
            continue;
        }
        m_diagnosticsGeneration[c] = s.generation;
        if (!clear_only) {
            emit logMessage(channelDiagnosticsText(s));
        }
    }
    if (changed) {
        m_diagnosticsShownCount = m_channelCount;
        rebuildDiagnostics();
    }
    if (clear_ended) {
        emit clearMovingChanged();
    }
}

void SessionBridge::rebuildDiagnostics()
{
    m_diagnostics.clear();
    for (uint32_t c = 0; c < m_diagnosticsShownCount; ++c) {
        m_diagnostics << diagnosticsEntry(m_diagnosticsShown[c],
                                          m_signalModel->unitSystem());
    }
    emit diagnosticsChanged();
}

void SessionBridge::readDiagnostics()
{
    if (m_connected.load(std::memory_order_acquire) && m_session != nullptr) {
        (void)ecuio_session_read_diagnostics(m_session);
    }
}

bool SessionBridge::clearTarget(const ecuio_channel_diagnostics_t& report)
{
    return (report.flags & ECUIO_DIAG_COMPLETE) != 0 &&
           (report.flags & ECUIO_DIAG_CLEARING) == 0;
}

bool SessionBridge::nothingStored(const ecuio_channel_diagnostics_t& r)
{
    const auto empty = [&r](uint32_t list) {
        return r.lists[list].outcome == ECUIO_DIAG_ANSWERED &&
               r.lists[list].reported == 0 && r.lists[list].held == 0;
    };
    return (r.flags & ECUIO_DIAG_READINESS) != 0 &&
           (r.flags & ECUIO_DIAG_MIL_ON) == 0 && r.dtc_count == 0 &&
           empty(ECUIO_DIAG_STORED) && empty(ECUIO_DIAG_PENDING) &&
           (r.flags & ECUIO_DIAG_FREEZE_FRAME) == 0;
}

bool SessionBridge::clearFindsNothing() const
{
    bool any = false;
    for (uint32_t c = 0; c < m_diagnosticsShownCount; ++c) {
        if (!clearTarget(m_diagnosticsShown[c])) {
            continue;
        }
        if (!nothingStored(m_diagnosticsShown[c])) {
            return false;
        }
        any = true;
    }
    return any;
}

QVariantMap SessionBridge::inspection() const
{
    uint32_t year = 0;
    ecuio_vin_info_t v{};
    const QByteArray vin = m_vin.toLatin1();
    if (!m_vin.isEmpty() &&
        ecuio_vin_decode(vin.constData(),
                         static_cast<uint32_t>(QDate::currentDate().year() + 1),
                         &v) == ECUIO_OK) {
        year = v.model_year;
    }
    return inspectionEntry(m_diagnosticsShown, m_diagnosticsShownCount, year);
}

QVariantMap
SessionBridge::inspectionEntry(const ecuio_channel_diagnostics_t* reports,
                               uint32_t count, uint32_t modelYear)
{
    QVariantMap m;
    ecuio_inspection_t i{};
    if (ecuio_diag_inspect(reports, count, modelYear, &i) != ECUIO_OK) {
        i.verdict = ECUIO_INSPECTION_UNKNOWN;
    }
    QStringList names;
    for (uint8_t id = 0; id < 16; ++id) {
        if (((i.incomplete_ids >> id) & 1u) != 0) {
            names << QString::fromLatin1(
                ecuio_monitor_short_name(id, i.diag_flags));
        }
    }
    const QString incomplete =
        (i.incomplete == 1
             ? QStringLiteral("1 monitor incomplete")
             : QStringLiteral("%1 monitors incomplete").arg(i.incomplete)) +
        QStringLiteral(" (%1)").arg(names.join(QStringLiteral(", ")));
    QString verdict;
    QString text;
    switch (i.verdict) {
    case ECUIO_INSPECTION_READY:
        verdict = QStringLiteral("ready");
        if (i.incomplete == 0) {
            text =
                QStringLiteral("Ready for an emissions inspection: every "
                               "monitor complete, the check-engine light off.");
        } else if (modelYear == 0) {
            text =
                QStringLiteral("Ready for an emissions inspection: %1, which "
                               "a car of any model year may have.")
                    .arg(incomplete);
        } else {
            text =
                QStringLiteral("Ready for an emissions inspection: %1, which "
                               "a model year %2 car may have.")
                    .arg(incomplete)
                    .arg(modelYear);
        }
        break;
    case ECUIO_INSPECTION_NOT_READY:
        verdict = QStringLiteral("not_ready");
        text =
            (modelYear == 0
                 ? QStringLiteral("Not ready for an emissions inspection: %1, "
                                  "and no model year may have more than 2.")
                       .arg(incomplete)
                 : QStringLiteral(
                       "Not ready for an emissions inspection: %1, "
                       "and a model year %2 car may have %3 at most.")
                       .arg(incomplete)
                       .arg(modelYear)
                       .arg(i.allowed)) +
            QStringLiteral(" A station would send it away to be driven until "
                           "they complete.");
        break;
    case ECUIO_INSPECTION_MIL_ON:
        verdict = QStringLiteral("mil_on");
        text = QStringLiteral("Would fail an emissions inspection: an ECU "
                              "commands the check-engine light on.");
        break;
    case ECUIO_INSPECTION_YEAR_NEEDED:
        verdict = QStringLiteral("year_needed");
        text =
            QStringLiteral("%1: ready for an emissions inspection if the car "
                           "is model year 1996-2000, not if it's 2001 or "
                           "newer, and no VIN gave its year.")
                .arg(incomplete);
        break;
    case ECUIO_INSPECTION_NOT_COVERED:
        verdict = QStringLiteral("not_covered");
        text = QStringLiteral("Model year %1: the federal inspection rule "
                              "covers 1996 and newer.")
                   .arg(modelYear);
        break;
    default:
        break;
    }
    m[QStringLiteral("verdict")] = verdict;
    m[QStringLiteral("text")] = text;
    m[QStringLiteral("rule")] = QStringLiteral(
        "By the federal rule, 40 CFR 85.2222, from the monitors and "
        "the VIN's model year. A state's program sets its own, "
        "and some are stricter.");
    return m;
}

QStringList SessionBridge::clearTargets() const
{
    QStringList ecus;
    for (uint32_t c = 0; c < m_diagnosticsShownCount; ++c) {
        if (clearTarget(m_diagnosticsShown[c])) {
            ecus << hexId(m_diagnosticsShown[c].rx_id);
        }
    }
    return ecus;
}

/* ADR-0038: what a clear erases stays on record, so the reports go to a
 * file of their own before anything is sent, whether or not the session
 * records; each ECU is then cleared at the generation that file holds,
 * so a report that moved on since is refused unsent */
QString SessionBridge::clearDiagnostics()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr) {
        return {};
    }
    ecuio_channel_diagnostics_t reports[ECUIO_SESSION_INFO_MAX_CHANNELS];
    uint32_t channels[ECUIO_SESSION_INFO_MAX_CHANNELS];
    uint32_t n = 0;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        ecuio_channel_diagnostics_t s{};
        if (ecuio_session_get_channel_diagnostics(m_session, c, &s) ==
                ECUIO_OK &&
            clearTarget(s)) {
            reports[n] = s;
            channels[n] = c;
            ++n;
        }
    }
    if (n == 0) {
        emit logMessage(
            QStringLiteral("Clear not sent: no ECU has a completed read"));
        return {};
    }

    const auto refuse = [this](const QString& why) {
        emit logMessage(
            QStringLiteral("Clear not sent: the report couldn't be saved (%1)")
                .arg(why));
        return QString();
    };
    const QString root = recordingsRoot();
    if (root.isEmpty()) {
        return refuse(QStringLiteral("no Documents folder"));
    }
    // The picked vehicle names an adapter's file only, as a recording's
    // folder (ADR-0027 section 1)
    const bool adapter = m_currentUri.startsWith(QStringLiteral("j2534://"),
                                                 Qt::CaseInsensitive) ||
                         m_currentUri.startsWith(QStringLiteral("elm327://"),
                                                 Qt::CaseInsensitive);
    const QString label = adapter ? folderLabel(m_vehicleLabel) : QString();
    const QString base =
        QStringLiteral("diagnostics-") + QDateTime::currentDateTime().toString(
                                             QStringLiteral("yyyyMMdd-HHmmss"));
    QString path;
    for (int k = 1; k <= 9 && path.isEmpty(); ++k) {
        const QString name =
            base + (k > 1 ? QStringLiteral("-%1").arg(k) : QString()) +
            (label.isEmpty() ? QString() : QLatin1Char(' ') + label) +
            QStringLiteral(".jsonl");
        if (!QFileInfo::exists(root + QLatin1Char('/') + name)) {
            path = root + QLatin1Char('/') + name;
        }
    }
    if (path.isEmpty()) {
        return refuse(
            QStringLiteral("nine files already have this second's name"));
    }
    /* No folder, no file: either way nothing is sent */
    const bool folder = QDir().mkpath(root);
    QSaveFile file(path);
    const QByteArray text = diagnosticsFileText(reports, n);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() ||
        !file.commit()) {
        return refuse(folder ? file.errorString()
                             : QStringLiteral("can't create %1")
                                   .arg(documentsRelative(root)));
    }
    const QString shown = documentsRelative(path);
    emit logMessage(
        QStringLiteral("Diagnostics saved before the clear: %1").arg(shown));
    m_clearSavedTo = shown;
    for (bool& waiting : m_clearWaiting) {
        waiting = false;
    }
    emit clearMovingChanged();

    for (uint32_t i = 0; i < n; ++i) {
        const ecuio_status_t rc = ecuio_session_clear_diagnostics_with(
            m_session, channels[i], reports[i].generation,
            ECUIO_CLEAR_ALLOW_RUNNING);
        if (rc == ECUIO_OK) {
            m_clearAsked[channels[i]] = true;
        } else {
            emit logMessage(
                QStringLiteral("ECU %1 clear: not sent: %2")
                    .arg(hexId(reports[i].rx_id),
                         rc == ECUIO_ERR_REPORT_CHANGED
                             ? QStringLiteral("the report changed")
                             : QString::fromUtf8(ecuio_status_string(rc))));
        }
    }
    return shown;
}

QStringList SessionBridge::clearMoving() const
{
    QStringList ecus;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        if (m_clearAsked[c]) {
            return {}; /* asked once all have ended */
        }
    }
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        if (m_clearWaiting[c]) {
            ecus << hexId(m_diagnosticsShown[c].rx_id);
        }
    }
    return ecus;
}

QString SessionBridge::clearMovingSpeed() const
{
    uint32_t kmh = UINT32_MAX;
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        if (m_clearWaiting[c] && m_clearWaitKmh[c] != UINT32_MAX &&
            (kmh == UINT32_MAX || m_clearWaitKmh[c] > kmh)) {
            kmh = m_clearWaitKmh[c];
        }
    }
    if (kmh == UINT32_MAX) {
        return {};
    }
    return QStringLiteral("%1 %2")
        .arg(qRound(displayValue(QStringLiteral("km/h"), kmh)))
        .arg(displayUnit(QStringLiteral("km/h")));
}

void SessionBridge::clearDiagnosticsMoving()
{
    if (!m_connected.load(std::memory_order_acquire) || m_session == nullptr ||
        clearMoving().isEmpty()) {
        return;
    }
    emit logMessage(QStringLiteral("Clearing while moving; the report was "
                                   "saved before the clear: %1")
                        .arg(m_clearSavedTo));
    for (uint32_t c = 0; c < m_channelCount; ++c) {
        if (!m_clearWaiting[c]) {
            continue;
        }
        m_clearWaiting[c] = false;
        const ecuio_status_t rc = ecuio_session_clear_diagnostics_with(
            m_session, c, m_clearWaitGeneration[c],
            ECUIO_CLEAR_ALLOW_RUNNING | ECUIO_CLEAR_ALLOW_MOVING);
        if (rc == ECUIO_OK) {
            m_clearAsked[c] = true;
        } else {
            emit logMessage(
                QStringLiteral("ECU %1 clear: not sent: %2")
                    .arg(hexId(m_diagnosticsShown[c].rx_id),
                         rc == ECUIO_ERR_REPORT_CHANGED
                             ? QStringLiteral("the report changed")
                             : QString::fromUtf8(ecuio_status_string(rc))));
        }
    }
    emit clearMovingChanged();
}

void SessionBridge::cancelClearMoving()
{
    const QStringList ecus = clearMoving();
    if (ecus.isEmpty()) {
        return;
    }
    for (bool& waiting : m_clearWaiting) {
        waiting = false;
    }
    emit logMessage(QStringLiteral("Clear while moving not sent: %1")
                        .arg(ecus.join(QStringLiteral(", "))));
    emit clearMovingChanged();
}

QByteArray
SessionBridge::diagnosticsFileText(const ecuio_channel_diagnostics_t* reports,
                                   uint32_t count)
{
    QByteArray out;
    char buf[8192];
    cli::Line start(buf, sizeof buf);
    start.raw("{\"type\":\"start\"");
    start.key("schema");
    start.u64(1);
    start.key("core");
    start.string(ecuio_version_string(), SIZE_MAX);
    start.ch('}');
    if (start.finish()) {
        out.append(start.data(), static_cast<qsizetype>(start.size()));
    }
    for (uint32_t i = 0; i < count; ++i) {
        cli::Line l(buf, sizeof buf);
        cli::diagnostics_line(l, reports[i]);
        if (l.finish()) {
            out.append(l.data(), static_cast<qsizetype>(l.size()));
        }
    }
    return out;
}

QVariantMap
SessionBridge::diagnosticsEntry(const ecuio_channel_diagnostics_t& s,
                                uint32_t units)
{
    const auto outcome = [](uint8_t o) {
        switch (o) {
        case ECUIO_DIAG_ANSWERED:
            return QStringLiteral("answered");
        case ECUIO_DIAG_REFUSED:
            return QStringLiteral("refused");
        case ECUIO_DIAG_UNANSWERED:
            return QStringLiteral("unanswered");
        case ECUIO_DIAG_UNSUPPORTED:
            return QStringLiteral("unsupported");
        default:
            return QStringLiteral("unasked");
        }
    };
    const auto nrc = [](uint8_t o, uint8_t service, uint8_t code) {
        return o == ECUIO_DIAG_REFUSED
                   ? QStringLiteral("7F %1 %2")
                         .arg(hexByte(service), hexByte(code))
                   : QString();
    };
    const auto dtc = [](uint16_t code) {
        char text[8] = {};
        (void)ecuio_dtc_format(code, text, sizeof(text));
        return QString::fromLatin1(text);
    };

    QVariantMap m;
    m[QStringLiteral("ecu")] = hexId(s.rx_id);
    m[QStringLiteral("asked")] = (s.flags & ECUIO_DIAG_ASKED) != 0;
    m[QStringLiteral("complete")] = (s.flags & ECUIO_DIAG_COMPLETE) != 0;
    m[QStringLiteral("readiness")] = outcome(s.readiness);
    m[QStringLiteral("readinessNrc")] = nrc(s.readiness, 0x01, s.readiness_nrc);
    m[QStringLiteral("mil")] = (s.flags & ECUIO_DIAG_MIL_ON) != 0;
    m[QStringLiteral("dtcCount")] = static_cast<int>(s.dtc_count);
    m[QStringLiteral("compression")] = (s.flags & ECUIO_DIAG_COMPRESSION) != 0;
    m[QStringLiteral("cycle")] = outcome(s.cycle);

    static const char* const kKeys[ECUIO_DIAG_LISTS] = {"stored", "pending",
                                                        "permanent"};
    static const uint8_t kServices[ECUIO_DIAG_LISTS] = {0x03, 0x07, 0x0A};
    QVariantList lists;
    for (uint32_t l = 0; l < ECUIO_DIAG_LISTS; ++l) {
        const ecuio_dtc_list_t& list = s.lists[l];
        const uint8_t held = std::min<uint8_t>(list.held, ECUIO_DIAG_MAX_DTCS);
        QStringList codes;
        QStringList titles; // each code's (ADR-0044), "" with none
        for (uint8_t i = 0; i < held; ++i) {
            codes << dtc(list.codes[i]);
            titles << QString::fromLatin1(ecuio_dtc_title(list.codes[i]));
        }
        QVariantMap e;
        e[QStringLiteral("key")] = QString::fromLatin1(kKeys[l]);
        e[QStringLiteral("outcome")] = outcome(list.outcome);
        e[QStringLiteral("nrc")] = nrc(list.outcome, kServices[l], list.nrc);
        e[QStringLiteral("codes")] = codes;
        e[QStringLiteral("titles")] = titles;
        e[QStringLiteral("more")] =
            list.reported > held ? list.reported - held : 0;
        lists << e;
    }
    m[QStringLiteral("lists")] = lists;

    QVariantList monitors;
    const uint8_t n =
        std::min<uint8_t>(s.monitor_count, ECUIO_DIAG_MAX_MONITORS);
    for (uint8_t i = 0; i < n; ++i) {
        QVariantMap e;
        e[QStringLiteral("id")] = static_cast<int>(s.monitors[i].id);
        e[QStringLiteral("name")] =
            QString::fromLatin1(ecuio_monitor_name(s.monitors[i].id, s.flags));
        e[QStringLiteral("complete")] =
            (s.monitors[i].flags & ECUIO_MONITOR_COMPLETE) != 0;
        e[QStringLiteral("cycleEnabled")] =
            (s.monitors[i].flags & ECUIO_MONITOR_CYCLE_ENABLED) != 0;
        e[QStringLiteral("cycleComplete")] =
            (s.monitors[i].flags & ECUIO_MONITOR_CYCLE_COMPLETE) != 0;
        monitors << e;
    }
    m[QStringLiteral("monitors")] = monitors;

    /* Each freeze-frame value by the obd2 def that decoded it (ADR-0037
     * section 4), in the user's units, as Live shows a live one */
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    QVariantList values;
    const uint8_t fn = std::min<uint8_t>(s.freeze_count, ECUIO_DIAG_MAX_FREEZE);
    for (uint8_t i = 0; i < fn; ++i) {
        const ecuio_freeze_value_t& f = s.freeze_values[i];
        for (uint32_t k = 0; k < obd2->count; ++k) {
            const ecuio_signal_def_t& def = obd2->defs[k];
            if (def.id != f.def_id) {
                continue;
            }
            ecuio_display_value_t out{};
            if (ecuio_display_convert(def.unit, def.decimal_places, f.value,
                                      units, &out) != ECUIO_DISPLAY_OK) {
                out = {};
                out.value = f.value;
                qstrncpy(out.unit, def.unit, sizeof(out.unit));
                out.decimal_places = def.decimal_places;
            }
            QVariantMap e;
            e[QStringLiteral("name")] = QString::fromUtf8(def.name);
            e[QStringLiteral("value")] = out.value;
            e[QStringLiteral("unit")] = QString::fromUtf8(out.unit);
            e[QStringLiteral("decimals")] =
                static_cast<int>(out.decimal_places);
            values << e;
            break;
        }
    }
    QVariantMap freeze;
    freeze[QStringLiteral("outcome")] = outcome(s.freeze);
    freeze[QStringLiteral("nrc")] = nrc(s.freeze, 0x02, s.freeze_nrc);
    freeze[QStringLiteral("stored")] = (s.flags & ECUIO_DIAG_FREEZE_FRAME) != 0;
    freeze[QStringLiteral("code")] = (s.flags & ECUIO_DIAG_FREEZE_FRAME) != 0
                                         ? dtc(s.freeze_dtc)
                                         : QString();
    freeze[QStringLiteral("title")] =
        (s.flags & ECUIO_DIAG_FREEZE_FRAME) != 0
            ? QString::fromLatin1(ecuio_dtc_title(s.freeze_dtc))
            : QString();
    freeze[QStringLiteral("values")] = values;
    m[QStringLiteral("freeze")] = freeze;

    /* The last clear (ADR-0038) and whether one runs */
    m[QStringLiteral("clearing")] = (s.flags & ECUIO_DIAG_CLEARING) != 0;
    QString clear;
    QString clearNrc;
    switch (s.clear) {
    case ECUIO_CLEAR_CLEARED:
        clear = QStringLiteral("cleared");
        break;
    case ECUIO_CLEAR_REFUSED:
        clear = QStringLiteral("refused");
        clearNrc = QStringLiteral("7F 04 %1").arg(hexByte(s.clear_nrc));
        break;
    case ECUIO_CLEAR_UNANSWERED:
        clear = QStringLiteral("unanswered");
        break;
    case ECUIO_CLEAR_ENGINE_RUNNING:
        clear = QStringLiteral("engineRunning");
        break;
    case ECUIO_CLEAR_NO_ENGINE_SPEED:
        clear = QStringLiteral("noEngineSpeed");
        if (s.clear_nrc != 0) {
            clearNrc = QStringLiteral("7F 01 %1").arg(hexByte(s.clear_nrc));
        }
        break;
    case ECUIO_CLEAR_REPORT_CHANGED:
        clear = QStringLiteral("reportChanged");
        break;
    case ECUIO_CLEAR_MOVING:
        clear = QStringLiteral("moving");
        break;
    case ECUIO_CLEAR_NO_VEHICLE_SPEED:
        clear = QStringLiteral("noVehicleSpeed");
        if (s.clear_nrc != 0) {
            clearNrc = QStringLiteral("7F 01 %1").arg(hexByte(s.clear_nrc));
        }
        break;
    default:
        clear = QStringLiteral("none");
        break;
    }
    m[QStringLiteral("clear")] = clear;
    m[QStringLiteral("clearNrc")] = clearNrc;
    m[QStringLiteral("clearRpm")] = static_cast<int>(s.clear_rpm);
    /* What the clear read: the engine turning (a 7F 04 22 then means it
     * won't clear running), and the vehicle speed in the unit system */
    m[QStringLiteral("clearRunning")] =
        (s.flags & ECUIO_DIAG_CLEAR_RPM) != 0 && s.clear_rpm > 0;
    m[QStringLiteral("clearSpeed")] = (s.flags & ECUIO_DIAG_CLEAR_KMH) != 0
                                          ? clearSpeedText(s.clear_kmh, units)
                                          : QString();
    return m;
}

QString SessionBridge::vinKey(const QString& salt) const
{
    if (m_vin.isEmpty()) {
        return {};
    }
    return QString::fromLatin1(
        QCryptographicHash::hash((salt + m_vin).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
}

QString SessionBridge::vinSummary(const QString& vin, int latestModelYear)
{
    const QByteArray text = vin.toLatin1();
    ecuio_vin_info_t v{};
    if (vin.size() != ECUIO_VIN_LEN ||
        ecuio_vin_decode(text.constData(),
                         static_cast<uint32_t>(std::max(latestModelYear, 0)),
                         &v) != ECUIO_OK) {
        return QStringLiteral(
            "not a VIN: 49 CFR 565 takes 17 digits and capital "
            "letters, never I, O or Q");
    }
    QStringList parts;
    if (v.model_year == 0) {
        parts << QStringLiteral("no model year (position 10 is %1)")
                     .arg(vin.at(9));
    } else if ((v.flags & ECUIO_VIN_YEAR_EARLIER) != 0) {
        parts << QStringLiteral("model year %1 (not %2, which is still ahead)")
                     .arg(v.model_year)
                     .arg(v.model_year + 30);
    } else {
        parts << QStringLiteral("model year %1").arg(v.model_year);
    }
    const QString make = QString::fromLatin1(ecuio_vin_make(v.wmi));
    const QString wmi = QString::fromLatin1(v.wmi);
    parts << (make.isEmpty()
                  ? QStringLiteral("manufacturer code %1 isn't in NHTSA's list")
                        .arg(wmi)
                  : QStringLiteral("%1 (manufacturer code %2)").arg(make, wmi));
    parts << ((v.flags & ECUIO_VIN_CHECK_MATCHES) != 0
                  ? QStringLiteral("check digit matches")
                  : QStringLiteral(
                        "check digit %1 isn't the %2 it works out to (North "
                        "America requires it; other markets may not)")
                        .arg(vin.at(8))
                        .arg(QChar::fromLatin1(v.check_digit)));
    return parts.join(QStringLiteral("; "));
}

QString SessionBridge::describeVin(const QString& typed) const
{
    QString vin = typed.toUpper();
    vin.remove(QLatin1Char(' '));
    if (vin.isEmpty()) {
        return {};
    }
    return vinSummary(vin, QDate::currentDate().year() + 1);
}

QString SessionBridge::vinSource() const
{
    return QString::fromLatin1(ecuio_vin_source());
}

QString SessionBridge::describeDtc(const QString& typed) const
{
    QString text = typed.toUpper();
    text.remove(QLatin1Char(' '));
    if (text.isEmpty()) {
        return {};
    }
    const QByteArray latin = text.toLatin1();
    uint16_t code = 0;
    if (text.size() != 5 ||
        ecuio_dtc_parse(latin.constData(), &code) != ECUIO_OK) {
        return QStringLiteral("not a trouble code: a letter P, C, B or U, then "
                              "four hex digits, the first 0-3");
    }
    const QString title = QString::fromLatin1(ecuio_dtc_title(code));
    return title.isEmpty()
               ? QStringLiteral("%1: no title here (Havne names SAE's generic "
                                "powertrain and network codes; a manufacturer "
                                "defines its own)")
                     .arg(text)
               : QStringLiteral("%1: %2").arg(text, title);
}

QString SessionBridge::newSalt() const
{
    QByteArray bytes(16, '\0');
    for (char& b : bytes) {
        b = static_cast<char>(QRandomGenerator::system()->bounded(256));
    }
    return QString::fromLatin1(bytes.toHex());
}

void SessionBridge::setVehicleLabel(const QString& label)
{
    if (label == m_vehicleLabel) {
        return;
    }
    m_vehicleLabel = label;
    emit vehicleLabelChanged();
}

QString SessionBridge::folderLabel(const QString& label)
{
    QString out;
    for (const QChar ch : label) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char(' ') ||
            ch == QLatin1Char('-') || ch == QLatin1Char('_') ||
            ch == QLatin1Char('.')) {
            out += ch;
        }
    }
    out = out.simplified().left(40);
    // Windows refuses a folder name ending in a dot or a space
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))) {
        out.chop(1);
    }
    return out;
}

/* One channel's "ECU 0x… identity:" line: each answered item by its
 * name and text, then the refused requests with their codes, then how
 * many went unanswered. Static, like channelSupportText, so test_bridge
 * pins the wording without an adapter. */
QString SessionBridge::channelIdentityText(const ecuio_channel_identity_t& s)
{

    QStringList parts;
    QStringList refused;
    uint32_t unanswered = 0;
    const uint32_t items = std::min<uint32_t>(s.items, ECUIO_IDENTITY_MAX_ITEMS);
    for (uint32_t i = 0; i < items; ++i) {
        const ecuio_identity_item_t& it = s.list[i];
        QStringList key;
        for (uint8_t k = 0; k < it.length && k < sizeof(it.request); ++k) {
            key << hexByte(it.request[k]);
        }
        const char* name = ecuio_identity_request_name(it.request, it.length);
        const QString label = (*name != '\0') ? QString::fromLatin1(name)
                                              : key.join(QLatin1Char(' '));
        switch (it.outcome) {
            case ECUIO_IDENTITY_ANSWERED: {
                const uint8_t n = std::min<uint8_t>(it.text_length,
                                                    ECUIO_IDENTITY_TEXT_LEN - 1);
                parts << label + QLatin1Char(' ') +
                             (n > 0 ? QString::fromLatin1(it.text, n)
                                    : QStringLiteral("(empty)"));
                break;
            }
            case ECUIO_IDENTITY_REFUSED:
                refused << key.join(QLatin1Char(' ')) +
                               QStringLiteral(" (7F %1 %2)").arg(hexByte(it.request[0]),
                                                                 hexByte(it.nrc));
                break;
            case ECUIO_IDENTITY_UNANSWERED:
                ++unanswered;
                break;
            default:
                break;
        }
    }
    if (!refused.isEmpty()) {
        parts << QStringLiteral("%1 refused: ").arg(refused.size()) +
                     refused.join(QStringLiteral(", "));
    }
    if (unanswered > 0) {
        parts << QStringLiteral("%1 unanswered").arg(unanswered);
    }
    if (parts.isEmpty()) {
        parts << QStringLiteral("nothing asked");
    }
    return QStringLiteral("ECU %1 identity: ").arg(hexId(s.rx_id)) +
           parts.join(QStringLiteral("; "));
}

/* One channel's "ECU 0x… clear:" line (ADR-0038): what became of the
 * clear its report names, a speed in the unit system the page shows */
QString SessionBridge::channelClearText(const ecuio_channel_diagnostics_t& s,
                                        uint32_t units)
{
    QString what;
    const bool rpm_read = (s.flags & ECUIO_DIAG_CLEAR_RPM) != 0;
    const bool kmh_read = (s.flags & ECUIO_DIAG_CLEAR_KMH) != 0;
    switch (s.clear) {
    case ECUIO_CLEAR_CLEARED:
        what = QStringLiteral("cleared");
        /* The speeds it was cleared at, when the user's word let it run */
        if (kmh_read) {
            what +=
                QStringLiteral(" (%1, %2)")
                    .arg(rpm_read ? QStringLiteral("%1 rpm").arg(s.clear_rpm)
                                  : QStringLiteral("engine speed unknown"))
                    .arg(clearSpeedText(s.clear_kmh, units));
        }
        break;
    case ECUIO_CLEAR_REFUSED:
        what = QStringLiteral("refused (7F 04 %1)").arg(hexByte(s.clear_nrc));
        /* J1979: an ECU that can't clear with the engine running answers
         * conditionsNotCorrect */
        if (s.clear_nrc == 0x22 && rpm_read && s.clear_rpm > 0) {
            what += QStringLiteral(": this ECU won't clear with the engine "
                                   "running: stop and turn it off, then try "
                                   "again");
        }
        break;
    case ECUIO_CLEAR_UNANSWERED:
        what = QStringLiteral("no answer");
        break;
    case ECUIO_CLEAR_ENGINE_RUNNING:
        what = QStringLiteral("not sent: engine running (%1 rpm)")
                   .arg(s.clear_rpm);
        break;
    case ECUIO_CLEAR_NO_ENGINE_SPEED:
        what = s.clear_nrc != 0
                   ? QStringLiteral("not sent: no engine speed (7F 01 %1)")
                         .arg(hexByte(s.clear_nrc))
                   : QStringLiteral("not sent: no engine speed");
        break;
    case ECUIO_CLEAR_REPORT_CHANGED:
        what = QStringLiteral("not sent: the report changed");
        break;
    case ECUIO_CLEAR_MOVING:
        what = QStringLiteral("not sent: moving (%1)")
                   .arg(clearSpeedText(s.clear_kmh, units));
        break;
    case ECUIO_CLEAR_NO_VEHICLE_SPEED:
        what = s.clear_nrc != 0
                   ? QStringLiteral("not sent: no vehicle speed (7F 01 %1)")
                         .arg(hexByte(s.clear_nrc))
                   : QStringLiteral("not sent: no vehicle speed");
        break;
    default:
        what = QStringLiteral("not asked");
        break;
    }
    return QStringLiteral("ECU %1 clear: %2").arg(hexId(s.rx_id), what);
}

/* One channel's "ECU 0x… diagnostics:" line: the MIL, each code list,
 * readiness (the incomplete monitors by their short names) and the
 * freeze frame's code, each as answered, refused with its code or
 * unanswered; an item never asked is left out. Static, like
 * channelIdentityText, so test_bridge pins the wording without an
 * adapter. */
QString
SessionBridge::channelDiagnosticsText(const ecuio_channel_diagnostics_t& s)
{
    const auto dtc = [](uint16_t code) {
        char text[8] = {};
        (void)ecuio_dtc_format(code, text, sizeof(text));
        return QString::fromLatin1(text);
    };
    const auto refused = [](uint8_t service, uint8_t nrc) {
        return QStringLiteral(" refused (7F %1 %2)")
            .arg(hexByte(service), hexByte(nrc));
    };
    QStringList parts;
    if (s.readiness == ECUIO_DIAG_ANSWERED) {
        parts << ((s.flags & ECUIO_DIAG_MIL_ON) != 0
                      ? QStringLiteral("MIL on")
                      : QStringLiteral("MIL off"));
    }

    static const char* const kNames[ECUIO_DIAG_LISTS] = {"stored", "pending",
                                                         "permanent"};
    static const uint8_t kServices[ECUIO_DIAG_LISTS] = {0x03, 0x07, 0x0A};
    for (uint32_t l = 0; l < ECUIO_DIAG_LISTS; ++l) {
        const ecuio_dtc_list_t& list = s.lists[l];
        const QString name = QString::fromLatin1(kNames[l]);
        switch (list.outcome) {
        case ECUIO_DIAG_ANSWERED: {
            QStringList codes;
            const uint8_t held =
                std::min<uint8_t>(list.held, ECUIO_DIAG_MAX_DTCS);
            for (uint8_t i = 0; i < held; ++i) {
                codes << dtc(list.codes[i]);
            }
            if (codes.isEmpty()) {
                codes << QStringLiteral("none");
            }
            QString part =
                name + QLatin1Char(' ') + codes.join(QLatin1Char(' '));
            if (list.reported > held) {
                part +=
                    QStringLiteral(" and %1 more").arg(list.reported - held);
            }
            parts << part;
            break;
        }
        case ECUIO_DIAG_REFUSED:
            parts << name + refused(kServices[l], list.nrc);
            break;
        case ECUIO_DIAG_UNANSWERED:
            parts << name + QStringLiteral(" unanswered");
            break;
        default:
            break;
        }
    }

    switch (s.readiness) {
    case ECUIO_DIAG_ANSWERED: {
        QStringList incomplete;
        const uint8_t n =
            std::min<uint8_t>(s.monitor_count, ECUIO_DIAG_MAX_MONITORS);
        for (uint8_t i = 0; i < n; ++i) {
            if ((s.monitors[i].flags & ECUIO_MONITOR_COMPLETE) == 0) {
                incomplete << QString::fromLatin1(
                    ecuio_monitor_short_name(s.monitors[i].id, s.flags));
            }
        }
        QString part = QStringLiteral("readiness %1 of %2 complete")
                           .arg(n - incomplete.size())
                           .arg(n);
        if (!incomplete.isEmpty()) {
            part += QStringLiteral(" (%1 incomplete)")
                        .arg(incomplete.join(QStringLiteral(", ")));
        }
        parts << part;
        break;
    }
    case ECUIO_DIAG_REFUSED:
        parts << QStringLiteral("readiness") + refused(0x01, s.readiness_nrc);
        break;
    case ECUIO_DIAG_UNANSWERED:
        parts << QStringLiteral("readiness unanswered");
        break;
    default:
        break;
    }

    switch (s.freeze) {
    case ECUIO_DIAG_ANSWERED:
        parts << ((s.flags & ECUIO_DIAG_FREEZE_FRAME) != 0
                      ? QStringLiteral("freeze frame for %1")
                            .arg(dtc(s.freeze_dtc))
                      : QStringLiteral("no freeze frame"));
        break;
    case ECUIO_DIAG_REFUSED:
        parts << QStringLiteral("freeze frame") + refused(0x02, s.freeze_nrc);
        break;
    case ECUIO_DIAG_UNANSWERED:
        parts << QStringLiteral("freeze frame unanswered");
        break;
    default:
        break;
    }
    if (parts.isEmpty()) {
        parts << QStringLiteral("nothing asked");
    }
    return QStringLiteral("ECU %1 diagnostics: ").arg(hexId(s.rx_id)) +
           parts.join(QStringLiteral("; "));
}

/* The URI as the header pill and the Dashboard show it. A direct-load
 * selector (one holding a separator, as resolve_uri tells a path from a
 * vendor name) is cut to the DLL's file name, j2534://.../vendor.dll, so
 * a local folder never reaches the screen or a screenshot, and a long
 * path can't elide the DLL off the pill's end. The query stays; every
 * other URI is unchanged. Settings and the activity log keep it whole. */
QString SessionBridge::uriLabelFor(const QString& uri)
{
    // A Bluetooth adapter's stream (ADR-0028): the address is personal
    // data, and the label reaches the lock screen through the session's
    // notification, so only its last two octets stay, enough to tell two
    // adapters apart
    const QString elm = QStringLiteral("elm327://");
    if (uri.startsWith(elm)) {
        const qsizetype query = uri.indexOf(QLatin1Char('?'), elm.size());
        const qsizetype end = query < 0 ? uri.size() : query;
        const QString port = uri.mid(elm.size(), end - elm.size());
        const qsizetype colon = port.indexOf(QLatin1Char(':'));
        const QString kind = port.left(colon);
        const qsizetype kept = 6; // ":AB:CD"
        if (colon > 0 &&
            (kind == QLatin1String("bt") || kind == QLatin1String("ble")) &&
            port.size() - colon - 1 > kept) {
            return elm + kind + QLatin1Char(':') + QChar(0x2026) +
                   port.right(kept) + uri.mid(end);
        }
        return uri;
    }
    const bool replay = uri.startsWith(QStringLiteral("replay://"));
    const QString scheme =
        replay ? QStringLiteral("replay://") : QStringLiteral("j2534://");
    if (!uri.startsWith(scheme)) {
        return uri;
    }
    const qsizetype query = uri.indexOf(QLatin1Char('?'), scheme.size());
    const qsizetype end = query < 0 ? uri.size() : query;
    QString selector = uri.mid(scheme.size(), end - scheme.size());
    const auto lastSep = [](const QString& s) {
        return std::max(s.lastIndexOf(QLatin1Char('/')),
                        s.lastIndexOf(QLatin1Char('\\')));
    };
    if (replay) {
        // The recording's folder name, and its frames.csv when named
        while (selector.size() > 1 &&
               lastSep(selector) == selector.size() - 1) {
            selector.chop(1);
        }
        qsizetype sep = lastSep(selector);
        if (sep > 0 &&
            selector.mid(sep + 1).compare(QStringLiteral("frames.csv"),
                                          Qt::CaseInsensitive) == 0) {
            sep = lastSep(selector.left(sep));
        }
        if (sep < 0) {
            return uri;
        }
        return scheme + QChar(0x2026) + QLatin1Char('/') +
               selector.mid(sep + 1) + uri.mid(end);
    }
    const qsizetype sep = lastSep(selector);
    if (sep < 0) {
        return uri;
    }
    return scheme + QChar(0x2026) + QLatin1Char('/') + selector.mid(sep + 1) +
           uri.mid(end);
}

/* A replay without loop=1 stops sending at the end of its file and the
 * session stays open (ADR-0023); this says so once, when every frame the
 * file holds has been read and every one its ?faults= didn't take
 * (ADR-0041) has reached onFrame, taken or dropped */
void SessionBridge::pollReplayEnd()
{
    if (m_replayEndLogged || m_session == nullptr) {
        return;
    }
    ecuio_replay_faults_t f{};
    ecuio_session_get_replay_faults(m_session, &f);
    if (f.frames < m_replayFrames ||
        m_framesDelivered.load(std::memory_order_relaxed) + f.dropped +
                f.overflowed <
            m_replayFrames) {
        return;
    }
    m_replayEndLogged = true;
    if (f.dropped + f.overflowed + f.corrupted + f.truncated + f.stalled ==
        0) {
        emit logMessage(
            QStringLiteral("Replay finished: %1 frames; the session stays open")
                .arg(m_replayFrames));
        return;
    }
    emit logMessage(
        QStringLiteral("Replay finished: %1 frames, faults injected: %2 "
                       "dropped, %3 lost to an overflow, %4 corrupted, %5 cut "
                       "short, %6 held back; the session stays open")
            .arg(m_replayFrames)
            .arg(f.dropped)
            .arg(f.overflowed)
            .arg(f.corrupted)
            .arg(f.truncated)
            .arg(f.stalled));
}

/* A sleep while connected (ADR-0033 section 6): the machine's slept count rose
 * a second or more since the last tick. Session time counts the sleep, so
 * Live and the recording show it as a gap; this says what the gap is.
 * Smaller rises are taken in without a line */
void SessionBridge::pollSleep()
{
    const uint64_t now = sleptUs();
    const uint64_t rise = now > m_sleptUs ? now - m_sleptUs : 0;
    m_sleptUs = now;
    if (rise < 1000000u) {
        return;
    }
    logUnnoted(QStringLiteral("Asleep for %1 s: nothing was read meanwhile")
                   .arg(static_cast<double>(rise) / 1e6, 0, 'f', 1));
}

/* The published state strings, in one place; QML's Theme.linkColor and
 * Theme.linkLabel key on them. Static and session-free so test_bridge
 * can pin every mapping without a transport: linkState() itself stays
 * a connected-check + delegation. */
QString SessionBridge::linkStateForError(int32_t errorCode)
{
    switch (errorCode) {
        case ECUIO_OK:                         return QStringLiteral("ok");
        case ECUIO_ERR_VEHICLE_TIMEOUT:        return QStringLiteral("quiet");
        case ECUIO_ERR_REQUEST_FAILED:         return QStringLiteral("requests");
        case ECUIO_ERR_TRANSPORT_HARD_FAILURE: return QStringLiteral("fault");
        case ECUIO_ERR_RECORDING_CHANGED:      return QStringLiteral("stopped");
        default: return QStringLiteral("degraded"); /* forward-compatible */
    }
}

/* What the session runs on, for the activity log (and so the
 * recording's events.log). Static, like linkStateForError, so
 * test_bridge pins the wording without an adapter. */
QString SessionBridge::sessionInfoText(const ecuio_session_info_t& info)
{
    const QString profile = QStringLiteral("profile ") + QString::fromUtf8(
        info.profile, static_cast<qsizetype>(
            qstrnlen(info.profile, ECUIO_SESSION_INFO_PROFILE_LEN)));
    if (info.backend == ECUIO_BACKEND_MOCK) {
        return QStringLiteral("Session: mock transport; ") + profile;
    }
    if (info.backend == ECUIO_BACKEND_REPLAY) {
        // The path is in the "Connected:" line just before
        const QString speed =
            info.replay_speed_milli == 0
                ? QStringLiteral("at full speed")
                : QStringLiteral("at %1x").arg(QString::number(
                      info.replay_speed_milli / 1000.0, 'g', 6));
        const bool loop = (info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP) != 0;
        const bool faults =
            (info.flags & ECUIO_SESSION_INFO_REPLAY_FAULTS) != 0;
        // A last row cut short by a crash or a power cut (ADR-0049 section 7)
        const QString torn =
            info.replay_rows_torn != 0
                ? QStringLiteral(", its last row cut short and left out")
                : QString();
        return QStringLiteral("Session: replay of a recording, %1 frames over "
                              "%2 s %3%4%5%6; ")
                   .arg(info.replay_frames)
                   .arg(QString::number(info.replay_span_ms / 1000.0, 'f', 1),
                        speed, loop ? QStringLiteral(", looping") : QString(),
                        faults ? QStringLiteral(", with faults injected")
                               : QString(),
                        torn) +
               profile;
    }
    if (info.backend == ECUIO_BACKEND_SOCKETCAN) {
        // The interface is in the "Connected:" line just before. A real
        // controller has a bit rate (core reads it from the interface);
        // vcan has none. A profile that requests runs through ISO-TP on
        // its channels; any other only listens
        const bool listenOnly =
            (info.flags & ECUIO_SESSION_INFO_LISTEN_ONLY) != 0;
        const QString bus =
            listenOnly ? QStringLiteral(
                             "a CAN controller in listen-only mode at %1 bit/s")
                             .arg(info.baud)
            : info.baud != 0
                ? QStringLiteral("a CAN controller at %1 bit/s").arg(info.baud)
                : QStringLiteral("a virtual CAN bus");
        const QString mode =
            info.channel_count > 0
                ? (info.channel_count == 1
                       ? QStringLiteral("requesting on 1 channel")
                       : QStringLiteral("requesting on %1 channels")
                             .arg(info.channel_count))
                : QStringLiteral("listening");
        return QStringLiteral("Session: SocketCAN, %1, %2; ").arg(bus, mode) +
               profile;
    }
    const bool elm327 = info.backend == ECUIO_BACKEND_ELM327;
    if (!elm327 && info.backend != ECUIO_BACKEND_J2534_SURROGATE) {
        return QStringLiteral("Session: backend %1").arg(info.backend);
    }
    const QString versions = versionsText(info);
    const QString versionsOrNone =
        versions.isEmpty() ? QStringLiteral("no version reading") : versions;

    QStringList parts;
    if (elm327) {
        // The port is in the "Connected:" line just before
        parts << QStringLiteral("ELM327 adapter (%1)").arg(versionsOrNone);
    } else {
        const QString arch =
            (info.dll_arch == ECUIO_DLL_ARCH_X86) ? QStringLiteral("x86 DLL")
            : (info.dll_arch == ECUIO_DLL_ARCH_X64)
                ? QStringLiteral("x64 DLL")
                : QStringLiteral("DLL of unknown architecture");
        parts << arch + QStringLiteral(", through the surrogate");
    }

    QString link = (info.protocol_id == 6) ? QStringLiteral("ISO 15765")
                 : (info.protocol_id == 5) ? QStringLiteral("CAN")
                 : QStringLiteral("protocol %1").arg(info.protocol_id);
    link += QStringLiteral(" at %1 baud").arg(info.baud);
    if (info.connect_flags != 0) {
        link += QStringLiteral(", flags ") + hexId(info.connect_flags);
    }
    parts << link;

    const uint32_t channels = std::min<uint32_t>(info.channel_count,
                                                 ECUIO_SESSION_INFO_MAX_CHANNELS);
    if (channels == 0) {
        parts << profile + QStringLiteral(", no channels (passive)");
    } else {
        QStringList pairs;
        for (uint32_t i = 0; i < channels; ++i) {
            pairs << hexId(info.rx_ids[i]) + QLatin1Char('/') + hexId(info.tx_ids[i]);
        }
        parts << QStringLiteral("%1, %2 %3 (rx/tx)")
                     .arg(profile,
                          channels == 1 ? QStringLiteral("channel")
                                        : QStringLiteral("channels"),
                          pairs.join(QStringLiteral(", ")));
    }

    parts << ((info.flags & ECUIO_SESSION_INFO_VBATT)
                  ? QStringLiteral("battery %1 V").arg(info.vbatt_mv / 1000.0, 0, 'f', 1)
                  : QStringLiteral("no battery reading"));
    if (elm327) {
        // An STN with parameter 2A on and its bit 2 clear stops listening
        // at a 7F xx 78, before the answer comes (ADR-0026)
        if ((info.flags & ECUIO_SESSION_INFO_PENDING_OFF) != 0) {
            parts << QStringLiteral(
                "the adapter is set not to wait past 7F xx 78 "
                "(STN parameter 2A), so a pending reply's "
                "answer is lost");
        }
        return QStringLiteral("Session: ") + parts.join(QStringLiteral("; "));
    }
    parts << versionsOrNone;
    return QStringLiteral("Session: ") + parts.join(QStringLiteral("; "));
}

/* The "Session:" line's versions, which Settings also keeps for each
 * device: an ELM327's AT I answer, then STI's on an STN chip; a J2534
 * adapter's firmware, DLL and API. "" without a reading, and for the
 * backends that have no adapter. */
QString SessionBridge::versionsText(const ecuio_session_info_t& info)
{
    if ((info.flags & ECUIO_SESSION_INFO_VERSIONS) == 0) {
        return {};
    }
    const auto text = [](const char* s) {
        const QString t = QString::fromUtf8(
            s,
            static_cast<qsizetype>(qstrnlen(s, ECUIO_SESSION_INFO_TEXT_LEN)));
        return t.isEmpty() ? QStringLiteral("(none)") : t;
    };
    if (info.backend == ECUIO_BACKEND_ELM327) {
        return text(info.firmware_version);
    }
    if (info.backend == ECUIO_BACKEND_J2534_SURROGATE) {
        return QStringLiteral("firmware %1, DLL %2, API %3")
            .arg(text(info.firmware_version), text(info.dll_version),
                 text(info.api_version));
    }
    return {};
}

QString SessionBridge::linkState() const
{
    if (!m_connected.load(std::memory_order_acquire)) {
        return QStringLiteral("disconnected");
    }
    return linkStateForError(m_linkErrorCode);
}
