// File/layer: tests/test_recording_import_map.cpp | GoogleTest, Qt6 Core, bridge tier
// Testing: BRIDGE: BridgeTest (test_bridge)

#include "test_recording_import_support.h"

namespace {

// The edit of a header column the panel hands composeMap
QVariantMap edit(const QString& column, bool include, const QString& name,
                 const QString& unit, const QString& kind)
{
    return {{QStringLiteral("column"), column},
            {QStringLiteral("include"), include},
            {QStringLiteral("name"), name},
            {QStringLiteral("unit"), unit},
            {QStringLiteral("kind"), kind}};
}

} // namespace

/* The guess is core's map for the file, as core wrote it, with each header
 * column's use, why not, name, unit and kind, and the first rows of its
 * cells for display */
TEST(BridgeTest, ImportGuessesAColumnMapForTheFile)
{
    RecordingImport im([] { return QString(); });
    const QVariantMap g = guessed(im, fileUrl(fixture("logger-a/log.csv")));
    ASSERT_TRUE(ok({g}))
        << g.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(
        g.value(QStringLiteral("mapText")).toString().remove(QLatin1Char('\r')),
        text(fixture("logger-a/guessed-map.json")).trimmed());
    EXPECT_TRUE(g.value(QStringLiteral("unitKnown")).toBool());
    EXPECT_TRUE(g.value(QStringLiteral("decimalComma")).toBool());
    EXPECT_TRUE(g.value(QStringLiteral("preamble")).toBool());
    EXPECT_EQ(g.value(QStringLiteral("delimiter")).toString(),
              QStringLiteral(";"));
    EXPECT_EQ(g.value(QStringLiteral("headerRow")).toInt(), 3);
    EXPECT_EQ(g.value(QStringLiteral("timeColumn")).toString(),
              QStringLiteral("Time [s]"));
    EXPECT_EQ(g.value(QStringLiteral("timeUnit")).toString(),
              QStringLiteral("s"));

    ASSERT_EQ(g.value(QStringLiteral("columns")).toList().size(), 5);
    const QVariantMap time = column(g, 0);
    EXPECT_TRUE(time.value(QStringLiteral("isTime")).toBool());
    EXPECT_EQ(time.value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral("0,00"), QStringLiteral("0,25"),
                           QStringLiteral("0,50"), QStringLiteral("0,75"),
                           QStringLiteral("1,00")}))
        << "the first five rows below the header row, as the file has them";
    const QVariantMap rpm = column(g, 1);
    EXPECT_FALSE(rpm.value(QStringLiteral("isTime")).toBool());
    EXPECT_TRUE(rpm.value(QStringLiteral("used")).toBool());
    EXPECT_EQ(rpm.value(QStringLiteral("column")).toString(),
              QStringLiteral("Engine Speed [rpm]"));
    EXPECT_EQ(rpm.value(QStringLiteral("name")).toString(),
              QStringLiteral("Engine Speed"));
    EXPECT_EQ(rpm.value(QStringLiteral("unit")).toString(),
              QStringLiteral("rpm"));
    EXPECT_EQ(rpm.value(QStringLiteral("kind")).toString(),
              QStringLiteral("number"));
    EXPECT_EQ(column(g, 4).value(QStringLiteral("kind")).toString(),
              QStringLiteral("text"));
}

/* A quoted file is split as its quotes say (a BOM, a quoted comma, a
 * doubled quote); a long cell is cut; the file's path is nowhere in the
 * result */
