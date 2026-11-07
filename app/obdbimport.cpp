// File/layer: app/obdbimport.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: ObdbImportTest, BridgeTest.Obdb* (test_bridge)

#include "obdbimport.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

#include "ecuio/obdb.h"
#include "ecuio/vin.h"

namespace {

// The importer's source block (core/src/obdb_import.cpp's file_text):
// the names it gives every converted file
constexpr char kSourceName[] = "OBDb editors and contributors";
constexpr char kModified[] = "Converted to Havne's signal-file format";

int collect(const ecuio_obdb_note_t* n, void* user)
{
    auto* p = static_cast<ObdbImport::Preview*>(user);
    const QString signal = QString::fromUtf8(n->signal);
    const QString text = QString::fromUtf8(n->text);
    const auto line = static_cast<int>(n->line);
    switch (n->what) {
    case ECUIO_OBDB_NOTE_CONVERTED:
        ++p->converted;
        break;
    case ECUIO_OBDB_NOTE_CHANGED:
        p->changed << QVariantMap{{QStringLiteral("line"), line},
                                  {QStringLiteral("signal"), signal},
                                  {QStringLiteral("key"),
                                   QString::fromUtf8(n->key)},
                                  {QStringLiteral("field"),
                                   QString::fromUtf8(n->field)},
                                  {QStringLiteral("text"), text}};
        break;
    case ECUIO_OBDB_NOTE_SKIPPED:
        p->skipped << QVariantMap{{QStringLiteral("line"), line},
                                  {QStringLiteral("signal"), signal},
                                  {QStringLiteral("text"), text}};
        break;
    default: // ECUIO_OBDB_NOTE_FILE, and any kind added later
        p->file << QVariantMap{{QStringLiteral("line"), line},
                               {QStringLiteral("field"),
                                QString::fromUtf8(n->field)},
                               {QStringLiteral("text"), text}};
        break;
    }
    return 0;
}

} // namespace

ObdbImport::Preview ObdbImport::read(const QByteArray& bytes,
                                     uint32_t modelYear, const QString& name)
{
    Preview p;
    if (bytes.size() > static_cast<qsizetype>(ECUIO_OBDB_MAX_BYTES)) {
        p.words = QStringLiteral("The file is over %1 MiB, an OBDb set's most")
                      .arg(ECUIO_OBDB_MAX_BYTES / (1024 * 1024));
        return p;
    }
    const QByteArray name8 = name.toUtf8();
    ecuio_obdb_options_t options{};
    options.model_year = modelYear;
    options.name = name8.isEmpty() ? nullptr : name8.constData();
    char* json = nullptr;
    size_t length = 0;
    const ecuio_status_t rc =
        ecuio_obdb_import(bytes.constData(), static_cast<size_t>(bytes.size()),
                          &options, collect, &p, &json, &length);
    if (rc != ECUIO_OK) {
        const QString detail = QString::fromUtf8(ecuio_last_error_detail());
        p = Preview{};
        p.words = rc == ECUIO_ERR_INVALID_ARGUMENT && !detail.isEmpty()
                      ? QStringLiteral("Not an OBDb set Havne can read: %1")
                            .arg(detail)
                      : QString::fromUtf8(ecuio_status_string(rc));
        return p;
    }
    p.json = QByteArray(json, static_cast<qsizetype>(length));
    ecuio_obdb_free(json);
    p.ok = true;
    return p;
}

QVariantMap ObdbImport::report(const Preview& p, const QString& name,
                               uint32_t modelYear)
{
    if (!p.ok) {
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("words"), p.words}};
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("name"), name},
            {QStringLiteral("year"), static_cast<int>(modelYear)},
            {QStringLiteral("converted"), p.converted},
            {QStringLiteral("changed"), p.changed},
            {QStringLiteral("skipped"), p.skipped},
            {QStringLiteral("file"), p.file}};
}

bool ObdbImport::isConversion(const QByteArray& bytes)
{
    const QJsonObject source =
        QJsonDocument::fromJson(bytes).object().value(QStringLiteral("source"))
            .toObject();
    return source.value(QStringLiteral("name")).toString() ==
               QLatin1String(kSourceName) &&
           source.value(QStringLiteral("modified")).toString() ==
               QLatin1String(kModified);
}

int ObdbImport::modelYear(const QString& vin, int latestModelYear)
{
    QString text = vin.toUpper();
    text.remove(QLatin1Char(' '));
    if (text.size() != ECUIO_VIN_LEN) {
        return 0;
    }
    const QByteArray latin = text.toLatin1();
    ecuio_vin_info_t info{};
    if (ecuio_vin_decode(latin.constData(),
                         static_cast<uint32_t>(std::max(latestModelYear, 0)),
                         &info) != ECUIO_OK) {
        return 0;
    }
    return info.model_year;
}
