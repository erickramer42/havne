// File/layer: tests/test_recording_view_open.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: RecordingViewTest (test_bridge)

#include "test_recording_view_support.h"

// An open started over another drops the first one's result
TEST(RecordingViewTest, ALaterOpenWins)
{
    RecordingView v;
    v.open(fixture("findings"));
    v.open(fixture("pull-early"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_EQ(v.state(), RecordingView::Open);
    EXPECT_EQ(v.folder(), fixture("pull-early"));
    EXPECT_EQ(v.signalList().size(), 4);
    // Whatever the first pass posted is dropped, now and later
    waitFor([] { return false; }, 200);
    EXPECT_EQ(v.signalList().size(), 4);
    EXPECT_TRUE(v.findings().isEmpty());
}

// A long open reports its progress, and a close stops it where it is:
// nothing it posts afterwards lands
TEST(RecordingViewTest, AnOpenReportsProgressAndACloseStopsIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    {
        // About 30 MB of rows: the reader reports every 4 MiB
        QFile f(dir.filePath(QStringLiteral("signals.csv")));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("host_us,signal,value,unit\n");
        QByteArray chunk;
        for (qint64 t = 0; t < 1500000; ++t) {
            chunk += QByteArray::number(100000 + t * 1000) + ",RPM,1234,rpm\n";
            if (chunk.size() > (1 << 20)) {
                f.write(chunk);
                chunk.clear();
            }
        }
        f.write(chunk);
    }
    RecordingView v;
    double seen = 0;
    QObject::connect(&v, &RecordingView::progressChanged, [&] {
        if (v.state() == RecordingView::Opening)
            seen = std::max(seen, v.progress());
    });
    v.open(dir.path());
    ASSERT_TRUE(waitFor([&] { return seen > 0 || settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Opening)
        << "the open ended before any progress";
    EXPECT_GT(seen, 0.0);
    EXPECT_LT(seen, 1.0);
    v.close();
    EXPECT_EQ(v.state(), RecordingView::Closed);
    EXPECT_EQ(v.progress(), 0.0);
    waitFor([] { return false; }, 200);
    EXPECT_EQ(v.state(), RecordingView::Closed);
    EXPECT_TRUE(v.signalList().isEmpty());
}

// A view destroyed mid-open stops the pass and leaves nothing behind
TEST(RecordingViewTest, DestroyedWhileOpeningIsSafe)
{
    {
        RecordingView v;
        v.open(fixture("findings"));
    }
    waitFor([] { return false; }, 100);
    SUCCEED();
}

// ---- The values from the frames (ADR-0049's appendix of 2026-11-06) ----

namespace {

const QString kReplayFixtures = QStringLiteral(ECUIO_REPLAY_FIXTURES);

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
}

// A signal file of one DID on 0x7E8, its profile user.frames-view
const QByteArray kOneDid = R"({
  "havne_signals": 1,
  "name": "Frames view",
  "source": { "name": "Written for the test" },
  "ecus": [ { "id": "pcm", "rx": "7E8" } ],
  "signals": [
    { "key": "a", "name": "Input A", "ecu": "pcm",
      "read": { "service": "22", "number": "1111" },
      "field": { "byte": 0, "bits": 8 } }
  ]
})";

// Three of that DID's replies as a listener saw them, Single Frames
const QByteArray kOneDidFrames = "host_us,timestamp_us,id,flags,data\n"
                                 "100000,100000,0x7E8,0x0,04 62 11 11 07\n"
                                 "200000,200000,0x7E8,0x0,04 62 11 11 08\n"
                                 "300000,300000,0x7E8,0x0,04 62 11 11 09\n";

} // namespace

// A listener's wire frames through the profile its header names, as
// havne-cli view --frames reads them
TEST(RecordingViewTest, AListenersFramesDecodeThroughItsRecordedProfile)
{
    RecordingView v;
    v.openFrames(fixture("findings"), QString());
    EXPECT_EQ(v.state(), RecordingView::Opening);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_EQ(v.decodedWith(), QStringLiteral("obd2"));
    EXPECT_EQ(v.recordedProfile(), QStringLiteral("obd2"));
    EXPECT_FALSE(v.ownSignalFile());
    EXPECT_TRUE(v.check().value(QStringLiteral("ran")).toBool());
    const QVariantMap rpm = signalNamed(v, "RPM");
    EXPECT_EQ(rpm.value(QStringLiteral("samples")).toDouble(), 307.0);
    EXPECT_EQ(rpm.value(QStringLiteral("lowest")).toDouble(), 804.0);
    EXPECT_EQ(rpm.value(QStringLiteral("firstMs")).toDouble(), 100.0)
        << "the frame's time, not signals.csv's";
    EXPECT_TRUE(rpm.value(QStringLiteral("plotted")).toBool());

    v.open(fixture("findings"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    EXPECT_TRUE(v.decodedWith().isEmpty()) << "signals.csv again";
}

// The replay fixture holds frames.csv alone: open goes to the frames, and
// with no header to name a profile it says to pick one
TEST(RecordingViewTest, AFolderOfFramesAloneOpensThroughAProfile)
{
    const QString folder =
        kReplayFixtures + QStringLiteral("/2026-10-25-engine-start");
    RecordingView v;
    v.open(folder);
    EXPECT_EQ(v.state(), RecordingView::Failed) << "nothing names a profile";
    EXPECT_EQ(v.error(),
              QStringLiteral("it names no profile: pick one to decode its "
                             "frames with"));
    v.openFrames(folder, QStringLiteral("obd2"));
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_EQ(v.decodedWith(), QStringLiteral("obd2"));
    EXPECT_EQ(signalNamed(v, "RPM").value(QStringLiteral("highest")).toDouble(),
              1261.25);
}

// What can't decode fails at once, in words that never name the path
TEST(RecordingViewTest, AProfileThatCantDecodeFailsAtOnce)
{
    RecordingView v;
    const struct {
        const char* profile;
        const char* words;
    } cases[] = {
        {"raw", "the profile raw has no signals to decode with"},
        {"user.not-loaded", "the profile user.not-loaded isn't loaded"},
        {"signals.json",
         "its signals.json isn't the file it was recorded with"},
    };
    for (const auto& c : cases) {
        v.openFrames(fixture("findings"), QString::fromLatin1(c.profile));
        EXPECT_EQ(v.state(), RecordingView::Failed) << c.profile;
        EXPECT_EQ(v.error(), QString::fromLatin1(c.words));
        EXPECT_TRUE(v.decodedWith().isEmpty());
    }

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeFile(dir.filePath(QStringLiteral("events.log")),
              "ecuio session recording, format 2 (ADR-0049)\n"
              "profile  raw\n\n");
    writeFile(dir.filePath(QStringLiteral("frames.csv")), kOneDidFrames);
    v.openFrames(dir.path(), QString());
    EXPECT_EQ(v.state(), RecordingView::Failed);
    EXPECT_EQ(v.error(),
              QStringLiteral("it was recorded under raw, which decodes "
                             "nothing: pick a profile to decode its frames "
                             "with"));
    EXPECT_FALSE(v.error().contains(dir.path()));
}

// The set is copied before the open's thread starts: a profile gone the
// moment after still decodes the open under way
TEST(RecordingViewTest, TheSetIsCopiedBeforeTheOpenRuns)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeFile(dir.filePath(QStringLiteral("frames.csv")), kOneDidFrames);
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(kOneDid.constData(),
                                     static_cast<size_t>(kOneDid.size()),
                                     &file),
              ECUIO_OK);
    const char* profile = ecuio_signal_file_profile(file);
    ASSERT_STREQ(profile, "user.frames-view");
    ASSERT_EQ(
        ecuio_profile_register(profile, ecuio_signal_file_set(file), nullptr),
        ECUIO_OK);
    RecordingView v;
    v.openFrames(dir.path(), QStringLiteral("user.frames-view"));
    ASSERT_EQ(ecuio_profile_unregister(profile), ECUIO_OK);
    ecuio_signal_file_free(file);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    const QVariantMap a = signalNamed(v, "Input A");
    EXPECT_EQ(a.value(QStringLiteral("samples")).toDouble(), 3.0);
    EXPECT_EQ(a.value(QStringLiteral("highest")).toDouble(), 9.0);
}

// The recording's own signals.json, while it hashes to the one its header
// names: offered, and what the default picks; changed, it isn't
TEST(RecordingViewTest, ItsOwnSignalFileDecodesItWhileItMatches)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray sha =
        QCryptographicHash::hash(kOneDid, QCryptographicHash::Sha256).toHex();
    writeFile(dir.filePath(QStringLiteral("signals.json")), kOneDid);
    writeFile(dir.filePath(QStringLiteral("events.log")),
              "ecuio session recording, format 3 (ADR-0049)\n"
              "profile  user.frames-view\n"
              "signal file  signals.json," +
                  sha + "\n\n");
    writeFile(dir.filePath(QStringLiteral("frames.csv")), kOneDidFrames);
    RecordingView v;
    v.open(dir.path());
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_TRUE(v.ownSignalFile());
    EXPECT_EQ(v.decodedWith(), QStringLiteral("signals.json"));
    EXPECT_EQ(
        signalNamed(v, "Input A").value(QStringLiteral("samples")).toDouble(),
        3.0);

    writeFile(dir.filePath(QStringLiteral("signals.json")),
              kOneDid + QByteArray("\n"));
    v.openFrames(dir.path(), QString());
    EXPECT_FALSE(v.ownSignalFile());
    EXPECT_EQ(v.state(), RecordingView::Failed)
        << "the recorded profile, which isn't loaded";
    EXPECT_EQ(v.error(),
              QStringLiteral("the profile user.frames-view isn't loaded"));
}

