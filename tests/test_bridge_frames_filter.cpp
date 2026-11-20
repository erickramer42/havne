// File/layer: tests/test_bridge_frames_filter.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: FrameFilterTest, BridgeTest (test_bridge)

#include "test_bridge_frames_support.h"

// ---- The Log page's filter and By ID view (framefilter.h) ----

namespace {

// A frame for the model: its ID, flags, time and payload
struct FrameMaker {
    std::unique_ptr<ecuio_frame_t> frame = std::make_unique<ecuio_frame_t>();
    FrameMaker() { *frame = ecuio_frame_t{}; }
    void add(FrameListModel& model, uint32_t id, std::initializer_list<uint8_t> bytes,
             quint64 timeUs, uint32_t flags = 0)
    {
        frame->arbitration_id = id;
        frame->flags = flags;
        frame->timestamp_us = timeUs;
        uint16_t n = 0;
        for (uint8_t b : bytes) {
            frame->data[n++] = b;
        }
        frame->data_length = n;
        model.appendFrame(*frame, n);
    }
};

// Counts rows as the model's signals say, so a view that trusts them
// sees what rowCount says
struct RowMirror {
    int rows = 0;
    explicit RowMirror(QAbstractItemModel& m)
    {
        rows = m.rowCount();
        QObject::connect(&m, &QAbstractItemModel::rowsInserted,
                         [this](const QModelIndex&, int first, int last) {
                             rows += last - first + 1;
                         });
        QObject::connect(&m, &QAbstractItemModel::rowsRemoved,
                         [this](const QModelIndex&, int first, int last) {
                             rows -= last - first + 1;
                         });
        QObject::connect(&m, &QAbstractItemModel::modelReset,
                         [this, &m]() { rows = m.rowCount(); });
    }
};

QStringList idsShown(const FrameListModel& model)
{
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row) {
        out << QString::number(
                   model.data(model.index(row, 0), FrameListModel::ArbitrationIdRole)
                       .toUInt(),
                   16)
                   .toUpper();
    }
    return out;
}

bool passes(const QString& ids, const QString& data, uint32_t id,
            std::initializer_list<uint8_t> bytes, uint32_t flags = 0)
{
    FrameFilter f;
    EXPECT_EQ(FrameFilter::parseIds(ids, &f.ids).toStdString(), "");
    EXPECT_EQ(FrameFilter::parseData(data, &f.data, &f.anchored).toStdString(), "");
    std::vector<uint8_t> payload(bytes);
    return f.matches(id, flags, 0, payload.data(),
                     static_cast<int>(payload.size()));
}

} // namespace

/* The ID field: hex IDs and ranges, commas between, "0x" optional;
 * what doesn't parse says which item and why */
TEST(FrameFilterTest, TheIdFieldTakesIdsAndRanges)
{
    std::vector<FrameFilter::IdRange> r;
    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral(" 7E8, 0x7e0-7EF ,18DAF110"), &r)
                  .toStdString(),
              "");
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].first, 0x7E8u);
    EXPECT_EQ(r[0].last, 0x7E8u);
    EXPECT_EQ(r[1].first, 0x7E0u);
    EXPECT_EQ(r[1].last, 0x7EFu);
    EXPECT_EQ(r[2].first, 0x18DAF110u);
    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral("  "), &r).toStdString(), "");
    EXPECT_TRUE(r.empty());

    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral("7E8,,7E9"), &r).toStdString(),
              "an empty item between commas");
    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral("7G8"), &r).toStdString(),
              "\"7G8\" isn't a hex ID (up to 1FFFFFFF) or a range like 7E0-7EF");
    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral("20000000"), &r).toStdString(),
              "\"20000000\" isn't a hex ID (up to 1FFFFFFF) or a range like 7E0-7EF");
    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral("7EF-7E0"), &r).toStdString(),
              "\"7EF-7E0\" runs backward");
    EXPECT_EQ(FrameFilter::parseIds(QStringLiteral("7E0-"), &r).toStdString(),
              "\"7E0-\" isn't a hex ID (up to 1FFFFFFF) or a range like 7E0-7EF");
    EXPECT_TRUE(r.empty()) << "a refusal leaves nothing";
}

/* The data field: hex bytes and ??, anywhere unless ^ anchors them */
TEST(FrameFilterTest, TheDataFieldTakesBytesAndWildcards)
{
    std::vector<int> d;
    bool anchored = true;
    EXPECT_EQ(FrameFilter::parseData(QStringLiteral("62 f1 ??"), &d, &anchored)
                  .toStdString(),
              "");
    EXPECT_EQ(d, (std::vector<int>{0x62, 0xF1, -1}));
    EXPECT_FALSE(anchored);
    EXPECT_EQ(FrameFilter::parseData(QStringLiteral("^ 620D"), &d, &anchored)
                  .toStdString(),
              "");
    EXPECT_EQ(d, (std::vector<int>{0x62, 0x0D}));
    EXPECT_TRUE(anchored);
    EXPECT_EQ(FrameFilter::parseData(QStringLiteral("^"), &d, &anchored).toStdString(),
              "\"^\" needs bytes after it");
    EXPECT_EQ(FrameFilter::parseData(QStringLiteral("62 F"), &d, &anchored).toStdString(),
              "\"F\" isn't whole bytes: two hex digits each, or ??");
    EXPECT_EQ(FrameFilter::parseData(QStringLiteral("6Z"), &d, &anchored).toStdString(),
              "\"6Z\" isn't a hex byte or ??");
    EXPECT_TRUE(d.empty());
}

/* What passes: the IDs (an ID past 0x7FF only an extended frame's), the
 * bytes anywhere or at byte 0, the direction and the one flag */
