// File/layer: app/bridge_store.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "sessionbridge.h"
#include "healthreport.h"
#include "storemigration.h"
#include "urlwrite.h"
#include "ecuio/obdb.h"

#include <QByteArray>
#include <QDate>
#include <QFile>
#include <QJsonDocument>
#include <QStringList>
#include <QtGlobal>

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
    // A picked file opens through openPicked: Android's content:// by the content resolver
    QFile file;
    QVariantMap result;
    QString kind;
    QString line;
    const QString tooLarge =
        QStringLiteral("The file is over %1 MiB, a plugin's most")
            .arg(ECUIO_PLUGIN_PACKAGE_MAX / (1024 * 1024));
    QByteArray bytes;
    if (openPicked(file, pathOfUrl(url), QIODevice::ReadOnly)) {
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
    // Before the move: only a mark an earlier start set removes anything
    QStringList lines = StoreMigration::removeOld(prefs, m_pluginStore).lines;
    lines += StoreMigration::run(prefs, m_pluginStore).lines;
    return lines;
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
