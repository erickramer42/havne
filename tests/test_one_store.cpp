// File/layer: tests/test_one_store.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest.OneStore* (test_bridge)
//
// The one store (ADR-0064 step 3, app/pluginstore.{h,cpp} and the
// bridge's calls over it): a signal file and an OBDb set wrapped into
// plugins of the user's own, a package kept as it is, the store's index
// (mine, importedFor, wrappedFrom) and what it does with a file it can't
// trust, the update of a plugin of the user's own by its lineage and
// version, Export plugin's bytes and its share-alike note, a variant's
// signal file, Copy to my plugin, and a vehicle's ordered picks. Every
// case has a settings folder of its own.

#include <gtest/gtest.h>

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
QByteArray signalFile(const QString& name,
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

QString sha256Of(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QUrl writeScratch(const QString& folder, const QString& name,
                  const QByteArray& bytes)
{
    QDir().mkpath(folder);
    QFile f(folder + QLatin1Char('/') + name);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
    return QUrl::fromLocalFile(f.fileName());
}

QByteArray readBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool registered(const QString& profile)
{
    const ecuio_signal_set_t* set = nullptr;
    return ecuio_profile_lookup(profile.toUtf8().constData(), &set) == ECUIO_OK;
}

// The store's kept copies, by name
QStringList copies()
{
    return QDir(PluginStore::root())
        .entryList({QStringLiteral("*") + kSuffix}, QDir::Files, QDir::Name);
}

QString copyPath(const QString& sha)
{
    return PluginStore::root() + QLatin1Char('/') + sha + kSuffix;
}

QString indexPath()
{
    return PluginStore::root() + QStringLiteral("/store.json");
}

// The index's facts for a plugin id, {} for none
QJsonObject factsOf(const QString& id)
{
    return QJsonDocument::fromJson(readBytes(indexPath()))
        .object()
        .value(QStringLiteral("plugins"))
        .toObject()
        .value(id)
        .toObject();
}

bool ok(const QVariantMap& r)
{
    return r.value(QStringLiteral("ok")).toBool();
}

std::string words(const QVariantMap& r)
{
    return r.value(QStringLiteral("words")).toString().toStdString();
}

QString str(const QVariantMap& r, const char* key)
{
    return r.value(QString::fromLatin1(key)).toString();
}

QStringList list(const QVariantMap& r, const char* key)
{
    return r.value(QString::fromLatin1(key)).toStringList();
}

// The index's wrappedFrom for a plugin id: file hash to variant id
QJsonObject madeFrom(const QString& id)
{
    return factsOf(id).value(QStringLiteral("wrappedFrom")).toObject();
}

// One key of every row, in the rows' order
QStringList column(const QVariantList& rows, const char* key)
{
    QStringList out;
    for (const QVariant& v : rows) {
        out << v.toMap().value(QString::fromLatin1(key)).toString();
    }
    return out;
}

// Core's own wrap of a file with every default: what the store must keep
QByteArray coreWrap(const QByteArray& file)
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

QUrl obdbUrl()
{
    return QUrl::fromLocalFile(QStringLiteral(ECUIO_OBDB_FIXTURE));
}

// The synthetic OBDb set converted for a year and kept for a vehicle
QVariantMap keepObdb(SessionBridge& bridge, int year, const QString& name,
                     const QString& vehicle, bool yearTyped)
{
    const QVariantMap p = bridge.previewObdb(obdbUrl(), year, name);
    EXPECT_TRUE(ok(p)) << words(p);
    return bridge.keepObdbPlugin(vehicle, yearTyped);
}

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

bool disconnectAndWait(SessionBridge& bridge)
{
    bridge.disconnectSession();
    QElapsedTimer clock;
    clock.start();
    while (bridge.disconnecting()) {
        if (clock.elapsed() > 5000) {
            return false;
        }
        spin(10);
    }
    return true;
}

} // namespace

/* A signal file is read, held and kept as a plugin of the user's own:
 * core's wrap of it byte for byte, one variant, registered, with the
 * index saying whose it is, which vehicle it came for and which file it
 * was made from; a later bridge loads it as the user's own */
TEST(BridgeTest, OneStoreASignalFileBecomesAPluginOfTheUsersOwn)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QByteArray file = signalFile(QStringLiteral("Daily driver extras"),
                                       QStringLiteral("D100"), true);
    const QString id = QStringLiteral("daily-driver-extras");
    const QString profile = QStringLiteral("plugin.daily-driver-extras.base");
    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    {
        Logged b;
        QVariantMap r = b.bridge.readImport(
            writeScratch(from.path(), QStringLiteral("extras.json"), file));
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "kind").toStdString(), "signals");
        EXPECT_EQ(str(r, "name").toStdString(), "Daily driver extras");
        EXPECT_EQ(str(r, "id"), id);
        EXPECT_EQ(str(r, "variant"), id) << "the name's slug, within 24";
        EXPECT_TRUE(copies().isEmpty()) << "nothing kept by a read";
        EXPECT_TRUE(b.bridge.plugins().isEmpty());
        EXPECT_FALSE(registered(profile));
        EXPECT_TRUE(b.log.contains(QStringLiteral(
            "Signal file read: Daily driver extras (as a plugin, "
            "daily-driver-extras)")))
            << b.lines();

        r = b.bridge.keepSignals(QString(), QString(), QStringLiteral("v3"));
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "id"), id);
        EXPECT_EQ(str(r, "name").toStdString(), "Daily driver extras");
        EXPECT_EQ(str(r, "version").toStdString(), "1.0.0");
        EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("base")});
        EXPECT_EQ(list(r, "profiles"), QStringList{profile});
        EXPECT_EQ(list(r, "extends"), QStringList{QStringLiteral("obd2")});
        EXPECT_EQ(r.value(QStringLiteral("counts")).toList(),
                  QVariantList{static_cast<int>(obd2->count) + 1});
        EXPECT_EQ(r.value(QStringLiteral("owns")).toList(), QVariantList{1});
        EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
        EXPECT_EQ(str(r, "importedFor").toStdString(), "v3");
        EXPECT_EQ(str(r, "lineage"), sha256Of(file).left(32));
        EXPECT_FALSE(r.value(QStringLiteral("obdb")).toBool());
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
        EXPECT_EQ(str(r, "replaced"), QString());
        EXPECT_TRUE(list(r, "replacedProfiles").isEmpty());
        EXPECT_EQ(words(r), "");
        EXPECT_TRUE(registered(profile));

        const QByteArray wrap = coreWrap(file);
        ASSERT_FALSE(wrap.isEmpty());
        EXPECT_EQ(str(r, "sha256"), sha256Of(wrap));
        EXPECT_EQ(copies(), QStringList{sha256Of(wrap) + kSuffix});
        EXPECT_EQ(readBytes(copyPath(sha256Of(wrap))), wrap);

        const QJsonObject index =
            QJsonDocument::fromJson(readBytes(indexPath())).object();
        EXPECT_EQ(index.value(QStringLiteral("format")).toInt(), 1);
        EXPECT_EQ(
            factsOf(id),
            (QJsonObject{
                {QStringLiteral("mine"), true},
                {QStringLiteral("importedFor"), QStringLiteral("v3")},
                {QStringLiteral("wrappedFrom"),
                 QJsonObject{{sha256Of(file), QStringLiteral("base")}}}}));
        EXPECT_TRUE(b.log.contains(
            QStringLiteral("Plugin made: Daily driver extras 1.0.0 (%1)")
                .arg(profile)))
            << b.lines();

        r = b.bridge.keepSignals(QString(), QString(), QString());
        EXPECT_FALSE(ok(r)) << "the import was taken";
        EXPECT_EQ(words(r), "No signal file is waiting: import it again");
    }
    EXPECT_FALSE(registered(profile)) << "freed at close";

    SessionBridge again;
    const QVariantList all = again.plugins();
    ASSERT_EQ(all.size(), 1);
    const QVariantMap e = all.at(0).toMap();
    EXPECT_TRUE(ok(e));
    EXPECT_TRUE(e.value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(str(e, "importedFor").toStdString(), "v3");
    EXPECT_TRUE(registered(profile));
}

/* The same file again finds its copy; another file whose name gives an
 * id the store has is numbered -2, -3, within the id's 31 bytes and
 * with no '-' left before the number */
TEST(BridgeTest, OneStoreTheSameFileIsKeptOnceAndATakenIdIsNumbered)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    SessionBridge bridge;
    const auto keep = [&bridge](const QByteArray& file) {
        return bridge.keepSignalText(QString::fromUtf8(file), QString(),
                                     QString(), QString());
    };
    const QByteArray a = signalFile(QStringLiteral("Extras"));
    QVariantMap r = keep(a);
    ASSERT_TRUE(ok(r)) << words(r);
    const QString first = str(r, "sha256");
    EXPECT_EQ(str(r, "id").toStdString(), "extras");
    EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());

    r = keep(a);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), first);
    EXPECT_EQ(copies().size(), 1);
    EXPECT_EQ(bridge.plugins().size(), 1);

    const QByteArray b =
        signalFile(QStringLiteral("Extras"), QStringLiteral("D101"));
    r = keep(b);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "extras-2");
    EXPECT_EQ(str(r, "name").toStdString(), "Extras");
    EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_TRUE(registered(QStringLiteral("plugin.extras-2.base")));
    const QString second = str(r, "sha256");

    r = keep(b);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool())
        << "found under its number";
    EXPECT_EQ(str(r, "sha256"), second);
    EXPECT_EQ(words(r), "") << "unchanged since: nothing to say";
    EXPECT_EQ(madeFrom(QStringLiteral("extras-2")),
              (QJsonObject{{sha256Of(b), QStringLiteral("base")}}));

    r = keep(signalFile(QStringLiteral("Extras"), QStringLiteral("D102")));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "extras-3");
    EXPECT_EQ(column(bridge.plugins(), "id"),
              (QStringList{QStringLiteral("extras"), QStringLiteral("extras-2"),
                           QStringLiteral("extras-3")}))
        << "plugins of one name are listed by id";

    // An id of 31 bytes gives up two for its number
    const QString full = QStringLiteral("abcdefghij abcdefghij abcdefghi");
    r = keep(signalFile(full));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "abcdefghij-abcdefghij-abcdefghi");
    r = keep(signalFile(full, QStringLiteral("D101")));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "abcdefghij-abcdefghij-abcdefg-2");

    // A cut that ends on a '-' drops it before the number
    const QString dashed = QStringLiteral("abcdefghij abcdefghij abcdef hij");
    r = keep(signalFile(dashed));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "abcdefghij-abcdefghij-abcdef-hi");
    r = keep(signalFile(dashed, QStringLiteral("D101")));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "abcdefghij-abcdefghij-abcdef-2");
}

/* One Import tells the three kinds apart by the file's first key: a
 * package goes in, a signal file waits, an OBDb set is left to its
 * report; anything else, and a file a loader refuses, is refused in
 * words that never name the path */