TEST(FrameFilterTest, AFrameMatchesEveryFieldInForce)
{
    EXPECT_TRUE(passes(QStringLiteral("7E0-7EF"), QString(), 0x7E8, {0x41}));
    EXPECT_FALSE(passes(QStringLiteral("7E0-7EF"), QString(), 0x7DF, {0x41}));
    EXPECT_TRUE(passes(QStringLiteral("7E8"), QString(), 0x7E8, {0x41},
                       ECUIO_FRAME_FLAG_EXTENDED))
        << "a 29-bit 0x7E8 is still 0x7E8";
    EXPECT_FALSE(passes(QStringLiteral("18DAF110"), QString(), 0x18DAF110, {0x41}))
        << "no 11-bit frame carries a 29-bit ID";
    EXPECT_TRUE(passes(QStringLiteral("18DAF110"), QString(), 0x18DAF110, {0x41},
                       ECUIO_FRAME_FLAG_EXTENDED));

    EXPECT_TRUE(passes(QString(), QStringLiteral("F1 ?? 01"), 0x7E8, {0x62, 0xF1, 0x90, 0x01}));
    EXPECT_FALSE(passes(QString(), QStringLiteral("^F1"), 0x7E8, {0x62, 0xF1}));
    EXPECT_TRUE(passes(QString(), QStringLiteral("^62 F1"), 0x7E8, {0x62, 0xF1}));
    EXPECT_FALSE(passes(QString(), QStringLiteral("62 F1 90"), 0x7E8, {0x62, 0xF1}))
        << "longer than the payload";
    EXPECT_TRUE(passes(QString(), QStringLiteral("??"), 0x7E8, {0x00}));
    EXPECT_FALSE(passes(QString(), QStringLiteral("??"), 0x7E8, {}));

    FrameFilter f;
    f.direction = FrameFilter::Sent;
    const uint8_t b[] = {0x01};
    EXPECT_FALSE(f.matches(0x7E0, 0, 0, b, 1));
    EXPECT_TRUE(f.matches(0x7E0, ECUIO_FRAME_FLAG_TX_ECHO, 0, b, 1));
    f.direction = FrameFilter::Received;
    EXPECT_TRUE(f.matches(0x7E0, 0, 0, b, 1));
    EXPECT_FALSE(f.matches(0x7E0, ECUIO_FRAME_FLAG_TX_ECHO, 0, b, 1));
    f.direction = FrameFilter::AnyDirection;
    const std::pair<int, uint32_t> flags[] = {
        {FrameFilter::Extended, ECUIO_FRAME_FLAG_EXTENDED},
        {FrameFilter::Fd, ECUIO_FRAME_FLAG_FD},
        {FrameFilter::Reassembled, ECUIO_FRAME_FLAG_REASSEMBLED},
        {FrameFilter::Truncated, ECUIO_FRAME_FLAG_TRUNCATED},
        {FrameFilter::Overflow, ECUIO_FRAME_FLAG_OVERFLOW},
        {FrameFilter::Error, ECUIO_FRAME_FLAG_ERROR}};
    for (const auto& [flag, bit] : flags) {
        f.flag = flag;
        EXPECT_TRUE(f.matches(0x7E0, bit, 0, b, 1)) << flag;
        EXPECT_FALSE(f.matches(0x7E0, 0, 0, b, 1)) << flag;
    }
    f.flag = FrameFilter::Standard;
    EXPECT_TRUE(f.matches(0x7E0, 0, 0, b, 1));
    EXPECT_FALSE(f.matches(0x7E0, ECUIO_FRAME_FLAG_EXTENDED, 0, b, 1));
    EXPECT_EQ(f.terms(), 1);
    f.flag = FrameFilter::AnyFlag;
    EXPECT_EQ(f.terms(), 0);
    EXPECT_TRUE(f.matches(0x7E0, ECUIO_FRAME_FLAG_ERROR, 0, b, 1))
        << "no filter";
}

/* An ID under a mask, candump's can_id:can_mask: the ID's bits where
 * the mask has ones. A value or mask past 0x7FF is an extended frame's;
 * "/" is refused with the ":" form named */
