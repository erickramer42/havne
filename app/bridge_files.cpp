// File/layer: app/bridge_files.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "sessionbridge.h"
#include "bridgetext.h"
#include "healthreport.h"
#include "obdbimport.h"
#include "urlwrite.h"
#include "ecuio/obdb.h"

#include <QByteArray>
#include <QDate>
#include <QFile>
#include <QStringConverter>
#include <QStringList>
#include <QtGlobal>

using namespace bridgetext;

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

QString SessionBridge::presetLayoutText(const QString& name,
                                        const QString& form) const
{
    static const QStringList kNames = {QStringLiteral("standard"),
                                       QStringLiteral("temperatures"),
                                       QStringLiteral("showcase")};
    if (!kNames.contains(name) ||
        (form != QStringLiteral("phone") && form != QStringLiteral("desktop"))) {
        return {};
    }
    QFile file(QStringLiteral(":/qt/qml/App/layouts/%1/%2.json").arg(form, name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
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

int SessionBridge::vinModelYear(const QString& typedVin) const
{
    return ObdbImport::modelYear(
        typedVin.trimmed().isEmpty() ? m_vin : typedVin,
        QDate::currentDate().year() + 1);
}