// A copy past a written file's megabyte, a session's whole set (ADR-0065
// section 3), is read whole and decodes its recording
TEST(RecordingViewTest, ItsOwnSignalFilePastAWrittenFilesBytesDecodesIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray big = kOneDid.trimmed();
    ASSERT_TRUE(big.endsWith('}'));
    big.insert(big.size() - 1, QByteArray(ECUIO_SIGNAL_FILE_MAX_BYTES, ' '));
    ASSERT_GT(big.size(), static_cast<qsizetype>(ECUIO_SIGNAL_FILE_MAX_BYTES));
    const QByteArray sha =
        QCryptographicHash::hash(big, QCryptographicHash::Sha256).toHex();
    writeFile(dir.filePath(QStringLiteral("signals.json")), big);
    writeFile(dir.filePath(QStringLiteral("events.log")),
              "ecuio session recording, format 3 (ADR-0049)\n"
              "profile  user.frames-view\n"
              "signal file  signals.json," +
                  sha + "\n\n");
    writeFile(dir.filePath(QStringLiteral("frames.csv")), kOneDidFrames);
    RecordingView v;
    v.open(dir.path());
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    EXPECT_TRUE(v.ownSignalFile());
    EXPECT_EQ(v.decodedWith(), QStringLiteral("signals.json"));
    EXPECT_EQ(
        signalNamed(v, "Input A").value(QStringLiteral("samples")).toDouble(),
        3.0);
}