TEST(FrameFilterTest, TheIdFieldTakesAMaskAsCandumpWritesIt)
{
    std::vector<FrameFilter::IdRange> r;
    EXPECT_EQ(FrameFilter::parseIds(
                  QStringLiteral("7E0:7F8, 7DF, 0x18DAF100:0x1FFFFF00"), &r)
                  .toStdString(),
              "");
    ASSERT_EQ(r.size(), 3u);
    EXPECT_TRUE(r[0].masked);
    EXPECT_EQ(r[0].first, 0x7E0u);
    EXPECT_EQ(r[0].last, 0x7E0u);
    EXPECT_EQ(r[0].mask, 0x7F8u);
    EXPECT_FALSE(r[1].masked);
    EXPECT_EQ(r[1].first, 0x7DFu);
    EXPECT_EQ(r[2].mask, 0x1FFFFF00u);

    EXPECT_EQ(
        FrameFilter::parseIds(QStringLiteral("7E8, 7E0/7F8"), &r).toStdString(),
        "\"7E0/7F8\": an ID and its mask take \":\", as in 7E0:7F8");
    for (const char* bad :
         {"7E0:", ":7F8", "7E0-7EF:7F8", "7E0:7F8:1", "7E0:20000000"}) {
        EXPECT_EQ(
            FrameFilter::parseIds(QString::fromLatin1(bad), &r).toStdString(),
            "\"" + std::string(bad) +
                "\" isn't a hex ID and mask like 7E0:7F8");
    }
    EXPECT_TRUE(r.empty());

    for (uint32_t id = 0x7E0; id <= 0x7E7; ++id) {
        EXPECT_TRUE(passes(QStringLiteral("7E0:7F8"), QString(), id, {0x01}))
            << id;
    }
    EXPECT_FALSE(passes(QStringLiteral("7E0:7F8"), QString(), 0x7E8, {0x01}));
    EXPECT_FALSE(passes(QStringLiteral("7E0:7F8"), QString(), 0x7D8, {0x01}));
    EXPECT_TRUE(passes(QStringLiteral("7E5:7F8"), QString(), 0x7E2, {0x01}))
        << "the value's bits outside the mask don't count";
    EXPECT_TRUE(passes(QStringLiteral("7E0:7F8"), QString(), 0x7E3, {0x01},
                       ECUIO_FRAME_FLAG_EXTENDED))
        << "a 29-bit 0x7E3 is still 0x7E3";
    EXPECT_TRUE(passes(QStringLiteral("18DAF100:1FFFFF00"), QString(),
                       0x18DAF1E8, {0x01}, ECUIO_FRAME_FLAG_EXTENDED));
    EXPECT_FALSE(passes(QStringLiteral("18DAF100:1FFFFF00"), QString(),
                        0x18DAF2E8, {0x01}, ECUIO_FRAME_FLAG_EXTENDED));
    // Past 0x7FF, the value or the mask: an extended frame's alone
    EXPECT_FALSE(
        passes(QStringLiteral("7E0:1FFFFFF8"), QString(), 0x7E0, {0x01}));
    EXPECT_TRUE(passes(QStringLiteral("7E0:1FFFFFF8"), QString(), 0x7E0, {0x01},
                       ECUIO_FRAME_FLAG_EXTENDED));
    EXPECT_FALSE(
        passes(QStringLiteral("18DA0000:0"), QString(), 0x7E0, {0x01}));
    EXPECT_TRUE(passes(QStringLiteral("18DA0000:0"), QString(), 0x7E0, {0x01},
                       ECUIO_FRAME_FLAG_EXTENDED))
        << "mask 0 passes every ID";
    EXPECT_TRUE(passes(QStringLiteral("7FF:0"), QString(), 0x123, {0x01}));
}

/* The length field: decimal lengths in bytes and ranges, commas between,
 * against the frame's data length */
TEST(FrameFilterTest, TheLengthFieldTakesCountsAndRanges)
{
    std::vector<FrameFilter::LengthRange> r;
    EXPECT_EQ(FrameFilter::parseLengths(QStringLiteral(" 8, 1-3 ,0, 4096"), &r)
                  .toStdString(),
              "");
    ASSERT_EQ(r.size(), 4u);
    EXPECT_EQ(r[0].first, 8);
    EXPECT_EQ(r[0].last, 8);
    EXPECT_EQ(r[1].first, 1);
    EXPECT_EQ(r[1].last, 3);
    EXPECT_EQ(r[2].last, 0);
    EXPECT_EQ(r[3].first, 4096);
    EXPECT_EQ(FrameFilter::parseLengths(QStringLiteral("  "), &r).toStdString(),
              "");
    EXPECT_TRUE(r.empty());

    EXPECT_EQ(
        FrameFilter::parseLengths(QStringLiteral("8,,9"), &r).toStdString(),
        "an empty item between commas");
    for (const char* bad :
         {"x", "4097", "-3", "3-", "1.5", "+8", "0x8", "8 9"}) {
        EXPECT_EQ(
            FrameFilter::parseLengths(QString::fromLatin1(bad), &r)
                .toStdString(),
            "\"" + std::string(bad) +
                "\" isn't a length in bytes (up to 4096) or a range like 1-7");
    }
    EXPECT_EQ(
        FrameFilter::parseLengths(QStringLiteral("8-1"), &r).toStdString(),
        "\"8-1\" runs backward");
    EXPECT_TRUE(r.empty());

    FrameFilter f;
    ASSERT_EQ(FrameFilter::parseLengths(QStringLiteral("2-3, 8"), &f.lengths)
                  .toStdString(),
              "");
    const uint8_t b[8] = {};
    EXPECT_FALSE(f.matches(0x7E8, 0, 0, b, 1));
    EXPECT_TRUE(f.matches(0x7E8, 0, 0, b, 2));
    EXPECT_TRUE(f.matches(0x7E8, 0, 0, b, 3));
    EXPECT_FALSE(f.matches(0x7E8, 0, 0, b, 4));
    EXPECT_TRUE(f.matches(0x7E8, 0, 0, b, 8));
    EXPECT_FALSE(f.matches(0x7E8, 0, 0, b, 0));
    EXPECT_EQ(f.terms(), 1);
}

/* The time field: one range of the table's seconds, both ends in it,
 * either end left open */
