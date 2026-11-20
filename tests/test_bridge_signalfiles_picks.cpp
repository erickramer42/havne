// File/layer: tests/test_bridge_signalfiles_picks.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "test_bridge_signalfiles_support.h"

/* The bridge's own refusal of a file it can't read names no path: the
 * folder can hold the account's name */
TEST(BridgeTest, AFileThatCantBeReadIsRefusedWithoutItsPath)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QUrl missing = QUrl::fromLocalFile(
        from.path() + QStringLiteral("/missing-extras.json"));
    SessionBridge bridge;
    for (const QVariantMap& r :
         {bridge.readImport(missing), bridge.importPlugin(missing)}) {
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
        const QString words = r.value(QStringLiteral("words")).toString();
        EXPECT_EQ(words.toStdString(), "Couldn't read the file");
        EXPECT_FALSE(words.contains(QStringLiteral("missing")));
    }
}

/* A layout file (ADR-0057's appendix on the layout file): the text goes out
 * and comes back as it was, a file already at the target is replaced whole,
 * and a write that can't be made leaves what was there. */
TEST(BridgeTest, ALayoutFileRoundTripsItsText)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    SessionBridge bridge;
    const QString text =
        QStringLiteral("{\"format\":2,\"label\":\"caf\u00e9 \U0001F600\",\"pages\":[]}");
    const QUrl target = QUrl::fromLocalFile(dir.path() + QStringLiteral("/l.json"));
    writeScratch(dir.path(), "l.json", QByteArray(5000, 'x'));
    const QVariantMap saved = bridge.saveLayoutFile(text, target);
    EXPECT_TRUE(saved.value(QStringLiteral("ok")).toBool())
        << saved.value(QStringLiteral("words")).toString().toStdString();
    QFile f(target.toLocalFile());
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), text.toUtf8()) << "the text as UTF-8, replacing the file";
    f.close();
    const QVariantMap read = bridge.layoutFileText(target);
    EXPECT_TRUE(read.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(read.value(QStringLiteral("text")).toString(), text);
}

TEST(BridgeTest, ALayoutFileIsReadWithinItsBoundAndAsUtf8)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    SessionBridge bridge;
    const auto refused = [&](const QUrl& url, const char* expected) {
        const QVariantMap r = bridge.layoutFileText(url);
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool()) << expected;
        const QString words = r.value(QStringLiteral("words")).toString();
        EXPECT_EQ(words.toStdString(), expected);
        EXPECT_FALSE(words.contains(dir.path()));
        EXPECT_TRUE(r.value(QStringLiteral("text")).toString().isEmpty());
    };
    refused(QUrl::fromLocalFile(dir.path() + QStringLiteral("/none.json")),
            "Couldn't read the file");
    const qint64 most = SessionBridge::layoutFileMaxBytes;
    const QUrl at = writeScratch(dir.path(), "at.json", QByteArray(most, ' '));
    EXPECT_TRUE(bridge.layoutFileText(at).value(QStringLiteral("ok")).toBool());
    const QUrl over =
        writeScratch(dir.path(), "over.json", QByteArray(most + 1, ' '));
    refused(over, "The file is over 82400 bytes, a layout file's most");
    refused(writeScratch(dir.path(), "latin1.json", QByteArray("{\"a\":\"\xE9\"}")),
            "The file isn't UTF-8 text");
    refused(writeScratch(dir.path(), "cut.json", QByteArray("{\"a\":\"\xE2\x82\"}")),
            "The file isn't UTF-8 text");
    const QVariantMap bom = bridge.layoutFileText(
        writeScratch(dir.path(), "bom.json", QByteArray("\xEF\xBB\xBF{}")));
    EXPECT_TRUE(bom.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(bom.value(QStringLiteral("text")).toString().toStdString(), "{}")
        << "a leading BOM is skipped, once";
}

TEST(BridgeTest, ALayoutThatCantBeSavedLeavesTheFileAsItWas)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    SessionBridge bridge;
    const QByteArray before(3000, 'y');
    const QUrl target = writeScratch(dir.path(), "keep.json", before);
    const auto unchanged = [&] {
        QFile f(target.toLocalFile());
        EXPECT_TRUE(f.open(QIODevice::ReadOnly));
        EXPECT_EQ(f.readAll(), before) << "an existing file is never touched";
    };
    // Past the bound: refused before anything is opened
    QVariantMap r = bridge.saveLayoutFile(
        QString(SessionBridge::layoutFileMaxBytes + 1, QLatin1Char('x')), target);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "The layout is over 82400 bytes, a layout file's most");
    unchanged();
    // Nothing to save
    r = bridge.saveLayoutFile(QString(), target);
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "There is no layout to save");
    unchanged();
    // A target that can't take a file: a folder stays a folder, nothing
    // is removed, and the words name no path
    r = bridge.saveLayoutFile(QStringLiteral("{}"),
                              QUrl::fromLocalFile(dir.path()));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("words")).toString().toStdString(),
              "Couldn't write the file");
    EXPECT_TRUE(QFileInfo(dir.path()).isDir());
    unchanged();
    r = bridge.saveLayoutFile(
        QStringLiteral("{}"),
        QUrl::fromLocalFile(dir.path() + QStringLiteral("/no/such/folder/l.json")));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(QFileInfo(dir.path() + QStringLiteral("/no")).exists());
    unchanged();
}