// ---- The health report, the viewer's Save report (ADR-0054's appendix
// "the health report as built") ----------------------------------------

// The whole recording's report, on a thread of its own: one at a time,
// its result posted back, the folder's name (the vehicle's label) in
// neither the words nor the line
TEST(RecordingViewTest, HealthReportIsTheWholeRecordingsOnItsOwnThread)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    const QUrl url =
        QUrl::fromLocalFile(root.path() + QStringLiteral("/report.json"));
    EXPECT_FALSE(v.saveHealthReport(url, 1)) << "no recording open";
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();

    QList<QVariantMap> saved;
    int reportingChanges = 0;
    QObject::connect(&v, &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    QObject::connect(&v, &RecordingView::reportingChanged,
                     [&] { ++reportingChanges; });
    ASSERT_TRUE(v.saveHealthReport(url, 1));
    EXPECT_TRUE(v.reporting());
    EXPECT_FALSE(v.saveHealthReport(url, 1)) << "one at a time";
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_FALSE(v.reporting());
    EXPECT_EQ(reportingChanges, 2);
    ASSERT_EQ(saved.size(), 1);
    const QVariantMap r = saved[0];
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("ecus")).toInt(), 1);
    EXPECT_EQ(r.value(QStringLiteral("codes")).toInt(), 1);
    EXPECT_EQ(r.value(QStringLiteral("values")).toInt(), 1) << "RPM's";
    EXPECT_EQ(r.value(QStringLiteral("line")).toString().toStdString(),
              "Health report saved: 1 ECU, 1 trouble code, 1 key value (0 "
              "identification items left out)");

    QFile f(url.toLocalFile());
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    f.close();
    EXPECT_FALSE(bytes.contains("Daily"));
    const QJsonObject doc = QJsonDocument::fromJson(bytes).object();
    const QJsonObject source = doc.value(QStringLiteral("source")).toObject();
    EXPECT_EQ(source.value(QStringLiteral("kind")).toString(),
              QStringLiteral("recording"));
    EXPECT_EQ(source.value(QStringLiteral("from_s")).toDouble(-1), 0.0)
        << "the whole recording";

    // A folder isn't a file it can write: core's report made, the copy
    // refused, in words that name neither
    saved.clear();
    ASSERT_TRUE(v.saveHealthReport(QUrl::fromLocalFile(root.path()), 0));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_FALSE(saved[0].value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(saved[0].value(QStringLiteral("words")).toString().toStdString(),
              "Couldn't write the file");
    for (const QVariantMap& m : saved) {
        EXPECT_FALSE(m.value(QStringLiteral("line"))
                         .toString()
                         .contains(QStringLiteral("Daily")));
    }
}