TEST(BridgeTest, OneStoreAnImportTellsItsKindByItsFirstKey)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    Logged b;
    const auto read = [&](const char* name, const QByteArray& bytes) {
        return b.bridge.readImport(
            writeScratch(from.path(), QString::fromLatin1(name), bytes));
    };

    QVariantMap r = read("a.json", packed_plugin::example());
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "plugin");
    EXPECT_EQ(str(r, "id").toStdString(), "example-sim");
    EXPECT_FALSE(r.value(QStringLiteral("mine")).toBool())
        << "a package is someone else's until it is marked";
    EXPECT_EQ(str(r, "updated"), QString());
    EXPECT_EQ(copies().size(), 1);
    EXPECT_FALSE(QFile::exists(indexPath())) << "a package leaves no fact";

    // An OBDb set is held, read once: its report converts the held
    // bytes, as often as it is asked, with the file gone
    const QByteArray set = readBytes(QStringLiteral(ECUIO_OBDB_FIXTURE));
    ASSERT_FALSE(set.isEmpty());
    r = b.bridge.previewObdbImport(2019, QStringLiteral("Early"));
    EXPECT_FALSE(ok(r)) << "nothing read yet";
    EXPECT_EQ(words(r), "No OBDb set is waiting: import it again");
    const QUrl setUrl =
        writeScratch(from.path(), QStringLiteral("set.json"), set);
    r = b.bridge.readImport(setUrl);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "obdb");
    ASSERT_TRUE(QFile::remove(setUrl.toLocalFile()));
    EXPECT_EQ(copies().size(), 1) << "an OBDb set shows its report first";
    r = b.bridge.keepObdbPlugin(QStringLiteral("v1"), false);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "No OBDb set is waiting: import it again");
    r = b.bridge.keepSignals(QString(), QString(), QString());
    EXPECT_FALSE(ok(r)) << "the file held is no signal file";
    EXPECT_EQ(words(r), "No signal file is waiting: import it again");
    r = b.bridge.previewObdbImport(2019, QStringLiteral("Held OBDb"));
    ASSERT_TRUE(ok(r)) << words(r);
    r = b.bridge.previewObdbImport(2020, QStringLiteral("Held OBDb"));
    ASSERT_TRUE(ok(r)) << words(r) << " (another year, the same bytes)";
    r = b.bridge.keepObdbPlugin(QStringLiteral("v1"), true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("my2020")});
    const QString held = str(r, "sha256");
    r = b.bridge.previewObdbImport(2020, QStringLiteral("Held OBDb"));
    EXPECT_FALSE(ok(r)) << "kept: the import is done";
    ASSERT_TRUE(ok(b.bridge.removePlugin(held)));
    ASSERT_EQ(copies().size(), 1);

    // A new read forgets what the last one held
    r = b.bridge.readImport(
        writeScratch(from.path(), QStringLiteral("set.json"), set));
    ASSERT_TRUE(ok(r)) << words(r);
    r = b.bridge.readImport(writeScratch(
        from.path(), QStringLiteral("none.json"), QByteArray("{}")));
    EXPECT_FALSE(ok(r));
    r = b.bridge.previewObdbImport(2019, QStringLiteral("Held OBDb"));
    EXPECT_FALSE(ok(r));

    // An editor's byte-order mark is passed by, for each kind
    const QByteArray mark("\xEF\xBB\xBF");
    const QByteArray marked = signalFile(QStringLiteral("Marked"));
    r = read("bom.json", mark + marked);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "signals");
    r = b.bridge.keepSignals(QString(), QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "sha256"), sha256Of(coreWrap(marked)))
        << "the text without its mark is what was wrapped";
    ASSERT_TRUE(ok(b.bridge.removePlugin(str(r, "sha256"))));
    r = read("bom-plugin.json",
             mark + coreWrap(signalFile(QStringLiteral("Marked pack"))));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "plugin");
    ASSERT_TRUE(ok(b.bridge.removePlugin(str(r, "sha256"))));
    r = read("bom-set.json", mark + set);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "obdb");
    b.bridge.dropImport();
    ASSERT_EQ(copies().size(), 1);

    // Leading space is passed by; the first key decides
    r = read("b.json", " \r\n\t" + signalFile(QStringLiteral("Spaced")));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "signals");
    b.bridge.dropImport();
    r = b.bridge.keepSignals(QString(), QString(), QString());
    EXPECT_FALSE(ok(r)) << "a dropped import is gone";

    const QByteArray neither[] = {
        R"({"a":1})",
        "not json",
        "",
        "[1]",
        R"({"commands_":[]})",
        // The format key must come first
        R"({"name":"x","havne_signals":1,"source":{"name":"t"},"signals":[]})",
        R"({"name":"x","havne_plugin":1})",
    };
    for (const QByteArray& bytes : neither) {
        r = read("c.json", bytes);
        EXPECT_FALSE(ok(r)) << bytes.toStdString();
        EXPECT_EQ(str(r, "kind"), QString()) << bytes.toStdString();
        EXPECT_EQ(words(r), "The file is neither a plugin, a signal file "
                            "nor an OBDb signal set")
            << bytes.toStdString();
    }
    EXPECT_TRUE(b.log.contains(QStringLiteral(
        "Import refused: The file is neither a plugin, a signal file nor "
        "an OBDb signal set")))
        << b.lines();

    // A signal file the loader refuses: its words, and nothing waiting
    r = read("broken-name.json",
             signalFile(QStringLiteral("Broken"))
                 .replace("\"source\"", "\"scael\":1,\"source\""));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(str(r, "kind").toStdString(), "signals");
    const QString why = str(r, "words");
    EXPECT_TRUE(why.contains(QStringLiteral("line"))) << why.toStdString();
    EXPECT_TRUE(why.contains(QStringLiteral("scael"))) << why.toStdString();
    EXPECT_FALSE(why.contains(QStringLiteral("broken-name")))
        << why.toStdString();
    EXPECT_FALSE(why.contains(from.path())) << why.toStdString();
    r = b.bridge.keepSignals(QString(), QString(), QString());
    EXPECT_FALSE(ok(r)) << "a refused file isn't held";

    // Past a signal file's bound, and past a plugin's
    QByteArray large = signalFile(QStringLiteral("Large"));
    large.insert(1, QByteArray(ECUIO_SIGNAL_FILE_MAX_BYTES, ' '));
    r = read("large.json", large);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The file is over 1024 KiB, a signal file's most");
    r = read("huge.json", QByteArray(ECUIO_PLUGIN_PACKAGE_MAX + 1, ' '));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The file is over 16 MiB, a plugin's most");

    // An OBDb set at its bound is read; one byte past it is refused as
    // what it is, by the import and by the report's own read
    QByteArray most = set;
    most.insert(1, QByteArray(ECUIO_OBDB_MAX_BYTES - set.size(), ' '));
    ASSERT_EQ(most.size(), static_cast<qsizetype>(ECUIO_OBDB_MAX_BYTES));
    r = read("most.json", most);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "kind").toStdString(), "obdb");
    r = b.bridge.previewObdbImport(2019, QStringLiteral("Most OBDb"));
    EXPECT_TRUE(ok(r)) << words(r);
    most.insert(1, ' ');
    r = read("over.json", most);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(str(r, "kind").toStdString(), "obdb");
    EXPECT_EQ(words(r), "The file is over 1 MiB, an OBDb set's most");
    EXPECT_TRUE(b.log.contains(QStringLiteral(
        "OBDb set refused: The file is over 1 MiB, an OBDb set's most")))
        << b.lines();
    r = b.bridge.previewObdbImport(2019, QStringLiteral("Most OBDb"));
    EXPECT_FALSE(ok(r)) << "a refused file isn't held";
    r = b.bridge.previewObdb(
        writeScratch(from.path(), QStringLiteral("over.json"), most), 2019,
        QStringLiteral("Most OBDb"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The file is over 1 MiB, an OBDb set's most");

    r = b.bridge.readImport(
        QUrl::fromLocalFile(from.path() + QStringLiteral("/missing.json")));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Couldn't read the file");
    EXPECT_EQ(str(r, "kind"), QString());

    EXPECT_EQ(copies().size(), 1) << "only the package was kept";
    for (const QString& line : std::as_const(b.log)) {
        EXPECT_FALSE(line.contains(from.path())) << line.toStdString();
    }
}

/* An OBDb set converted and kept is a plugin of the user's own, imported
 * for its vehicle; its variant is my<year> only for a year the user
 * typed, base for one decoded from a VIN or for none */
TEST(BridgeTest, OneStoreAnObdbSetIsWrappedAndOnlyATypedYearNamesItsVariant)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    Logged b;
    QVariantMap r =
        keepObdb(b.bridge, 2019, QStringLiteral("Daily driver OBDb"),
                 QStringLiteral("v1"), true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb");
    EXPECT_EQ(str(r, "version").toStdString(), "1.0.0");
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("my2019")});
    EXPECT_EQ(list(r, "profiles"),
              QStringList{QStringLiteral("plugin.daily-driver-obdb.my2019")});
    EXPECT_EQ(list(r, "extends"), QStringList{QStringLiteral("obd2")});
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("obdb")).toBool());
    EXPECT_EQ(str(r, "importedFor").toStdString(), "v1");
    EXPECT_TRUE(registered(QStringLiteral("plugin.daily-driver-obdb.my2019")));
    EXPECT_TRUE(b.log.contains(
        QStringLiteral("Plugin from OBDb: Daily driver OBDb 1.0.0 "
                       "(plugin.daily-driver-obdb.my2019)")))
        << b.lines();
    EXPECT_FALSE(ok(b.bridge.keepObdbPlugin(QStringLiteral("v1"), true)))
        << "the set was taken";

    r = keepObdb(b.bridge, 2019, QStringLiteral("Weekend car OBDb"),
                 QStringLiteral("v2"), false);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("base")})
        << "a year decoded from a VIN names nothing";
    EXPECT_FALSE(readBytes(copyPath(str(r, "sha256"))).contains("my2019"));

    r = keepObdb(b.bridge, 0, QStringLiteral("Third OBDb"),
                 QStringLiteral("v3"), true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("base")})
        << "no year, no name";
}

/* An OBDb set imported again for its vehicle takes the place of that
 * vehicle's earlier conversion of the same name, and of nothing else:
 * not another vehicle's, not a file the user wrote; while a session
 * decodes with the earlier one it waits */
TEST(BridgeTest, OneStoreAnObdbReimportTakesItsVehiclesEarlierImportsPlace)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    Logged b;
    const QString name = QStringLiteral("Daily driver OBDb");
    const QString v1 = QStringLiteral("v1");
    const QVariantMap first = keepObdb(b.bridge, 2019, name, v1, true);
    ASSERT_TRUE(ok(first)) << words(first);

    QVariantMap r = keepObdb(b.bridge, 2021, name, v1, true);
    ASSERT_TRUE(ok(r)) << words(r);
    const QString second = str(r, "sha256");
    EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb");
    EXPECT_EQ(str(r, "replaced").toStdString(), "1.0.0");
    EXPECT_EQ(list(r, "replacedProfiles"),
              QStringList{QStringLiteral("plugin.daily-driver-obdb.my2019")})
        << "what a pick of the earlier year is moved by";
    EXPECT_FALSE(r.value(QStringLiteral("merged")).toBool());
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("my2021")});
    EXPECT_EQ(copies(), QStringList{second + kSuffix});
    EXPECT_EQ(b.bridge.plugins().size(), 1);
    EXPECT_FALSE(registered(QStringLiteral("plugin.daily-driver-obdb.my2019")));
    EXPECT_TRUE(registered(QStringLiteral("plugin.daily-driver-obdb.my2021")));
    const QJsonObject facts = factsOf(QStringLiteral("daily-driver-obdb"));
    EXPECT_EQ(madeFrom(QStringLiteral("daily-driver-obdb")).size(), 1)
        << "the conversion it holds, not the one that went";
    EXPECT_EQ(madeFrom(QStringLiteral("daily-driver-obdb"))
                  .begin()
                  .value()
                  .toString()
                  .toStdString(),
              "my2021");
    EXPECT_EQ(facts.value(QStringLiteral("importedFor")).toString(), v1);
    EXPECT_TRUE(b.log.contains(QStringLiteral(
        "Plugin from OBDb: Daily driver OBDb 1.0.0 "
        "(plugin.daily-driver-obdb.my2021), in the earlier import's place")))
        << b.lines();

    // The same bytes for another vehicle: the copy is here, and stays
    // the first vehicle's
    r = keepObdb(b.bridge, 2021, name, QStringLiteral("v3"), true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), second);
    EXPECT_EQ(str(r, "importedFor"), v1);

    // Another vehicle's set of that name is a plugin of its own
    r = keepObdb(b.bridge, 2020, name, QStringLiteral("v2"), true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb-2");
    EXPECT_EQ(str(r, "replaced"), QString());
    EXPECT_EQ(str(r, "importedFor").toStdString(), "v2");
    EXPECT_EQ(b.bridge.plugins().size(), 2);

    // The first vehicle's again: its own goes, the other stays
    r = keepObdb(b.bridge, 2019, name, v1, true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb");
    EXPECT_EQ(str(r, "replaced").toStdString(), "1.0.0");
    EXPECT_EQ(b.bridge.plugins().size(), 2);
    EXPECT_TRUE(
        registered(QStringLiteral("plugin.daily-driver-obdb-2.my2020")));

    // A file the user wrote under that name replaces nothing
    r = b.bridge.keepSignalText(QString::fromUtf8(signalFile(name)), QString(),
                                QString(), v1);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb-3");
    EXPECT_EQ(str(r, "replaced"), QString());
    EXPECT_FALSE(r.value(QStringLiteral("obdb")).toBool());
    EXPECT_EQ(b.bridge.plugins().size(), 3);

    // The vehicle's set under another name is another plugin
    r = keepObdb(b.bridge, 2019, QStringLiteral("Second set OBDb"), v1, true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "second-set-obdb");
    EXPECT_EQ(str(r, "replaced"), QString());
    EXPECT_EQ(b.bridge.plugins().size(), 4);
    ASSERT_TRUE(ok(b.bridge.removePlugin(str(r, "sha256"))));

    // A file the user wrote and kept for a vehicle is never taken for
    // that vehicle's earlier conversion
    const QString solo = QStringLiteral("Solo OBDb");
    r = b.bridge.keepSignalText(QString::fromUtf8(signalFile(solo)), QString(),
                                QString(), QStringLiteral("v5"));
    ASSERT_TRUE(ok(r)) << words(r);
    const QString written = str(r, "sha256");
    r = keepObdb(b.bridge, 2019, solo, QStringLiteral("v5"), true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "solo-obdb-2");
    EXPECT_EQ(str(r, "replaced"), QString());
    EXPECT_TRUE(QFile::exists(copyPath(written)));
    ASSERT_TRUE(ok(b.bridge.removePlugin(str(r, "sha256"))));
    ASSERT_TRUE(ok(b.bridge.removePlugin(written)));
    EXPECT_EQ(b.bridge.plugins().size(), 3);

    // A session on the earlier import: the new one waits, still pending
    ASSERT_TRUE(b.bridge
                    .connectSession(QStringLiteral(
                        "mock://obdb?profile=plugin.daily-driver-obdb.my2019"))
                    .isEmpty());
    r = keepObdb(b.bridge, 2021, name, v1, true);
    EXPECT_FALSE(ok(r));
    EXPECT_TRUE(r.value(QStringLiteral("inUse")).toBool());
    EXPECT_EQ(words(r), "A session decodes with Daily driver OBDb 1.0.0: "
                        "disconnect, then import it again");
    EXPECT_TRUE(registered(QStringLiteral("plugin.daily-driver-obdb.my2019")));
    EXPECT_EQ(b.bridge.plugins().size(), 3);
    ASSERT_TRUE(disconnectAndWait(b.bridge));
    r = b.bridge.keepObdbPlugin(v1, true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("my2021")});
    EXPECT_EQ(b.bridge.plugins().size(), 3);
}