// A provider's document is opened by its URI as Android's Uri.parse wants it:
// encoded, so a name the picker makes for a name taken ("report (1).csv")
// keeps its space and parentheses apart from the path's own separators
TEST(BridgeTest, APickedUrlIsOpenedByItsEncodedName)
{
    EXPECT_EQ(
        pathOfUrl(
            QUrl(QStringLiteral(
                "content://com.android.externalstorage.documents/document/"
                "primary%3ADocuments%2Freport (1).csv")))
            .toStdString(),
        "content://com.android.externalstorage.documents/document/"
        "primary%3ADocuments%2Freport%20(1).csv");
    EXPECT_EQ(pathOfUrl(QUrl::fromLocalFile(QStringLiteral("/tmp/a b (1).csv")))
                  .toStdString(),
              "/tmp/a b (1).csv");
}

// Android's document picker hands back a URI's text, encoded: the URL made of
// it opens by that same text, so the content resolver is asked for the
// document picked, its escapes and parentheses as they came
TEST(BridgeTest, APickersUriOpensByTheTextItCameAs)
{
    for (const char* uri : {
             "content://com.android.externalstorage.documents/document/"
             "primary%3ADownload%2Fname%20(1).json",
             "content://com.android.externalstorage.documents/document/"
             "primary%3ADownload%2Fname%20%281%29.json",
             "content://com.android.providers.downloads.documents/document/"
             "msf%3A1000000034",
             "content://com.android.providers.media.documents/document/"
             "document%3A42",
             "content://com.google.android.apps.docs.storage/document/"
             "acc%3D1%3Bdoc%3Dencoded%3DAbC-_d%2Be%3D%3D",
             "content://com.android.externalstorage.documents/document/"
             "primary%3ADocuments%2F%C3%B8l%20%26%20%23%20%25.json",
         }) {
        const QString text = QString::fromLatin1(uri);
        const QUrl url = pickedUrl(text);
        EXPECT_TRUE(url.isValid()) << uri;
        EXPECT_FALSE(url.isLocalFile()) << uri;
        EXPECT_EQ(pathOfUrl(url).toStdString(), uri);
    }
}

// A picker through a provider makes the document before the app writes. A
// QUrl that isn't a file URL stands in for a content:// one: QFile opens its
// text as a path. A save that refuses removes only a document that was empty
// when the save began
TEST(BridgeTest, ARefusedSaveThroughAProviderRemovesOnlyAnEmptyDocument)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // pathOfUrl encodes a URL: a folder whose path an encoding changes (a
    // space or a non-ASCII name in it; a drive letter's case is no change)
    // can't stand in for a provider's
    if (QUrl(dir.path(), QUrl::TolerantMode)
            .toString(QUrl::FullyEncoded)
            .compare(dir.path(), Qt::CaseInsensitive) != 0) {
        GTEST_SKIP() << "the temporary folder's path changes when encoded";
    }
    SessionBridge bridge;
    const auto provider = [&](const QString& name) {
        return QUrl(dir.path() + QLatin1Char('/') + name, QUrl::TolerantMode);
    };
    const auto make = [&](const QUrl& url, const QByteArray& bytes) {
        QFile f(url.isLocalFile() ? url.toLocalFile() : url.toString());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly))
            << url.toString().toStdString();
        f.write(bytes);
    };
    const auto tooBig =
        QString(SessionBridge::layoutFileMaxBytes + 1, QLatin1Char('x'));
    const QByteArray theirs("the user's layout");
    struct Save {
        const char* name;
        std::function<QVariantMap(const QUrl&)> run;
    };
    const Save saves[] = {
        {"nothing to save",
         [&](const QUrl& u) { return bridge.saveLayoutFile(QString(), u); }},
        {"over the bound",
         [&](const QUrl& u) { return bridge.saveLayoutFile(tooBig, u); }},
        {"a file that didn't move, whose bytes aren't kept",
         [&](const QUrl& u) {
             return bridge.saveUnmigratedFile(
                 QStringLiteral("0123456789abcdef"), u);
         }},
    };
    for (const Save& save : saves) {
        SCOPED_TRACE(save.name);
        const QUrl made = provider(QStringLiteral("made(1).json"));
        make(made, QByteArray());
        EXPECT_FALSE(save.run(made).value(QStringLiteral("ok")).toBool());
        EXPECT_FALSE(QFile::exists(made.toString()))
            << "the picker's empty document goes";
        const QUrl chosen = provider(QStringLiteral("chosen.json"));
        make(chosen, theirs);
        EXPECT_FALSE(save.run(chosen).value(QStringLiteral("ok")).toBool());
        QFile after(chosen.toString());
        ASSERT_TRUE(after.open(QIODevice::ReadOnly));
        EXPECT_EQ(after.readAll(), theirs) << "a replaced document stays";
        after.close();
        QFile::remove(chosen.toString());
    }
    // A local target is never removed by a refusal
    const QUrl local =
        QUrl::fromLocalFile(dir.path() + QStringLiteral("/local.json"));
    make(local, QByteArray());
    EXPECT_FALSE(bridge.saveLayoutFile(QString(), local)
                     .value(QStringLiteral("ok"))
                     .toBool());
    EXPECT_TRUE(QFile::exists(local.toLocalFile()));
}

