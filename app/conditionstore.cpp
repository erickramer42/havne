// File/layer: app/conditionstore.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "conditionstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

ConditionSetPtr loadConditionSet(const QByteArray& bytes, QString* words)
{
    ecuio_condition_set_t* set = nullptr;
    const ecuio_status_t rc = ecuio_condition_set_load(
        bytes.constData(), static_cast<size_t>(bytes.size()), &set);
    if (rc != ECUIO_OK) {
        const char* detail = ecuio_last_error_detail();
        *words = detail != nullptr && detail[0] != '\0'
                     ? QString::fromUtf8(detail)
                     : QString::fromUtf8(ecuio_status_string(rc));
        return nullptr;
    }
    return ConditionSetPtr(set);
}

namespace {

QString tooLong()
{
    return QStringLiteral("a condition file is at most %1 KiB")
        .arg(ECUIO_CONDITION_FILE_MAX_BYTES / 1024);
}

} // namespace

QString ConditionStore::path()
{
    const QString dir = qEnvironmentVariable("HAVNE_SETTINGS_DIR");
    const QString base =
        dir.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            : dir;
    return base + QStringLiteral("/conditions.json");
}

bool ConditionStore::load(QString* words)
{
    m_bytes.clear();
    m_set.reset();
    m_refused.clear();
    QFile file(path());
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        // Qt's error strings can name the path: the words never do
        *words = QStringLiteral("the saved conditions can't be read");
        return false;
    }
    const QByteArray bytes = file.read(ECUIO_CONDITION_FILE_MAX_BYTES + 1);
    if (bytes.size() > static_cast<qsizetype>(ECUIO_CONDITION_FILE_MAX_BYTES)) {
        *words = tooLong();
        return false;
    }
    ConditionSetPtr set = loadConditionSet(bytes, words);
    if (!set) {
        m_refused = bytes;
        return false;
    }
    m_bytes = bytes;
    m_set = std::move(set);
    return true;
}

QVariantMap ConditionStore::save(const QByteArray& bytes)
{
    const auto refuse = [](const QString& words) {
        return QVariantMap{{QStringLiteral("ok"), false},
                           {QStringLiteral("words"), words}};
    };
    if (bytes.size() > static_cast<qsizetype>(ECUIO_CONDITION_FILE_MAX_BYTES)) {
        return refuse(tooLong());
    }
    QString words;
    ConditionSetPtr set = loadConditionSet(bytes, &words);
    if (!set) {
        return refuse(words);
    }
    const QString where = path();
    if (!QDir().mkpath(QFileInfo(where).absolutePath())) {
        return refuse(QStringLiteral("the settings folder can't be made"));
    }
    // A temporary beside it, renamed over the old file at commit
    QSaveFile file(where);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !file.commit()) {
        return refuse(QStringLiteral("the conditions can't be saved"));
    }
    m_bytes = bytes;
    m_set = std::move(set);
    m_refused.clear();
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("conditions"),
             static_cast<int>(ecuio_condition_set_count(m_set.get()))},
            {QStringLiteral("name"),
             QString::fromUtf8(ecuio_condition_set_name(m_set.get()))}};
}