/* Only a plugin of the user's own takes a new, changed or removed
 * variant. A package becomes one by Mine; each change raises the minor,
 * replaces the kept copy, and says when an ECU of the file's took
 * another id; the last variant never goes */
TEST(BridgeTest, OneStoreOnlyAPluginOfTheUsersOwnTakesAVariant)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    Logged b;
    QVariantMap r = b.bridge.readImport(writeScratch(
        from.path(), QStringLiteral("a.json"), packed_plugin::example()));
    ASSERT_TRUE(ok(r)) << words(r);
    const QString theirs = str(r, "sha256");
    const QString extra = QStringLiteral("plugin.example-sim.extra");

    // The file's ECU is named as the plugin's pcm, at another address
    const QByteArray file =
        signalFile(QStringLiteral("Extra file"), QStringLiteral("D150"), false,
                   QStringLiteral("7EA"));
    r = b.bridge.readImport(
        writeScratch(from.path(), QStringLiteral("extra.json"), file));
    ASSERT_TRUE(ok(r)) << words(r);
    r = b.bridge.keepSignals(theirs, QStringLiteral("extra"), QString());
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Example: the simulated car isn't a plugin of your "
                        "own: only your own plugins are changed");
    r = b.bridge.dropPluginVariant(theirs, QStringLiteral("my2019"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Example: the simulated car isn't a plugin of your "
                        "own: only your own plugins are changed");
    EXPECT_EQ(copies(), QStringList{theirs + kSuffix}) << "nothing changed";
    EXPECT_TRUE(registered(QStringLiteral("plugin.example-sim.my2019")));

    r = b.bridge.markPluginMine(QString(64, QLatin1Char('0')));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The store has no such plugin");
    r = b.bridge.markPluginMine(theirs);
    ASSERT_TRUE(ok(r));
    EXPECT_EQ(words(r), "");
    EXPECT_TRUE(b.bridge.plugins()
                    .at(0)
                    .toMap()
                    .value(QStringLiteral("mine"))
                    .toBool());
    EXPECT_EQ(factsOf(QStringLiteral("example-sim")),
              (QJsonObject{{QStringLiteral("mine"), true}}));

    // A variant id core refuses leaves the file waiting
    r = b.bridge.keepSignals(theirs, QStringLiteral("Bad Id"), QString());
    EXPECT_FALSE(ok(r));
    EXPECT_FALSE(words(r).empty());

    r = b.bridge.keepSignals(theirs, QStringLiteral("extra"), QString());
    ASSERT_TRUE(ok(r)) << words(r) << " (the refusals left it pending)";
    const QString second = str(r, "sha256");
    EXPECT_EQ(str(r, "version").toStdString(), "1.1.0");
    EXPECT_EQ(str(r, "replaced").toStdString(), "1.0.0");
    EXPECT_EQ(list(r, "variants"),
              (QStringList{QStringLiteral("base"), QStringLiteral("my2019"),
                           QStringLiteral("extra")}));
    EXPECT_TRUE(str(r, "notes").contains(QStringLiteral("pcm_2")))
        << str(r, "notes").toStdString();
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_TRUE(registered(extra));
    EXPECT_EQ(copies(), QStringList{second + kSuffix});
    EXPECT_EQ(madeFrom(QStringLiteral("example-sim")),
              (QJsonObject{{sha256Of(file), QStringLiteral("extra")}}));
    EXPECT_TRUE(b.log.last().startsWith(
        QStringLiteral("Plugin changed: Example: the simulated car 1.1.0 "
                       "(plugin.example-sim.base, plugin.example-sim.my2019, "
                       "plugin.example-sim.extra); ")))
        << b.lines();

    // The same id again: put in its place, no note for a shared ECU,
    // and the file that variant was made from before is named no more
    const QByteArray other =
        signalFile(QStringLiteral("Other"), QStringLiteral("D151"));
    r = b.bridge.keepSignalText(QString::fromUtf8(other), second,
                                QStringLiteral("extra"), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "version").toStdString(), "1.2.0");
    EXPECT_EQ(list(r, "variants").size(), 3);
    EXPECT_EQ(str(r, "notes"), QString());
    EXPECT_EQ(madeFrom(QStringLiteral("example-sim")),
              (QJsonObject{{sha256Of(other), QStringLiteral("extra")}}));

    // No id given: the file's name as core cuts it
    const QByteArray track =
        signalFile(QStringLiteral("Track day"), QStringLiteral("D152"));
    r = b.bridge.keepSignalText(QString::fromUtf8(track), str(r, "sha256"),
                                QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "version").toStdString(), "1.3.0");
    EXPECT_EQ(list(r, "variants").last().toStdString(), "track-day");
    EXPECT_EQ(madeFrom(QStringLiteral("example-sim")),
              (QJsonObject{{sha256Of(other), QStringLiteral("extra")},
                           {sha256Of(track), QStringLiteral("track-day")}}))
        << "the index names the id core gave the variant";
    const QString fourth = str(r, "sha256");

    r = b.bridge.dropPluginVariant(fourth, QStringLiteral("nothing"));
    EXPECT_FALSE(ok(r));
    EXPECT_FALSE(words(r).empty());
    EXPECT_EQ(copies(), QStringList{fourth + kSuffix});

    r = b.bridge.dropPluginVariant(fourth, QStringLiteral("extra"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "version").toStdString(), "1.4.0");
    EXPECT_EQ(list(r, "variants"),
              (QStringList{QStringLiteral("base"), QStringLiteral("my2019"),
                           QStringLiteral("track-day")}));
    EXPECT_FALSE(registered(extra));
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(copies(), QStringList{str(r, "sha256") + kSuffix});
    EXPECT_EQ(list(r, "replacedProfiles").size(), 4);
    EXPECT_TRUE(list(r, "replacedProfiles").contains(extra));
    EXPECT_EQ(madeFrom(QStringLiteral("example-sim")),
              (QJsonObject{{sha256Of(track), QStringLiteral("track-day")}}))
        << "a variant that went takes its file's name with it";

    // A plugin's last variant stays
    r = b.bridge.keepSignalText(
        QString::fromUtf8(signalFile(QStringLiteral("Only one"))), QString(),
        QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    const QString one = str(r, "sha256");
    r = b.bridge.dropPluginVariant(one, QStringLiteral("base"));
    EXPECT_FALSE(ok(r));
    EXPECT_FALSE(words(r).empty());
    EXPECT_TRUE(registered(QStringLiteral("plugin.only-one.base")));
    EXPECT_TRUE(QFile::exists(copyPath(one)));
}

/* A plugin exported on one device, imported and marked Mine on another,
 * updates in place from its next export, the index's facts kept and a
 * pick of a variant the new version lacks shown as missing. The same
 * bytes change nothing; the same lineage at the same or a lower version
 * and a package that only reuses the id are refused, nothing changed */
TEST(BridgeTest, OneStoreAPluginOfTheUsersOwnUpdatesOnlyFromItsLaterVersion)
{
    SettingsDir desktop;
    SettingsDir phone;
    ASSERT_TRUE(desktop.valid() && phone.valid());
    QTemporaryDir files;
    const QString id = QStringLiteral("trip-extras");
    const QUrl firstUrl = QUrl::fromLocalFile(
        files.path() + QStringLiteral("/trip-extras-1.havne-plugin.json"));
    const QUrl secondUrl = QUrl::fromLocalFile(
        files.path() + QStringLiteral("/trip-extras-2.havne-plugin.json"));
    QString desktopSha;
    {
        desktop.enter();
        SessionBridge b;
        QVariantMap r = b.keepSignalText(
            QString::fromUtf8(signalFile(QStringLiteral("Trip extras"))),
            QString(), QString(), QStringLiteral("v1"));
        ASSERT_TRUE(ok(r)) << words(r);
        r = b.keepSignalText(
            QString::fromUtf8(
                signalFile(QStringLiteral("Two"), QStringLiteral("D101"))),
            str(r, "sha256"), QStringLiteral("two"), QString());
        ASSERT_TRUE(ok(r)) << words(r);
        ASSERT_EQ(str(r, "version").toStdString(), "1.1.0");
        desktopSha = str(r, "sha256");
        ASSERT_TRUE(ok(b.exportPlugin(desktopSha, firstUrl)));
    }
    const QByteArray v110 = readBytes(firstUrl.toLocalFile());
    ASSERT_FALSE(v110.isEmpty());
    {
        phone.enter();
        SessionBridge b;
        const QVariantMap r = b.readImport(firstUrl);
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "kind").toStdString(), "plugin");
        EXPECT_EQ(str(r, "version").toStdString(), "1.1.0");
        EXPECT_FALSE(r.value(QStringLiteral("mine")).toBool())
            << "nothing of the first device's index travels";
        ASSERT_TRUE(ok(b.markPluginMine(str(r, "sha256"))));
    }
    {
        // A fact the phone holds of it, to be kept through the update
        QJsonObject index =
            QJsonDocument::fromJson(readBytes(indexPath())).object();
        QJsonObject plugins = index.value(QStringLiteral("plugins")).toObject();
        QJsonObject facts = plugins.value(id).toObject();
        ASSERT_TRUE(facts.value(QStringLiteral("mine")).toBool());
        facts.insert(QStringLiteral("importedFor"), QStringLiteral("v9"));
        plugins.insert(id, facts);
        index.insert(QStringLiteral("plugins"), plugins);
        QFile f(indexPath());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(index).toJson());
    }
    {
        desktop.enter();
        SessionBridge b;
        QVariantMap r = b.keepSignalText(
            QString::fromUtf8(
                signalFile(QStringLiteral("Three"), QStringLiteral("D102"))),
            desktopSha, QStringLiteral("three"), QString());
        ASSERT_TRUE(ok(r)) << words(r);
        r = b.dropPluginVariant(str(r, "sha256"), QStringLiteral("two"));
        ASSERT_TRUE(ok(r)) << words(r);
        ASSERT_EQ(str(r, "version").toStdString(), "1.3.0");
        ASSERT_TRUE(ok(b.exportPlugin(str(r, "sha256"), secondUrl)));
    }
    const QByteArray v130 = readBytes(secondUrl.toLocalFile());
    ASSERT_FALSE(v130.isEmpty());

    phone.enter();
    Logged b;
    const QStringList picks = {QStringLiteral("plugin.trip-extras.two"),
                               QStringLiteral("plugin.trip-extras.base")};
    QVariantMap r = b.bridge.readImport(secondUrl);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "updated").toStdString(), "1.1.0");
    EXPECT_EQ(str(r, "version").toStdString(), "1.3.0");
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "importedFor").toStdString(), "v9");
    EXPECT_EQ(copies(), QStringList{sha256Of(v130) + kSuffix});
    EXPECT_EQ(factsOf(id), (QJsonObject{{QStringLiteral("mine"), true},
                                        {QStringLiteral("importedFor"),
                                         QStringLiteral("v9")}}));
    EXPECT_TRUE(b.log.contains(
        QStringLiteral("Plugin updated: Trip extras from 1.1.0 to 1.3.0 "
                       "(plugin.trip-extras.base, plugin.trip-extras.three)")))
        << b.lines();
    const QVariantList row = b.bridge.pickRow(picks, false);
    ASSERT_EQ(row.size(), 2);
    EXPECT_TRUE(row.at(0).toMap().value(QStringLiteral("missing")).toBool())
        << "the variant the new version dropped";
    EXPECT_FALSE(row.at(1).toMap().value(QStringLiteral("missing")).toBool());

    const auto unchanged = [&](const char* what) {
        const QVariantList all = b.bridge.plugins();
        ASSERT_EQ(all.size(), 1) << what;
        EXPECT_EQ(str(all.at(0).toMap(), "version").toStdString(), "1.3.0")
            << what;
        EXPECT_EQ(copies(), QStringList{sha256Of(v130) + kSuffix}) << what;
        EXPECT_TRUE(registered(QStringLiteral("plugin.trip-extras.three")))
            << what;
    };

    r = b.bridge.readImport(secondUrl);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "updated"), QString()) << "the same bytes update nothing";
    unchanged("the same bytes");

    r = b.bridge.readImport(firstUrl);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r),
              "This file holds Trip extras 1.1.0, older than yours at 1.3.0");
    unchanged("a lower version");

    const auto read = [&](const char* name, const QByteArray& bytes) {
        return b.bridge.readImport(
            writeScratch(files.path(), QString::fromLatin1(name), bytes));
    };
    QByteArray apart = v130;
    apart.replace("\"name\": \"Trip extras\"", "\"name\": \"Trip extras B\"");
    ASSERT_NE(apart, v130);
    r = read("apart.json", apart);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Your Trip extras is at 1.3.0 and this file holds "
                        "1.3.0 with other content: the two were changed "
                        "apart, so keep one of them");
    unchanged("the same version, other bytes");

    const std::string reused =
        "This package has the id of your own plugin Trip extras 1.3.0 "
        "(trip-extras) and isn't a later version of it: remove yours first";
    QByteArray higher = v130;
    higher.replace("\"version\": \"1.3.0\"", "\"version\": \"9.0.0\"");
    ASSERT_NE(higher, v130);
    QString text = QString::fromUtf8(higher);
    const QRegularExpression lineage(
        QStringLiteral("\\s*\"lineage\": \"[0-9a-f]{32}\","));
    ASSERT_TRUE(lineage.match(text).hasMatch());
    text.remove(lineage);
    r = read("no-lineage.json", text.toUtf8());
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), reused);
    unchanged("no lineage");

    text = QString::fromUtf8(higher);
    text.replace(
        QRegularExpression(QStringLiteral("\"lineage\": \"[0-9a-f]{32}\"")),
        QStringLiteral("\"lineage\": \"%1\"")
            .arg(QString(32, QLatin1Char('0'))));
    ASSERT_NE(text.toUtf8(), higher);
    r = read("other-lineage.json", text.toUtf8());
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), reused);
    unchanged("another lineage");

    // The later version it is, once more to be sure nothing above stuck
    r = read("higher.json", higher);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "updated").toStdString(), "1.3.0");
    EXPECT_EQ(str(r, "version").toStdString(), "9.0.0");

    // Versions compare as numbers: 10 is later than 9
    QByteArray ten = v130;
    ten.replace("\"version\": \"1.3.0\"", "\"version\": \"10.0.0\"");
    r = read("ten.json", ten);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "updated").toStdString(), "9.0.0");
    EXPECT_EQ(str(r, "version").toStdString(), "10.0.0");

    // Where the plugin isn't the user's own, a package of its id takes
    // its place whatever its version
    SettingsDir other;
    ASSERT_TRUE(other.valid());
    SessionBridge c;
    ASSERT_TRUE(ok(c.readImport(secondUrl)));
    r = c.readImport(firstUrl);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "replaced").toStdString(), "1.3.0");
    EXPECT_EQ(str(r, "updated"), QString());
    EXPECT_EQ(str(r, "version").toStdString(), "1.1.0");
}

