// File/layer: tests/test_bridge_frames.cpp | GTest + QtCore/Network, app-layer tests
// Testing: BRIDGE: BridgeTest (test_bridge)
//
// Cases for the frame log, its filter, the By ID view and the frames export.

#include "test_bridge_frames_support.h"

/* The frame log belongs to a session: rows stay readable after a
 * disconnect, and the next connect starts an empty table. Checked as
 * the synchronous connect returns, before any queued frame can land. */
TEST(BridgeTest, ConnectStartsAnEmptyFrameLog)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://first").isEmpty());
    FrameListModel* proxy = bridge.model();
    ASSERT_TRUE(spinUntil([proxy]() { return proxy->rowCount() > 0; }, 5000));

    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_GT(proxy->rowCount(), 0) << "disconnect cleared the log";

    ASSERT_TRUE(bridge.connectSession("mock://second").isEmpty());
    EXPECT_EQ(proxy->rowCount(), 0);
    EXPECT_EQ(bridge.frameCount(), 0);

    bridge.disconnectSession();
}

/* Rows sort by arrival, not by frame timestamp: a J2534 timestamp is the
 * vendor's raw 32-bit stamp, which can wrap. A frame stamped earlier but
 * arriving later is still the newest row. */
TEST(BridgeTest, FrameLogOrdersByArrivalNotTimestamp)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://arrival").isEmpty());

    ecuio_frame_t first{};
    first.timestamp_us = 900000;
    first.arbitration_id = 0x123;   // not the mock's 0x0C0
    first.data_length = 1;
    ecuio_frame_t second = first;
    second.timestamp_us = 100;      // a wrapped stamp, say
    second.arbitration_id = 0x124;
    bridge.dispatchFrameForTesting(first);
    bridge.dispatchFrameForTesting(second);

    FrameListModel* proxy = bridge.model();
    auto row_of = [proxy](quint32 id) {
        for (int r = 0; r < proxy->rowCount(); ++r) {
            if (proxy->data(proxy->index(r, 0),
                            FrameListModel::ArbitrationIdRole).toUInt() == id) {
                return r;
            }
        }
        return -1;
    };
    ASSERT_TRUE(spinUntil([&row_of]() {
        return row_of(0x123) >= 0 && row_of(0x124) >= 0;
    }, 5000));

    EXPECT_LT(row_of(0x124), row_of(0x123)) << "newest first";
    proxy->setNewestFirst(false);
    EXPECT_LT(row_of(0x123), row_of(0x124)) << "oldest first";

    // A row answers only its roles, and only rows that exist answer
    EXPECT_FALSE(proxy->data(proxy->index(0, 0), Qt::DisplayRole).isValid());
    EXPECT_FALSE(proxy
                     ->data(proxy->index(proxy->rowCount(), 0),
                            FrameListModel::ArbitrationIdRole)
                     .isValid());
    EXPECT_EQ(proxy->count(), proxy->rowCount());
    EXPECT_EQ(bridge.maxFrameCount(), FrameListModel::MaxFrames);

    bridge.disconnectSession();
}

/* A table keeps its place by a row's arrival number, which a stamp that
 * steps back (a vendor's 32-bit stamp wrapping) leaves in order: each row is
 * found again by its own, in either order and under a filter, and one that
 * has gone or is filtered out gives the next row in the view's order. */