// The baseline given goes to core on the report's thread: another
// recording read by its start, the open one said to be the report's own,
// a folder that's gone said so with the report still saved, none ("")
// null; never a folder's name (the vehicle's label) in the file
TEST(RecordingViewTest, HealthReportNamesTheBaselineItIsGiven)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    QList<QVariantMap> saved;
    QObject::connect(&v, &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    const struct {
        QString folder;
        bool read;
        bool own;
    } cases[] = {
        {fixture("findings"), true, false},
        {folder, true, true},
        {root.path() + QStringLiteral("/vanished-folder"), false, false},
        {QString(), false, false},
    };
    int n = 0;
    for (const auto& c : cases) {
        SCOPED_TRACE(c.folder.toStdString());
        const QUrl url = QUrl::fromLocalFile(
            root.path() + QStringLiteral("/report-%1.json").arg(++n));
        saved.clear();
        ASSERT_TRUE(v.saveHealthReport(url, 1, c.folder));
        ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
        ASSERT_TRUE(saved[0].value(QStringLiteral("ok")).toBool())
            << saved[0].value(QStringLiteral("words")).toString().toStdString();
        EXPECT_EQ(saved[0].value(QStringLiteral("baselineRead")).toBool(),
                  c.read);
        QFile f(url.toLocalFile());
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        EXPECT_FALSE(bytes.contains("Daily"));
        EXPECT_FALSE(bytes.contains("vanished"));
        const QJsonValue b = QJsonDocument::fromJson(bytes).object().value(
            QStringLiteral("baseline"));
        if (c.folder.isEmpty()) {
            EXPECT_TRUE(b.isNull()) << bytes.toStdString();
            continue;
        }
        EXPECT_EQ(b.toObject().value(QStringLiteral("read")).toBool(), c.read);
        EXPECT_EQ(b.toObject().value(QStringLiteral("is_source")).toBool(),
                  c.own);
    }
}

