// File/layer: tests/test_viewer_frames.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: ViewerFramesTest (test_bridge)

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include "recordingframes.h"

#include "ecuio/recording.h"

namespace {

bool waitFor(const std::function<bool()>& done, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return done();
}

// A recording folder holding frames.csv alone (and the events.log that
// names its format): rows frames, the nth at host_us (n + 1) * 100000 and
// the frame's own session time 7 us later, so a table that shows the wrong
// one is seen. The data is the row number's low byte.
QString writeRecording(const QTemporaryDir& root, int rows,
                       const char* name = "rec")
{
    const QString folder = root.path() + QLatin1Char('/') + QLatin1String(name);
    QDir().mkpath(folder);
    QFile events(folder + QStringLiteral("/events.log"));
    if (!events.open(QIODevice::WriteOnly)) {
        return {};
    }
    events.write("ecuio session recording, format 2 (ADR-0049)\n"
                 "started  2026-11-07 10:00:00.000 (local time)\n"
                 "utc  2026-11-07T16:00:00.000Z\n"
                 "core  0.5.0\n"
                 "uri  elm327://COM3?profile=obd2\n"
                 "profile  obd2\n"
                 "frame flags  complete\n"
                 "caller  havne-cli 0.5.0\n"
                 "\n"
                 "10:00:01.000  +1.000  end  havne-cli: seconds: 3 frames, 0 "
                 "values, 0 records dropped, 0 notes dropped\n");
    events.close();
    QFile frames(folder + QStringLiteral("/frames.csv"));
    if (!frames.open(QIODevice::WriteOnly)) {
        return {};
    }
    QByteArray csv = "host_us,timestamp_us,id,flags,data\n";
    for (int n = 0; n < rows; ++n) {
        const quint64 host = static_cast<quint64>(n + 1) * 100000;
        csv +=
            QByteArray::number(host) + ',' + QByteArray::number(host + 7) +
            ",0x7E8,0x0,04 41 0C " +
            QByteArray::number(n & 0xFF, 16).rightJustified(2, '0').toUpper() +
            " 00 00 00 00\n";
    }
    frames.write(csv);
    return folder;
}

quint64 timeOfRow(RecordingFrames& r, int row)
{
    return r.frames()
        ->data(r.frames()->index(row), FrameListModel::TimestampRole)
        .toULongLong();
}

bool loaded(const RecordingFrames& r)
{
    return !r.loading();
}

} // namespace

// The rows of the part in view, both edges included, by the recording's
// own time (host_us): each row's time in the table is its host_us
TEST(ViewerFramesTest, LoadsThePartInView)
{
    QTemporaryDir root;
    const QString folder = writeRecording(root, 5);
    RecordingFrames r;
    r.load(folder, 200, 400);
    EXPECT_TRUE(r.loading());
    ASSERT_TRUE(waitFor([&] { return loaded(r); }));
    ASSERT_EQ(r.rows(), 3);
    EXPECT_EQ(timeOfRow(r, 0), 200000u) << "host_us, not the frame's 200007";
    EXPECT_EQ(timeOfRow(r, 2), 400000u);
    EXPECT_TRUE(r.note().isEmpty());
    EXPECT_TRUE(r.error().isEmpty());
    EXPECT_EQ(r.ids()->count(), 1) << "one ID over the rows";
}