TEST(BridgeTest, ImportShowsTheFirstRowsAsTheFileHasThem)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    const QString longCell(40, QLatin1Char('x'));
    const QByteArray csv = "\xEF\xBB\xBF\"time (s)\",\"a, b\",c,note\r\n"
                           "0,1,2,\"he said \"\"hi\"\"\"\r\n"
                           "1,3,4,x\r\n"
                           "2,5,6,y\r\n"
                           "3,4,5," +
                           longCell.toUtf8() + "\r\n";
    const QString path = write(dir, "quoted.csv", csv);
    const QVariantMap g = guessed(im, fileUrl(path));
    ASSERT_TRUE(ok({g}))
        << g.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(column(g, 1).value(QStringLiteral("column")).toString(),
              QStringLiteral("a, b"));
    EXPECT_EQ(column(g, 1).value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral("1"), QStringLiteral("3"),
                           QStringLiteral("5"), QStringLiteral("4")}));
    EXPECT_EQ(column(g, 2).value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral("2"), QStringLiteral("4"),
                           QStringLiteral("6"), QStringLiteral("5")}));
    EXPECT_EQ(column(g, 3).value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral("he said \"hi\""),
                           QStringLiteral("x"), QStringLiteral("y"),
                           longCell.left(29) + QStringLiteral("...")}));
    for (const QVariant& v : g)
        EXPECT_FALSE(v.toString().contains(path));
}

/* A padded file's cells show as the import reads them: an unquoted cell
 * trimmed (a blank one empty), a quoted one exact; a label is the header
 * without its padding, and a header of spaces is an empty one */
TEST(BridgeTest, ImportShowsPaddedCellsAsTheImportReadsThem)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    const QVariantMap g =
        guessed(im, fileUrl(write(dir, "padded.csv",
                                  "Time, \"Engine speed (rpm)\" , Mode, \n"
                                  "0,  900, \" OL \", \n"
                                  "250, 910, , \n"
                                  "500,\t950 , OL, \n")));
    ASSERT_TRUE(ok({g}))
        << g.value(QStringLiteral("words")).toString().toStdString();
    ASSERT_EQ(g.value(QStringLiteral("columns")).toList().size(), 4);
    EXPECT_EQ(column(g, 1).value(QStringLiteral("column")).toString(),
              QStringLiteral("Engine speed (rpm)"));
    EXPECT_EQ(column(g, 1).value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral("900"), QStringLiteral("910"),
                           QStringLiteral("950")}));
    EXPECT_EQ(column(g, 2).value(QStringLiteral("column")).toString(),
              QStringLiteral(" Mode"));
    EXPECT_EQ(column(g, 2).value(QStringLiteral("label")).toString(),
              QStringLiteral("Mode"));
    EXPECT_EQ(column(g, 2).value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral(" OL "), QString(),
                           QStringLiteral("OL")}));
    const QVariantMap blank = column(g, 3);
    EXPECT_EQ(blank.value(QStringLiteral("label")).toString(), QString());
    EXPECT_EQ(blank.value(QStringLiteral("why")).toString(),
              QStringLiteral("its header cell is empty"));
    EXPECT_FALSE(blank.value(QStringLiteral("canPutBack")).toBool());
    EXPECT_EQ(blank.value(QStringLiteral("samples")).toStringList(),
              (QStringList{QString(), QString(), QString()}));
}

/* A tab delimiter right after a closing quote ends the cell, as core's
 * reader has it: the preview doesn't merge the cells around it */
TEST(BridgeTest, ImportShowsATabAfterAQuotedCellAsTheEndOfIt)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    const QVariantMap g = guessed(
        im, fileUrl(write(dir, "tabbed.tsv",
                          "Time\tMode\tLoad\n"
                          "0\t\"OL\"\t30\n"
                          "250\t\"CL\"\t40\n"
                          "500\t\"OL\" \t50\n")));
    ASSERT_TRUE(ok({g}))
        << g.value(QStringLiteral("words")).toString().toStdString();
    ASSERT_EQ(g.value(QStringLiteral("columns")).toList().size(), 3);
    EXPECT_EQ(column(g, 1).value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral("OL"), QStringLiteral("CL"),
                           QStringLiteral("OL")}));
    EXPECT_EQ(column(g, 2).value(QStringLiteral("samples")).toStringList(),
              (QStringList{QStringLiteral("30"), QStringLiteral("40"),
                           QStringLiteral("50")}));
}

/* A left-out column can be put back unless its header can't name it: an
 * empty header, or one the file has twice (core refuses a map naming it) */