// A report under way when the viewer closes or opens another recording
// ends unannounced: its thread runs out (only the viewer's destruction
// stops it), and the result is dropped
TEST(RecordingViewTest, AReportEndingAfterACloseIsDropped)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    int saved = 0;
    QObject::connect(&v, &RecordingView::reportSaved, [&] { ++saved; });
    const QUrl url =
        QUrl::fromLocalFile(root.path() + QStringLiteral("/r.txt"));
    ASSERT_TRUE(v.saveHealthReport(url, 0));
    v.close();
    ASSERT_TRUE(waitFor([&] { return !v.reporting(); }));
    EXPECT_EQ(saved, 0);

    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_TRUE(v.saveHealthReport(url, 0));
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return !v.reporting() && settled(v); }));
    EXPECT_EQ(saved, 0) << "another open drops it too";
}

// A report under way when the viewer goes (the app quitting) stops at
// core's next ask: the destructor's wait ends with no file written, where
// the same report run to its end writes one
TEST(RecordingViewTest, AReportStopsWhenTheViewerGoes)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    ASSERT_FALSE(folder.isEmpty());
    {
        QFile frames(folder + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::Append));
        QByteArray rows;
        for (int i = 0; i < 200000; ++i) {
            const QByteArray us = QByteArray::number(400000 + i * 100);
            rows += us + ',' + us + ",0x7E8,0x0,04 41 0C 0C 90 00 00 00\n";
        }
        ASSERT_EQ(frames.write(rows), rows.size());
    }
    auto v = std::make_unique<RecordingView>();
    v->open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(*v); }, 30000));
    ASSERT_EQ(v->state(), RecordingView::Open) << v->error().toStdString();
    QList<QVariantMap> saved;
    QObject::connect(v.get(), &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    const QUrl whole =
        QUrl::fromLocalFile(root.path() + QStringLiteral("/whole.txt"));
    ASSERT_TRUE(v->saveHealthReport(whole, 0));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }, 30000));
    ASSERT_TRUE(saved[0].value(QStringLiteral("ok")).toBool())
        << saved[0].value(QStringLiteral("words")).toString().toStdString();
    ASSERT_TRUE(QFile::exists(whole.toLocalFile()));

    const QUrl stopped =
        QUrl::fromLocalFile(root.path() + QStringLiteral("/stopped.txt"));
    ASSERT_TRUE(v->saveHealthReport(stopped, 0));
    v.reset();
    EXPECT_FALSE(QFile::exists(stopped.toLocalFile()));
}