TEST(BridgeTest, AFrameRowIsFoundAgainByItsArrivalAcrossAStampThatStepsBack)
{
    SessionBridge bridge;
    FrameListModel* proxy = bridge.model();
    EXPECT_EQ(proxy->seqAt(0), -1) << "no rows";
    EXPECT_EQ(proxy->rowAtSeq(0), -1) << "no rows";
    // The stamp wraps after the fourth frame; the IDs alternate
    const quint64 stamps[] = {4000, 5000, 6000, 7000, 100, 200, 300, 400};
    const int n = static_cast<int>(std::size(stamps));
    for (int k = 0; k < n; ++k) {
        ecuio_frame_t f{};
        f.timestamp_us = stamps[k];
        f.arbitration_id = k % 2 == 0 ? 0x123 : 0x124;
        f.data_length = 1;
        proxy->appendFrame(f, 1);
    }
    ASSERT_EQ(proxy->count(), n);
    for (const bool newestFirst : {true, false}) {
        proxy->setNewestFirst(newestFirst);
        for (int row = 0; row < n; ++row) {
            // Rows are in arrival order: the kth frame's number is k
            const int k = newestFirst ? n - 1 - row : row;
            EXPECT_EQ(proxy->seqAt(row), k) << "row " << row;
            EXPECT_EQ(proxy->rowAtSeq(k), row) << "row " << row;
        }
        EXPECT_EQ(proxy->seqAt(-1), -1);
        EXPECT_EQ(proxy->seqAt(n), -1);
        EXPECT_EQ(proxy->rowAtSeq(n + 5), newestFirst ? 0 : n - 1)
            << "past them all: the newest row";
    }

    // Only 0x124's frames (arrivals 1, 3, 5, 7): one the filter leaves out
    // gives the next shown row in the view's order
    ASSERT_TRUE(proxy->setFilter(QStringLiteral("124"), QString(), 0, 0)
                    .value(QStringLiteral("ids"))
                    .toString()
                    .isEmpty());
    ASSERT_EQ(proxy->count(), 4);
    proxy->setNewestFirst(false);
    EXPECT_EQ(proxy->rowAtSeq(5), 2) << "a shown arrival is its own row";
    EXPECT_EQ(proxy->rowAtSeq(4), 2)
        << "oldest first: the first to arrive after";
    EXPECT_EQ(proxy->rowAtSeq(0), 0);
    EXPECT_EQ(proxy->rowAtSeq(8), 3) << "past them all: the last row";
    proxy->setNewestFirst(true);
    EXPECT_EQ(proxy->rowAtSeq(5), 1) << "a shown arrival is its own row";
    EXPECT_EQ(proxy->rowAtSeq(4), 2)
        << "newest first: the last to arrive before";
    EXPECT_EQ(proxy->rowAtSeq(0), 3) << "none arrived before: the last row";
    EXPECT_EQ(proxy->rowAtSeq(8), 0);
    proxy->setFilter(QString(), QString(), 0, 0);

    // The arrival numbers run on across a clear, so a place noted before it
    // is before every row: the oldest one
    proxy->clear();
    for (int k = 0; k < 3; ++k) {
        ecuio_frame_t f{};
        f.timestamp_us = 50;
        f.arbitration_id = 0x123;
        f.data_length = 1;
        proxy->appendFrame(f, 1);
    }
    EXPECT_EQ(proxy->seqAt(2), n) << "newest first: the oldest row is last";
    EXPECT_EQ(proxy->rowAtSeq(3), 2);
    proxy->setNewestFirst(false);
    EXPECT_EQ(proxy->rowAtSeq(3), 0);
}

TEST(BridgeTest, FrameLogOrdersNewestFirstByDefault)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://sort-default").isEmpty());

    FrameListModel* proxy = bridge.model();
    // CONTRACT: more than two frames arrive sorted. Polled, not a fixed
    // sleep: starved CI runners can deliver only 2 in 300 ms.
    ASSERT_TRUE(spinUntil(
        [proxy]() { return proxy->rowCount() > 2; }, 5000))
        << "only " << proxy->rowCount() << " rows in 5s";
    EXPECT_TRUE(proxy->newestFirst());

    const quint64 first = proxy->data(proxy->index(0, 0),
        FrameListModel::TimestampRole).toULongLong();
    const quint64 last = proxy->data(proxy->index(proxy->rowCount() - 1, 0),
        FrameListModel::TimestampRole).toULongLong();
    EXPECT_GT(first, last);

    bridge.disconnectSession();
}

