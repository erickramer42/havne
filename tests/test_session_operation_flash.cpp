#include "dump_file_support.h"
#include "plugin_flash_support.h"
#include "session_operation_support.h"

#include "../core/src/operation_journal.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <string>

namespace {

using namespace session_operation;
using flash_test::Flash;

constexpr uint32_t kCode = ECUIO_SIM_MEMORY_CODE_ADDRESS;
constexpr uint32_t kSector = ECUIO_SIM_FLASH_SECTOR_BYTES;

/* Two sectors of the sim's code region; the first holds what the module
 * holds already, the second a made-up image */
Bytes image_two_sectors()
{
    Bytes b(2 * kSector);
    for (uint32_t i = 0; i < kSector; ++i) {
        b[i] = ecuio_sim_memory_fill(kCode + i);
        b[kSector + i] = static_cast<uint8_t>(i * 37 + 3);
    }
    return b;
}

Flash sim_flash()
{
    Flash f;
    f.images[0].blob = image_two_sectors();
    f.sectors = R"([{"address": "00080000", "bytes": 4096}, )"
                R"({"address": "00081000", "bytes": 4096}])";
    return f;
}

conversation_test::Made flash_made(const Flash& f = sim_flash())
{
    conversation_test::Made m = made();
    m.operations = f.json();
    m.files_extra = R"(, "code.bin": {"kind": "bin", "base64": ")" +
                    conversation_test::base64(f.images[0].blob) + "\"}";
    return m;
}

void arm(Bench& b, const ecuio_sim_flash_faults& faults = {})
{
    ASSERT_TRUE(ecuio_sim_flash_arm(b.node.s.flash, b.node.s.memory, faults));
}

constexpr uint32_t kFlags =
    ECUIO_OPERATION_MAY_UNLOCK | ECUIO_OPERATION_BATTERY_SUPPORT;

ecuio_status_t start(Bench& b, const char* copy = nullptr)
{
    const ecuio_operation_request_t q = request(kFlags);
    return ecuio_session_operation_start_from(b.s, &q, b.folder.str().c_str(),
                                              copy);
}

/* A copy of the code region as the module holds it, in the bench's folder */
std::string write_copy(Bench& b)
{
    ecuio_dump_file::record r = dump_file_test::record_of();
    ecuio_dump_file::range code;
    code.name = "code";
    code.step = ECUIO_DUMP_STEP_UPLOAD;
    code.session = ECUIO_DUMP_SESSION_PROGRAMMING;
    code.address = kCode;
    code.bytes = 2 * kSector;
    code.offset = 0;
    r.ranges = {code};
    r.operation = "flash-code";
    const Bytes now = b.region(kCode, 2 * kSector);
    const std::string file = ecuio_dump_file::build(r, now.data(), now.size());
    const std::string name = dump_file_test::name_for(file);
    std::ofstream(b.folder.str() + "/" + name, std::ios::binary) << file;
    return name;
}

size_t count_routine(const Bench& b, uint16_t id)
{
    size_t n = 0;
    for (const Bytes& q : b.sent) {
        n += q.size() >= 4 && q[0] == 0x31 && q[1] == 0x01 &&
                     q[2] == static_cast<uint8_t>(id >> 8) &&
                     q[3] == static_cast<uint8_t>(id)
                 ? 1u
                 : 0u;
    }
    return n;
}

ecuio_operation::journal_found journal_of(const Bench& b)
{
    std::string why;
    return ecuio_operation::journal_find(b.folder.str(), 0x7E8, &why);
}

} // namespace

TEST(SessionOperationTest, AFlashErasesWritesChecksAndResets)
{
    Bench b;
    arm(b);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    ASSERT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(b.state().sectors, 2u);
    EXPECT_EQ(b.state().bytes_total, 2u * kSector);
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::none)
        << "ready: nothing written yet";
    const ecuio_operation_state_t v = b.until_ended(20000);
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_WRITTEN) << v.words;
    EXPECT_STREQ(v.words, "");
    EXPECT_EQ(v.sectors_done, 2u);
    EXPECT_EQ(v.bytes_done, 2u * kSector);
    EXPECT_EQ(b.region(kCode, 2 * kSector), image_two_sectors());
    EXPECT_EQ(count_routine(b, 0xFF00), 2u);
    EXPECT_EQ(count_routine(b, 0xFF01), 1u);
    EXPECT_EQ(b.count(0x34), 2u);
    EXPECT_EQ(b.count(0x37), 2u);
    ASSERT_FALSE(b.sent.empty());
    EXPECT_EQ(b.sent.back(), (Bytes{0x11, 0x01})) << "the flash's leave";
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::none);
}

TEST(SessionOperationTest, AFlashAgainstACopyWritesOnlyTheSectorThatDiffers)
{
    Bench b;
    arm(b);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    const std::string copy = write_copy(b);
    ASSERT_EQ(start(b, copy.c_str()), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(b.state().sectors, 1u) << "the first sector holds the image";
    const ecuio_operation_state_t v = b.until_ended(20000);
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_WRITTEN) << v.words;
    EXPECT_EQ(count_routine(b, 0xFF00), 1u);
    bool second = false;
    for (const Bytes& q : b.sent) {
        second = second || (q.size() == 13 && q[0] == 0x31 && q[7] == 0x10);
    }
    EXPECT_TRUE(second) << "the erase names the second sector";
    EXPECT_EQ(b.region(kCode, 2 * kSector), image_two_sectors());
}