TEST(FrameFilterTest, TheTimeFieldTakesARangeOfSeconds)
{
    FrameFilter::TimeRange r;
    EXPECT_EQ(FrameFilter::parseTimes(QStringLiteral(" 12.5 - 30 "), &r)
                  .toStdString(),
              "");
    EXPECT_TRUE(r.set);
    EXPECT_EQ(r.first, 12500000u);
    EXPECT_EQ(r.last, 30000000u);
    EXPECT_EQ(
        FrameFilter::parseTimes(QStringLiteral("12.5-"), &r).toStdString(), "");
    EXPECT_EQ(r.first, 12500000u);
    EXPECT_EQ(r.last, UINT64_MAX);
    EXPECT_EQ(FrameFilter::parseTimes(QStringLiteral("-.25"), &r).toStdString(),
              "");
    EXPECT_EQ(r.first, 0u);
    EXPECT_EQ(r.last, 250000u);
    EXPECT_EQ(FrameFilter::parseTimes(QStringLiteral("1.000001-2."), &r)
                  .toStdString(),
              "");
    EXPECT_EQ(r.first, 1000001u);
    EXPECT_EQ(r.last, 2000000u);
    EXPECT_EQ(FrameFilter::parseTimes(QStringLiteral("  "), &r).toStdString(),
              "");
    EXPECT_FALSE(r.set);

    for (const char* bad : {"-", "12", "1-2-3", "a-3", "+1-3", "1e3-", "1,2-3",
                            "1-2,3", "3600000001-"}) {
        EXPECT_EQ(
            FrameFilter::parseTimes(QString::fromLatin1(bad), &r).toStdString(),
            "\"" + std::string(bad) +
                "\" isn't a range of seconds like 12.5-30, 12.5- (from then "
                "on) "
                "or -30 (up to then)");
        EXPECT_FALSE(r.set) << bad;
    }
    EXPECT_EQ(
        FrameFilter::parseTimes(QStringLiteral("30-12.5"), &r).toStdString(),
        "\"30-12.5\" runs backward");
    EXPECT_FALSE(r.set);

    FrameFilter f;
    ASSERT_EQ(FrameFilter::parseTimes(QStringLiteral("12.5-30"), &f.time)
                  .toStdString(),
              "");
    const uint8_t b[] = {0x01};
    EXPECT_FALSE(f.matches(0x7E8, 0, 12499999, b, 1));
    EXPECT_TRUE(f.matches(0x7E8, 0, 12500000, b, 1));
    EXPECT_TRUE(f.matches(0x7E8, 0, 30000000, b, 1));
    EXPECT_FALSE(f.matches(0x7E8, 0, 30000001, b, 1));
    EXPECT_EQ(f.terms(), 1);
    ASSERT_EQ(
        FrameFilter::parseTimes(QStringLiteral("-30"), &f.time).toStdString(),
        "");
    EXPECT_TRUE(f.matches(0x7E8, 0, 0, b, 1));
    ASSERT_EQ(
        FrameFilter::parseTimes(QStringLiteral("12.5-"), &f.time).toStdString(),
        "");
    EXPECT_TRUE(f.matches(0x7E8, 0, UINT64_MAX, b, 1));
    ASSERT_EQ(FrameFilter::parseLengths(QStringLiteral("1"), &f.lengths)
                  .toStdString(),
              "");
    EXPECT_EQ(f.terms(), 2) << "the length and the time, a term each";
}

/* The table shows what the filter passes, in either order, as frames
 * come and the ring wraps; the rows its signals report are rowCount's */
TEST(BridgeTest, FrameFilterShowsWhatItPasses)
{
    FrameListModel model;
    RowMirror mirror(model);
    FrameMaker m;
    quint64 t = 0;
    for (int i = 0; i < 6; ++i) {
        m.add(model, i % 2 == 0 ? 0x7E8 : 0x7E9, {0x41, static_cast<uint8_t>(i)}, ++t);
    }
    QVariantMap words = model.setFilter(QStringLiteral("7E9"), QString(), 0, 0);
    EXPECT_EQ(words.value(QStringLiteral("ids")).toString().toStdString(), "");
    EXPECT_EQ(model.filterTerms(), 1);
    EXPECT_EQ(model.count(), 3);
    EXPECT_EQ(model.heldCount(), 6);
    EXPECT_EQ(mirror.rows, 3);
    EXPECT_EQ(idsShown(model), (QStringList{"7E9", "7E9", "7E9"}));
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::PayloadRole).toString(),
              QStringLiteral("41 05"))
        << "newest first";

    // A frame the filter passes is a row; one it doesn't isn't
    m.add(model, 0x7E8, {0x41, 6}, ++t);
    EXPECT_EQ(mirror.rows, 3);
    m.add(model, 0x7E9, {0x41, 7}, ++t);
    EXPECT_EQ(mirror.rows, 4);
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::PayloadRole).toString(),
              QStringLiteral("41 07"));

    // A field that doesn't parse keeps its part in force and says why
    words = model.setFilter(QStringLiteral("7E9,"), QStringLiteral("^41"), 0, 0);
    EXPECT_EQ(words.value(QStringLiteral("ids")).toString().toStdString(),
              "an empty item between commas");
    EXPECT_EQ(model.filterTerms(), 2);
    EXPECT_EQ(model.count(), 4);

    // Oldest first, then no filter: every frame again
    model.setNewestFirst(false);
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::PayloadRole).toString(),
              QStringLiteral("41 01"));
    model.setFilter(QString(), QString(), 0, 0);
    EXPECT_EQ(model.filterTerms(), 0);
    EXPECT_EQ(model.count(), 8);
    EXPECT_EQ(mirror.rows, 8);
}

/* The length and time fields through the model: their answer's words,
 * a field that doesn't parse keeping its part, and the four-argument
 * call leaving both out */