TEST(BridgeTest, FrameLogTogglesToOldestFirst)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://sort-toggle").isEmpty());

    FrameListModel* proxy = bridge.model();
    ASSERT_TRUE(spinUntil(
        [proxy]() { return proxy->rowCount() > 2; }, 5000));

    proxy->setNewestFirst(false);
    EXPECT_FALSE(proxy->newestFirst());

    const quint64 first = proxy->data(proxy->index(0, 0),
        FrameListModel::TimestampRole).toULongLong();
    const quint64 last = proxy->data(proxy->index(proxy->rowCount() - 1, 0),
        FrameListModel::TimestampRole).toULongLong();
    EXPECT_LT(first, last);

    // Toggle back: must re-sort to descending without reconnect
    proxy->setNewestFirst(true);

    // Capture BOTH ends fresh
    const quint64 firstAgain = proxy->data(proxy->index(0, 0),
        FrameListModel::TimestampRole).toULongLong();
    const quint64 lastAgain = proxy->data(proxy->index(proxy->rowCount() - 1, 0),
        FrameListModel::TimestampRole).toULongLong();

    // Top is the newest we've ever seen (>= because the stream kept ticking)
    EXPECT_GE(firstAgain, last);
    // Bottom is the oldest: at most the original ascending-order top,
    // and strictly older than the top of the list
    EXPECT_LE(lastAgain, first);      // 'first' = oldest captured earlier
    EXPECT_GT(firstAgain, lastAgain); // reversal actually happened

    bridge.disconnectSession();
}

namespace {

/* Dispatches n frames with ID 0x123 and stamps first, first + 1, ...,
 * letting each burst drain so none is dropped at the in-flight gate. */
void dispatchNumbered(SessionBridge& bridge, int first, int n)
{
    ecuio_frame_t f{};
    f.arbitration_id = 0x123;   // not the mock's 0x0C0
    f.data_length = 2;
    const int burst = SessionBridge::maxInFlightForTesting() / 2;
    for (int i = 0; i < n; ++i) {
        f.timestamp_us = static_cast<uint64_t>(first + i);
        f.data[0] = static_cast<uint8_t>((first + i) >> 8);
        f.data[1] = static_cast<uint8_t>(first + i);
        bridge.dispatchFrameForTesting(f);
        if ((i + 1) % burst == 0) {
            spinLoop(5);
        }
    }
    spinLoop(50);
}

/* The 0x123 rows' stamps, top to bottom; mock frames between them are
 * counted in others. */
std::vector<quint64> numberedRows(const FrameListModel* model, int* others)
{
    std::vector<quint64> out;
    *others = 0;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex idx = model->index(r, 0);
        if (model->data(idx, FrameListModel::ArbitrationIdRole).toUInt() == 0x123u) {
            out.push_back(model->data(idx, FrameListModel::TimestampRole).toULongLong());
        } else {
            ++*others;
        }
    }
    return out;
}

} // namespace

/* Past the cap the log keeps the newest MaxFrames in arrival order, in
 * either direction, each row still showing its own frame: the ring's
 * slot arithmetic, through the overwrite of the oldest and a toggle.
 * A view sees the oldest row leave from its end and the new one arrive
 * at the other, which is what keeps a ListView's rows right. */