TEST(BridgeTest, ImportGuessPutsBackOnlyAColumnAMapCanName)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    const QVariantMap g =
        guessed(im, fileUrl(write(dir, "twice.csv",
                                  "time (s),rpm,dup,dup,empty,\n"
                                  "0,800,1,2,,\n1,900,3,4,,\n2,950,5,6,,\n")));
    ASSERT_TRUE(ok({g}))
        << g.value(QStringLiteral("words")).toString().toStdString();
    ASSERT_EQ(g.value(QStringLiteral("columns")).toList().size(), 6);
    EXPECT_FALSE(column(g, 2).value(QStringLiteral("used")).toBool());
    EXPECT_FALSE(column(g, 2).value(QStringLiteral("canPutBack")).toBool());
    EXPECT_FALSE(column(g, 3).value(QStringLiteral("canPutBack")).toBool());
    EXPECT_TRUE(column(g, 4).value(QStringLiteral("canPutBack")).toBool());
    EXPECT_FALSE(column(g, 5).value(QStringLiteral("canPutBack")).toBool());
}

/* A name or a shown cell is cut on a character, never between the halves
 * of a surrogate pair */
TEST(BridgeTest, ImportCutsNamesAndCellsOnWholeCharacters)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
    // 30 bytes, then a 4-byte character that crosses the 31-byte limit
    const QString header = QString(30, QLatin1Char('a')) + emoji;
    const QString cell =
        QString(28, QLatin1Char('x')) + emoji + QString(10, QLatin1Char('y'));
    const QByteArray csv = "time (s),rpm," + header.toUtf8() +
                           ",note\n0,800,," + cell.toUtf8() + "\n1,900,," +
                           cell.toUtf8() + "\n";
    const QVariantMap g = guessed(im, fileUrl(write(dir, "astral.csv", csv)));
    ASSERT_TRUE(ok({g}))
        << g.value(QStringLiteral("words")).toString().toStdString();
    ASSERT_EQ(g.value(QStringLiteral("columns")).toList().size(), 4);
    const QVariantMap col = column(g, 2);
    const QString name = col.value(QStringLiteral("name")).toString();
    EXPECT_EQ(name, QString(30, QLatin1Char('a')));
    EXPECT_EQ(QString::fromUtf8(name.toUtf8()), name);
    ASSERT_FALSE(
        column(g, 3).value(QStringLiteral("samples")).toStringList().isEmpty());
    EXPECT_EQ(
        column(g, 3).value(QStringLiteral("samples")).toStringList().first(),
        QString(28, QLatin1Char('x')) + QStringLiteral("..."));
}

/* A time named so, of whole numbers with no unit, reads as ms; the unit the
 * caller gives wins; a file with no header is refused in core's words */
TEST(BridgeTest, ImportGuessTakesTheTimeUnitTheCallerGives)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    const QUrl plain =
        fileUrl(write(dir, "plain.csv", "time,rpm\n0,800\n1,900\n2,1000\n"));
    QVariantMap g = guessed(im, plain);
    ASSERT_TRUE(ok({g}));
    EXPECT_TRUE(g.value(QStringLiteral("unitKnown")).toBool());
    EXPECT_TRUE(g.value(QStringLiteral("timeNumbers")).toBool());
    EXPECT_EQ(g.value(QStringLiteral("timeUnit")).toString(),
              QStringLiteral("ms"));

    g = guessed(im, plain, QStringLiteral("s"));
    ASSERT_TRUE(ok({g}));
    EXPECT_EQ(g.value(QStringLiteral("timeUnit")).toString(),
              QStringLiteral("s"));

    g = guessed(im, plain, QStringLiteral("ms"));
    ASSERT_TRUE(ok({g}));
    EXPECT_TRUE(g.value(QStringLiteral("unitKnown")).toBool());
    EXPECT_EQ(g.value(QStringLiteral("timeUnit")).toString(),
              QStringLiteral("ms"));

    g = guessed(im,
                fileUrl(write(dir, "none.csv", "just,words\nmore,words\n")));
    EXPECT_FALSE(ok({g}));
    EXPECT_FALSE(g.value(QStringLiteral("words")).toString().isEmpty());
    g = guessed(im, fileUrl(dir.filePath(QStringLiteral("missing.csv"))));
    EXPECT_FALSE(ok({g}));
    EXPECT_FALSE(
        g.value(QStringLiteral("words")).toString().contains(dir.path()));
}