// A report the picker's document (a provider's, which the picker has made
// already) doesn't end up holding removes the document, as a failed export
// does: one core refuses, and one stopped under way
TEST(RecordingViewTest, AFailedReportLeavesAPickedDocumentGone)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.setThroughProviderForTesting(true);
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    {
        QFile frames(folder + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::WriteOnly | QIODevice::Truncate));
        frames.write("not a frames file\n");
    }
    const QString picked = root.path() + QStringLiteral("/picked.txt");
    {
        QFile f(picked);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    QList<QVariantMap> saved;
    QObject::connect(&v, &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    ASSERT_TRUE(v.saveHealthReport(QUrl::fromLocalFile(picked), 0));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_FALSE(saved[0].value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(QFile::exists(picked)) << "the empty document goes";

    // A local file's picker is the file dialog's, which asked about
    // replacing: nothing is removed there
    v.setThroughProviderForTesting(false);
    {
        QFile f(picked);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    saved.clear();
    ASSERT_TRUE(v.saveHealthReport(QUrl::fromLocalFile(picked), 0));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_FALSE(saved[0].value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(QFile::exists(picked));
}

// A save pick's verdict reaches the viewer's report: an empty document the
// user chose to replace stays when core refuses the report
TEST(RecordingViewTest, ARefusedReportFollowsTheSavePicksVerdict)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.setThroughProviderForTesting(true);
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    {
        QFile frames(folder + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::WriteOnly | QIODevice::Truncate));
        frames.write("not a frames file\n");
    }
    const QString picked = root.path() + QStringLiteral("/picked.txt");
    {
        QFile f(picked);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    const QUrl url = QUrl::fromLocalFile(picked);
    notePick(pathOfUrl(url), PickMade::Replaced);
    QList<QVariantMap> saved;
    QObject::connect(&v, &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    ASSERT_TRUE(v.saveHealthReport(url, 0));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_FALSE(saved[0].value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(QFile::exists(picked)) << "a replaced empty document stays";
}

// A report the viewer refuses before its thread (nothing open) drops the
// pick's own empty document and leaves one the user chose to replace
TEST(RecordingViewTest, AReportRefusedWithNothingOpenFollowsTheSavePicksVerdict)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    RecordingView v;
    v.setThroughProviderForTesting(true);
    const auto empty = [&](const char* name) {
        const QString path =
            root.path() + QLatin1Char('/') + QString::fromLatin1(name);
        QFile f(path);
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        return path;
    };
    const QString made = empty("made.txt");
    notePick(pathOfUrl(QUrl::fromLocalFile(made)), PickMade::Made);
    EXPECT_FALSE(v.saveHealthReport(QUrl::fromLocalFile(made), 0));
    EXPECT_FALSE(QFile::exists(made)) << "the pick's own empty document goes";
    const QString replaced = empty("replaced.txt");
    notePick(pathOfUrl(QUrl::fromLocalFile(replaced)), PickMade::Replaced);
    EXPECT_FALSE(v.saveHealthReport(QUrl::fromLocalFile(replaced), 0));
    EXPECT_TRUE(QFile::exists(replaced)) << "a replaced document stays";
}

TEST(RecordingViewTest, ARefusedReportLeavesAReplacedDocumentAlone)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.setThroughProviderForTesting(true);
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    {
        QFile frames(folder + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::WriteOnly | QIODevice::Truncate));
        frames.write("not a frames file\n");
    }
    const QString picked = root.path() + QStringLiteral("/picked.txt");
    const QByteArray theirs("the user's earlier report\n");
    {
        QFile f(picked);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(theirs);
    }
    QList<QVariantMap> saved;
    QObject::connect(&v, &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    ASSERT_TRUE(v.saveHealthReport(QUrl::fromLocalFile(picked), 0));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_FALSE(saved[0].value(QStringLiteral("ok")).toBool());
    QFile after(picked);
    ASSERT_TRUE(after.open(QIODevice::ReadOnly));
    EXPECT_EQ(after.readAll(), theirs) << "the replaced document stays";
}

TEST(RecordingViewTest, AStoppedReportLeavesAPickedDocumentGone)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    ASSERT_FALSE(folder.isEmpty());
    {
        QFile frames(folder + QStringLiteral("/frames.csv"));
        ASSERT_TRUE(frames.open(QIODevice::Append));
        QByteArray rows;
        for (int i = 0; i < 200000; ++i) {
            const QByteArray us = QByteArray::number(400000 + i * 100);
            rows += us + ',' + us + ",0x7E8,0x0,04 41 0C 0C 90 00 00 00\n";
        }
        ASSERT_EQ(frames.write(rows), rows.size());
    }
    auto v = std::make_unique<RecordingView>();
    v->setThroughProviderForTesting(true);
    v->open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(*v); }, 30000));
    ASSERT_EQ(v->state(), RecordingView::Open) << v->error().toStdString();
    const QString picked = root.path() + QStringLiteral("/picked.txt");
    {
        QFile f(picked);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    ASSERT_TRUE(v->saveHealthReport(QUrl::fromLocalFile(picked), 0));
    v.reset();
    EXPECT_FALSE(QFile::exists(picked)) << "the empty document goes";
}

