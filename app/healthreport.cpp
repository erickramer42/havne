// File/layer: app/healthreport.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest, RecordingViewTest (test_bridge)

#include "healthreport.h"

#include "urlwrite.h"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "ecuio/core.h"

uint32_t healthFormatFor(const QUrl& url, int filterIndex)
{
    if (filterIndex == 1 ||
        url.path().endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
        return ECUIO_HEALTH_JSON;
    }
    return ECUIO_HEALTH_TEXT;
}

uint64_t healthUtcNow()
{
    return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()) * 1000u;
}

uint32_t healthLatestModelYear()
{
    return static_cast<uint32_t>(QDate::currentDate().year() + 1);
}

namespace {

// "1 ECU", "2 ECUs", "0 ECUs"
QString counted(qulonglong n, const char* one, const char* many)
{
    return n == 1 ? QStringLiteral("1 %1").arg(QLatin1String(one))
                  : QStringLiteral("%1 %2").arg(n).arg(QLatin1String(many));
}

QVariantMap failed(const QString& words)
{
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("words"), words},
            {QStringLiteral("line"),
             QStringLiteral("Health report not saved: %1").arg(words)}};
}

} // namespace

QString healthSummary(const ecuio_health_report_t& report)
{
    return counted(report.ecus, "ECU", "ECUs") + QStringLiteral(", ") +
           counted(report.codes, "trouble code", "trouble codes") +
           QStringLiteral(", ") +
           counted(report.values, "key value", "key values");
}

QString healthSavedLine(const ecuio_health_report_t& report)
{
    return QStringLiteral("Health report saved: %1 (%2 left out)")
        .arg(healthSummary(report),
             counted(report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION],
                     "identification item", "identification items"));
}

QByteArray healthBaselineArgument(const QString& folder)
{
    return folder.isEmpty() ? QByteArray()
                            : QDir::toNativeSeparators(folder).toUtf8();
}

namespace {

int stopAsked(void* user)
{
    return static_cast<const std::atomic<bool>*>(user)->load() ? 1 : 0;
}

} // namespace

QVariantMap writeHealthReport(ecuio_health_options_t options, const QUrl& url,
                              std::atomic<bool>* stop, bool throughProvider)
{
    // A picker has already made the document it names: one that ends up
    // holding no report goes
    const bool provider = throughProvider || !url.isLocalFile();
    const auto refused = [&](const QString& words) {
        if (provider) {
            QFile::remove(url.isLocalFile() ? url.toLocalFile()
                                            : url.toString());
        }
        return failed(words);
    };
    // Core's own name in a folder of the app's: never the vehicle's label;
    // removeStaleCopies finds the folder by its name
    QTemporaryDir dir(QDir::tempPath() +
                      QStringLiteral("/havne-health-XXXXXX"));
    if (!dir.isValid()) {
        return refused(QStringLiteral("Couldn't make a temporary folder"));
    }
    const QString path = dir.filePath(options.format == ECUIO_HEALTH_JSON
                                          ? QStringLiteral("health.json")
                                          : QStringLiteral("health.txt"));
    const QByteArray out = QDir::toNativeSeparators(path).toUtf8();
    options.out_path_utf8 = out.constData();

    ecuio_health_report_t report{};
    const ecuio_status_t rc =
        stop != nullptr
            ? ecuio_health_export_with(&options, stopAsked, stop, &report)
            : ecuio_health_export(&options, &report);
    if (rc != ECUIO_OK) {
        // Core's words name no path and no value
        const QString detail = QString::fromUtf8(ecuio_last_error_detail());
        return refused(detail.isEmpty()
                           ? QStringLiteral("the report couldn't be written "
                                            "(status %1)")
                                 .arg(static_cast<int>(rc))
                           : detail);
    }
    QFile written(path);
    if (!written.open(QIODevice::ReadOnly)) {
        return refused(QStringLiteral("Couldn't read the report back"));
    }
    const QByteArray bytes = written.readAll();
    written.close();
    if (!writeToUrl(url, bytes)) {
        return refused(QStringLiteral("Couldn't write the file"));
    }

    const qulonglong leftOut =
        report.left_out[ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION];
    const bool vin = (report.flags & ECUIO_HEALTH_VIN_DECODED) != 0;
    const bool baseline = (report.flags & ECUIO_HEALTH_BASELINE_READ) != 0;
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("ecus"), report.ecus},
            {QStringLiteral("codes"), report.codes},
            {QStringLiteral("values"), report.values},
            {QStringLiteral("sinceCleared"), report.since_cleared},
            {QStringLiteral("identificationLeftOut"), leftOut},
            {QStringLiteral("vinDecoded"), vin},
            {QStringLiteral("baselineRead"), baseline},
            {QStringLiteral("summary"), healthSummary(report)},
            {QStringLiteral("line"), healthSavedLine(report)}};
}