/* The same guess for a file the app can't open in place */
TEST(BridgeTest, ImportGuessCopiesAFileItCantReadInPlace)
{
    RecordingImport im([] { return QString(); });
    im.setCopyLocalFilesForTesting(true);
    const QVariantMap g = guessed(im, fileUrl(fixture("logger-b/log.csv")));
    ASSERT_TRUE(ok({g}))
        << g.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(g.value(QStringLiteral("timeUnit")).toString(),
              QStringLiteral("iso8601"));
    EXPECT_FALSE(g.value(QStringLiteral("timeNumbers")).toBool());
}

/* The edits become the map the import gets: a column left out or renamed,
 * a unit dropped, a kind changed (a text keeps no decimals), a left-out
 * column put back, the time's unit set, the time column never a signal; a
 * map that isn't JSON gives "" */
TEST(BridgeTest, ImportComposesTheMapFromTheEdits)
{
    RecordingImport im([] { return QString(); });
    const QVariantMap g = guessed(im, fileUrl(fixture("logger-a/log.csv")));
    ASSERT_TRUE(ok({g}));
    const QString base = g.value(QStringLiteral("mapText")).toString();
    const QVariantList edits{
        edit(QStringLiteral("Time [s]"), true, QStringLiteral("Time"), {},
             QStringLiteral("text")),
        edit(QStringLiteral("Engine Speed [rpm]"), true, QStringLiteral("RPM"),
             {}, QStringLiteral("number")),
        edit(QStringLiteral("Load [%]"), false, QStringLiteral("Load"),
             QStringLiteral("%"), QStringLiteral("number")),
        edit(QStringLiteral("Lambda"), true, QStringLiteral("Lambda"),
             QStringLiteral("l"), QStringLiteral("text")),
        edit(QStringLiteral("Brand new"), true, QStringLiteral("Brand new"), {},
             QStringLiteral("text"))};
    const QString composed = im.composeMap(base, edits, QStringLiteral("ms"));
    const QJsonObject map = QJsonDocument::fromJson(composed.toUtf8()).object();
    EXPECT_EQ(
        map.value(QStringLiteral("time")).toObject(),
        (QJsonObject{{QStringLiteral("column"), QStringLiteral("Time [s]")},
                     {QStringLiteral("unit"), QStringLiteral("ms")}}));
    EXPECT_EQ(map.value(QStringLiteral("delimiter")).toString(),
              QStringLiteral(";"))
        << "what the edits don't touch is the guess's";
    const QJsonArray sigs = map.value(QStringLiteral("signals")).toArray();
    ASSERT_EQ(sigs.size(), 3)
        << "an excluded column and the time column are out";
    for (const QJsonValue s : sigs) {
        EXPECT_NE(s.toObject().value(QStringLiteral("column")).toString(),
                  QStringLiteral("Time [s]"))
            << "core refuses the time column as a signal";
    }
    const QJsonObject rpm = sigs[0].toObject();
    EXPECT_EQ(rpm.value(QStringLiteral("name")).toString(),
              QStringLiteral("RPM"));
    EXPECT_FALSE(rpm.contains(QStringLiteral("unit")))
        << "an empty unit is dropped";
    EXPECT_EQ(rpm.value(QStringLiteral("decimals")).toInt(), 2)
        << "a number keeps its other keys";
    const QJsonObject lambda = sigs[1].toObject();
    EXPECT_EQ(lambda.value(QStringLiteral("kind")).toString(),
              QStringLiteral("text"));
    EXPECT_FALSE(lambda.contains(QStringLiteral("decimals")))
        << "a text has no decimals, which core refuses";
    EXPECT_EQ(lambda.value(QStringLiteral("unit")).toString(),
              QStringLiteral("l"));
    EXPECT_EQ(sigs[2].toObject().value(QStringLiteral("column")).toString(),
              QStringLiteral("Brand new"));

    EXPECT_EQ(im.composeMap(QStringLiteral("not json"), edits, {}), QString());
    EXPECT_FALSE(
        QJsonDocument::fromJson(im.composeMap(base, edits, {}).toUtf8())
            .object()
            .value(QStringLiteral("time"))
            .toObject()
            .value(QStringLiteral("unit"))
            .toString() == QLatin1String("ms"))
        << "no unit given keeps the map's";
}