TEST(BridgeTest, FullFrameLogKeepsTheNewestInOrder)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://ring").isEmpty());
    FrameListModel* model = bridge.model();
    int wrongRemovals = 0;
    int wrongInserts = 0;
    QObject::connect(model, &QAbstractItemModel::rowsAboutToBeRemoved,
                     [model, &wrongRemovals](const QModelIndex&, int first, int last) {
        const int oldest = model->newestFirst() ? model->rowCount() - 1 : 0;
        if (first != oldest || last != oldest) ++wrongRemovals;
    });
    QObject::connect(model, &QAbstractItemModel::rowsInserted,
                     [model, &wrongInserts](const QModelIndex&, int first, int last) {
        const int newest = model->newestFirst() ? 0 : model->rowCount() - 1;
        if (first != newest || last != newest) ++wrongInserts;
    });
    const int total = FrameListModel::MaxFrames + 250;
    dispatchNumbered(bridge, 1, total);
    ASSERT_EQ(bridge.droppedFrames(), 0);

    ASSERT_EQ(model->rowCount(), FrameListModel::MaxFrames);
    int others = 0;
    std::vector<quint64> rows = numberedRows(model, &others);
    ASSERT_EQ(static_cast<int>(rows.size()) + others, FrameListModel::MaxFrames);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front(), static_cast<quint64>(total)) << "the newest on top";
    for (size_t i = 1; i < rows.size(); ++i) {
        ASSERT_EQ(rows[i], rows[i - 1] - 1) << "row " << i << " out of order";
    }
    // Each row's payload is its own frame's, not a stale slot's: every
    // numbered row carries its stamp in its two bytes, and the mock's
    // rows between them keep their own length
    int checked = 0;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex idx = model->index(r, 0);
        if (model->data(idx, FrameListModel::ArbitrationIdRole).toUInt() != 0x123u) {
            EXPECT_NE(model->data(idx, FrameListModel::DlcRole).toUInt(), 2u)
                << "row " << r << " has a numbered frame's length";
            continue;
        }
        const quint64 n = model->data(idx, FrameListModel::TimestampRole).toULongLong();
        ASSERT_EQ(model->data(idx, FrameListModel::PayloadRole).toString(),
                  QStringLiteral("%1 %2")
                      .arg((n >> 8) & 0xFF, 2, 16, QLatin1Char('0'))
                      .arg(n & 0xFF, 2, 16, QLatin1Char('0')).toUpper())
            << "row " << r;
        ASSERT_EQ(model->data(idx, FrameListModel::DlcRole).toUInt(), 2u) << "row " << r;
        ++checked;
    }
    EXPECT_EQ(checked, static_cast<int>(rows.size()));

    model->setNewestFirst(false);
    rows = numberedRows(model, &others);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.back(), static_cast<quint64>(total)) << "the newest at the bottom";
    for (size_t i = 1; i < rows.size(); ++i) {
        ASSERT_EQ(rows[i], rows[i - 1] + 1) << "row " << i << " out of order";
    }

    // And it keeps going round while oldest-first
    dispatchNumbered(bridge, total + 1, 300);
    ASSERT_EQ(model->rowCount(), FrameListModel::MaxFrames);
    rows = numberedRows(model, &others);
    EXPECT_EQ(rows.back(), static_cast<quint64>(total + 300));
    for (size_t i = 1; i < rows.size(); ++i) {
        ASSERT_EQ(rows[i], rows[i - 1] + 1) << "row " << i << " out of order";
    }
    EXPECT_EQ(wrongRemovals, 0) << "a row other than the oldest left";
    EXPECT_EQ(wrongInserts, 0) << "a frame arrived at the wrong end";
    bridge.disconnectSession();
}

/* A payload up to InlinePayload bytes lives in its row; a longer one
 * holds a heap block only while its row is shown, so a run of long
 * replies leaves nothing behind once short frames take the slots. Clear
 * keeps the slots (no reallocation on the next connect) and frees the
 * blocks. */