// A view's left edge before the recording's start is the start, and a
// view wholly at or before it has no rows (core reads a to of 0 as the
// end, which a view never means)
TEST(ViewerFramesTest, AViewBeforeTheStartIsClampedOrEmpty)
{
    QTemporaryDir root;
    const QString folder = writeRecording(root, 5);
    std::atomic<int> reads{0};
    RecordingFrames r;
    r.setReadForTesting([&](const char* f, uint64_t from, uint64_t to,
                            ecuio_frame_row_callback_t cb, void* user,
                            ecuio_frames_read_t* summary) {
        ++reads;
        return ecuio_recording_read_frames(f, from, to, cb, user, summary);
    });
    r.load(folder, -20000, -8000);
    EXPECT_FALSE(r.loading()) << "no read for a view that has no rows";
    EXPECT_EQ(r.rows(), 0);
    r.load(folder, -20, 0);
    EXPECT_FALSE(r.loading());
    EXPECT_EQ(reads.load(), 0);

    r.load(folder, -1000, 250);
    ASSERT_TRUE(waitFor([&] { return loaded(r); }));
    ASSERT_EQ(r.rows(), 2) << "from the start to 0.25 s";
    EXPECT_EQ(timeOfRow(r, 0), 100000u);
    EXPECT_EQ(timeOfRow(r, 1), 200000u);
    EXPECT_EQ(reads.load(), 1);
}

// Exactly the cap is no more than fits: no note. One row past it is, and
// the note says where the table begins (never before the start)
TEST(ViewerFramesTest, TheCapIsTheRowAfterTheLastThatFits)
{
    QTemporaryDir root;
    constexpr double kEverything = 2000000; // ms: past the last row
    {
        const QString folder =
            writeRecording(root, RecordingFrames::MaxRows, "exact");
        RecordingFrames r;
        r.load(folder, 0, kEverything);
        ASSERT_TRUE(waitFor([&] { return loaded(r); }));
        EXPECT_EQ(r.rows(), RecordingFrames::MaxRows);
        EXPECT_TRUE(r.note().isEmpty()) << r.note().toStdString();
    }
    {
        const QString folder =
            writeRecording(root, RecordingFrames::MaxRows + 200, "over");
        RecordingFrames r;
        r.load(folder, 12300, kEverything);
        ASSERT_TRUE(waitFor([&] { return loaded(r); }));
        EXPECT_EQ(r.rows(), RecordingFrames::MaxRows);
        EXPECT_TRUE(r.error().isEmpty()) << "the cap's stop is not a failure";
        EXPECT_EQ(r.note().toStdString(),
                  "the first 10,000 frames from 12.3 s: zoom in for the rest");
        // The rows are the first from the left edge (host_us 12.3 s onward)
        EXPECT_EQ(timeOfRow(r, 0), 12300000u);

        // A view that starts before 0 says it begins at 0
        r.load(folder, -5000, kEverything);
        ASSERT_TRUE(waitFor([&] { return loaded(r); }));
        EXPECT_EQ(r.note().toStdString(),
                  "the first 10,000 frames from 0.0 s: zoom in for the rest");
        EXPECT_EQ(timeOfRow(r, 0), 100000u);
    }
}

TEST(ViewerFramesTest, AFolderWithNoFramesIsAnErrorWithCoresWords)
{
    QTemporaryDir root;
    QDir().mkpath(root.path() + QStringLiteral("/empty"));
    RecordingFrames r;
    r.load(root.path() + QStringLiteral("/empty"), 0, 1000);
    ASSERT_TRUE(waitFor([&] { return loaded(r); }));
    EXPECT_EQ(r.rows(), 0);
    EXPECT_NE(r.error().indexOf(QStringLiteral("frames.csv")), -1)
        << r.error().toStdString();
}

namespace {

// Sets a flag when it goes out of scope, declared after the object whose
// worker waits on the flag so a failed ASSERT cannot leave the worker
// spinning: the guard goes first, then the object joins
struct Releases {
    std::atomic<bool>& flag;
    ~Releases() { flag = true; }
};

// Waits until n reads have ended and been taken on the GUI thread
bool finishedAt(const RecordingFrames& r, int n)
{
    return waitFor([&] { return r.finishedForTesting() >= n; });
}

} // namespace

