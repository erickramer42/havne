#include "dump_file_support.h"
#include "session_flash_support.h"

#include "../core/src/operation_journal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>

namespace {

using namespace session_operation;
using namespace session_flash;

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

} // namespace

TEST(SessionOperationTest, AFlashErasesWritesChecksAndResets)
{
    Bench b;
    arm(b);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    std::vector<ecuio_operation::journal_found> at_erase;
    b.script = [&](const Bytes& q, size_t, Answer*) {
        if (q.size() > 4 && q[0] == 0x31 && q[2] == 0xFF && q[3] == 0x00) {
            at_erase.push_back(journal_of(b));
        }
        return false;
    };
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
    EXPECT_EQ(at_erase, (std::vector<ecuio_operation::journal_found>{
                            ecuio_operation::journal_found::unfinished,
                            ecuio_operation::journal_found::unfinished}))
        << "on the disk before each erase reaches the module";
    EXPECT_EQ(b.s->operation->journal.posted(), 4u)
        << "the start of writing, each sector's end, the end: never a block";
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
                          "module stopped answering): recover the module "
                          "before it runs");
    EXPECT_EQ(b.count(0x11), 0u) << "no reset of a half-written module";
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::unfinished);
    const size_t ended_at = b.sent.size();
    b.now += 30 * 1000000;
    b.passes(400);
    EXPECT_EQ(b.sent.size(), ended_at)
        << "nothing more is asked of it: no poll, keep-alive or entry";
    constexpr const char* kWords = "the module's software was left "
                                   "part-written: recover it first";
    EXPECT_EQ(start(b), ECUIO_ERR_IN_USE);
    EXPECT_STREQ(ecuio_last_error_detail(), kWords);
    uint32_t wait_ms = 0;
    EXPECT_EQ(ecuio_session_security_unlock(b.s, 0, 0x01, 0, &wait_ms),
              ECUIO_ERR_IN_USE);
    EXPECT_STREQ(ecuio_last_error_detail(), kWords);
    EXPECT_EQ(ecuio_session_clear_diagnostics(b.s, 0, 0), ECUIO_ERR_IN_USE);
    EXPECT_STREQ(ecuio_last_error_detail(), kWords);
}

TEST(SessionOperationTest, AnUnfinishedJournalRefusesTheFlash)
{
    Bench b;
    arm(b);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    std::ofstream(ecuio_operation::journal_path(b.folder.str(), 0x7E8),
                  std::ios::binary)
        << R"({"havne_journal": 1, "state": "writing"})";
    EXPECT_EQ(start(b), ECUIO_ERR_IN_USE);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the flash journal 000007E8.havnejournal says a flash of "
                 "this module didn't finish: the module may be mid-flash");
    EXPECT_EQ(b.count(0x10), 1u) << "only the bench's own entry; nothing sent";
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
                          "failed: recover the module before it runs");
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

TEST(SessionOperationTest, AJournalThatFailsPastTheFloorLeavesItUnfinished)
{
    Bench b;
    arm(b);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    bool struck = false;
    b.script = [&](const Bytes& q, size_t, Answer*) {
        if (!struck && q[0] == 0x34) {
            struck = true;
            ecuio_operation::journal_test_fail_at = 1;
        }
        return false;
    };
    ASSERT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_WRITTEN)
        << "past the floor the write goes on: " << v.words;
    EXPECT_EQ(b.region(kCode, 2 * kSector), image_two_sectors());
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::unfinished)
        << "the journal says what it last could";
}

TEST(SessionOperationTest, AFlashClosedWithItsEraseOutNeedsTheFlashAgain)
{
    Bench b;
    arm(b);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    ecuio_operation_state_t v{};
    v.struct_size = sizeof v;
    b.while_out = [&](Bench& x) {
        const Bytes& q = x.sent.back();
        if (v.end == ECUIO_OPERATION_END_NONE && q.size() > 4 && q[0] == 0x31 &&
            q[2] == 0xFF && q[3] == 0x00) {
            ecuio_session_close(x.s);
            (void)ecuio_session_get_operation(x.s, &v);
        }
    };
    ASSERT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
    for (int i = 0;
         i < 4000 && v.end == ECUIO_OPERATION_END_NONE && !b.s->closed.load();
         ++i) {
        b.pass();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_RECOVERY_NEEDED) << v.words;
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::unfinished);
}

TEST(SessionOperationTest, AFlashClosedBeforeItWritesSaysNothingWasWritten)
{
    Bench b;
    arm(b);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    ASSERT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
    ecuio_session_close(b.s);
    const ecuio_operation_state_t v = b.state();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_CANCELED);
    EXPECT_STREQ(v.words, "the session closed before the flash began "
                          "writing: nothing was written");
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::none);
}

TEST(SessionOperationTest, AFailedCheckWithNothingWrittenSaysSo)
{
    Flash fill = sim_flash();
    for (uint32_t i = 0; i < 2 * kSector; ++i) {
        fill.images[0].blob[i] = ecuio_sim_memory_fill(kCode + i);
    }
    Bench b;
    ecuio_sim_flash_faults f;
    f.check = true;
    arm(b, f);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made(fill)));
    const std::string copy = write_copy(b);
    ASSERT_EQ(start(b, copy.c_str()), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(b.state().sectors, 0u);
    const ecuio_operation_state_t v = b.until_ended(20000);
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_CHECK_FAILED);
    EXPECT_STREQ(v.words, "nothing was written, and the module's own check "
                          "failed: what it holds isn't the images, so flash "
                          "it without a copy");
    EXPECT_EQ(b.count(0x34), 0u);
    EXPECT_EQ(b.sent.back(), (Bytes{0x11, 0x01}));
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::none);
}

TEST(SessionOperationTest, AWrittenFlashWhoseJournalCantCloseSaysSo)
{
    Bench b;
    arm(b);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    b.script = [&](const Bytes& q, size_t, Answer*) {
        if (q.size() >= 4 && q[0] == 0x31 && q[2] == 0xFF && q[3] == 0x01) {
            ecuio_operation::journal_test_fail_at = 1;
        }
        return false;
    };
    ASSERT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_WRITTEN);
    EXPECT_EQ(std::string(v.words).rfind(
                  "written and checked, but the flash journal can't be "
                  "written: ",
                  0),
              0u)
        << v.words;
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::unfinished);
}

TEST(SessionOperationTest, ADumpAfterAFlashCountsNoSectors)
{
    conversation_test::Made m = flash_made();
    const std::string dump = made().operations;
    m.operations =
        m.operations.substr(0, m.operations.size() - 1) + ", " + dump.substr(1);
    Bench b;
    arm(b);
    ASSERT_NO_FATAL_FAILURE(b.start(m));
    ASSERT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(b.until_ended(20000).end, ECUIO_OPERATION_END_WRITTEN);
    ecuio_operation_request_t q = request(ECUIO_OPERATION_MAY_UNLOCK);
    q.operation = 1;
    ASSERT_EQ(ecuio_session_operation_start(b.s, &q, b.folder.str().c_str()),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(b.state().sectors, 0u);
    ASSERT_EQ(ecuio_session_operation_cancel(b.s), ECUIO_OK);
    EXPECT_EQ(b.until_ended().end, ECUIO_OPERATION_END_CANCELED);
}