TEST(BridgeTest, FrameLogHoldsLongPayloadsOnlyWhileShown)
{
    FrameListModel model;
    auto frame = std::make_unique<ecuio_frame_t>();
    *frame = ecuio_frame_t{};
    quint64 stamp = 0;
    auto append = [&](uint16_t len) {
        frame->timestamp_us = ++stamp;
        for (uint16_t i = 0; i < len; ++i) {
            frame->data[i] = static_cast<uint8_t>(stamp + i);
        }
        model.appendFrame(*frame, len);
    };
    // The newest row's bytes, as the Log page shows them
    auto newestHex = [&]() {
        return model.data(model.index(0, 0), FrameListModel::PayloadRole)
            .toString();
    };
    auto expectedHex = [](quint64 s, int len) {
        QByteArray bytes;
        for (int i = 0; i < len; ++i)
            bytes.append(static_cast<char>(s + i));
        return QString::fromLatin1(bytes.toHex(' ').toUpper());
    };
    const int cap = FrameListModel::MaxFrames;

    for (int i = 0; i < cap; ++i)
        append(200);
    EXPECT_GE(model.spilledBytesForTesting(), static_cast<size_t>(cap) * 200);
    EXPECT_EQ(newestHex(), expectedHex(stamp, 200));
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::DlcRole).toUInt(),
              200u);

    // Short frames round the ring: every long block is gone
    for (int i = 0; i < cap; ++i)
        append(27);
    EXPECT_EQ(model.spilledBytesForTesting(), 0u);
    EXPECT_EQ(model.slotsForTesting(), static_cast<size_t>(cap));
    EXPECT_EQ(newestHex(), expectedHex(stamp, 27));
    EXPECT_EQ(model.data(model.index(0, 0), FrameListModel::DlcRole).toUInt(),
              27u);

    // InlinePayload itself stays inline; one byte past it spills
    append(FrameListModel::InlinePayload);
    EXPECT_EQ(model.spilledBytesForTesting(), 0u);
    EXPECT_EQ(newestHex(), expectedHex(stamp, FrameListModel::InlinePayload));
    append(FrameListModel::InlinePayload + 1);
    EXPECT_GE(model.spilledBytesForTesting(),
              static_cast<size_t>(FrameListModel::InlinePayload + 1));
    EXPECT_EQ(newestHex(),
              expectedHex(stamp, FrameListModel::InlinePayload + 1));

    // Clear keeps the slots and frees the blocks
    model.clear();
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(model.slotsForTesting(), static_cast<size_t>(cap));
    EXPECT_EQ(model.spilledBytesForTesting(), 0u);
    for (int i = 0; i < 3; ++i)
        append(8);
    ASSERT_EQ(model.rowCount(), 3);
    EXPECT_EQ(model.slotsForTesting(), static_cast<size_t>(cap));
    for (int r = 0; r < 3; ++r) {
        EXPECT_EQ(model.data(model.index(r, 0), FrameListModel::TimestampRole)
                      .toULongLong(),
                  stamp - static_cast<quint64>(r))
            << "row " << r;
    }

    // A ring cleared before it filled grows only as far as it's used
    FrameListModel partial;
    for (int i = 0; i < 5; ++i)
        partial.appendFrame(*frame, 8);
    partial.clear();
    for (int i = 0; i < 7; ++i) {
        frame->timestamp_us = 100 + static_cast<quint64>(i);
        partial.appendFrame(*frame, 8);
    }
    EXPECT_EQ(partial.slotsForTesting(), 7u);
    ASSERT_EQ(partial.rowCount(), 7);
    for (int r = 0; r < 7; ++r) {
        EXPECT_EQ(
            partial.data(partial.index(r, 0), FrameListModel::TimestampRole)
                .toULongLong(),
            106u - static_cast<quint64>(r))
            << "row " << r;
    }
}

/* The Log page's selection holds frames, not row numbers: it stays on
 * them as rows arrive, through a flip of the order, and a frame leaves
 * it as it leaves the ring. A shift-click selects from the anchor; the
 * copied text is the selected rows in view order, under a header line. */