/* The guess, imported as it is and with an edit, makes a recording; an
 * edit core refuses is refused in core's words and makes none */
TEST(BridgeTest, ImportStartsFromTheMapsTextAndHoldsNoMapFile)
{
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("recordings"));
    RecordingImport im([root] { return root; });
    const QUrl log = fileUrl(fixture("logger-a/log.csv"));
    const QVariantMap g = guessed(im, log);
    ASSERT_TRUE(ok({g}));

    Imported i;
    bool done = false;
    QObject context;
    QObject::connect(&im, &RecordingImport::finished, &context,
                     [&](const QVariantMap& r) {
                         i.result = r;
                         done = true;
                     });
    ASSERT_TRUE(
        im.startWithMap(log, g.value(QStringLiteral("mapText")).toString()));
    EXPECT_FALSE(im.startWithMap(log, QString())) << "one at a time";
    ASSERT_TRUE(waitUntil([&] { return done; }));
    ASSERT_TRUE(ok(i)) << words(i).toStdString();
    EXPECT_GT(i.result.value(QStringLiteral("values")).toDouble(), 0.0);
    EXPECT_EQ(entries(root).size(), 1);

    // A name past 31 bytes, core's rule, refused with its words
    done = false;
    const QVariantList edits{
        edit(QStringLiteral("Time [s]"), false, {}, {}, QStringLiteral("text")),
        edit(QStringLiteral("Lambda"), true, QString(40, QLatin1Char('n')), {},
             QStringLiteral("number"))};
    ASSERT_TRUE(im.startWithMap(
        log, im.composeMap(g.value(QStringLiteral("mapText")).toString(), edits,
                           {})));
    ASSERT_TRUE(waitUntil([&] { return done; }));
    EXPECT_FALSE(ok(i));
    EXPECT_FALSE(words(i).isEmpty());
    EXPECT_EQ(entries(root).size(), 1) << "a refused import makes none";
}

/* Saved maps: the name made safe, kept under maps/, listed by name, never
 * overwritten */
TEST(BridgeTest, ImportSavesAMapUnderASafeNameAndListsIt)
{
    QTemporaryDir dir;
    RecordingImport im([] { return QString(); });
    im.setMapsRootForTesting(dir.filePath(QStringLiteral("settings/maps")));
    EXPECT_TRUE(im.savedMaps().isEmpty());

    QVariantMap r =
        im.saveMap(QStringLiteral("  My logger: v2/x "), QStringLiteral("{}"));
    ASSERT_TRUE(ok({r}))
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("name")).toString(),
              QStringLiteral("My-logger--v2-x"));
    EXPECT_EQ(text(r.value(QStringLiteral("url")).toUrl().toLocalFile()),
              QStringLiteral("{}"));

    r = im.saveMap(QStringLiteral("My logger: v2/x"),
                   QStringLiteral("{\"other\": 1}"));
    EXPECT_FALSE(ok({r}));
    EXPECT_EQ(r.value(QStringLiteral("words")).toString(),
              QStringLiteral("a map named My-logger--v2-x is saved already"));
    EXPECT_EQ(text(im.savedMaps()
                       .at(0)
                       .toMap()
                       .value(QStringLiteral("url"))
                       .toUrl()
                       .toLocalFile()),
              QStringLiteral("{}"))
        << "the first is kept";
    for (const char* none : {"", "   ", "///"}) {
        r = im.saveMap(QLatin1String(none), QStringLiteral("{}"));
        EXPECT_FALSE(ok({r}));
        EXPECT_EQ(r.value(QStringLiteral("words")).toString(),
                  QStringLiteral("give the map a name"));
    }
    ASSERT_TRUE(ok({im.saveMap(QStringLiteral("a"), QStringLiteral("{}"))}));
    const QVariantList saved = im.savedMaps();
    ASSERT_EQ(saved.size(), 2);
    EXPECT_EQ(saved.at(0).toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("a"));
    EXPECT_EQ(saved.at(1).toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("My-logger--v2-x"));
}