// A save pick's verdict, from its document's last_modified against the
// picker's launch and answer: the window allows 2 s of skew each way, and a
// time past the answer says nothing (a replaced document's upload ending
// meanwhile)
TEST(BridgeTest, ASavePicksVerdictIsItsDocumentsTimeAgainstTheLaunch)
{
    const qint64 launched = 1'700'000'000'000;
    const qint64 answered = launched + 60'000;
    EXPECT_EQ(kPickMadeWindowMs, 2000);
    const auto judged = [&](qint64 modified) {
        return judgePick(launched, answered, modified);
    };
    EXPECT_EQ(judged(launched), PickMade::Made);
    EXPECT_EQ(judged(answered), PickMade::Made);
    EXPECT_EQ(judged(launched - kPickMadeWindowMs), PickMade::Made)
        << "a provider's clock 2 s behind";
    EXPECT_EQ(judged(launched - kPickMadeWindowMs - 1), PickMade::Replaced);
    EXPECT_EQ(judged(launched - 86'400'000), PickMade::Replaced);
    EXPECT_EQ(judged(answered + kPickMadeWindowMs), PickMade::Made)
        << "a provider's clock 2 s ahead";
    EXPECT_EQ(judged(answered + kPickMadeWindowMs + 1), PickMade::Unknown)
        << "past the answer";
    EXPECT_EQ(judged(-1), PickMade::Unknown) << "no time";
    EXPECT_EQ(judged(0), PickMade::Unknown) << "a provider's none";
}

// A pick's verdict is taken once, a later pick of the same document replaces
// it, and one with no time clears it
TEST(BridgeTest, ASavePicksVerdictIsTakenOnce)
{
    const QString where = QStringLiteral("content://test/document/verdict");
    notePick(where, PickMade::Made);
    EXPECT_EQ(peekPick(where), PickMade::Made);
    notePick(where, PickMade::Replaced);
    EXPECT_EQ(takePick(where), PickMade::Replaced);
    EXPECT_EQ(takePick(where), PickMade::Unknown) << "taken once";
    notePick(where, PickMade::Made);
    notePick(where, PickMade::Unknown);
    EXPECT_EQ(takePick(where), PickMade::Unknown);
}

// Each branch of the rule a failed save removes its document by, judged at
// the save's entry; a missing document stands in for one a provider won't
// open (Proton Drive's, still uploading)
TEST(BridgeTest, APickedDocumentIsRemovableByItsVerdictAndItsBytes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray none;
    const QByteArray some("the user's bytes");
    struct Case {
        const char* name;
        PickMade verdict;
        const QByteArray* bytes; // null: unreadable
        bool removable;
    };
    const Case cases[] = {
        {"made, empty", PickMade::Made, &none, true},
        {"made, unreadable", PickMade::Made, nullptr, true},
        {"made, holding bytes", PickMade::Made, &some, false},
        {"replaced, empty", PickMade::Replaced, &none, false},
        {"replaced, unreadable", PickMade::Replaced, nullptr, false},
        {"no verdict, empty", PickMade::Unknown, &none, true},
        {"no verdict, unreadable", PickMade::Unknown, nullptr, false},
        {"no verdict, holding bytes", PickMade::Unknown, &some, false},
    };
    int n = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const QString path = dir.filePath(QStringLiteral("doc-%1").arg(++n));
        if (c.bytes != nullptr) {
            QFile f(path);
            ASSERT_TRUE(f.open(QIODevice::WriteOnly));
            f.write(*c.bytes);
        }
        notePick(path, c.verdict);
        EXPECT_EQ(pickedDocumentRemovable(path), c.removable);
        EXPECT_EQ(peekPick(path), PickMade::Unknown) << "the save took it";
    }
}