TEST(BridgeTest, FrameLogSelectionFollowsItsFrames)
{
    FrameListModel model;
    auto frame = std::make_unique<ecuio_frame_t>();
    *frame = ecuio_frame_t{};
    quint64 stamp = 0;
    auto append = [&]() {
        frame->timestamp_us = ++stamp * 1000;
        frame->arbitration_id = 0x7E8;
        frame->data[0] = static_cast<uint8_t>(stamp);
        frame->data[1] = 0x0C;
        model.appendFrame(*frame, 2);
    };
    auto selectedAt = [&](int row) {
        return model.data(model.index(row, 0), FrameListModel::SelectedRole).toBool();
    };
    auto stampAt = [&](int row) {
        return model.data(model.index(row, 0), FrameListModel::TimestampRole)
            .toULongLong();
    };
    int selectionSignals = 0;
    QObject::connect(&model, &FrameListModel::selectionChanged,
                     [&selectionSignals]() { ++selectionSignals; });

    for (int i = 0; i < 5; ++i)
        append();
    EXPECT_EQ(model.selectedText(), QString());
    model.toggleSelected(1);   // newest first: the 4th frame
    ASSERT_EQ(model.selectedCount(), 1);
    EXPECT_TRUE(selectedAt(1));
    EXPECT_EQ(selectionSignals, 1);

    // A new frame pushes the row down; the selection goes with it
    append();
    EXPECT_FALSE(selectedAt(1));
    EXPECT_TRUE(selectedAt(2));
    EXPECT_EQ(stampAt(2), 4000u);

    // Shift-click from the anchor (row 2) to row 4: frames 4, 3 and 2
    model.selectThrough(4);
    EXPECT_EQ(model.selectedCount(), 3);
    EXPECT_EQ(model.selectedText(),
              QStringLiteral("time_s\tdlc\text\tid\tdata\n"
                             "0.004000\t2\t-\t0x7E8\t04 0C\n"
                             "0.003000\t2\t-\t0x7E8\t03 0C\n"
                             "0.002000\t2\t-\t0x7E8\t02 0C\n"));

    // Oldest first, the same frames, in the view's new order
    model.setNewestFirst(false);
    EXPECT_EQ(model.selectedCount(), 3);
    EXPECT_TRUE(selectedAt(1) && selectedAt(2) && selectedAt(3));
    EXPECT_FALSE(selectedAt(0) || selectedAt(4) || selectedAt(5));
    EXPECT_TRUE(model.selectedText().startsWith(
        QStringLiteral("time_s\tdlc\text\tid\tdata\n0.002000\t")));

    // A tap on a selected row lets it go
    model.toggleSelected(1);
    EXPECT_EQ(model.selectedCount(), 2);
    EXPECT_FALSE(selectedAt(1));

    // The ring wraps: the selected frames leave it with their rows
    const int before = selectionSignals;
    for (int i = 0; i < FrameListModel::MaxFrames; ++i)
        append();
    EXPECT_EQ(model.selectedCount(), 0);
    EXPECT_EQ(model.selectedText(), QString());
    EXPECT_EQ(selectionSignals, before + 2) << "once for each frame that left";

    // The anchor left too: a shift-click selects just its own row
    model.selectThrough(7);
    EXPECT_EQ(model.selectedCount(), 1);
    EXPECT_TRUE(selectedAt(7));

    // Clear and Select none empty it
    model.clearSelection();
    EXPECT_EQ(model.selectedCount(), 0);
    EXPECT_FALSE(selectedAt(7));
    model.toggleSelected(0);
    model.clear();
    EXPECT_EQ(model.selectedCount(), 0);
    append();
    EXPECT_FALSE(selectedAt(0));
}

/* frameCount and guiNotifyCount reach QML on the 100 ms tick, not once
 * per frame: a burst of 400 frames changes each a handful of times. The
 * disconnect reports the last of them, so what QML last saw is exact. */