// A refill while a read runs, the older read ending last: it is dropped
// when it ends, whatever it read
TEST(ViewerFramesTest, AnOlderReadThatEndsLastLeavesTheNewerRowsAlone)
{
    QTemporaryDir root;
    const QString folder = writeRecording(root, 5);
    std::atomic<int> calls{0};
    std::atomic<int> olderRc{-1};
    std::atomic<bool> release{false};
    RecordingFrames r;
    Releases guard{release};
    r.setReadForTesting([&](const char* f, uint64_t from, uint64_t to,
                            ecuio_frame_row_callback_t cb, void* user,
                            ecuio_frames_read_t* summary) {
        if (++calls == 1) {
            while (!release.load()) {
                std::this_thread::yield();
            }
            const ecuio_status_t rc =
                ecuio_recording_read_frames(f, from, to, cb, user, summary);
            olderRc = static_cast<int>(rc);
            return rc;
        }
        return ecuio_recording_read_frames(f, from, to, cb, user, summary);
    });
    r.load(folder, 100, 500); // the older: held
    ASSERT_TRUE(waitFor([&] { return calls.load() == 1; }));
    r.load(folder, 400, 500); // the newer: two rows
    ASSERT_TRUE(finishedAt(r, 1));
    ASSERT_EQ(r.rows(), 2);
    EXPECT_EQ(timeOfRow(r, 0), 400000u);
    release = true;
    ASSERT_TRUE(finishedAt(r, 2)) << "the older read has ended";
    EXPECT_EQ(olderRc.load(), static_cast<int>(ECUIO_ERR_CANCELED))
        << "the stop reached the older read's callback";
    EXPECT_EQ(r.rows(), 2);
    EXPECT_EQ(timeOfRow(r, 0), 400000u);
    EXPECT_FALSE(r.loading());
}

// The older read ending first: its end is dropped and the newer's wait
// goes on
TEST(ViewerFramesTest, AnOlderReadThatEndsFirstIsDropped)
{
    QTemporaryDir root;
    const QString folder = writeRecording(root, 5);
    std::atomic<bool> release{false};
    RecordingFrames r;
    Releases guard{release};
    r.setReadForTesting([&](const char* f, uint64_t from, uint64_t to,
                            ecuio_frame_row_callback_t cb, void* user,
                            ecuio_frames_read_t* summary) {
        // The reads run on their own threads and may reach here in either
        // order, so the newer is picked out by its start (400 ms, in us)
        if (from == 400000u) {
            while (!release.load()) {
                std::this_thread::yield();
            }
        }
        return ecuio_recording_read_frames(f, from, to, cb, user, summary);
    });
    r.load(folder, 100, 500); // the older: reads at once
    r.load(folder, 400, 500); // the newer: held. The GUI thread has taken
                              // nothing yet, so the older's end is waiting
    ASSERT_TRUE(finishedAt(r, 1)) << "the older read's end";
    EXPECT_EQ(r.rows(), 0) << "it filled nothing";
    EXPECT_TRUE(r.loading()) << "the newer is still reading";
    release = true;
    ASSERT_TRUE(finishedAt(r, 2));
    ASSERT_EQ(r.rows(), 2);
    EXPECT_EQ(timeOfRow(r, 0), 400000u);
    EXPECT_FALSE(r.loading());
}

// A stopped read that cannot act on the stop and ends as a complete read
// of nothing still does not replace the newer's rows
TEST(ViewerFramesTest, AStoppedReadThatEndsCompleteIsDropped)
{
    QTemporaryDir root;
    const QString folder = writeRecording(root, 5);
    std::atomic<int> calls{0};
    std::atomic<bool> release{false};
    RecordingFrames r;
    Releases guard{release};
    r.setReadForTesting([&](const char* f, uint64_t from, uint64_t to,
                            ecuio_frame_row_callback_t cb, void* user,
                            ecuio_frames_read_t* summary) {
        if (++calls == 1) {
            while (!release.load()) {
                std::this_thread::yield();
            }
            if (summary != nullptr) {
                *summary = {};
            }
            return ECUIO_OK; // ignores the stop: an empty, complete read
        }
        return ecuio_recording_read_frames(f, from, to, cb, user, summary);
    });
    r.load(folder, 100, 500);
    ASSERT_TRUE(waitFor([&] { return calls.load() == 1; }));
    r.load(folder, 100, 200); // two rows
    ASSERT_TRUE(finishedAt(r, 1));
    ASSERT_EQ(r.rows(), 2);
    release = true;
    ASSERT_TRUE(finishedAt(r, 2));
    EXPECT_EQ(r.rows(), 2) << "the empty older read did not replace them";
}