/* Export plugin writes the store's copy byte for byte, for anyone
 * else's plugin and for the user's own; a plugin made for a vehicle
 * with a VIN holds no VIN, no hash of it, no match, no variant named by
 * its year and nothing of the store's index */
TEST(BridgeTest, OneStoreAnExportIsTheStoresCopyAndHoldsNothingOfTheDevice)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir out;
    Logged b;
    const QByteArray example = packed_plugin::example();
    QVariantMap r = b.bridge.readImport(
        writeScratch(out.path(), QStringLiteral("in.json"), example));
    ASSERT_TRUE(ok(r)) << words(r);
    const QString theirs = str(r, "sha256");
    const QUrl theirUrl = QUrl::fromLocalFile(
        out.path() + QStringLiteral("/example-sim.havne-plugin.json"));
    r = b.bridge.exportPlugin(theirs, theirUrl);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(readBytes(theirUrl.toLocalFile()), example);
    EXPECT_EQ(readBytes(theirUrl.toLocalFile()), readBytes(copyPath(theirs)));
    EXPECT_TRUE(
        b.log.contains(QStringLiteral("Plugin %1 exported to a file (%2 bytes)")
                           .arg(theirs.left(12))
                           .arg(example.size())))
        << b.lines();

    // The user's own, from an OBDb set at the year a VIN decodes to
    const QString vin = QStringLiteral("ZZZFAKE11M0000001");
    const int year = b.bridge.vinModelYear(vin);
    ASSERT_EQ(year, 2021);
    r = keepObdb(b.bridge, year, QStringLiteral("Daily driver OBDb"),
                 QStringLiteral("v7"), false);
    ASSERT_TRUE(ok(r)) << words(r);
    const QString own = str(r, "sha256");
    // The VIN's matches, worked out as the vehicle's list does
    EXPECT_FALSE(
        b.bridge.offeredVariants(QStringLiteral("v7"), vin, {}).isEmpty());
    const QUrl ownUrl = QUrl::fromLocalFile(
        out.path() + QStringLiteral("/daily-driver-obdb.havne-plugin.json"));
    r = b.bridge.exportPlugin(own, ownUrl);
    ASSERT_TRUE(ok(r)) << words(r);
    const QByteArray bytes = readBytes(ownUrl.toLocalFile());
    ASSERT_FALSE(bytes.isEmpty());
    EXPECT_EQ(bytes, readBytes(copyPath(own)));
    const QString salt = b.bridge.newSalt();
    const QByteArray hash = QCryptographicHash::hash((salt + vin).toUtf8(),
                                                     QCryptographicHash::Sha256)
                                .toHex();
    for (const QByteArray& gone :
         {vin.toUtf8(), vin.toLower().toUtf8(), vin.left(3).toUtf8(), hash,
          QByteArray("\"match\""), QByteArray("my20"), QByteArray("mine"),
          QByteArray("importedFor"), QByteArray("wrappedFrom"),
          QByteArray("\"v7\"")}) {
        EXPECT_FALSE(bytes.contains(gone)) << gone.toStdString();
    }
    const QJsonObject manifest = QJsonDocument::fromJson(bytes).object();
    EXPECT_FALSE(manifest.contains(QStringLiteral("match")));
    EXPECT_EQ(manifest.value(QStringLiteral("variants"))
                  .toArray()
                  .at(0)
                  .toObject()
                  .value(QStringLiteral("id"))
                  .toString()
                  .toStdString(),
              "base");
    EXPECT_FALSE(factsOf(QStringLiteral("daily-driver-obdb")).isEmpty())
        << "the facts are in the index, where they stay";

    // Refusals: no such plugin, a copy changed since, a file that can't
    // be written; none writes a file or names a path
    const QString nowhere = out.path() + QStringLiteral("/none.json");
    r = b.bridge.exportPlugin(QString(64, QLatin1Char('0')),
                              QUrl::fromLocalFile(nowhere));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The store has no such plugin");
    EXPECT_FALSE(QFile::exists(nowhere));

    r = b.bridge.exportPlugin(own, QUrl::fromLocalFile(out.path()));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Couldn't write the file");

    {
        QFile copy(copyPath(theirs));
        ASSERT_TRUE(copy.open(QIODevice::WriteOnly));
        copy.write(QByteArray(example).replace("Oil Temp", "Oil Tamp"));
    }
    r = b.bridge.exportPlugin(theirs, QUrl::fromLocalFile(nowhere));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Its copy has changed: import the plugin again");
    EXPECT_FALSE(QFile::exists(nowhere));
    for (const QString& line : std::as_const(b.log)) {
        EXPECT_FALSE(line.contains(out.path())) << line.toStdString();
    }
}

/* A vehicle's picks are an ordered list: a pick goes last, one picked
 * again after it was taken out goes last too, 16 at most, and only a
 * variant a plugin here has. On an adapter the first pick that extends
 * obd2 stands in for Default. The vehicle's list offers every variant,
 * the VIN's matches and the plugins imported for it first */
TEST(BridgeTest, OneStoreAVehiclesPicksKeepTheirOrderAndTheFirstObd2OneStandsIn)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    Logged b;
    ASSERT_TRUE(ok(b.bridge.readImport(writeScratch(
        from.path(), QStringLiteral("a.json"), packed_plugin::example()))));
    const auto made = [&](const char* name, const char* did, bool obd2) {
        const QVariantMap r = b.bridge.keepSignalText(
            QString::fromUtf8(signalFile(QString::fromLatin1(name),
                                         QString::fromLatin1(did), obd2)),
            QString(), QString(), QString());
        EXPECT_TRUE(ok(r)) << words(r);
        return list(r, "profiles").value(0);
    };
    const QString plain = made("Plain", "D100", false);
    const QString second = made("Second obd2", "D101", true);
    const QString first = made("First obd2", "D102", true);
    ASSERT_EQ(plain.toStdString(), "plugin.plain.base");

    QVariantMap p = b.bridge.pickVariant({}, plain);
    ASSERT_TRUE(ok(p)) << words(p);
    p = b.bridge.pickVariant(list(p, "picks"), second);
    p = b.bridge.pickVariant(list(p, "picks"), first);
    ASSERT_TRUE(ok(p)) << words(p);
    const QStringList picks = list(p, "picks");
    EXPECT_EQ(picks, (QStringList{plain, second, first}));
    p = b.bridge.pickVariant(picks, second);
    EXPECT_TRUE(ok(p));
    EXPECT_EQ(list(p, "picks"), picks) << "picked already: as it was";

    p = b.bridge.pickVariant(picks, QStringLiteral("plugin.gone.base"));
    EXPECT_FALSE(ok(p));
    EXPECT_EQ(words(p), "No plugin here has that variant");
    EXPECT_EQ(list(p, "picks"), picks);

    const QStringList without = b.bridge.unpickVariant(picks, second);
    EXPECT_EQ(without, (QStringList{plain, first}));
    p = b.bridge.pickVariant(without, second);
    EXPECT_EQ(list(p, "picks"), (QStringList{plain, first, second}))
        << "picked again: last";

    QStringList full;
    for (int i = 0; i < PluginStore::MaxPicks; ++i) {
        full << QStringLiteral("plugin.gone.v%1").arg(i);
    }
    p = b.bridge.pickVariant(full, plain);
    EXPECT_FALSE(ok(p));
    EXPECT_EQ(words(p),
              "A vehicle picks at most 16 variants: unpick one first");
    EXPECT_EQ(list(p, "picks"), full);
    p = b.bridge.pickVariant(full.mid(1), plain);
    EXPECT_TRUE(ok(p)) << words(p);
    EXPECT_EQ(list(p, "picks").size(), PluginStore::MaxPicks);
    EXPECT_EQ(list(p, "picks").last(), plain);

    // The row: only the first obd2 pick stands in, and only on an adapter
    const QStringList shown = {plain, second, first,
                               QStringLiteral("plugin.gone.base")};
    QVariantList row = b.bridge.pickRow(shown, true);
    EXPECT_EQ(column(row, "profile"), shown);
    EXPECT_EQ(column(row, "standIn"),
              (QStringList{QStringLiteral("false"), QStringLiteral("true"),
                           QStringLiteral("false"), QStringLiteral("false")}));
    EXPECT_EQ(column(row, "missing"),
              (QStringList{QStringLiteral("false"), QStringLiteral("false"),
                           QStringLiteral("false"), QStringLiteral("true")}));
    EXPECT_EQ(column(row, "extends"),
              (QStringList{QString(), QStringLiteral("obd2"),
                           QStringLiteral("obd2"), QString()}));
    EXPECT_EQ(
        column(row, "name"),
        (QStringList{QStringLiteral("Plain"), QStringLiteral("Second obd2"),
                     QStringLiteral("First obd2"), QString()}));
    EXPECT_EQ(column(row, "variant"),
              (QStringList{QStringLiteral("base"), QStringLiteral("base"),
                           QStringLiteral("base"), QString()}));
    row = b.bridge.pickRow(shown, false);
    EXPECT_FALSE(column(row, "standIn").contains(QStringLiteral("true")))
        << "off an adapter Default isn't obd2";
    row = b.bridge.pickRow({first, second}, true);
    EXPECT_EQ(column(row, "standIn"),
              (QStringList{QStringLiteral("true"), QStringLiteral("false")}))
        << "the order picked decides";

    // The vehicle's list
    const QVariantMap obdb =
        keepObdb(b.bridge, 2019, QStringLiteral("Daily driver OBDb"),
                 QStringLiteral("v1"), true);
    ASSERT_TRUE(ok(obdb)) << words(obdb);
    const QString imported = QStringLiteral("plugin.daily-driver-obdb.my2019");
    const QString base = QStringLiteral("plugin.example-sim.base");
    const QString my2019 = QStringLiteral("plugin.example-sim.my2019");
    const QStringList byName = {imported, base, my2019, first, plain, second};

    QVariantList offered =
        b.bridge.offeredVariants(QStringLiteral("v1"), QString(), {second});
    EXPECT_EQ(column(offered, "profile"), byName);
    EXPECT_EQ(column(offered, "forThisCar"),
              (QStringList{QStringLiteral("true"), QStringLiteral("false"),
                           QStringLiteral("false"), QStringLiteral("false"),
                           QStringLiteral("false"), QStringLiteral("false")}));
    EXPECT_EQ(column(offered, "picked"),
              (QStringList{QStringLiteral("false"), QStringLiteral("false"),
                           QStringLiteral("false"), QStringLiteral("false"),
                           QStringLiteral("false"), QStringLiteral("true")}));
    EXPECT_EQ(column(offered, "mine"),
              (QStringList{QStringLiteral("true"), QStringLiteral("false"),
                           QStringLiteral("false"), QStringLiteral("true"),
                           QStringLiteral("true"), QStringLiteral("true")}));
    EXPECT_EQ(column(offered, "extends").value(4), QString());
    EXPECT_EQ(column(offered, "extends").value(5).toStdString(), "obd2");
    EXPECT_EQ(column(offered, "variant").value(0).toStdString(), "my2019");
    EXPECT_EQ(column(offered, "name").value(1).toStdString(),
              "Example: the simulated car");

    // The VIN's matches join the imported one, each group by name
    offered = b.bridge.offeredVariants(QStringLiteral("v2"),
                                       QStringLiteral("ZZZFAKE11M0000001"), {});
    EXPECT_EQ(column(offered, "profile"),
              (QStringList{base, my2019, imported, first, plain, second}))
        << "another vehicle: the import isn't its own";
    EXPECT_EQ(column(offered, "forThisCar").mid(0, 3),
              (QStringList{QStringLiteral("true"), QStringLiteral("true"),
                           QStringLiteral("false")}));
    offered = b.bridge.offeredVariants(QStringLiteral("v1"),
                                       QStringLiteral("ZZZFAKE11M0000001"), {});
    EXPECT_EQ(column(offered, "profile"), byName);
    EXPECT_EQ(column(offered, "forThisCar").mid(0, 4),
              (QStringList{QStringLiteral("true"), QStringLiteral("true"),
                           QStringLiteral("true"), QStringLiteral("false")}));

    // No vehicle: nothing is its own
    offered = b.bridge.offeredVariants(QString(), QString(), {});
    EXPECT_EQ(column(offered, "profile"), byName);
    EXPECT_FALSE(
        column(offered, "forThisCar").contains(QStringLiteral("true")));
}