TEST(BridgeTest, FrameFilterTakesALengthAndATime)
{
    FrameListModel model;
    RowMirror mirror(model);
    FrameMaker m;
    // A frame a second for 10 s, its length 1 + i % 3
    for (int i = 0; i < 10; ++i) {
        const quint64 t = static_cast<quint64>(i) * 1000000;
        if (i % 3 == 0) {
            m.add(model, 0x7E8, {0x41}, t);
        } else if (i % 3 == 1) {
            m.add(model, 0x7E8, {0x41, 0x0C}, t);
        } else {
            m.add(model, 0x7E8, {0x41, 0x0C, 0x1A}, t);
        }
    }
    QVariantMap words = model.setFilter(QString(), QString(), 0, 0,
                                        QStringLiteral("2-3"), QString());
    EXPECT_EQ(words.value(QStringLiteral("lengths")).toString().toStdString(),
              "");
    EXPECT_EQ(words.value(QStringLiteral("times")).toString().toStdString(),
              "");
    EXPECT_EQ(model.filterTerms(), 1);
    EXPECT_EQ(model.count(), 6);
    EXPECT_EQ(mirror.rows, 6);

    words = model.setFilter(QString(), QString(), 0, 0, QStringLiteral("2-3"),
                            QStringLiteral("2-5"));
    EXPECT_EQ(model.filterTerms(), 2);
    EXPECT_EQ(model.count(), 3) << "2, 4 and 5 s; 3 s is one byte long";
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::TimestampRole)
                  .toULongLong(),
              5000000u)
        << "both ends in the range";

    // A field that doesn't parse keeps its part in force and says why
    words = model.setFilter(QString(), QString(), 0, 0, QStringLiteral("2-"),
                            QStringLiteral("5"));
    EXPECT_EQ(
        words.value(QStringLiteral("lengths")).toString().toStdString(),
        "\"2-\" isn't a length in bytes (up to 4096) or a range like 1-7");
    EXPECT_EQ(words.value(QStringLiteral("times")).toString().toStdString(),
              "\"5\" isn't a range of seconds like 12.5-30, 12.5- (from then "
              "on) or -30 "
              "(up to then)");
    EXPECT_EQ(words.value(QStringLiteral("ids")).toString().toStdString(), "");
    EXPECT_EQ(model.filterTerms(), 2);
    EXPECT_EQ(model.count(), 3);

    // Only the time: from 7 s on
    model.setFilter(QString(), QString(), 0, 0, QString(),
                    QStringLiteral("7-"));
    EXPECT_EQ(model.filterTerms(), 1);
    EXPECT_EQ(model.count(), 3);
    EXPECT_EQ(mirror.rows, 3);

    // The four-argument call: no length and no time
    model.setFilter(QStringLiteral("7E8"), QString(), 0, 0);
    EXPECT_EQ(model.filterTerms(), 1);
    EXPECT_EQ(model.count(), 10);
    EXPECT_EQ(mirror.rows, 10);
}

/* At the cap, the oldest frame leaves the view only when the view shows
 * it; the selection stays on its frames and lets go at a filter change */
TEST(BridgeTest, FrameFilterHoldsAsTheRingWraps)
{
    FrameListModel model;
    RowMirror mirror(model);
    FrameMaker m;
    quint64 t = 0;
    model.setFilter(QStringLiteral("100"), QString(), 0, 0);
    for (int i = 0; i < FrameListModel::MaxFrames; ++i) {
        m.add(model, i % 10 == 0 ? 0x100 : 0x200, {static_cast<uint8_t>(i)}, ++t);
    }
    EXPECT_EQ(model.count(), FrameListModel::MaxFrames / 10);
    EXPECT_EQ(mirror.rows, model.count());
    model.toggleSelected(0);   // the newest 0x100 frame
    const QString selected = model.selectedText();

    // The oldest frame, a 0x100, leaves; the next ones (0x200) leave the
    // ring unseen
    m.add(model, 0x200, {0xAA}, ++t);
    EXPECT_EQ(model.count(), FrameListModel::MaxFrames / 10 - 1);
    EXPECT_EQ(mirror.rows, model.count());
    for (int i = 0; i < 9; ++i) {
        m.add(model, 0x200, {0xAA}, ++t);
    }
    EXPECT_EQ(model.count(), FrameListModel::MaxFrames / 10 - 1);
    // A 0x100 comes as the next 0x100 leaves
    m.add(model, 0x100, {0xBB}, ++t);
    EXPECT_EQ(model.count(), FrameListModel::MaxFrames / 10 - 1);
    EXPECT_EQ(mirror.rows, model.count());
    EXPECT_EQ(model.heldCount(), FrameListModel::MaxFrames);
    EXPECT_EQ(model.selectedCount(), 1);
    EXPECT_EQ(model.selectedText(), selected) << "the selection kept its frame";
    EXPECT_TRUE(
        model.data(model.index(1, 0), FrameListModel::SelectedRole).toBool());

    model.setFilter(QStringLiteral("100"), QStringLiteral("BB"), 0, 0);
    EXPECT_EQ(model.selectedCount(), 0);
    EXPECT_EQ(model.count(), 1);
    model.clear();
    EXPECT_EQ(model.count(), 0);
    EXPECT_EQ(model.filterTerms(), 2) << "Clear empties the table, not the filter";
}

/* A row's flags in words, for the table's Flags column */
TEST(BridgeTest, FrameFlagsReadAsWords)
{
    EXPECT_EQ(FrameListModel::flagWords(0).toStdString(), "");
    EXPECT_EQ(FrameListModel::flagWords(ECUIO_FRAME_FLAG_EXTENDED).toStdString(), "")
        << "the Ext column says it";
    EXPECT_EQ(FrameListModel::flagWords(ECUIO_FRAME_FLAG_FD |
                                        ECUIO_FRAME_FLAG_BITRATE_SWITCH |
                                        ECUIO_FRAME_FLAG_REASSEMBLED)
                  .toStdString(),
              "FD BRS reassembled");
    EXPECT_EQ(FrameListModel::flagWords(ECUIO_FRAME_FLAG_FD | ECUIO_FRAME_FLAG_TRUNCATED |
                                        ECUIO_FRAME_FLAG_OVERFLOW | ECUIO_FRAME_FLAG_ERROR |
                                        ECUIO_FRAME_FLAG_TX_ECHO |
                                        ECUIO_FRAME_FLAG_REMOTE)
                  .toStdString(),
              "FD remote truncated after a loss error sent");
    FrameListModel model;
    FrameMaker m;
    m.add(model, 0x7E8, {0x41}, 1, ECUIO_FRAME_FLAG_REASSEMBLED);
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::FlagsTextRole).toString(),
              QStringLiteral("reassembled"));
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::FlagsRole).toUInt(),
              ECUIO_FRAME_FLAG_REASSEMBLED);
}

/* By ID: one row per ID and width over the frames shown, its count, its
 * rate over the last 10 s and its newest payload; rows keep their places */
