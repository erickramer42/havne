// File/layer: tests/test_recording_view_keys.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: RecordingViewTest: each signal's key (test_bridge)
//
// The viewer's signals by key (ADR-0065 section 9): a format 4
// recording's keys and none before it, and a frames open keyed by the
// set's signal file: the recording's own, or a registered profile's
// through RecordingFrameSet::setSignalFileText.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QVariantMap>

#include <functional>

#include "bridge_support.h"
#include "recording_frameset.h"
#include "recordingview.h"
#include "sessionbridge.h"

#include "ecuio/profile.h"
#include "ecuio/signals.h"

namespace {

bool opened(RecordingView& v)
{
    QElapsedTimer t;
    t.start();
    while (v.state() == RecordingView::Opening && t.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return v.state() == RecordingView::Open;
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
}

QVariantMap signalNamed(const RecordingView& v, const char* name)
{
    for (const QVariant& s : v.signalList()) {
        const QVariantMap m = s.toMap();
        if (m.value(QStringLiteral("name")).toString() == QLatin1String(name)) {
            return m;
        }
    }
    return {};
}

QString keyOf(const RecordingView& v, const char* name)
{
    return signalNamed(v, name).value(QStringLiteral("key")).toString();
}

// A file on obd2 with a computed signal its author keys "krpm" (the set
// alone gives that signal "id:<n>") and an enum signal
QByteArray keyedFile(const char* key = "krpm",
                     const char* name = "RPM in Thousands",
                     const char* extra = "",
                     const char* expr = "obd2.rpm / 1000",
                     const char* label = "Closed loop")
{
    return QByteArray(R"({"havne_signals": 1, "name": "Viewer keys",
        "extends": "obd2", "source": {"name": "Written for the test"},
        "signals": [{"key": ")") +
           key + R"(", "name": ")" + name +
           R"(", "kind": "computed", "expr": ")" + expr +
           R"(", "max_age": {"obd2.rpm": 2}, "unit": "krpm", "decimals": 2,
             "group": "engine"},
           {"key": "fuel_state", "name": "Fuel State",
            "read": {"service": "01", "number": "03"},
            "field": {"byte": 0, "bits": 8}, "kind": "enum",
            "values": {"1": "Open loop", "2": ")" +
           label + R"("}})" + extra + "]}";
}

// Engine speed replies on 0x7E8, a tenth of a second apart
const QByteArray kRpmFrames =
    "host_us,timestamp_us,id,flags,data\n"
    "100000,100000,0x7E8,0x0,04 41 0C 0C 90 00 00 00\n"
    "200000,200000,0x7E8,0x0,04 41 0C 0D 00 00 00 00\n"
    "300000,300000,0x7E8,0x0,04 41 0C 0D 70 00 00 00\n";

// The hook unset again when a case ends
struct HookScope {
    HookScope() = default;
    HookScope(const HookScope&) = delete;
    HookScope& operator=(const HookScope&) = delete;
    HookScope(HookScope&&) = delete;
    HookScope& operator=(HookScope&&) = delete;
    ~HookScope() { RecordingFrameSet::setSignalFileText({}); }
};

} // namespace

/* A format 4 recording keeps each signal under its key, which the row
 * carries; an older one has names alone, and its rows no key */
TEST(RecordingViewTest, EachRowHasItsRecordingsKey)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeFile(dir.filePath(QStringLiteral("events.log")),
              "ecuio session recording, format 4 (ADR-0049)\n"
              "started  2026-11-17 10:00:00.000 (local time)\n"
              "profile  obd2\n"
              "signal  7E8:410C:2.0:16,RPM,number,rpm,0,1,1\n"
              "signal  krpm,RPM in Thousands,number,krpm,2,0,1\n\n");
    writeFile(dir.filePath(QStringLiteral("signals.csv")),
              "host_us,signal,value,unit\n"
              "100000,7E8:410C:2.0:16,804,rpm\n"
              "100000,krpm,0.804,krpm\n");
    RecordingView v;
    v.open(dir.path());
    ASSERT_TRUE(opened(v)) << v.error().toStdString();
    EXPECT_EQ(keyOf(v, "RPM"), QStringLiteral("7E8:410C:2.0:16"));
    EXPECT_EQ(keyOf(v, "RPM in Thousands"), QStringLiteral("krpm"));

    v.open(QStringLiteral(ECUIO_RECORDING_FIXTURES) +
           QStringLiteral("/pull-early"));
    ASSERT_TRUE(opened(v)) << v.error().toStdString();
    ASSERT_FALSE(v.signalList().isEmpty());
    for (const QVariant& s : v.signalList()) {
        const QVariantMap m = s.toMap();
        EXPECT_TRUE(m.contains(QStringLiteral("key")));
        EXPECT_EQ(m.value(QStringLiteral("key")).toString(), QString())
            << m.value(QStringLiteral("name")).toString().toStdString();
    }
}

/* Frames decoded through the recording's own signals.json are keyed by
 * that file: a computed signal has its author's key, as the recording of
 * the file was written */
TEST(RecordingViewTest, ItsOwnSignalFileKeysTheFramesOpen)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray file = keyedFile();
    const QByteArray sha =
        QCryptographicHash::hash(file, QCryptographicHash::Sha256).toHex();
    writeFile(dir.filePath(QStringLiteral("signals.json")), file);
    writeFile(dir.filePath(QStringLiteral("events.log")),
              "ecuio session recording, format 3 (ADR-0049)\n"
              "profile  user.viewer-keys\n"
              "signal file  signals.json," +
                  sha + "\n\n");
    writeFile(dir.filePath(QStringLiteral("frames.csv")), kRpmFrames);
    RecordingView v;
    v.open(dir.path());
    ASSERT_TRUE(opened(v)) << v.error().toStdString();
    EXPECT_EQ(v.decodedWith(), QStringLiteral("signals.json"));
    EXPECT_EQ(keyOf(v, "RPM in Thousands"), QStringLiteral("krpm"));
    EXPECT_EQ(keyOf(v, "RPM"), QStringLiteral("7E8:410C:2.0:16"));
    EXPECT_EQ(signalNamed(v, "RPM in Thousands")
                  .value(QStringLiteral("samples"))
                  .toDouble(),
              3.0);
}