/* The index is the store's own file: it keeps keys it doesn't know,
 * holds the migration's mark, maps a wrapped file to its plugin, is no
 * copy to an older store's listing, and goes with its last fact; a
 * plugin's facts go with the plugin, a copy that no longer loads too */
TEST(BridgeTest, OneStoreTheIndexKeepsWhatItDoesntKnowAndGoesWithItsLastFact)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray file = signalFile(QStringLiteral("Kept"));
    const QString id = QStringLiteral("kept");
    {
        PluginStore store;
        EXPECT_FALSE(store.migrated());
        EXPECT_TRUE(store.wrappedFrom(sha256Of(file)).isEmpty());
        const QVariantMap r = store.wrap(file, {QStringLiteral("v1"), {}});
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(
            store.wrappedFrom(sha256Of(file)),
            (QVariantList{QVariantMap{
                {QStringLiteral("id"), id},
                {QStringLiteral("variant"), QStringLiteral("base")},
                {QStringLiteral("profile"), QStringLiteral("plugin.kept.base")},
                {QStringLiteral("wrapped"), true}}}));
        EXPECT_TRUE(store.wrappedFrom(sha256Of("another")).isEmpty());
        ASSERT_TRUE(QFile::exists(indexPath()));
        ASSERT_TRUE(ok(store.remove(str(r, "sha256"))));
        EXPECT_TRUE(store.wrappedFrom(sha256Of(file)).isEmpty());
        EXPECT_FALSE(QFile::exists(indexPath()))
            << "an index with nothing to say leaves no file";
        EXPECT_FALSE(QDir(PluginStore::root()).exists())
            << "the empty folder goes too";
    }

    // The mark alone is something to say: the index stays for it
    {
        PluginStore store;
        ASSERT_TRUE(store.setMigrated());
        EXPECT_TRUE(QFile::exists(indexPath()));
    }
    {
        PluginStore store;
        EXPECT_TRUE(store.migrated());
        EXPECT_TRUE(store.all().isEmpty());
    }

    // Keys from a later Havne stay as they are
    ASSERT_TRUE(QDir().mkpath(PluginStore::root()));
    {
        QFile f(indexPath());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(R"({"format":1,"later":{"x":1},)"
                R"("plugins":{"ghost":{"mine":true,"note":"kept"}}})");
    }
    QString sha;
    {
        PluginStore store;
        const QVariantMap r = store.wrap(file, {});
        ASSERT_TRUE(ok(r)) << words(r);
        sha = str(r, "sha256");
        EXPECT_EQ(store.all().size(), 1) << "the index is no copy";
        QJsonObject index =
            QJsonDocument::fromJson(readBytes(indexPath())).object();
        EXPECT_EQ(index.value(QStringLiteral("later"))
                      .toObject()
                      .value(QStringLiteral("x"))
                      .toInt(),
                  1);
        EXPECT_EQ(
            factsOf(QStringLiteral("ghost")),
            (QJsonObject{{QStringLiteral("mine"), true},
                         {QStringLiteral("note"), QStringLiteral("kept")}}));
        EXPECT_EQ(factsOf(id),
                  (QJsonObject{
                      {QStringLiteral("mine"), true},
                      {QStringLiteral("wrappedFrom"),
                       QJsonObject{{sha256Of(file), QStringLiteral("base")}}}}))
            << "no vehicle given, none written";

        EXPECT_FALSE(store.migrated());
        ASSERT_TRUE(store.setMigrated());
        EXPECT_TRUE(store.migrated());
        index = QJsonDocument::fromJson(readBytes(indexPath())).object();
        EXPECT_EQ(index.value(QStringLiteral("migrated")).toInt(), 1);
        EXPECT_FALSE(factsOf(id).isEmpty()) << "the mark changes no fact";
    }
    {
        PluginStore store;
        EXPECT_TRUE(store.migrated()) << "read back by a later start";
        const QVariantList all = store.all();
        ASSERT_EQ(all.size(), 1);
        EXPECT_TRUE(all.at(0).toMap().value(QStringLiteral("mine")).toBool());
    }

    // A copy changed since still says whose facts go with it
    {
        QFile copy(copyPath(sha));
        ASSERT_TRUE(copy.open(QIODevice::ReadWrite));
        QByteArray bytes = copy.readAll();
        bytes.replace("\"name\": \"Kept\"", "\"name\": \"Kopt\"");
        copy.resize(0);
        copy.write(bytes);
    }
    {
        PluginStore store;
        const QVariantList all = store.all();
        ASSERT_EQ(all.size(), 1);
        EXPECT_FALSE(ok(all.at(0).toMap()));
        EXPECT_EQ(words(all.at(0).toMap()),
                  "Its copy has changed: import the plugin again");
        ASSERT_TRUE(ok(store.remove(sha)));
        EXPECT_TRUE(factsOf(id).isEmpty());
        EXPECT_FALSE(factsOf(QStringLiteral("ghost")).isEmpty())
            << "another plugin's facts stay";
        EXPECT_TRUE(store.migrated());
    }

    // A fact another running app wrote since this one read the index
    // is kept by this one's next change
    PluginStore store;
    const QVariantMap r = store.wrap(file, {});
    ASSERT_TRUE(ok(r)) << words(r);
    {
        QJsonObject index =
            QJsonDocument::fromJson(readBytes(indexPath())).object();
        QJsonObject plugins = index.value(QStringLiteral("plugins")).toObject();
        plugins.insert(QStringLiteral("elsewhere"),
                       QJsonObject{{QStringLiteral("mine"), true}});
        plugins.remove(QStringLiteral("ghost"));
        index.insert(QStringLiteral("plugins"), plugins);
        index.remove(QStringLiteral("migrated"));
        QFile f(indexPath());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(index).toJson());
    }
    ASSERT_TRUE(ok(store.markMine(str(r, "sha256"))));
    EXPECT_FALSE(factsOf(QStringLiteral("elsewhere")).isEmpty());
    EXPECT_TRUE(factsOf(QStringLiteral("ghost")).isEmpty())
        << "what the other removed stays removed";
    EXPECT_FALSE(factsOf(id).isEmpty());
    EXPECT_FALSE(store.migrated()) << "the file as it is now is the base";
}

/* An index the store can't trust is never written over: one that isn't
 * an index (not JSON, not an object, too large) is moved aside whole as
 * store.json.bad-N and a new one started; one that can't be moved, and a
 * later Havne's, is left as it is, and what is marked holds for the
 * running app and is said to the user */
TEST(BridgeTest, OneStoreAnIndexThatCantBeTrustedIsNeverWrittenOver)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    const QString aside = indexPath() + QStringLiteral(".bad-%1");
    const auto writeIndex = [](const QByteArray& bytes) {
        QDir().mkpath(PluginStore::root());
        QFile f(indexPath());
        EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
    };
    const QString moved =
        QStringLiteral("The plugin store's index couldn't be read: it is "
                       "kept as store.json.bad-%1 and a new one is started");
    const QString unwritten = QStringLiteral(
        "The plugin is kept, but the store's index couldn't be written yet: "
        "if no later change writes it before the app closes, after a restart "
        "it isn't marked as yours");

    // A hand edit's trailing comma
    const QByteArray comma =
        R"({"format":1,"migrated":1,"plugins":{"ghost":{"mine":true},}})";
    writeIndex(comma);
    {
        PluginStore store;
        QStringList said;
        store.setLog([&said](const QString& line) { said << line; });
        EXPECT_FALSE(store.migrated());
        EXPECT_EQ(readBytes(aside.arg(1)), comma);
        EXPECT_FALSE(QFile::exists(indexPath()));
        EXPECT_EQ(said, QStringList{moved.arg(1)});
        const QVariantMap r =
            store.wrap(signalFile(QStringLiteral("Kept")), {});
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(words(r), "");
        EXPECT_FALSE(factsOf(QStringLiteral("kept")).isEmpty());
        EXPECT_TRUE(factsOf(QStringLiteral("ghost")).isEmpty());
        EXPECT_EQ(readBytes(aside.arg(1)), comma)
            << "whole after the new index was written";
        EXPECT_EQ(said.size(), 1);
    }

    // Each is no index: the next free name takes it, every byte kept
    const QByteArray good =
        R"({"format":1,"migrated":1,"plugins":{"ghost":{"mine":true}}})";
    const qsizetype most = 4 * 1024 * 1024;
    QByteArray large = good;
    large.insert(1, QByteArray(most + 1 - good.size(), ' '));
    ASSERT_EQ(large.size(), most + 1);
    const QByteArray bad[] = {QByteArray(), QByteArray("[1]"),
                              QByteArray(R"({"format":1,"plugins":5})"), large};
    int n = 1;
    for (const QByteArray& bytes : bad) {
        ++n;
        writeIndex(bytes);
        PluginStore store;
        EXPECT_FALSE(store.migrated()) << n;
        EXPECT_TRUE(QFile::exists(aside.arg(n))) << n;
        EXPECT_EQ(readBytes(aside.arg(n)), bytes) << n;
        EXPECT_FALSE(QFile::exists(indexPath())) << n;
    }

    // At its bound an index is read
    large.remove(1, 1);
    ASSERT_EQ(large.size(), most);
    writeIndex(large);
    {
        PluginStore store;
        EXPECT_TRUE(store.migrated());
        EXPECT_EQ(readBytes(indexPath()).size(), most);
        EXPECT_FALSE(QFile::exists(aside.arg(n + 1)));
    }

    // No name left to move it to: the file stays as it is, the plugin is
    // the user's own while the app runs, and the user is told
    for (int i = 1; i <= 99; ++i) {
        if (!QFile::exists(aside.arg(i))) {
            QFile f(aside.arg(i));
            ASSERT_TRUE(f.open(QIODevice::WriteOnly));
            f.write("x");
        }
    }
    writeIndex(comma);
    {
        Logged b;
        const QString stuck = QStringLiteral(
            "The plugin store's index can't be read or moved aside: it is "
            "left as it is, and what is marked here holds until the app "
            "closes");
        QVariantMap r = b.bridge.keepSignalText(
            QString::fromUtf8(signalFile(QStringLiteral("Stuck"))), QString(),
            QString(), QStringLiteral("v1"));
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "words"), unwritten);
        EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
        EXPECT_EQ(str(r, "importedFor").toStdString(), "v1");
        EXPECT_EQ(readBytes(indexPath()), comma) << "never written over";
        EXPECT_FALSE(QFile::exists(aside.arg(100)));
        EXPECT_TRUE(b.log.contains(
            QStringLiteral("Plugin made: Stuck 1.0.0 (plugin.stuck.base); ") +
            unwritten))
            << b.lines();
        r = b.bridge.keepSignalText(
            QString::fromUtf8(signalFile(QStringLiteral("Stuck two"))),
            QString(), QString(), QString());
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "words"), unwritten);
        EXPECT_EQ(b.log.count(stuck), 1) << b.lines();
        EXPECT_EQ(readBytes(indexPath()), comma);
        EXPECT_TRUE(b.bridge.plugins()
                        .at(1)
                        .toMap()
                        .value(QStringLiteral("mine"))
                        .toBool())
            << "the first is still the user's own";

        // A name freed: the next change moves the file there and writes
        // what the running app held
        ASSERT_TRUE(QFile::remove(aside.arg(50)));
        ASSERT_TRUE(ok(b.bridge.markPluginMine(str(r, "sha256"))));
        EXPECT_EQ(readBytes(aside.arg(50)), comma);
        EXPECT_TRUE(b.log.contains(moved.arg(50))) << b.lines();
        EXPECT_EQ(factsOf(QStringLiteral("stuck"))
                      .value(QStringLiteral("importedFor"))
                      .toString()
                      .toStdString(),
                  "v1");
        EXPECT_FALSE(factsOf(QStringLiteral("stuck-two")).isEmpty());
    }

    // A later Havne's index is read and left as it is
    const QByteArray later = R"({"format":2,"shape":"new",)"
                             R"("plugins":{"stuck":{"mine":true,"x":1}}})";
    writeIndex(later);
    Logged b;
    const QVariantList all = b.bridge.plugins();
    ASSERT_EQ(column(all, "id"),
              (QStringList{QStringLiteral("kept"), QStringLiteral("stuck"),
                           QStringLiteral("stuck-two")}));
    EXPECT_EQ(column(all, "mine"),
              (QStringList{QStringLiteral("false"), QStringLiteral("true"),
                           QStringLiteral("false")}))
        << "read for what this version knows";
    EXPECT_TRUE(b.log.contains(QStringLiteral(
        "The plugin store's index is a later Havne's: it is read and left "
        "as it is, so what is marked here holds until the app closes")))
        << b.lines();
    QVariantMap r = b.bridge.keepSignalText(
        QString::fromUtf8(signalFile(QStringLiteral("Later"))), QString(),
        QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "words"), unwritten);
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    const QString kept =
        all.at(0).toMap().value(QStringLiteral("sha256")).toString();
    r = b.bridge.markPluginMine(kept);
    EXPECT_TRUE(ok(r));
    EXPECT_EQ(words(r), "It is marked as yours, but the store's index "
                        "couldn't be written yet: if no later change "
                        "writes it before the app closes, after a "
                        "restart it isn't marked as yours");
    EXPECT_TRUE(b.log.contains(
        QStringLiteral("Plugin %1 marked as yours; It is marked as yours, but "
                       "the store's index couldn't be written yet: if no "
                       "later change writes it before the app closes, after "
                       "a restart it isn't marked as yours")
            .arg(kept.left(12))))
        << b.lines();
    EXPECT_EQ(column(b.bridge.plugins(), "mine"),
              (QStringList{QStringLiteral("true"), QStringLiteral("true"),
                           QStringLiteral("true"), QStringLiteral("false")}))
        << "each mark holds while the app runs: Kept, Later, Stuck";
    EXPECT_EQ(readBytes(indexPath()), later);
}

