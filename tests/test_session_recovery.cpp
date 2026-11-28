#include "session_flash_support.h"

#include "../core/src/operation_journal.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <string>

namespace {

using namespace session_operation;
using namespace session_flash;
using ecuio_operation::journal_head;
using ecuio_operation::journal_mark;
using ecuio_operation::journal_state;

constexpr char kHeld[] =
    "the module's software was left part-written: recover it first";

journal_head head_of()
{
    journal_head h;
    h.operation = "flash-code";
    h.fingerprint = std::string(64, 'a');
    h.rx = 0x7E8;
    h.images = {std::string(64, 'b')};
    h.sectors = {{kCode, kSector}, {kCode + kSector, kSector}};
    h.utc = 1795000000;
    return h;
}

void put_journal(const Bench& b, const std::string& text)
{
    std::ofstream(ecuio_operation::journal_path(b.folder.str(), 0x7E8),
                  std::ios::binary)
        << text;
}

void put_journal(const Bench& b, const journal_head& h, const journal_mark& m)
{
    put_journal(b, ecuio_operation::journal_text(h, m));
}

journal_mark writing(uint32_t erased, uint32_t written)
{
    journal_mark m;
    m.state = journal_state::writing;
    m.erased = erased;
    m.written = written;
    return m;
}

ecuio_module_recovery_t recovery_of(const Bench& b, uint32_t channel = 0)
{
    ecuio_module_recovery_t r{};
    r.struct_size = sizeof r;
    EXPECT_EQ(ecuio_session_get_module_recovery(b.s, channel, &r), ECUIO_OK);
    return r;
}

} // namespace

TEST(SessionRecoveryTest, AnUnfinishedJournalHoldsTheModuleFromTheStart)
{
    Bench b;
    b.journals = true;
    put_journal(b, head_of(), writing(2, 1));
    arm(b);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    const ecuio_module_recovery_t r = recovery_of(b);
    EXPECT_EQ(r.state, ECUIO_RECOVERY_NEEDED);
    EXPECT_EQ(r.flags, 0u);
    EXPECT_EQ(r.attempts, 0u);
    EXPECT_EQ(r.operation, 0u);
    EXPECT_EQ(r.sectors, 2u);
    EXPECT_EQ(r.sectors_written, 1u);
    EXPECT_STREQ(r.words, "a flash of this module didn't finish: recover it "
                          "before it runs");
    EXPECT_TRUE(b.sent.empty()) << "nothing is asked of a held module";
    b.passes(50);
    EXPECT_TRUE(b.sent.empty());
    EXPECT_EQ(start(b), ECUIO_ERR_IN_USE);
    EXPECT_STREQ(ecuio_last_error_detail(), kHeld);
    uint32_t wait_ms = 0;
    EXPECT_EQ(ecuio_session_security_unlock(b.s, 0, 0x01, 0, &wait_ms),
              ECUIO_ERR_IN_USE);
    EXPECT_STREQ(ecuio_last_error_detail(), kHeld);
    EXPECT_EQ(ecuio_session_clear_diagnostics(b.s, 0, 0), ECUIO_ERR_IN_USE);
    EXPECT_STREQ(ecuio_last_error_detail(), kHeld);
}

TEST(SessionRecoveryTest, AJournalWithNoEraseMarkedSaysTheModuleIsLikelyIntact)
{
    Bench b;
    b.journals = true;
    journal_head h = head_of();
    h.copy = "copy.havnedump";
    put_journal(b, h, writing(0, 0));
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    const ecuio_module_recovery_t r = recovery_of(b);
    EXPECT_EQ(r.state, ECUIO_RECOVERY_NEEDED) << "still offered";
    EXPECT_EQ(r.flags,
              ECUIO_RECOVERY_NOTHING_ERASED | ECUIO_RECOVERY_COPY_PINNED);
    EXPECT_STREQ(r.words, "the flash stopped before anything was erased; the "
                          "module is likely intact: recovering it is still "
                          "offered");
    EXPECT_EQ(start(b), ECUIO_ERR_IN_USE) << "and held as any other";
}

TEST(SessionRecoveryTest, AnEraseMarkedSentGetsNoIntactWords)
{
    Bench b;
    b.journals = true;
    put_journal(b, head_of(), writing(1, 0));
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    const ecuio_module_recovery_t r = recovery_of(b);
    EXPECT_EQ(r.state, ECUIO_RECOVERY_NEEDED);
    EXPECT_EQ(r.flags & ECUIO_RECOVERY_NOTHING_ERASED, 0u);
}

TEST(SessionRecoveryTest, AJournalWithNoEraseCountReadsAsErased)
{
    Bench b;
    b.journals = true;
    std::string t = ecuio_operation::journal_text(head_of(), writing(0, 0));
    const std::string field = ",\n  \"erased\": 0";
    ASSERT_NE(t.find(field), std::string::npos);
    t.erase(t.find(field), field.size());
    put_journal(b, t);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    const ecuio_module_recovery_t r = recovery_of(b);
    EXPECT_EQ(r.state, ECUIO_RECOVERY_NEEDED);
    EXPECT_EQ(r.flags & ECUIO_RECOVERY_NOTHING_ERASED, 0u);
}

TEST(SessionRecoveryTest, AJournalThatDoesntReadHoldsTheModuleInItsWords)
{
    Bench b;
    b.journals = true;
    put_journal(b, R"({"havne_journal": 1, "state": "writing"})");
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    const ecuio_module_recovery_t r = recovery_of(b);
    EXPECT_EQ(r.state, ECUIO_RECOVERY_UNREADABLE);
    constexpr const char* kWords = "the flash journal 000007E8.havnejournal "
                                   "isn't one this Havne reads: the module "
                                   "may be mid-flash";
    EXPECT_STREQ(r.words, kWords);
    EXPECT_EQ(start(b), ECUIO_ERR_IN_USE);
    EXPECT_STREQ(ecuio_last_error_detail(), kWords);
}

TEST(SessionRecoveryTest, AFinishedJournalOrNoFolderHoldsNothing)
{
    for (const journal_state state :
         {journal_state::ready, journal_state::done}) {
        Bench b;
        b.journals = true;
        journal_mark m;
        m.state = state;
        put_journal(b, head_of(), m);
        arm(b);
        ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
        EXPECT_EQ(recovery_of(b).state, ECUIO_RECOVERY_NONE);
        EXPECT_STREQ(recovery_of(b).words, "");
        EXPECT_FALSE(b.sent.empty()) << "the module is asked as ever";
    }
    Bench unnamed;
    put_journal(unnamed, head_of(), writing(2, 1));
    ASSERT_NO_FATAL_FAILURE(unnamed.start(flash_made()));
    EXPECT_EQ(recovery_of(unnamed).state, ECUIO_RECOVERY_NONE)
        << "no folder named: no journal read";
}

TEST(SessionRecoveryTest, TheCallsRefuseWhatTheyDontTake)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    EXPECT_EQ(ecuio_session_journal_folder(b.s, "x"),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "after start";
    EXPECT_EQ(ecuio_session_journal_folder(nullptr, "x"),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_journal_folder(b.s, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_module_recovery_t r{};
    r.struct_size = sizeof r;
    EXPECT_EQ(ecuio_session_get_module_recovery(nullptr, 0, &r),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_module_recovery(b.s, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_module_recovery(b.s, 1, &r),
              ECUIO_ERR_INVALID_ARGUMENT);
    r.struct_size = sizeof r - 1;
    r.state = 77;
    EXPECT_EQ(ecuio_session_get_module_recovery(b.s, 0, &r),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(r.state, 77u) << "nothing written";
}