// A report thread the system can't start says no, with nothing reporting,
// and the picker's document goes
TEST(RecordingViewTest, AReportWhoseThreadCantStartSaysSo)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString folder = carRecording(root);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.setThroughProviderForTesting(true);
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    const QString picked = root.path() + QStringLiteral("/picked.txt");
    {
        QFile f(picked);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    int changes = 0;
    QObject::connect(&v, &RecordingView::reportingChanged, [&] { ++changes; });
    v.setFailThreadForTesting(true);
    EXPECT_FALSE(v.saveHealthReport(QUrl::fromLocalFile(picked), 0));
    EXPECT_FALSE(v.reporting());
    EXPECT_EQ(changes, 0);
    EXPECT_FALSE(QFile::exists(picked));

    v.setFailThreadForTesting(false);
    QList<QVariantMap> saved;
    QObject::connect(&v, &RecordingView::reportSaved,
                     [&](const QVariantMap& r) { saved << r; });
    ASSERT_TRUE(v.saveHealthReport(QUrl::fromLocalFile(picked), 0));
    ASSERT_TRUE(waitFor([&] { return !saved.isEmpty(); }));
    EXPECT_TRUE(saved[0].value(QStringLiteral("ok")).toBool());
}

// A statistics ask put in while a column means pass is under way is
// answered before the pass ends (the pass gives way and resumes), and the
// means still arrive, the same as a pass that was never interrupted
TEST(RecordingViewTest, AStatisticsAskDuringAMeansPassIsAnsweredBeforeItEnds)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = denseRecording(dir);
    ASSERT_FALSE(folder.isEmpty());
    RecordingView v;
    v.open(folder);
    ASSERT_TRUE(waitFor([&] { return settled(v); }, 120000));
    ASSERT_EQ(v.state(), RecordingView::Open) << v.error().toStdString();
    const QVariantList ids{0, 1, 2};
    const int columns = 65536;
    QElapsedTimer timer;
    timer.start();
    // An unoptimized build on CI's macOS runner takes over 10 s
    const std::vector<QList<double>> alone =
        askedMeans(v, ids, 0, 6.0e6, columns, 120000);
    const qint64 whole = timer.elapsed();
    ASSERT_EQ(v.meansYieldsForTesting(), 0);

    qint64 statsAt = -1;
    qint64 meansAt = -1;
    std::vector<QList<double>> got;
    QObject::connect(
        &v, &RecordingView::statsReady,
        [&](int, const QVariantList&) { statsAt = timer.elapsed(); });
    QObject::connect(&v, &RecordingView::columnMeansReady,
                     [&](int, const QVariantList& m) {
                         meansAt = timer.elapsed();
                         for (const QVariant& one : m) {
                             got.push_back(one.toMap()
                                               .value(QStringLiteral("means"))
                                               .value<QList<double>>());
                         }
                     });
    // The pass is taken and held, so the statistics ask waits before its
    // first column and it gives way at its second
    const int hold = 100;
    v.setAsksHoldForTesting(hold);
    timer.restart();
    ASSERT_GT(v.columnMeans(ids, 0, 6.0e6, columns), 0);
    ASSERT_TRUE(waitFor([&] { return v.asksHeldForTesting() > 0; }));
    v.setAsksHoldForTesting(0);
    const qint64 askedAt = timer.elapsed();
    ASSERT_GT(v.stats(ids, 2.9e6, 3.1e6), 0);
    ASSERT_TRUE(waitFor([&] { return statsAt >= 0 && meansAt >= 0; }, 120000));
    EXPECT_LT(statsAt, meansAt) << "the statistics waited for the whole pass";
    EXPECT_EQ(v.meansYieldsForTesting(), 1);
    ASSERT_EQ(got.size(), alone.size());
    for (size_t k = 0; k < got.size(); ++k) {
        ASSERT_EQ(got[k].size(), alone[k].size());
        for (qsizetype c = 0; c < got[k].size(); ++c) {
            const double a = got[k][c];
            const double b = alone[k][c];
            ASSERT_TRUE(a == b || (std::isnan(a) && std::isnan(b)))
                << "signal " << k << " column " << c;
        }
    }
    if (whole < 100) {
        GTEST_SKIP() << "the wait isn't judged: a whole pass took " << whole
                     << " ms, too short to tell";
    }
    EXPECT_LT(statsAt - askedAt, hold + whole / 2)
        << "a whole pass takes " << whole << " ms";
}