/* An OBDb set imported again for a vehicle whose earlier conversion the
 * user has added variants to joins them as one more: nothing of the
 * user's goes, the lineage stays and the version rises. The conversion
 * it already holds changes nothing */
TEST(BridgeTest, OneStoreAnObdbReimportKeepsTheVariantsTheUserAdded)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    Logged b;
    const QString name = QStringLiteral("Daily driver OBDb");
    const QString v1 = QStringLiteral("v1");
    const QString id = QStringLiteral("daily-driver-obdb");
    const QString my2019 = QStringLiteral("plugin.daily-driver-obdb.my2019");
    const QString my2021 = QStringLiteral("plugin.daily-driver-obdb.my2021");
    const QString track = QStringLiteral("plugin.daily-driver-obdb.track");
    QVariantMap r = keepObdb(b.bridge, 2019, name, v1, true);
    ASSERT_TRUE(ok(r)) << words(r);
    const QString lineage = str(r, "lineage");
    ASSERT_EQ(lineage.size(), 32);
    const QByteArray own =
        signalFile(QStringLiteral("Track day"), QStringLiteral("D150"));
    r = b.bridge.keepSignalText(QString::fromUtf8(own), str(r, "sha256"),
                                QStringLiteral("track"), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    ASSERT_EQ(str(r, "version").toStdString(), "1.1.0");
    ASSERT_TRUE(r.value(QStringLiteral("obdb")).toBool())
        << "still a conversion to the store";

    r = keepObdb(b.bridge, 2021, name, v1, true);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("merged")).toBool());
    EXPECT_EQ(str(r, "id"), id);
    EXPECT_EQ(list(r, "variants"),
              (QStringList{QStringLiteral("my2019"), QStringLiteral("track"),
                           QStringLiteral("my2021")}));
    EXPECT_EQ(str(r, "version").toStdString(), "1.2.0");
    EXPECT_EQ(str(r, "lineage"), lineage)
        << "another device's copy still updates from this one";
    EXPECT_EQ(str(r, "replaced").toStdString(), "1.1.0");
    EXPECT_EQ(list(r, "replacedProfiles"), (QStringList{my2019, track}));
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(str(r, "importedFor"), v1);
    EXPECT_EQ(b.bridge.plugins().size(), 1);
    EXPECT_EQ(copies(), QStringList{str(r, "sha256") + kSuffix});
    EXPECT_TRUE(registered(my2019));
    EXPECT_TRUE(registered(track)) << "the user's variant is still there";
    EXPECT_TRUE(registered(my2021));
    QStringList made;
    const QJsonObject from = madeFrom(id);
    for (auto it = from.begin(); it != from.end(); ++it) {
        made << it.value().toString();
    }
    made.sort();
    EXPECT_EQ(made,
              (QStringList{QStringLiteral("my2019"), QStringLiteral("my2021"),
                           QStringLiteral("track")}));
    EXPECT_EQ(from.value(sha256Of(own)).toString().toStdString(), "track");
    EXPECT_TRUE(b.log.last().startsWith(QStringLiteral(
        "Plugin from OBDb: Daily driver OBDb 1.2.0 "
        "(plugin.daily-driver-obdb.my2019, plugin.daily-driver-obdb.track, "
        "plugin.daily-driver-obdb.my2021), as a variant of the earlier "
        "import")))
        << b.lines();
    const QString merged = str(r, "sha256");

    // The conversion it holds, again: nothing changes, and it is said
    for (const int year : {2021, 2019}) {
        r = keepObdb(b.bridge, year, name, v1, true);
        ASSERT_TRUE(ok(r)) << words(r) << year;
        EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool()) << year;
        EXPECT_FALSE(r.value(QStringLiteral("merged")).toBool()) << year;
        EXPECT_EQ(str(r, "sha256"), merged) << year;
        EXPECT_EQ(str(r, "version").toStdString(), "1.2.0") << year;
        EXPECT_EQ(words(r), "This file made your Daily driver OBDb "
                            "(daily-driver-obdb), now at 1.2.0: nothing new "
                            "was kept")
            << year;
    }
    EXPECT_EQ(b.log.last().toStdString(),
              "Plugin from OBDb: Daily driver OBDb 1.2.0 "
              "(plugin.daily-driver-obdb.my2019, "
              "plugin.daily-driver-obdb.track, "
              "plugin.daily-driver-obdb.my2021); This file made your Daily "
              "driver OBDb (daily-driver-obdb), now at 1.2.0: nothing new was "
              "kept");
    EXPECT_EQ(copies(), QStringList{merged + kSuffix});

    // The year again as another variant id joins too
    r = keepObdb(b.bridge, 2021, name, v1, false);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("merged")).toBool());
    EXPECT_EQ(list(r, "variants").last().toStdString(), "base");
    EXPECT_EQ(list(r, "variants").size(), 4);
    EXPECT_EQ(str(r, "version").toStdString(), "1.3.0");
}

/* A file that made a plugin of the user's own finds that plugin however
 * it has changed since, and makes no second one; a file only given to a
 * plugin as a variant still makes a plugin of its own. The index maps a
 * file to every variant made from it, the plugin it made first */
TEST(BridgeTest, OneStoreAFileFindsThePluginItMadeHoweverItHasChanged)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray file = signalFile(QStringLiteral("Extras"));
    const QByteArray hill =
        signalFile(QStringLiteral("Hill climb"), QStringLiteral("D160"));
    {
        Logged b;
        const auto keep = [&b](const QByteArray& f, const QString& into,
                               const QString& variant) {
            return b.bridge.keepSignalText(QString::fromUtf8(f), into, variant,
                                           QString());
        };
        QVariantMap r = keep(file, QString(), QString());
        ASSERT_TRUE(ok(r)) << words(r);
        r = keep(hill, str(r, "sha256"), QStringLiteral("hill"));
        ASSERT_TRUE(ok(r)) << words(r);
        const QString changed = str(r, "sha256");

        r = keep(file, QString(), QString());
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
        EXPECT_EQ(str(r, "sha256"), changed);
        EXPECT_EQ(str(r, "id").toStdString(), "extras");
        EXPECT_EQ(str(r, "version").toStdString(), "1.1.0");
        EXPECT_EQ(words(r), "This file made your Extras (extras), now at "
                            "1.1.0: nothing new was kept");
        EXPECT_EQ(b.bridge.plugins().size(), 1) << "no extras-2";
        EXPECT_EQ(copies(), QStringList{changed + kSuffix});
        EXPECT_EQ(b.log.last().toStdString(),
                  "Plugin already here: Extras 1.1.0 (plugin.extras.base, "
                  "plugin.extras.hill); This file made your Extras (extras), "
                  "now at 1.1.0: nothing new was kept");

        // Given to a plugin as a variant, and now a plugin of its own
        r = keep(hill, QString(), QString());
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
        EXPECT_EQ(str(r, "id").toStdString(), "hill-climb");
        EXPECT_EQ(words(r), "");
        EXPECT_EQ(b.bridge.plugins().size(), 2);
        EXPECT_EQ(b.log.last().toStdString(),
                  "Plugin made: Hill climb 1.0.0 (plugin.hill-climb.base)");
    }
    {
        PluginStore store;
        const auto row = [](const char* id, const char* variant, bool wrapped) {
            return QVariantMap{
                {QStringLiteral("id"), QString::fromLatin1(id)},
                {QStringLiteral("variant"), QString::fromLatin1(variant)},
                {QStringLiteral("profile"),
                 QStringLiteral("plugin.%1.%2")
                     .arg(QString::fromLatin1(id),
                          QString::fromLatin1(variant))},
                {QStringLiteral("wrapped"), wrapped}};
        };
        EXPECT_EQ(store.wrappedFrom(sha256Of(file)),
                  QVariantList{row("extras", "base", true)});
        EXPECT_EQ(store.wrappedFrom(sha256Of(hill)),
                  (QVariantList{row("hill-climb", "base", true),
                                row("extras", "hill", false)}))
            << "the plugin the file made comes before one it was given to";
        EXPECT_TRUE(store.wrappedFrom(sha256Of("neither")).isEmpty());

        // A variant put in the place of the file's own: the plugin holds
        // the file no more, so the file makes a plugin again
        const QVariantList all = store.all();
        ASSERT_EQ(column(all, "id").value(0).toStdString(), "extras");
        QVariantMap r = store.put(
            all.at(0).toMap().value(QStringLiteral("sha256")).toString(),
            QStringLiteral("base"),
            signalFile(QStringLiteral("Other base"), QStringLiteral("D161")));
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_TRUE(store.wrappedFrom(sha256Of(file)).isEmpty());
        r = store.wrap(file, {});
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "id").toStdString(), "extras-2");
        EXPECT_FALSE(r.value(QStringLiteral("kept")).toBool());
    }
}

/* Facts left in the index by a copy that no longer loads are not a new
 * plugin's: a package of that id comes in as someone else's, and a file
 * wrapped under that id starts with facts of its own */
TEST(BridgeTest, OneStoreAPluginThatComesInNewTakesNoFactsLeftByAnother)
{
    const QByteArray file = signalFile(QStringLiteral("Kept"));
    const QString id = QStringLiteral("kept");
    const auto leaveStale = [&]() {
        QString sha;
        {
            PluginStore store;
            const QVariantMap r = store.wrap(file, {QStringLiteral("v1"), {}});
            EXPECT_TRUE(ok(r)) << words(r);
            sha = str(r, "sha256");
        }
        QFile copy(copyPath(sha));
        EXPECT_TRUE(copy.open(QIODevice::ReadWrite));
        QByteArray bytes = copy.readAll();
        bytes.replace("\"name\": \"Kept\"", "\"name\": \"Kopt\"");
        copy.resize(0);
        copy.write(bytes);
        return sha;
    };
    {
        SettingsDir dir;
        ASSERT_TRUE(dir.valid());
        const QString stale = leaveStale();
        PluginStore store;
        const QVariantList all = store.all();
        ASSERT_EQ(all.size(), 1);
        ASSERT_FALSE(ok(all.at(0).toMap()));
        ASSERT_TRUE(factsOf(id).value(QStringLiteral("mine")).toBool());

        // Someone else's package with that id
        const QByteArray theirs = coreWrap(
            signalFile(QStringLiteral("Kept"), QStringLiteral("D177")));
        QVariantMap r = store.import(theirs);
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "id"), id);
        EXPECT_FALSE(r.value(QStringLiteral("mine")).toBool());
        EXPECT_EQ(str(r, "importedFor"), QString());
        EXPECT_TRUE(factsOf(id).isEmpty());
        // Not the user's own, so any package of its id takes its place
        r = store.import(coreWrap(
            signalFile(QStringLiteral("Kept"), QStringLiteral("D178"))));
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "replaced").toStdString(), "1.0.0");

        // The copy that didn't load goes, and takes no other's facts
        ASSERT_TRUE(ok(store.markMine(str(r, "sha256"))));
        ASSERT_TRUE(ok(store.remove(stale)));
        EXPECT_TRUE(factsOf(id).value(QStringLiteral("mine")).toBool());
    }
    {
        SettingsDir dir;
        ASSERT_TRUE(dir.valid());
        (void)leaveStale();
        PluginStore store;
        const QByteArray again =
            signalFile(QStringLiteral("Kept"), QStringLiteral("D179"));
        const QVariantMap r = store.wrap(again, {});
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "id"), id);
        EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
        EXPECT_EQ(str(r, "importedFor"), QString())
            << "the vehicle was the other plugin's";
        EXPECT_EQ(madeFrom(id),
                  (QJsonObject{{sha256Of(again), QStringLiteral("base")}}));
    }
}