TEST(BridgeTest, FrameIdsCountRateAndNewestPayload)
{
    FrameListModel frames;
    FrameIdModel ids(&frames);
    RowMirror mirror(ids);
    FrameMaker m;
    // 0x7E8 every 100 ms for 20 s; 0x7E9 every second; a 29-bit 0x7E8
    // once
    for (int i = 0; i < 200; ++i) {
        const quint64 t = static_cast<quint64>(i) * 100000;
        m.add(frames, 0x7E8, {0x41, static_cast<uint8_t>(i)}, t);
        if (i % 10 == 0) {
            m.add(frames, 0x7E9, {0x62}, t);
        }
    }
    m.add(frames, 0x7E8, {0x01}, 19900000, ECUIO_FRAME_FLAG_EXTENDED);
    ids.refresh();
    ASSERT_EQ(ids.count(), 3);
    EXPECT_EQ(mirror.rows, 3);
    auto at = [&](int row, int role) { return ids.data(ids.index(row, 0), role); };
    EXPECT_EQ(at(0, FrameIdModel::ArbIdRole).toUInt(), 0x7E8u);
    EXPECT_FALSE(at(0, FrameIdModel::ExtendedRole).toBool());
    EXPECT_EQ(at(0, FrameIdModel::CountRole).toInt(), 200);
    EXPECT_DOUBLE_EQ(at(0, FrameIdModel::RateRole).toDouble(), 10.0)
        << "the last 10 s, its first edge left out: 100 frames";
    EXPECT_EQ(at(0, FrameIdModel::PayloadRole).toString(), QStringLiteral("41 C7"));
    EXPECT_EQ(at(1, FrameIdModel::ArbIdRole).toUInt(), 0x7E9u);
    EXPECT_DOUBLE_EQ(at(1, FrameIdModel::RateRole).toDouble(), 1.0);
    EXPECT_TRUE(at(2, FrameIdModel::ExtendedRole).toBool()) << "29-bit sorts after";

    // The filter narrows it; a new ID is inserted in its place
    frames.setFilter(QStringLiteral("7E9-7EA"), QString(), 0, 0);
    ids.refresh();
    EXPECT_EQ(ids.count(), 1);
    EXPECT_EQ(mirror.rows, 1);
    int resets = 0;
    QObject::connect(&ids, &QAbstractItemModel::modelReset, [&resets]() { ++resets; });
    m.add(frames, 0x7EA, {0x01}, 20000000);
    ids.refresh();
    EXPECT_EQ(ids.count(), 2);
    EXPECT_EQ(mirror.rows, 2);
    EXPECT_EQ(resets, 0) << "inserted, not reset";
    EXPECT_EQ(at(1, FrameIdModel::ArbIdRole).toUInt(), 0x7EAu);
    EXPECT_EQ(at(1, FrameIdModel::RateRole).toDouble(), 0.1)
        << "one frame in the 10 s window";

    // One frame alone has no span to rate over
    FrameListModel lone;
    FrameIdModel loneIds(&lone);
    m.add(lone, 0x123, {0x01}, 5);
    loneIds.refresh();
    EXPECT_EQ(loneIds.data(loneIds.index(0, 0), FrameIdModel::RateRole).toDouble(), 0.0);
    frames.clear();
    ids.refresh();
    EXPECT_EQ(ids.count(), 0);
    EXPECT_EQ(mirror.rows, 0);
}

/* By ID marks the newest payload's bytes that differ from the frame
 * before it among those shown; a length change marks every byte past
 * the shorter payload; a lone frame marks none */
TEST(BridgeTest, FrameIdsMarkTheBytesThatChanged)
{
    FrameListModel frames;
    FrameIdModel ids(&frames);
    FrameMaker m;
    auto changed = [&](int row) {
        QList<int> out;
        for (const QVariant& v :
             ids.data(ids.index(row, 0), FrameIdModel::ChangedRole).toList()) {
            out.append(v.toInt());
        }
        return out;
    };
    m.add(frames, 0x100, {0x01, 0x02, 0x03}, 1);
    ids.refresh();
    EXPECT_EQ(changed(0), QList<int>()) << "one frame: nothing before it";

    int changes = 0;
    QObject::connect(&ids, &QAbstractItemModel::dataChanged,
                     [&changes](const QModelIndex&, const QModelIndex&,
                                const QList<int>& roles) {
                         if (roles.contains(FrameIdModel::ChangedRole)) {
                             ++changes;
                         }
                     });
    m.add(frames, 0x100, {0x01, 0x22, 0x03}, 2);
    m.add(frames, 0x200, {0xAA}, 3);
    ids.refresh();
    EXPECT_EQ(changed(0), QList<int>{1});
    EXPECT_EQ(changed(1), QList<int>()) << "each ID its own";
    EXPECT_EQ(changes, 1);

    m.add(frames, 0x100, {0x01, 0x22, 0x03}, 4);
    ids.refresh();
    EXPECT_EQ(changed(0), QList<int>()) << "the same payload again";

    m.add(frames, 0x100, {0x01, 0x22, 0x33, 0x04, 0x05}, 5);
    ids.refresh();
    EXPECT_EQ(changed(0), (QList<int>{2, 3, 4}))
        << "longer: the bytes past the old length";

    m.add(frames, 0x100, {0x01, 0x23}, 6);
    ids.refresh();
    EXPECT_EQ(changed(0), QList<int>{1})
        << "shorter: its own bytes that differ";

    // The frame before among those shown: one the filter hides doesn't count
    m.add(frames, 0x100, {0xFF, 0x23}, 7);
    m.add(frames, 0x100, {0x01, 0x24}, 8);
    frames.setFilter(QString(), QStringLiteral("^01"), 0, 0);
    ids.refresh();
    EXPECT_EQ(changed(0), QList<int>{1}) << "from 01 23, past the hidden FF 23";
    EXPECT_EQ(ids.roleNames().value(FrameIdModel::ChangedRole),
              QByteArray("changedBytes"));
}