TEST(BridgeTest, FrameCountsAreReportedOnTheTick)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://counts").isEmpty());
    int countSignals = 0;
    int notifySignals = 0;
    int seenFrames = -1;
    int seenNotify = -1;
    QObject::connect(&bridge, &SessionBridge::frameCountChanged,
                     [&]() { ++countSignals; seenFrames = bridge.frameCount(); });
    QObject::connect(&bridge, &SessionBridge::notifyCountChanged,
                     [&](int n) { ++notifySignals; seenNotify = n; });

    const int before = bridge.frameCount();
    dispatchNumbered(bridge, 1, 400);
    spinLoop(250);   // two ticks past the burst
    EXPECT_GE(bridge.frameCount(), before + 400);
    EXPECT_GT(countSignals, 0) << "the frame count never reached QML";
    EXPECT_GT(notifySignals, 0) << "the notify count never reached QML";
    EXPECT_LT(countSignals, 40) << "reported per frame, not per tick";
    EXPECT_LT(notifySignals, 40) << "reported per frame, not per tick";

    bridge.disconnectSession();   // no frame is counted after this
    EXPECT_EQ(seenFrames, bridge.frameCount());
    EXPECT_EQ(seenNotify, bridge.guiNotifyCount());
    EXPECT_EQ(bridge.guiNotifyCount(), bridge.frameCount())
        << "one GUI-thread delivery per frame";
}

/* Frames flagged ECUIO_FRAME_FLAG_OVERFLOW (the adapter lost frames
 * before them) are counted on the tick, the first with one activity-log
 * line; a new session starts the count again */
TEST(BridgeTest, FramesAfterALossAtTheAdapterAreCountedAndLoggedOnce)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    int changes = 0;
    QObject::connect(&bridge, &SessionBridge::overflowFramesChanged,
                     [&changes]() { ++changes; });
    ASSERT_TRUE(bridge.connectSession("mock://overflow").isEmpty());
    ecuio_frame_t f{};
    f.arbitration_id = 0x123;
    f.data_length = 1;
    bridge.dispatchFrameForTesting(f);
    f.flags = ECUIO_FRAME_FLAG_OVERFLOW;
    bridge.dispatchFrameForTesting(f);
    spinLoop(250);
    EXPECT_EQ(bridge.overflowFrames(), 1);
    bridge.dispatchFrameForTesting(f);
    bridge.dispatchFrameForTesting(f);
    spinLoop(250);
    EXPECT_EQ(bridge.overflowFrames(), 3);
    EXPECT_GE(changes, 2);
    const QString line =
        QStringLiteral("The adapter's buffer overflowed: frames were lost");
    EXPECT_EQ(log.count(line), 1) << log.join('\n').toStdString();
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_EQ(bridge.overflowFrames(), 3) << "readable after the disconnect";
    ASSERT_TRUE(bridge.connectSession("mock://overflow-again").isEmpty());
    EXPECT_EQ(bridge.overflowFrames(), 0);
    bridge.dispatchFrameForTesting(f);
    spinLoop(250);
    EXPECT_EQ(bridge.overflowFrames(), 1);
    EXPECT_EQ(log.count(line), 2) << "once a session";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