/* A file is found by its bytes too: when the index has forgotten what a
 * plugin was made from, the same file again still finds its copy and
 * keeps it, for a conversion and for a plain file, and a package from
 * someone else that is the file's wrap byte for byte becomes the user's
 * own */
TEST(BridgeTest, OneStoreAFileTheIndexForgotIsFoundByItsBytes)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    const ObdbImport::Preview set =
        ObdbImport::read(readBytes(QStringLiteral(ECUIO_OBDB_FIXTURE)), 2019,
                         QStringLiteral("Daily driver OBDb"));
    ASSERT_TRUE(set.ok) << set.words.toStdString();
    const QByteArray plain = signalFile(QStringLiteral("Plain"));
    const PluginStore::Wrap forV1{QStringLiteral("v1"),
                                  QStringLiteral("my2019")};
    QString conversion;
    QString own;
    {
        PluginStore store;
        QVariantMap r = store.wrap(set.json, forV1);
        ASSERT_TRUE(ok(r)) << words(r);
        conversion = str(r, "sha256");
        r = store.wrap(plain, {});
        ASSERT_TRUE(ok(r)) << words(r);
        own = str(r, "sha256");
    }
    {
        QJsonObject index =
            QJsonDocument::fromJson(readBytes(indexPath())).object();
        QJsonObject plugins = index.value(QStringLiteral("plugins")).toObject();
        for (const QString& id : plugins.keys()) {
            QJsonObject facts = plugins.value(id).toObject();
            ASSERT_FALSE(facts.value(QStringLiteral("wrappedFrom"))
                             .toObject()
                             .isEmpty());
            facts.remove(QStringLiteral("wrappedFrom"));
            plugins.insert(id, facts);
        }
        index.insert(QStringLiteral("plugins"), plugins);
        QFile f(indexPath());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(index).toJson());
    }
    PluginStore store;
    QVariantMap r = store.wrap(set.json, forV1);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), conversion);
    EXPECT_EQ(str(r, "replaced"), QString());
    EXPECT_EQ(words(r), "");
    EXPECT_TRUE(QFile::exists(copyPath(conversion)));
    EXPECT_TRUE(registered(QStringLiteral("plugin.daily-driver-obdb.my2019")));
    EXPECT_EQ(madeFrom(QStringLiteral("daily-driver-obdb")),
              (QJsonObject{{sha256Of(set.json), QStringLiteral("my2019")}}))
        << "named again";

    r = store.wrap(plain, {});
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), own);
    EXPECT_EQ(store.all().size(), 2);
    EXPECT_EQ(copies().size(), 2);

    const QByteArray shared =
        signalFile(QStringLiteral("Shared"), QStringLiteral("D170"));
    r = store.import(coreWrap(shared));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_FALSE(r.value(QStringLiteral("mine")).toBool());
    const QString theirs = str(r, "sha256");
    r = store.wrap(shared, {});
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), theirs);
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(store.all().size(), 3);
}

/* The vehicle book gives a new vehicle the highest id plus one, so the
 * id of a forgotten vehicle comes back. Forgetting a vehicle releases
 * what was imported for it: the plugins stay the user's own, for no
 * vehicle, and the next vehicle with that id neither lists them as its
 * own nor has its OBDb set take the old car's place. A set kept for no
 * vehicle is a plugin of its own too */
TEST(BridgeTest, OneStoreAForgottenVehiclesIdTakesNoImportToTheNextVehicle)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    const QString name = QStringLiteral("Daily driver OBDb");
    const QString v1 = QStringLiteral("v1");
    const QString v2 = QStringLiteral("v2");
    const QString importedFor = QStringLiteral("importedFor");
    const QString releasedOne =
        QStringLiteral("Plugins no longer imported for a forgotten vehicle: 2");
    const QString releasedAll = QStringLiteral(
        "Plugins no longer imported for the forgotten vehicles: 2");
    const auto own = [](SessionBridge& bridge, const QString& vehicle) {
        QStringList profiles;
        for (const QVariant& v :
             bridge.offeredVariants(vehicle, QString(), {})) {
            if (v.toMap().value(QStringLiteral("forThisCar")).toBool()) {
                profiles
                    << v.toMap().value(QStringLiteral("profile")).toString();
            }
        }
        return profiles;
    };
    {
        Logged b;
        QVariantMap r = keepObdb(b.bridge, 2019, name, v2, true);
        ASSERT_TRUE(ok(r)) << words(r);
        const QString old = str(r, "sha256");
        r = b.bridge.keepSignalText(
            QString::fromUtf8(signalFile(QStringLiteral("Own extras"))),
            QString(), QString(), v2);
        ASSERT_TRUE(ok(r)) << words(r);
        r = b.bridge.keepSignalText(
            QString::fromUtf8(signalFile(QStringLiteral("Other car"),
                                         QStringLiteral("D101"))),
            QString(), QString(), v1);
        ASSERT_TRUE(ok(r)) << words(r);
        ASSERT_EQ(
            own(b.bridge, v2),
            (QStringList{QStringLiteral("plugin.daily-driver-obdb.my2019"),
                         QStringLiteral("plugin.own-extras.base")}));

        // A vehicle no plugin names: nothing to release, nothing logged,
        // the index not written (a compact text stays compact)
        const QByteArray compact =
            QJsonDocument(
                QJsonDocument::fromJson(readBytes(indexPath())).object())
                .toJson(QJsonDocument::Compact);
        {
            QFile index(indexPath());
            ASSERT_TRUE(index.open(QIODevice::WriteOnly | QIODevice::Truncate));
            index.write(compact);
        }
        const qsizetype lines = b.log.size();
        r = b.bridge.forgetVehicleImports(QStringLiteral("v9"));
        EXPECT_TRUE(ok(r));
        EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 0);
        r = b.bridge.forgetVehicleImports(QString());
        EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 0);
        EXPECT_EQ(b.log.size(), lines);
        EXPECT_EQ(readBytes(indexPath()), compact);
        EXPECT_EQ(own(b.bridge, v2).size(), 2);

        r = b.bridge.forgetVehicleImports(v2);
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(words(r), "");
        EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 2);
        EXPECT_EQ(b.log.last(), releasedOne) << b.lines();
        EXPECT_EQ(factsOf(QStringLiteral("daily-driver-obdb")).keys(),
                  (QStringList{QStringLiteral("mine"),
                               QStringLiteral("wrappedFrom")}))
            << "still the user's own, for no vehicle";
        EXPECT_FALSE(
            factsOf(QStringLiteral("own-extras")).contains(importedFor));
        EXPECT_EQ(
            factsOf(QStringLiteral("other-car")).value(importedFor).toString(),
            v1)
            << "another vehicle's stays";
        EXPECT_EQ(b.bridge.plugins().size(), 3);

        // The next vehicle is given the id again
        EXPECT_TRUE(own(b.bridge, v2).isEmpty());
        EXPECT_EQ(own(b.bridge, v1),
                  QStringList{QStringLiteral("plugin.other-car.base")});
        r = keepObdb(b.bridge, 2021, name, v2, true);
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb-2");
        EXPECT_EQ(str(r, "replaced"), QString());
        EXPECT_FALSE(r.value(QStringLiteral("merged")).toBool());
        EXPECT_TRUE(QFile::exists(copyPath(old))) << "the old car's plugin";
        EXPECT_TRUE(
            registered(QStringLiteral("plugin.daily-driver-obdb.my2019")));
        EXPECT_EQ(own(b.bridge, v2), QStringList{QStringLiteral(
                                         "plugin.daily-driver-obdb-2.my2021")});

        // Every vehicle forgotten
        r = b.bridge.forgetAllVehicleImports();
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 2)
            << "the other car's and the new one's";
        EXPECT_EQ(b.log.last(), releasedAll) << b.lines();
        EXPECT_TRUE(own(b.bridge, v1).isEmpty());
        EXPECT_TRUE(own(b.bridge, v2).isEmpty());
        EXPECT_EQ(
            column(b.bridge.plugins(), "mine"),
            (QStringList{QStringLiteral("true"), QStringLiteral("true"),
                         QStringLiteral("true"), QStringLiteral("true")}));

        // A set kept for no vehicle goes to no earlier conversion, one
        // that is for no vehicle included
        r = keepObdb(b.bridge, 2020, name, QString(), true);
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(str(r, "id").toStdString(), "daily-driver-obdb-3");
        EXPECT_EQ(str(r, "replaced"), QString());
        EXPECT_EQ(str(r, "importedFor"), QString());
        EXPECT_EQ(b.bridge.plugins().size(), 5);
    }
    // Written: a later start finds no import for either id
    SessionBridge again;
    EXPECT_EQ(again.plugins().size(), 5);
    EXPECT_TRUE(own(again, v1).isEmpty());
    EXPECT_TRUE(own(again, v2).isEmpty());
}

/* A wrap whose facts never reached the index (the write failed, the app
 * stopped) leaves a plugin with no facts: the same file again finds it
 * by its bytes and writes them, and makes no second plugin */