// A log shorter than the window: each ID's frames less one over its own
// span, so a 10 Hz ID reads 10/s from its second frame on
TEST(BridgeTest, FrameIdsRateAShortLogByItsIntervals)
{
    FrameListModel frames;
    FrameIdModel ids(&frames);
    FrameMaker m;
    auto rate = [&](int row) {
        return ids.data(ids.index(row, 0), FrameIdModel::RateRole).toDouble();
    };
    for (int i = 0; i <= 10; ++i) {
        const quint64 t = 3000000 + static_cast<quint64>(i) * 100000;
        m.add(frames, 0x7E8, {0x41, static_cast<uint8_t>(i)}, t);
        if (i % 10 == 0) {
            m.add(frames, 0x7E9, {0x62}, t);
        }
        if (i == 5) {
            m.add(frames, 0x7EA, {0x01}, t);
            ids.refresh();
            ASSERT_EQ(ids.count(), 3);
            EXPECT_DOUBLE_EQ(rate(0), 10.0)
                << "0.5 s in: 6 frames, 5 intervals";
            EXPECT_DOUBLE_EQ(rate(1), 0.0) << "one frame so far";
        }
    }
    ids.refresh();
    ASSERT_EQ(ids.count(), 3);
    EXPECT_DOUBLE_EQ(rate(0), 10.0) << "1 s in: 11 frames, 10 intervals";
    EXPECT_DOUBLE_EQ(rate(1), 1.0) << "two frames a second apart";
    EXPECT_DOUBLE_EQ(rate(2), 0.0) << "a lone frame has no interval";
}

// ---- Export CSV (ADR-0054's appendix "frames in memory") ----

// The VIN reply delivered whole, a value, another ECU's value, the
// tester's own request, then a text signal's DID
void heldForExport(FrameListModel& model, FrameMaker& m)
{
    m.add(model, 0x7E8, {0x49, 0x02, 0x01, 'Z', 'Z', 'Z', 'F', 'A', 'K', 'E',
                         '1',  '1',  'M',  '0', '0', '0', '0', '0', '0', '1'},
          1000, ECUIO_FRAME_FLAG_REASSEMBLED);
    m.add(model, 0x7E8, {0x04, 0x41, 0x0C, 0x1A, 0xF8}, 2000);
    m.add(model, 0x7E9, {0x03, 0x41, 0x05, 0x7B}, 3000);
    m.add(model, 0x7E0, {0x02, 0x01, 0x0C}, 4000, ECUIO_FRAME_FLAG_TX_ECHO);
    m.add(model, 0x7E8,
          {0x62, 0xD8, 0x02, 'E', 'C', 'M', '-', 'H', 'a', 'v', 'n', 'e'}, 5000,
          ECUIO_FRAME_FLAG_REASSEMBLED);
}

std::string csvHead(int written, const char* filter, int held, int ident,
                    int text)
{
    char line[512];
    std::snprintf(line, sizeof line,
                  "# Havne %s, the Log page's frames, scrubbed (ADR-0054): %d "
                  "written%s; left out of the %d held: %d frames of "
                  "identification replies, %d of text signals' messages, 0 "
                  "segments of messages that couldn't be reassembled\n"
                  "time_s,id,ext,dir,dlc,flags,data\n",
                  ecuio_version_string(), written, filter, held, ident, text);
    return line;
}

// Core's scrub leaves the identification reply out, a text def of a
// session's set leaves its message out (the set noted, the session long
// gone), and the filter picks among the frames kept
TEST(BridgeTest, FrameCsvLeavesIdentificationOutAndWritesWhatTheFilterShows)
{
    FrameListModel model;
    FrameMaker m;
    heldForExport(model, m);

    FrameListModel::ScrubbedCsv csv = model.scrubbedCsv();
    ASSERT_TRUE(csv.ok) << csv.words.toStdString();
    EXPECT_EQ(csv.bytes.toStdString(),
              csvHead(4, "", 5, 1, 0) +
                  "0.002000,0x7E8,0,rx,5,,04 41 0C 1A F8\n"
                  "0.003000,0x7E9,0,rx,4,,03 41 05 7B\n"
                  "0.004000,0x7E0,0,tx,3,sent,02 01 0C\n"
                  "0.005000,0x7E8,0,rx,12,reassembled,62 D8 02 45 43 4D 2D 48 "
                  "61 76 6E 65\n");
    EXPECT_EQ(csv.written, 4);
    EXPECT_EQ(csv.identification, 1u);

    const std::string json =
        R"({"havne_signals": 1, "name": "Scrub test", )"
        R"("source": {"name": "test_bridge"}, )"
        R"("ecus": [{"id": "pcm", "rx": "7E8"}], )"
        R"("signals": [{"key": "ecu_name", "name": "ECU name", "ecu": "pcm", )"
        R"("read": {"service": "22", "number": "D802"}, )"
        R"("field": {"byte": 0}, "kind": "text"}]})";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(json.data(), json.size(), &file), ECUIO_OK)
        << ecuio_last_error_detail();
    model.noteTextDefs(ecuio_signal_file_set(file));
    model.noteTextDefs(ecuio_signal_file_set(file));
    ecuio_signal_file_free(file); // the copies stay

    model.setFilter(QStringLiteral("7E8"), QString(), 0, 0);
    csv = model.scrubbedCsv();
    ASSERT_TRUE(csv.ok) << csv.words.toStdString();
    EXPECT_EQ(csv.bytes.toStdString(),
              csvHead(1, " (those the filter shows)", 5, 1, 1) +
                  "0.002000,0x7E8,0,rx,5,,04 41 0C 1A F8\n");
    EXPECT_EQ(csv.textValues, 1u);

    // A frame echoing the VIN fails the check: no bytes, the class alone
    FrameListModel echo;
    m.add(echo, 0x7E8, {0x62, 0xD8, 0x01, 'Z', 'Z', 'Z', 'F', 'A', 'K', 'E',
                        '1',  '1',  'M',  '0', '0', '0', '0', '0', '0', '1'},
          1000, ECUIO_FRAME_FLAG_REASSEMBLED);
    csv = echo.scrubbedCsv();
    EXPECT_FALSE(csv.ok);
    EXPECT_TRUE(csv.bytes.isEmpty());
    EXPECT_EQ(csv.words.toStdString(),
              "The check found a VIN-shaped token in a frame the export would "
              "write: nothing was written");
}