TEST(BridgeTest, DispatchedShortFrameHonorsDataLength)
{
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://short").isEmpty());

    // Unique timestamp so we can find OUR row among mock frames
    ecuio_frame_t f{};
    f.timestamp_us = 777777;   // steady-clock us from the mock won't collide
    f.arbitration_id = 0x0C0;  // telemetry ID: decodes (gate-consistent)
    f.channel_id = 0;
    f.data_length = 4;          // the whole point: NOT 8
    f.flags = ECUIO_FRAME_FLAG_EXTENDED;    // was: f.is_extended = 1
    f.data[0] = 0x34;           // arbitrary RPM-looking bytes
    f.data[1] = 0x12;
    f.data[2] = 0xAA;
    f.data[3] = 0x55;

    bridge.dispatchFrameForTesting(f);

    // CONTRACT: our dispatched frame reaches the model. Poll on the
    // finding condition instead of a fixed 200ms settle: the row must
    // appear through the queued-invocation path regardless of runner speed.
    FrameListModel* proxy = bridge.model();
    auto row_finder = [proxy]() {
        for (int r = 0; r < proxy->rowCount(); ++r) {
            if (proxy->data(proxy->index(r, 0),
                    FrameListModel::TimestampRole).toULongLong() == 777777) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(spinUntil(row_finder, 5000))
        << "dispatched frame never reached the model";

    int foundRow = -1;
    for (int r = 0; r < proxy->rowCount(); ++r) {
        if (proxy->data(proxy->index(r, 0),
                FrameListModel::TimestampRole).toULongLong() == 777777) {
            foundRow = r;
            break;
        }
    }
    ASSERT_NE(foundRow, -1) << "row vanished between poll and assert";

    const QModelIndex idx = proxy->index(foundRow, 0);
    // DLC must reflect the ACTUAL length, not an assumed 8
    EXPECT_EQ(proxy->data(idx, FrameListModel::DlcRole).toUInt(), 4u);
    // Exactly the 4 bytes, not 8, as spaced uppercase hex
    EXPECT_EQ(proxy->data(idx, FrameListModel::PayloadRole).toString(),
              QStringLiteral("34 12 AA 55"));
    EXPECT_TRUE(proxy->data(idx, FrameListModel::ExtendedRole).toBool());
    EXPECT_EQ(proxy->data(idx, FrameListModel::ArbitrationIdRole).toUInt(), 0x0C0u);

    bridge.disconnectSession();
}

/* LogPage binds frame columns by role NAME. A name missing here isn't a
 * build error: the column renders empty and every delegate throws. Read
 * through Bridge.model, what QML gets. */
TEST(BridgeTest, FrameRoleNamesMatchLogPageBindings)
{
    SessionBridge bridge;
    const QHash<int, QByteArray> roles = bridge.model()->roleNames();
    EXPECT_EQ(roles.value(FrameListModel::TimestampRole), QByteArray("timestampUs"));
    EXPECT_EQ(roles.value(FrameListModel::DlcRole), QByteArray("dlc"));
    EXPECT_EQ(roles.value(FrameListModel::ExtendedRole), QByteArray("extended"));
    EXPECT_EQ(roles.value(FrameListModel::PayloadRole), QByteArray("payloadHex"));
    EXPECT_EQ(roles.value(FrameListModel::ArbitrationIdRole), QByteArray("arbId"));
    EXPECT_EQ(roles.value(FrameListModel::SelectedRole), QByteArray("selected"));
}

TEST(BridgeTest, LogPauseStopsFrameLogNotDecode)
{
    /* Pause holds the frame log still to read: nothing is appended while
     * paused, but frames are still counted and decoded, so Live and the
     * Dashboard carry on. Resume appends again, and pause belongs to a
     * session: disconnect clears it and a new session starts unpaused. */
    SessionBridge bridge;
    ASSERT_TRUE(bridge.connectSession("mock://pause").isEmpty());
    FrameListModel* proxy = bridge.model();
    ASSERT_TRUE(spinUntil([proxy]() { return proxy->rowCount() > 0; }, 5000));

    bridge.setLogPaused(true);
    ASSERT_TRUE(bridge.logPaused());
    const int rows = proxy->rowCount();
    const int frames = bridge.frameCount();
    const int batches = bridge.signalBatchCount();

    // CONTRACT: frames and batches keep coming while the log holds. The
    // row count is exact: pause is set on the GUI thread, where every
    // append happens.
    ASSERT_TRUE(spinUntil([&bridge, frames, batches]() {
        return bridge.frameCount() > frames + 2 && bridge.signalBatchCount() > batches;
    }, 5000)) << "stream stopped while paused";
    EXPECT_EQ(proxy->rowCount(), rows);

    bridge.setLogPaused(false);
    ASSERT_TRUE(spinUntil([proxy, rows]() { return proxy->rowCount() > rows; }, 5000))
        << "log did not resume";

    bridge.setLogPaused(true);
    ASSERT_TRUE(disconnectAndWait(bridge));
    EXPECT_FALSE(bridge.logPaused());

    bridge.setLogPaused(true);
    ASSERT_TRUE(bridge.connectSession("mock://pause-again").isEmpty());
    EXPECT_FALSE(bridge.logPaused());
    bridge.disconnectSession();
}