/* Through a registered profile the hook's text keys the open, loaded by
 * the viewer itself, so the profile can go mid-open; unset, given no
 * text, text that doesn't load or the text of another set (other defs,
 * one more, another formula or label under the same defs), the open
 * decodes with the set alone */
TEST(RecordingViewTest, ARegisteredProfilesFileTextKeysTheFramesOpen)
{
    HookScope scope;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeFile(dir.filePath(QStringLiteral("frames.csv")), kRpmFrames);
    const QByteArray mine = keyedFile();
    // Each case: the hook's text (null: unset), the computed signal's key
    // and the name its row has
    const struct {
        const char* tag;
        QByteArray text;
        bool hooked;
        const char* key; // nullptr: "id:<n>"
    } cases[] = {
        {"its own text", mine, true, "krpm"},
        {"unset", QByteArray(), false, nullptr},
        {"no text", QByteArray(), true, nullptr},
        {"text that doesn't load", QByteArray("{"), true, nullptr},
        {"another set's defs", keyedFile("hrpm", "RPM in Hundreds"), true,
         nullptr},
        {"a set with one more def",
         keyedFile("krpm", "RPM in Thousands",
                   R"(, {"key": "extra", "name": "Extra", "kind": "computed",
                      "expr": "obd2.rpm / 10", "max_age": {"obd2.rpm": 2}})"),
         true, nullptr},
        {"same defs, another formula",
         keyedFile("krpm", "RPM in Thousands", "", "obd2.rpm / 100"), true,
         nullptr},
        {"same defs, another enum label",
         keyedFile("krpm", "RPM in Thousands", "", "obd2.rpm / 1000",
                   "Closed loop, trimmed"),
         true, nullptr},
    };
    for (const auto& c : cases) {
        ecuio_signal_file_t* file = nullptr;
        ASSERT_EQ(ecuio_signal_file_load(mine.constData(),
                                         static_cast<size_t>(mine.size()),
                                         &file),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        const char* profile = ecuio_signal_file_profile(file);
        const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
        ASSERT_EQ(ecuio_profile_register(profile, set, nullptr), ECUIO_OK);
        const QByteArray text = c.text;
        RecordingFrameSet::setSignalFileText(
            c.hooked ? RecordingFrameSet::SignalFileText(
                           [set, text](const ecuio_signal_set_t* asked) {
                               return asked == set ? text : QByteArray();
                           })
                     : RecordingFrameSet::SignalFileText());
        RecordingView v;
        v.openFrames(dir.path(), QString::fromUtf8(profile));
        // The store drops the file while the open runs
        ASSERT_EQ(ecuio_profile_unregister(profile), ECUIO_OK);
        ecuio_signal_file_free(file);
        ASSERT_TRUE(opened(v)) << c.tag << ": " << v.error().toStdString();
        const QString key = keyOf(v, "RPM in Thousands");
        if (c.key != nullptr) {
            EXPECT_EQ(key, QString::fromLatin1(c.key)) << c.tag;
        } else {
            EXPECT_TRUE(key.startsWith(QStringLiteral("id:")))
                << c.tag << ": " << key.toStdString();
        }
        EXPECT_EQ(signalNamed(v, "RPM in Thousands")
                      .value(QStringLiteral("samples"))
                      .toDouble(),
                  3.0)
            << c.tag << ": decoded by the profile's set";
        EXPECT_TRUE(signalNamed(v, "Extra").isEmpty()) << c.tag;
        EXPECT_TRUE(signalNamed(v, "RPM in Hundreds").isEmpty()) << c.tag;
    }
}

/* The hook as main() sets it: the bridge answers with the file of a
 * profile its plugin store registered, a kept file's variant's written
 * text, which loads to the registered set's defs, so the open has the
 * file's keys */
TEST(RecordingViewTest, TheBridgesStoreFileKeysTheFramesOpen)
{
    HookScope scope;
    SettingsDirScope settings;
    ASSERT_TRUE(settings.valid());
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeFile(dir.filePath(QStringLiteral("frames.csv")), kRpmFrames);
    SessionBridge bridge;
    RecordingFrameSet::setSignalFileText(
        [&bridge](const ecuio_signal_set_t* set) {
            return bridge.signalFileTextOfSet(set);
        });
    const QVariantMap r = bridge.keepSignalText(
        QString::fromUtf8(keyedFile()), QString(), QString(), QString());
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    const QString profile =
        r.value(QStringLiteral("profiles")).toStringList().value(0);
    const ecuio_signal_set_t* set = nullptr;
    ASSERT_EQ(ecuio_profile_lookup(profile.toUtf8().constData(), &set),
              ECUIO_OK);
    EXPECT_FALSE(bridge.signalFileTextOfSet(set).isEmpty());
    EXPECT_TRUE(bridge.signalFileTextOfSet(nullptr).isEmpty());

    RecordingView v;
    v.openFrames(dir.path(), profile);
    ASSERT_TRUE(opened(v)) << v.error().toStdString();
    EXPECT_EQ(keyOf(v, "RPM in Thousands"), QStringLiteral("krpm"));
    EXPECT_EQ(removeEveryPlugin(bridge), 1);
}