// The bridge writes the file whole, answers with the counts and logs one
// line naming no path; a failed check or a target it can't write leaves
// what was there
TEST(BridgeTest, ExportFramesWritesTheFileAndLogsOneLine)
{
    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    SessionBridge bridge;
    QStringList lines;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&](const QString& line) { lines << line; });
    FrameMaker m;
    heldForExport(*bridge.model(), m);

    const QString target = out.path() + QStringLiteral("/frames.csv");
    const QVariantMap r = bridge.exportFrames(QUrl::fromLocalFile(target));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("written")).toInt(), 4);
    EXPECT_EQ(r.value(QStringLiteral("leftOut")).toULongLong(), 1u);
    EXPECT_EQ(r.value(QStringLiteral("unattributed")).toULongLong(), 0u);
    QFile f(target);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    f.close();
    EXPECT_TRUE(bytes.startsWith(QByteArray(csvHead(4, "", 5, 1, 0).c_str())));
    EXPECT_FALSE(bytes.contains("FAKE11M")) << "the VIN reply left out";
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines[0].toStdString(),
              "Frames exported: 4 written, 1 left out (identification and text "
              "signals), 0 segments unattributed");

    // A frame echoing the VIN: nothing written, the file as it was
    m.add(*bridge.model(), 0x7E8,
          {0x62, 0xD8, 0x01, 'Z', 'Z', 'Z', 'F', 'A', 'K', 'E',
           '1',  '1',  'M',  '0', '0', '0', '0', '0', '0', '1'},
          6000, ECUIO_FRAME_FLAG_REASSEMBLED);
    lines.clear();
    const QVariantMap no = bridge.exportFrames(QUrl::fromLocalFile(target));
    EXPECT_FALSE(no.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(no.value(QStringLiteral("written")).toInt(), 0);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), bytes);
    f.close();
    ASSERT_EQ(lines.size(), 1);
    EXPECT_TRUE(lines[0].startsWith(
        QStringLiteral("Frames not exported: The check found")))
        << lines[0].toStdString();

    // A folder isn't a file it can write
    bridge.model()->clear();
    heldForExport(*bridge.model(), m);
    const QVariantMap folder =
        bridge.exportFrames(QUrl::fromLocalFile(out.path()));
    EXPECT_FALSE(folder.value(QStringLiteral("ok")).toBool());
    const QString words = folder.value(QStringLiteral("words")).toString();
    EXPECT_EQ(words.toStdString(), "Couldn't write the file");
    for (const QString& line : lines) {
        EXPECT_FALSE(line.contains(out.path())) << line.toStdString();
    }
}

// The frames CSV through a provider (a non-file QUrl stands in, as in
// ARefusedSaveThroughAProviderRemovesOnlyAnEmptyDocument): a refused export
// removes the picker's empty document and leaves one that held bytes, and a
// good one writes into the empty document
TEST(BridgeTest, ARefusedFramesExportThroughAProviderRemovesOnlyAnEmptyDocument)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    if (QUrl(dir.path(), QUrl::TolerantMode)
            .toString(QUrl::FullyEncoded)
            .compare(dir.path(), Qt::CaseInsensitive) != 0) {
        GTEST_SKIP() << "the temporary folder's path changes when encoded";
    }
    SessionBridge bridge;
    FrameMaker m;
    heldForExport(*bridge.model(), m);
    const auto provider = [&](const QString& name) {
        return QUrl(dir.path() + QLatin1Char('/') + name, QUrl::TolerantMode);
    };
    const auto make = [&](const QUrl& url, const QByteArray& bytes) {
        QFile f(url.toString());
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    };
    // A good export fills the picker's empty document
    const QUrl fresh = provider(QStringLiteral("fresh.csv"));
    make(fresh, QByteArray());
    EXPECT_TRUE(
        bridge.exportFrames(fresh).value(QStringLiteral("ok")).toBool());
    {
        QFile f(fresh.toString());
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        EXPECT_FALSE(f.readAll().isEmpty());
    }
    // A frame echoing the VIN: the check refuses
    m.add(*bridge.model(), 0x7E8,
          {0x62, 0xD8, 0x01, 'Z', 'Z', 'Z', 'F', 'A', 'K', 'E',
           '1',  '1',  'M',  '0', '0', '0', '0', '0', '0', '1'},
          6000, ECUIO_FRAME_FLAG_REASSEMBLED);
    const QUrl made = provider(QStringLiteral("made.csv"));
    make(made, QByteArray());
    EXPECT_FALSE(
        bridge.exportFrames(made).value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(QFile::exists(made.toString()))
        << "the picker's empty document goes";
    const QUrl chosen = provider(QStringLiteral("chosen.csv"));
    const QByteArray theirs("the user's earlier export\n");
    make(chosen, theirs);
    EXPECT_FALSE(
        bridge.exportFrames(chosen).value(QStringLiteral("ok")).toBool());
    QFile after(chosen.toString());
    ASSERT_TRUE(after.open(QIODevice::ReadOnly));
    EXPECT_EQ(after.readAll(), theirs) << "a replaced document stays";
}