TEST(SessionOperationTest, ARefusedEraseIsACleanAbort)
{
    Bench b;
    ecuio_sim_flash_faults f;
    f.erase = true;
    arm(b, f);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    ASSERT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_REFUSED);
    EXPECT_EQ(v.nrc, 0x22);
    EXPECT_STREQ(v.words, "the module refused a request: nothing was written");
    EXPECT_EQ(b.sent.back(), (Bytes{0x11, 0x01}))
        << "before the floor the leave goes";
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::none);
    ASSERT_EQ(start(b), ECUIO_OK) << "and a flash may start again";
    ASSERT_EQ(ecuio_session_operation_cancel(b.s), ECUIO_OK);
    const ecuio_operation_state_t again = b.until_ended();
    EXPECT_EQ(again.end, ECUIO_OPERATION_END_CANCELED) << again.words;
}

TEST(SessionOperationTest, AModuleLostPastTheFloorNeedsTheFlashAgain)
{
    Bench b;
    ecuio_sim_flash_faults f;
    f.silent = true;
    arm(b, f);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    ASSERT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_RECOVERY_NEEDED);
    EXPECT_STREQ(v.words, "the flash stopped after it began writing (the "
                          "module stopped answering): flash the module again "
                          "before it runs");
    EXPECT_EQ(b.count(0x11), 0u) << "no reset of a half-written module";
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::unfinished);
    EXPECT_EQ(start(b), ECUIO_ERR_IN_USE);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the flash journal 000007E8.havnejournal says a flash of "
                 "this module didn't finish: the module may be mid-flash");
}

TEST(SessionOperationTest, AFailedCheckEndsInItsOwnWords)
{
    Bench b;
    ecuio_sim_flash_faults f;
    f.check = true;
    arm(b, f);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    ASSERT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_CHECK_FAILED);
    EXPECT_STREQ(v.words, "the module's own check of what was written "
                          "failed: flash it again before it runs");
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::unfinished);
}

TEST(SessionOperationTest, ABlockRefusedOnceForItsCounterIsSentAgain)
{
    Bench b;
    ecuio_sim_flash_faults f;
    f.counter = true;
    arm(b, f);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    ASSERT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_WRITTEN) << v.words;
    EXPECT_EQ(b.region(kCode, 2 * kSector), image_two_sectors());
    EXPECT_EQ(b.count(0x36), 9u) << "eight blocks and the one sent again";
}

TEST(SessionOperationTest, AFlashsStartIsRefusedInWords)
{
    Bench elm;
    elm.uri = "elm327://bench";
    ASSERT_NO_FATAL_FAILURE(elm.start(flash_made()));
    EXPECT_EQ(start(elm), ECUIO_ERR_NOT_IMPLEMENTED);

    Bench narrow;
    narrow.write_max = 12;
    ASSERT_NO_FATAL_FAILURE(narrow.start(flash_made()));
    EXPECT_EQ(start(narrow), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the flash's erase is asked in 13 bytes: this adapter "
                 "carries no write that long");

    Bench b;
    arm(b);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    EXPECT_EQ(start(b, "no-such.havnedump"), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::string(ecuio_last_error_detail())
                  .rfind("the copy no-such.havnedump can't be read: ", 0),
              0u)
        << ecuio_last_error_detail();
    EXPECT_EQ(start(b, "sub/copy.havnedump"), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the copy is named as the folder lists it, not by a path");
    EXPECT_TRUE(b.files().empty()) << "a refusal leaves no journal";

    const ecuio_operation_request_t q = request(kFlags);
    EXPECT_EQ(b.s->battery.source, true);
    b.s->battery.source = false;
    EXPECT_EQ(ecuio_session_operation_start_from(
                  b.s, &q, b.folder.str().c_str(), nullptr),
              ECUIO_ERR_NOT_IMPLEMENTED)
        << "no voltage source: no flash";
    b.s->battery.source = true;
    EXPECT_EQ(ecuio_session_operation_start_from(nullptr, &q, "", nullptr),
              ECUIO_ERR_NULL_ARGUMENT);

    Bench dump;
    ASSERT_NO_FATAL_FAILURE(dump.start());
    const ecuio_operation_request_t d = request(ECUIO_OPERATION_MAY_UNLOCK);
    EXPECT_EQ(ecuio_session_operation_start_from(
                  dump.s, &d, dump.folder.str().c_str(), "x.havnedump"),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "only a flash is judged against a copy");
}

TEST(SessionOperationTest, ACopyWhoseRangesArentTheImagesIsRefused)
{
    Bench b;
    arm(b);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    const std::string file =
        dump_file_test::file_of(dump_file_test::record_of());
    const std::string name = dump_file_test::name_for(file);
    std::ofstream(b.folder.str() + "/" + name, std::ios::binary) << file;
    EXPECT_EQ(start(b, name.c_str()), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 ("the copy " + name +
                  "'s ranges aren't this flash's images: its sectors can't "
                  "be judged by it")
                     .c_str());
}

TEST(SessionOperationTest, AJournalThatCantBeWrittenRefusesTheStart)
{
    Bench b;
    arm(b);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    ecuio_operation::journal_test_fail_at = 1;
    EXPECT_EQ(start(b), ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(std::string(ecuio_last_error_detail())
                  .rfind("the flash journal can't be written: ", 0),
              0u)
        << ecuio_last_error_detail();
    EXPECT_TRUE(b.files().empty());
    EXPECT_EQ(b.count(0x31), 0u) << "nothing sent";
    ASSERT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_operation_cancel(b.s), ECUIO_OK);
    EXPECT_EQ(b.until_ended().end, ECUIO_OPERATION_END_CANCELED);
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::none);
}
