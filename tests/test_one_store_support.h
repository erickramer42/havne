// File/layer: tests/test_one_store_support.h | GTest + QtCore, app-layer tests
// Testing: BRIDGE: the helpers of test_one_store*.cpp (test_bridge)

#pragma once

#include <gtest/gtest.h>

#include <functional>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include "obdbimport.h"
#include "packed_plugin.h"
#include "pluginstore.h"
#include "sessionbridge.h"
#include "urlwrite.h"
#include <ecuio/obdb.h>
#include <ecuio/plugin.h>
#include <ecuio/profile.h>

namespace {

const QString kSuffix = QStringLiteral(".havne-plugin.json");

// A settings folder of its own, entered when made; enter() comes back to
// it after another was made, so one case can be two devices in turn
class SettingsDir
{
public:
    SettingsDir() : m_before(qgetenv("HAVNE_SETTINGS_DIR")) { enter(); }
    ~SettingsDir() { qputenv("HAVNE_SETTINGS_DIR", m_before); }
    SettingsDir(const SettingsDir&) = delete;
    SettingsDir& operator=(const SettingsDir&) = delete;

    bool valid() const { return m_dir.isValid(); }
    void enter() const { qputenv("HAVNE_SETTINGS_DIR", m_dir.path().toUtf8()); }

private:
    QTemporaryDir m_dir;
    QByteArray m_before;
};

// A bridge and what it logged
struct Logged {
    SessionBridge bridge;
    QStringList log;
    Logged()
    {
        QObject::connect(&bridge, &SessionBridge::logMessage,
                         [this](const QString& line) { log << line; });
    }
    std::string lines() const
    {
        return log.join(QLatin1Char('\n')).toStdString();
    }
};

// A signal file of one DID on an ECU named pcm at rx; with obd2 it adds
// to the built-in set
inline QByteArray signalFile(const QString& name,
                      const QString& did = QStringLiteral("D100"),
                      bool obd2 = false,
                      const QString& rx = QStringLiteral("7E8"))
{
    return QStringLiteral(
               R"({"havne_signals":1,"name":"%1","source":{"name":"t"},%3)"
               R"("ecus":[{"id":"pcm","rx":"%4"}],"signals":[)"
               R"({"key":"a","name":"A","read":{"service":"22","number":"%2"},)"
               R"("field":{"byte":0,"bits":8}}]})")
        .arg(name, did,
             obd2 ? QStringLiteral(R"("extends":"obd2",)") : QString(), rx)
        .toUtf8();
}

inline QString sha256Of(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

inline QUrl writeScratch(const QString& folder, const QString& name,
                  const QByteArray& bytes)
{
    QDir().mkpath(folder);
    QFile f(folder + QLatin1Char('/') + name);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
    return QUrl::fromLocalFile(f.fileName());
}

inline QByteArray readBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

inline bool registered(const QString& profile)
{
    const ecuio_signal_set_t* set = nullptr;
    return ecuio_profile_lookup(profile.toUtf8().constData(), &set) == ECUIO_OK;
}

// The store's kept copies, by name
inline QStringList copies()
{
    return QDir(PluginStore::root())
        .entryList({QStringLiteral("*") + kSuffix}, QDir::Files, QDir::Name);
}

inline QString copyPath(const QString& sha)
{
    return PluginStore::root() + QLatin1Char('/') + sha + kSuffix;
}

inline QString indexPath()
{
    return PluginStore::root() + QStringLiteral("/store.json");
}

// The index's facts for a plugin id, {} for none
inline QJsonObject factsOf(const QString& id)
{
    return QJsonDocument::fromJson(readBytes(indexPath()))
        .object()
        .value(QStringLiteral("plugins"))
        .toObject()
        .value(id)
        .toObject();
}

inline bool ok(const QVariantMap& r)
{
    return r.value(QStringLiteral("ok")).toBool();
}

inline std::string words(const QVariantMap& r)
{
    return r.value(QStringLiteral("words")).toString().toStdString();
}

inline QString str(const QVariantMap& r, const char* key)
{
    return r.value(QString::fromLatin1(key)).toString();
}

inline QStringList list(const QVariantMap& r, const char* key)
{
    return r.value(QString::fromLatin1(key)).toStringList();
}

// The index's wrappedFrom for a plugin id: file hash to variant id
inline QJsonObject madeFrom(const QString& id)
{
    return factsOf(id).value(QStringLiteral("wrappedFrom")).toObject();
}

// One key of every row, in the rows' order
inline QStringList column(const QVariantList& rows, const char* key)
{
    QStringList out;
    for (const QVariant& v : rows) {
        out << v.toMap().value(QString::fromLatin1(key)).toString();
    }
    return out;
}

// Core's own wrap of a file with every default: what the store must keep
inline QByteArray coreWrap(const QByteArray& file)
{
    char* text = nullptr;
    size_t length = 0;
    EXPECT_EQ(ecuio_plugin_wrap(file.constData(),
                                static_cast<size_t>(file.size()), nullptr,
                                &text, &length),
              ECUIO_OK)
        << ecuio_last_error_detail();
    const QByteArray out =
        text != nullptr ? QByteArray(text, static_cast<qsizetype>(length))
                        : QByteArray();
    ecuio_plugin_text_free(text);
    return out;
}

inline QUrl obdbUrl()
{
    return QUrl::fromLocalFile(QStringLiteral(ECUIO_OBDB_FIXTURE));
}

// The synthetic OBDb set converted for a year and kept for a vehicle
inline QVariantMap keepObdb(SessionBridge& bridge, int year, const QString& name,
                     const QString& vehicle, bool yearTyped)
{
    const QVariantMap p = bridge.previewObdb(obdbUrl(), year, name);
    EXPECT_TRUE(ok(p)) << words(p);
    return bridge.keepObdbPlugin(vehicle, yearTyped);
}

} // namespace
