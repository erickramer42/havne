// File/layer: tests/test_recorder_modules.cpp | test_core, Qt-free, C++17
// Testing: UNIT: RecorderModulesTest (a recording's module lines, ADR-0065
//          step 5: through ecuio_record_modules on a recorder attached to
//          an unstarted mock session, this thread standing in for its
//          worker)

#include "test_condition_recording_support.h"

#include "car_sized_set.h"

#include <ecuio/operation.h>

namespace {

/* The car-sized set's two modules, 0x7E8 and 0x7E9, on the mock, a
 * recorder attached before start: this thread is the one that puts
 * records. closed is what the worker's last take left (the header's) */
struct ModulesRecorded {
    struct Profile {
        Profile()
        {
            EXPECT_EQ(ecuio_profile_register("recorder-modules-car",
                                             car_sized::set(), nullptr),
                      ECUIO_OK);
        }
        ~Profile()
        {
            EXPECT_EQ(ecuio_profile_unregister("recorder-modules-car"),
                      ECUIO_OK);
        }
    } profile; /* before the session, which holds the set */
    Root root;
    Session session{"mock://x?profile=recorder-modules-car"};
    ecuio_recorder_t* recorder = nullptr;
    fs::path folder;

    explicit ModulesRecorded(uint32_t closed = 0, uint32_t ring_bytes = 0)
    {
        session.s->modules_closed.store(closed);
        recorder = open_recorder(root, ring_bytes);
        if (recorder != nullptr) {
            folder = folder_of(recorder);
            EXPECT_EQ(ecuio_session_record(session.s, recorder), ECUIO_OK)
                << ecuio_last_error_detail();
        }
    }
    void put(uint32_t closed, uint32_t no_room, uint8_t first, uint8_t second)
    {
        const uint8_t reasons[2] = {first, second};
        ecuio_record_modules(session.s->record_link.get(), closed, no_room,
                             reasons, 2);
    }
    /* Until the writer drained what the ring held: a wake counts itself
     * before it reads the hold, so the second wake follows a drain */
    void drained() const
    {
        const uint64_t from = ecuio_recorder_test_wakes(recorder);
        const auto give_up =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (ecuio_recorder_test_wakes(recorder) < from + 2) {
            if (std::chrono::steady_clock::now() >= give_up) {
                ADD_FAILURE() << "the writer never woke twice";
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    void close()
    {
        if (recorder != nullptr) {
            EXPECT_EQ(ecuio_recorder_close(recorder, "done"), ECUIO_OK);
            recorder = nullptr;
        }
    }
    ~ModulesRecorded() { close(); }
    ModulesRecorded(const ModulesRecorded&) = delete;
    ModulesRecorded& operator=(const ModulesRecorded&) = delete;
};

/* events.log's header lines of the modules */
std::vector<std::string> header_modules(const fs::path& folder)
{
    std::vector<std::string> out;
    for (const std::string& l : lines_of(folder / "events.log")) {
        if (l.rfind("module  ", 0) == 0) {
            out.push_back(l);
        }
    }
    return out;
}

constexpr uint8_t kRange = ECUIO_MODULE_REASON_RANGE;
constexpr uint8_t kRead = ECUIO_MODULE_REASON_READ;
constexpr uint8_t kVisit = ECUIO_MODULE_REASON_VISIT;

} // namespace

/* A timed line for each module whose state moved, in channel order, an
 * open one with the reasons that want it; a record that moves nothing
 * writes nothing, and a session whose modules were all open at the attach
 * writes no header line */
TEST(RecorderModulesTest, EachChangedModuleIsOneTimedLineWithItsReason)
{
    ModulesRecorded m;
    ASSERT_NE(m.recorder, nullptr);
    m.put(0x2, 0, kRange, 0);
    m.put(0, 0, kRange, kRead | kVisit);
    m.put(0x1, 0x2, 0, kRead);
    m.put(0x1, 0x2, 0, kRead);
    m.close();
    EXPECT_EQ(kind_lines(m.folder, "module"),
              (std::vector<std::string>{"0x7E9,closed", "0x7E9,open,read+visit",
                                        "0x7E8,closed", "0x7E9,no-room"}));
    EXPECT_TRUE(header_modules(m.folder).empty());
}

/* The header names each module not open at the attach, and the timed
 * lines start from what it said */
TEST(RecorderModulesTest, AHeaderLineOnlyForAModuleNotOpenAtTheAttach)
{
    ModulesRecorded m(0x2);
    ASSERT_NE(m.recorder, nullptr);
    m.put(0x2, 0, kRange, 0);
    m.put(0, 0, kRange, kRead);
    m.close();
    EXPECT_EQ(header_modules(m.folder),
              (std::vector<std::string>{"module  0x7E9,closed"}));
    EXPECT_EQ(kind_lines(m.folder, "module"),
              (std::vector<std::string>{"0x7E9,open,read"}));
}

/* States that find the ring full are carried, the latest winning, and
 * written once there is room */
TEST(RecorderModulesTest, AFullRingCarriesTheLatestModuleState)
{
    ModulesRecorded m(0, 4096);
    ASSERT_NE(m.recorder, nullptr);
    ecuio_recorder_test_hold(m.recorder, 1);
    for (int i = 0; i < 200; ++i) {
        engine_speed(m.session.s, 1000000 + i * 10000u, 800);
    }
    m.put(0x2, 0, kRange, 0);
    m.put(0x3, 0, 0, 0);
    ecuio_recorder_test_hold(m.recorder, 0);
    m.drained();
    engine_speed(m.session.s, 4000000, 800);
    m.close();
    EXPECT_EQ(kind_lines(m.folder, "module"),
              (std::vector<std::string>{"0x7E8,closed", "0x7E9,closed"}));
}

namespace {

void fill_the_ring(ModulesRecorded& m)
{
    ecuio_recorder_test_hold(m.recorder, 1);
    for (int i = 0; i < 200; ++i) {
        engine_speed(m.session.s, 1000000 + i * 10000u, 800);
    }
}

void run_line(ModulesRecorded& m, const char* id, uint32_t end)
{
    ecuio_record_operation(m.session.s->record_link.get(), id, 0x7E8, 6144,
                           end);
}

} // namespace

TEST(RecorderModulesTest, AFullRingCarriesARunsStartBeforeItsEnd)
{
    ModulesRecorded m(0, 4096);
    ASSERT_NE(m.recorder, nullptr);
    fill_the_ring(m);
    run_line(m, "first", ECUIO_OPERATION_END_NONE);
    run_line(m, "first", ECUIO_OPERATION_END_REFUSED);
    run_line(m, "second", ECUIO_OPERATION_END_NONE);
    run_line(m, "second", ECUIO_OPERATION_END_CANCELED);
    /* past what is carried: the newest gives way, the order stays */
    run_line(m, "third", ECUIO_OPERATION_END_NONE);
    ecuio_recorder_test_hold(m.recorder, 0);
    m.drained();
    engine_speed(m.session.s, 4000000, 800);
    m.close();
    EXPECT_EQ(kind_lines(m.folder, "operation"),
              (std::vector<std::string>{"dump,first,0x7E8,6144,started",
                                        "dump,first,0x7E8,6144,refused",
                                        "dump,second,0x7E8,6144,started",
                                        "dump,third,0x7E8,6144,started"}));
}

TEST(RecorderModulesTest, EachEndAndAnEnvelopeHaveTheirWords)
{
    ModulesRecorded m(0, 4096);
    ASSERT_NE(m.recorder, nullptr);
    ecuio_record_envelope(m.session.s->record_link.get(), 0x7E8, 12600, 0, 0,
                          0x47);
    ecuio_record_envelope(m.session.s->record_link.get(), 0x7E8, 0, -1, 3, 0x8);
    run_line(m, "copy", ECUIO_OPERATION_END_ENGINE_RUNNING);
    run_line(m, "copy", ECUIO_OPERATION_END_MOVING);
    run_line(m, "copy", ECUIO_OPERATION_END_VOLTAGE_LOW);
    run_line(m, "copy", ECUIO_OPERATION_END_VOLTAGE_HIGH);
    m.close();
    EXPECT_EQ(
        kind_lines(m.folder, "envelope"),
        (std::vector<std::string>{"0x7E8,12600,0,0,0x47", "0x7E8,-,-,3,0x8"}));
    EXPECT_EQ(kind_lines(m.folder, "operation"),
              (std::vector<std::string>{"dump,copy,0x7E8,6144,engine-running",
                                        "dump,copy,0x7E8,6144,moving",
                                        "dump,copy,0x7E8,6144,voltage-low",
                                        "dump,copy,0x7E8,6144,voltage-high"}));
}

TEST(RecorderModulesTest, ACloseWritesTheRunLinesStillCarried)
{
    ModulesRecorded m(0, 4096);
    ASSERT_NE(m.recorder, nullptr);
    fill_the_ring(m);
    run_line(m, "copy", ECUIO_OPERATION_END_NONE);
    run_line(m, "copy", ECUIO_OPERATION_END_CANCELED);
    m.close();
    EXPECT_EQ(kind_lines(m.folder, "operation"),
              (std::vector<std::string>{"dump,copy,0x7E8,6144,started",
                                        "dump,copy,0x7E8,6144,canceled"}));
}