TEST(ViewerFramesTest, ClearEmptiesTheModelsAndSilencesARunningRead)
{
    QTemporaryDir root;
    const QString folder = writeRecording(root, 5);
    RecordingFrames r;
    r.load(folder, 0, 1000);
    ASSERT_TRUE(waitFor([&] { return r.rows() == 5; }));
    r.clear();
    EXPECT_EQ(r.rows(), 0);
    EXPECT_EQ(r.frames()->heldCount(), 0);
    EXPECT_EQ(r.ids()->count(), 0);

    const int before = r.finishedForTesting();
    r.load(folder, 0, 1000);
    r.clear(); // before the read ends
    ASSERT_TRUE(finishedAt(r, before + 1)) << "the cleared read's end";
    EXPECT_EQ(r.rows(), 0) << "a read cleared away fills nothing";
    EXPECT_FALSE(r.loading());
}

// A read cleared away that cannot be stopped (one that ignores the stop
// and ends as a complete read) still reports nothing when it ends
TEST(ViewerFramesTest, AClearedReadThatEndsAnywayReportsNothing)
{
    QTemporaryDir root;
    const QString folder = writeRecording(root, 5);
    std::atomic<bool> release{false};
    RecordingFrames r;
    Releases guard{release};
    r.setReadForTesting([&](const char*, uint64_t, uint64_t,
                            ecuio_frame_row_callback_t, void*,
                            ecuio_frames_read_t* summary) {
        while (!release.load()) {
            std::this_thread::yield();
        }
        if (summary != nullptr) {
            *summary = {};
        }
        return ECUIO_OK;
    });
    int loadedCount = 0;
    QObject::connect(&r, &RecordingFrames::loadedChanged, &r,
                     [&] { ++loadedCount; });
    r.load(folder, 0, 1000);
    r.clear();
    EXPECT_EQ(loadedCount, 1) << "clear() itself";
    release = true;
    ASSERT_TRUE(finishedAt(r, 1));
    EXPECT_EQ(loadedCount, 1) << "the cleared read's end said nothing";
    EXPECT_FALSE(r.loading());
}

// The object destroyed while a read is held: the destructor waits for the
// worker, whose end is then posted to nothing. Without the join the thread
// outlives its job and std::thread terminates
TEST(ViewerFramesTest, DestroyingWhileAReadIsHeldWaitsForItAndPostsToNothing)
{
    QTemporaryDir root;
    const QString folder = writeRecording(root, 5);
    std::atomic<int> calls{0};
    std::atomic<bool> release{false};
    std::atomic<bool> returned{false};
    Releases guard{release};
    auto r = std::make_unique<RecordingFrames>();
    r->setReadForTesting([&](const char* f, uint64_t from, uint64_t to,
                             ecuio_frame_row_callback_t cb, void* user,
                             ecuio_frames_read_t* summary) {
        ++calls;
        while (!release.load()) {
            std::this_thread::yield();
        }
        const ecuio_status_t rc =
            ecuio_recording_read_frames(f, from, to, cb, user, summary);
        returned = true;
        return rc;
    });
    r->load(folder, 100, 500);
    ASSERT_TRUE(waitFor([&] { return calls.load() == 1; }));

    // The read is let go a moment after the destructor starts waiting
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        release = true;
    });
    r.reset();
    releaser.join();
    EXPECT_TRUE(returned.load()) << "the destructor waited for the read";
    for (int i = 0; i < 20; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    EXPECT_EQ(calls.load(), 1);
}