// The verdict through the bridge's saves (a non-file QUrl stands in for a
// provider's, as in ARefusedSaveThroughAProviderRemovesOnlyAnEmptyDocument):
// a refused layout save and the live report's refusal with no session
// remove the pick's own empty document and leave one the user chose to
// replace, empty or not
TEST(BridgeTest, ARefusedSaveFollowsItsSavePicksVerdict)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    if (QUrl(dir.path(), QUrl::TolerantMode)
            .toString(QUrl::FullyEncoded)
            .compare(dir.path(), Qt::CaseInsensitive) != 0) {
        GTEST_SKIP() << "the temporary folder's path changes when encoded";
    }
    SessionBridge bridge;
    const auto provider = [&](const QString& name) {
        return QUrl(dir.path() + QLatin1Char('/') + name, QUrl::TolerantMode);
    };
    const auto make = [&](const QUrl& url, const QByteArray& bytes) {
        QFile f(url.toString());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    };
    const QByteArray theirs("the user's earlier save");
    struct Save {
        const char* name;
        std::function<QVariantMap(const QUrl&)> run;
    };
    const Save saves[] = {
        {"a layout with nothing to save",
         [&](const QUrl& u) { return bridge.saveLayoutFile(QString(), u); }},
        {"a report with no session",
         [&](const QUrl& u) { return bridge.saveHealthReport(u, 0); }},
    };
    for (const Save& save : saves) {
        SCOPED_TRACE(save.name);
        const QUrl made = provider(QStringLiteral("made.txt"));
        make(made, QByteArray());
        notePick(pathOfUrl(made), PickMade::Made);
        EXPECT_FALSE(save.run(made).value(QStringLiteral("ok")).toBool());
        EXPECT_FALSE(QFile::exists(made.toString()))
            << "the pick's own empty document goes";

        const QUrl replaced = provider(QStringLiteral("replaced.txt"));
        make(replaced, QByteArray());
        notePick(pathOfUrl(replaced), PickMade::Replaced);
        EXPECT_FALSE(save.run(replaced).value(QStringLiteral("ok")).toBool());
        EXPECT_TRUE(QFile::exists(replaced.toString()))
            << "an empty document the user chose to replace stays";
        EXPECT_EQ(peekPick(pathOfUrl(replaced)), PickMade::Unknown)
            << "the save took the verdict";

        const QUrl full = provider(QStringLiteral("full.txt"));
        make(full, theirs);
        notePick(pathOfUrl(full), PickMade::Made);
        EXPECT_FALSE(save.run(full).value(QStringLiteral("ok")).toBool());
        QFile after(full.toString());
        ASSERT_TRUE(after.open(QIODevice::ReadOnly));
        EXPECT_EQ(after.readAll(), theirs) << "a document with bytes stays";
        after.close();
        QFile::remove(replaced.toString());
        QFile::remove(full.toString());
    }
}

// A save a page refuses before it calls the bridge (a layout that can't be
// written as a file) drops the pick's own empty document by the same rule
TEST(BridgeTest, ASaveThePageRefusesDropsOnlyThePicksOwnDocument)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    if (QUrl(dir.path(), QUrl::TolerantMode)
            .toString(QUrl::FullyEncoded)
            .compare(dir.path(), Qt::CaseInsensitive) != 0) {
        GTEST_SKIP() << "the temporary folder's path changes when encoded";
    }
    SessionBridge bridge;
    const auto empty = [&](const QString& name) {
        const QUrl url(dir.path() + QLatin1Char('/') + name,
                       QUrl::TolerantMode);
        QFile f(url.toString());
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        return url;
    };
    const QUrl made = empty(QStringLiteral("made.json"));
    notePick(pathOfUrl(made), PickMade::Made);
    bridge.dropPicked(made);
    EXPECT_FALSE(QFile::exists(made.toString()));
    const QUrl replaced = empty(QStringLiteral("replaced.json"));
    notePick(pathOfUrl(replaced), PickMade::Replaced);
    bridge.dropPicked(replaced);
    EXPECT_TRUE(QFile::exists(replaced.toString()));
    EXPECT_EQ(peekPick(pathOfUrl(replaced)), PickMade::Unknown);
}