TEST(BridgeTest, OneStoreAWrapWhoseFactsWereNeverWrittenGetsThemTheNextTime)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    const QByteArray file = signalFile(QStringLiteral("Cut short"));
    const QString id = QStringLiteral("cut-short");
    const PluginStore::Wrap forV4{QStringLiteral("v4"), {}};
    QString sha;
    {
        PluginStore store;
        store.failIndexWriteForTesting();
        const QVariantMap r = store.wrap(file, forV4);
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(words(r), "The plugin is kept, but the store's index "
                            "couldn't be written yet: if no later change "
                            "writes it before the app closes, after a "
                            "restart it isn't marked as yours");
        EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool())
            << "while the app runs";
        sha = str(r, "sha256");
        EXPECT_TRUE(QFile::exists(copyPath(sha)));
        EXPECT_FALSE(QFile::exists(indexPath()));
    }
    PluginStore store;
    const QVariantList all = store.all();
    ASSERT_EQ(all.size(), 1);
    EXPECT_FALSE(all.at(0).toMap().value(QStringLiteral("mine")).toBool());
    QVariantMap r = store.wrap(file, forV4);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(words(r), "");
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), sha);
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(str(r, "importedFor").toStdString(), "v4");
    EXPECT_EQ(
        factsOf(id),
        (QJsonObject{{QStringLiteral("mine"), true},
                     {QStringLiteral("importedFor"), QStringLiteral("v4")},
                     {QStringLiteral("wrappedFrom"),
                      QJsonObject{{sha256Of(file), QStringLiteral("base")}}}}));
    EXPECT_EQ(copies(), QStringList{sha + kSuffix});

    // The seam fails one write: the next is written, with what the
    // failed one held
    store.failIndexWriteForTesting();
    r = store.wrap(signalFile(QStringLiteral("Second")), {});
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_FALSE(words(r).empty());
    EXPECT_TRUE(factsOf(QStringLiteral("second")).isEmpty());
    ASSERT_TRUE(ok(store.markMine(sha)));
    EXPECT_FALSE(factsOf(QStringLiteral("second")).isEmpty());

    // A forget and a Mine the index never got: the words say a later
    // change may still write them
    store.failIndexWriteForTesting();
    r = store.forgetVehicle(QStringLiteral("v4"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(r.value(QStringLiteral("count")).toInt(), 1);
    EXPECT_EQ(words(r), "They are released, but the store's index couldn't "
                        "be written yet: if no later change writes it before "
                        "the app closes, after a restart each may be "
                        "imported for its vehicle again");
    EXPECT_EQ(factsOf(id).value(QStringLiteral("importedFor")).toString(),
              QStringLiteral("v4"));
    r = store.import(coreWrap(signalFile(QStringLiteral("Third"))));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_FALSE(r.value(QStringLiteral("mine")).toBool());
    const QString third = str(r, "sha256");
    store.failIndexWriteForTesting();
    r = store.markMine(third);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(words(r), "It is marked as yours, but the store's index "
                        "couldn't be written yet: if no later change "
                        "writes it before the app closes, after a "
                        "restart it isn't marked as yours");
    EXPECT_TRUE(factsOf(QStringLiteral("third")).isEmpty());

    // The next change written carries both: a new store finds them
    ASSERT_TRUE(store.setMigrated());
    store.close();
    PluginStore later;
    QStringList seen;
    for (const QVariant& v : later.all()) {
        const QVariantMap e = v.toMap();
        seen << QStringLiteral("%1 %2 %3")
                    .arg(e.value(QStringLiteral("id")).toString(),
                         e.value(QStringLiteral("mine")).toString(),
                         e.value(QStringLiteral("importedFor")).toString());
    }
    seen.sort();
    EXPECT_EQ(seen, (QStringList{QStringLiteral("cut-short true "),
                                 QStringLiteral("second true "),
                                 QStringLiteral("third true ")}));
    EXPECT_TRUE(later.migrated());
}

namespace {

// A one-signal file whose source names a license
QByteArray licensedFile(const QString& name, const QString& license,
                        const QString& did = QStringLiteral("D200"))
{
    return QStringLiteral(
               R"({"havne_signals":1,"name":"%1","source":{"name":"OBDb",)"
               R"("license":"%2"},"ecus":[{"id":"pcm","rx":"7E8"}],)"
               R"("signals":[{"key":"b","name":"B","read":{"service":"22",)"
               R"("number":"%3"},"field":{"byte":0,"bits":8}}]})")
        .arg(name, license, did)
        .toUtf8();
}

// A signal file's source as core reads it
ecuio_signal_source_t sourceOf(const QByteArray& file)
{
    ecuio_signal_source_t source{};
    source.struct_size = sizeof source;
    ecuio_signal_file_t* f = nullptr;
    EXPECT_EQ(ecuio_signal_file_load(file.constData(),
                                     static_cast<size_t>(file.size()), &f),
              ECUIO_OK)
        << ecuio_last_error_detail();
    if (f != nullptr) {
        EXPECT_EQ(ecuio_signal_file_source(f, &source), ECUIO_OK);
        ecuio_signal_file_free(f);
    }
    return source;
}

} // namespace

/* Save as a signal file, and the form's text: a variant's signal file is
 * core's text for it, loads alone under user.<the plugin's name>, and is
 * written byte for byte */
TEST(BridgeTest, OneStoreAVariantsSignalFileIsCoresTextAndLoadsAlone)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir out;
    Logged b;
    QVariantMap r = b.bridge.keepSignalText(
        QString::fromUtf8(signalFile(QStringLiteral("Daily extras"))),
        QString(), QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    const QString sha = str(r, "sha256");
    const QString version = str(r, "version");

    r = b.bridge.pluginVariantText(sha, QStringLiteral("base"));
    ASSERT_TRUE(ok(r)) << words(r);
    const QByteArray text = str(r, "text").toUtf8();

    // Core's own text for the kept copy's variant
    const QByteArray copy = readBytes(copyPath(sha));
    ecuio_plugin_t* p = nullptr;
    ASSERT_EQ(ecuio_plugin_load(copy.constData(),
                                static_cast<size_t>(copy.size()), nullptr,
                                nullptr, &p),
              ECUIO_OK)
        << ecuio_last_error_detail();
    size_t length = 0;
    const char* core =
        ecuio_signal_file_text(ecuio_plugin_variant_file(p, 0), &length);
    EXPECT_EQ(text, QByteArray(core, static_cast<qsizetype>(length)));
    EXPECT_EQ(ecuio_plugin_free(p), ECUIO_OK);

    ecuio_signal_file_t* f = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(text.constData(),
                                     static_cast<size_t>(text.size()), &f),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_STREQ(ecuio_signal_file_profile(f), "user.daily-extras");
    ecuio_signal_file_free(f);
    EXPECT_TRUE(
        text.contains(QStringLiteral("Plugin daily-extras %1, variant base")
                          .arg(version)
                          .toUtf8()))
        << text.constData();

    // Saved: the same bytes
    const QUrl url =
        QUrl::fromLocalFile(out.path() + QStringLiteral("/extras.json"));
    r = b.bridge.saveVariantFile(sha, QStringLiteral("base"), url);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(readBytes(url.toLocalFile()), text);
    EXPECT_EQ(b.log.last(),
              QStringLiteral("Variant base of plugin %1 saved as a signal file "
                             "(%2 bytes)")
                  .arg(sha.left(12))
                  .arg(text.size()))
        << b.lines();

    // Refusals, in words that name no path
    r = b.bridge.pluginVariantText(sha, QStringLiteral("my2019"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Daily extras has no variant 'my2019'");
    r = b.bridge.pluginVariantText(QStringLiteral("0123"),
                                   QStringLiteral("base"));
    EXPECT_EQ(words(r), "The store has no such plugin");
    const QUrl other =
        QUrl::fromLocalFile(out.path() + QStringLiteral("/other.json"));
    r = b.bridge.saveVariantFile(sha, QStringLiteral("my2019"), other);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Daily extras has no variant 'my2019'");
    EXPECT_FALSE(QFile::exists(other.toLocalFile()));
    const QUrl nowhere = QUrl::fromLocalFile(
        out.path() + QStringLiteral("/no-such-folder/extras.json"));
    r = b.bridge.saveVariantFile(sha, QStringLiteral("base"), nowhere);
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Couldn't write the file");
    EXPECT_EQ(b.log.last(),
              QStringLiteral("Variant base of plugin %1 not saved: Couldn't "
                             "write the file")
                  .arg(sha.left(12)))
        << b.lines();
}

/* Copy to my plugin: someone else's variant goes into a new plugin of the
 * user's own or one they pick, its source kept, and says it won't follow
 * the original's updates; it never takes a variant's place, and the
 * original stays as it was */
TEST(BridgeTest, OneStoreACopiedVariantKeepsItsSourceAndFollowsNoUpdates)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir out;
    Logged b;
    const QByteArray file = licensedFile(QStringLiteral("Their car"),
                                         QStringLiteral("CC-BY-SA-4.0"));
    QVariantMap r = b.bridge.readImport(writeScratch(
        out.path(), QStringLiteral("theirs.json"), coreWrap(file)));
    ASSERT_TRUE(ok(r)) << words(r);
    const QString theirs = str(r, "sha256");
    EXPECT_FALSE(r.value(QStringLiteral("mine")).toBool());
    r = b.bridge.pluginVariantText(theirs, QStringLiteral("base"));
    ASSERT_TRUE(ok(r)) << words(r);
    const QByteArray text = str(r, "text").toUtf8();
    const QString follows =
        QStringLiteral("The copy won't follow Their car's updates to base");

    // Into a new plugin of the user's own, as the variant named
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"), QString(),
                             QStringLiteral("imported"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("mine")).toBool());
    EXPECT_EQ(str(r, "id").toStdString(), "their-car-2");
    EXPECT_EQ(list(r, "variants"), QStringList{QStringLiteral("imported")});
    EXPECT_EQ(str(r, "importedFor"), QString());
    EXPECT_EQ(str(r, "notes"), follows);
    EXPECT_TRUE(b.log.last().startsWith(QStringLiteral("Plugin made: ")) &&
                b.log.last().endsWith(QStringLiteral("; ") + follows))
        << b.lines();
    EXPECT_EQ(factsOf(QStringLiteral("their-car-2")),
              (QJsonObject{{QStringLiteral("mine"), true},
                           {QStringLiteral("wrappedFrom"),
                            QJsonObject{{sha256Of(text),
                                         QStringLiteral("imported")}}}}));
    const QString copy = str(r, "sha256");
    r = b.bridge.pluginVariantText(copy, QStringLiteral("imported"));
    ASSERT_TRUE(ok(r)) << words(r);
    const ecuio_signal_source_t kept = sourceOf(str(r, "text").toUtf8());
    EXPECT_STREQ(kept.name, "OBDb");
    EXPECT_STREQ(kept.license, "CC-BY-SA-4.0");
    EXPECT_NE(kept.flags & ECUIO_SOURCE_SHARE_ALIKE, 0u);
    EXPECT_TRUE(QFile::exists(copyPath(theirs))) << "the original stays";
    EXPECT_EQ(column(b.bridge.plugins(), "mine"),
              (QStringList{QStringLiteral("false"), QStringLiteral("true")}));

    // The same variant again, under the same name, finds that plugin
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"), QString(),
                             QStringLiteral("imported"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_TRUE(r.value(QStringLiteral("kept")).toBool());
    EXPECT_EQ(str(r, "sha256"), copy);
    EXPECT_TRUE(
        b.log.last().startsWith(QStringLiteral("Plugin already here: ")))
        << b.lines();
    EXPECT_EQ(b.bridge.plugins().size(), 2);

    // Into a plugin of the user's own, as a variant of its own: named,
    // then by the file's name
    r = b.bridge.keepSignalText(
        QString::fromUtf8(signalFile(QStringLiteral("Own extras"))), QString(),
        QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    QString own = str(r, "sha256");
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"), own,
                             QStringLiteral("theirs"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "id").toStdString(), "own-extras");
    EXPECT_EQ(list(r, "variants"),
              (QStringList{QStringLiteral("base"), QStringLiteral("theirs")}));
    EXPECT_TRUE(str(r, "notes").endsWith(follows))
        << str(r, "notes").toStdString();
    EXPECT_TRUE(b.log.last().startsWith(QStringLiteral("Plugin changed: ")))
        << b.lines();
    own = str(r, "sha256");
    r = b.bridge.pluginVariantText(own, QStringLiteral("theirs"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_STREQ(sourceOf(str(r, "text").toUtf8()).license, "CC-BY-SA-4.0");
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"), own, QString());
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(list(r, "variants"),
              (QStringList{QStringLiteral("base"), QStringLiteral("theirs"),
                           QStringLiteral("their-car")}));
    own = str(r, "sha256");

    // Never in a variant's place, named or by the file's name, nor into
    // the plugin it is in: nothing changes
    const QStringList before = copies();
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"), own,
                             QStringLiteral("base"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Own extras already has a variant 'base': give the "
                        "copy another name");
    EXPECT_EQ(b.log.last(),
              QStringLiteral("Plugin not changed: Own extras already has a "
                             "variant 'base': give the copy another name"))
        << b.lines();
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"), own, QString());
    EXPECT_EQ(words(r), "Own extras already has a variant 'their-car': give "
                        "the copy another name");
    r = b.bridge.copyVariant(own, QStringLiteral("base"), own,
                             QStringLiteral("again"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Copy to another plugin: this variant is already in "
                        "Own extras");
    EXPECT_EQ(copies(), before);
    r = b.bridge.pluginVariantText(own, QStringLiteral("base"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_STRNE(sourceOf(str(r, "text").toUtf8()).name, "OBDb")
        << "the user's own base stays";

    // Never into someone else's plugin or one not here, nor from a
    // variant not there
    r = b.bridge.copyVariant(own, QStringLiteral("base"), theirs,
                             QStringLiteral("base"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Their car isn't a plugin of your own: only your own "
                        "plugins are changed");
    EXPECT_EQ(b.log.last(),
              QStringLiteral("Plugin not changed: Their car isn't a plugin of "
                             "your own: only your own plugins are changed"))
        << b.lines();
    r = b.bridge.copyVariant(theirs, QStringLiteral("base"),
                             QStringLiteral("0123"), QStringLiteral("x"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The store has no such plugin");
    r = b.bridge.copyVariant(theirs, QStringLiteral("x"), QString(), QString());
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "Their car has no variant 'x'");
    EXPECT_EQ(b.bridge.plugins().size(), 3);
}

/* Export plugin's note: each variant whose source's license is
 * share-alike is named with it, in havne-cli plugins check's words, and a
 * plugin with none has no note */
TEST(BridgeTest, OneStoreExportsNoteNamesEachShareAlikeVariant)
{
    SettingsDir dir;
    ASSERT_TRUE(dir.valid());
    Logged b;
    QVariantMap r = b.bridge.keepSignalText(
        QString::fromUtf8(signalFile(QStringLiteral("Mixed"))), QString(),
        QString(), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    QString sha = str(r, "sha256");
    r = b.bridge.pluginExportNote(sha);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "note"), QString());
    EXPECT_TRUE(r.value(QStringLiteral("shareAlike")).toList().isEmpty());

    // A share-alike variant and an attribution-only one beside the base
    r = b.bridge.keepSignalText(
        QString::fromUtf8(licensedFile(QStringLiteral("Shared"),
                                       QStringLiteral("CC-BY-SA-4.0"))),
        sha, QStringLiteral("obdb"), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    r = b.bridge.keepSignalText(
        QString::fromUtf8(licensedFile(QStringLiteral("Credited"),
                                       QStringLiteral("CC-BY-4.0"),
                                       QStringLiteral("D201"))),
        str(r, "sha256"), QStringLiteral("credited"), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    sha = str(r, "sha256");
    EXPECT_EQ(list(r, "variants"),
              (QStringList{QStringLiteral("base"), QStringLiteral("obdb"),
                           QStringLiteral("credited")}));
    const QString line =
        QStringLiteral("Variant 'obdb' holds data under CC-BY-SA-4.0, a "
                       "share-alike license: whoever passes the plugin on "
                       "keeps that license and the variant's source with it");
    r = b.bridge.pluginExportNote(sha);
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "note"), line);
    const QVariantList shared = r.value(QStringLiteral("shareAlike")).toList();
    ASSERT_EQ(shared.size(), 1);
    EXPECT_EQ(shared.at(0).toMap(),
              (QVariantMap{
                  {QStringLiteral("variant"), QStringLiteral("obdb")},
                  {QStringLiteral("license"), QStringLiteral("CC-BY-SA-4.0")},
                  {QStringLiteral("words"), line}}));

    // An OBDb set kept as a plugin: its one variant, under OBDb's license
    r = keepObdb(b.bridge, 2021, QStringLiteral("Daily driver OBDb"), QString(),
                 false);
    ASSERT_TRUE(ok(r)) << words(r);
    r = b.bridge.pluginExportNote(str(r, "sha256"));
    ASSERT_TRUE(ok(r)) << words(r);
    EXPECT_EQ(str(r, "note"),
              QStringLiteral("Variant 'base' holds data under CC-BY-SA-4.0, a "
                             "share-alike license: whoever passes the plugin "
                             "on keeps that license and the variant's source "
                             "with it"));

    r = b.bridge.pluginExportNote(QStringLiteral("0123"));
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(words(r), "The store has no such plugin");
}
