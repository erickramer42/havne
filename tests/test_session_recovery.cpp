#include "session_flash_support.h"

#include "../core/src/operation_journal.h"
#include "../core/src/security_store.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

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

namespace {

std::string read_journal(const Bench& b)
{
    std::ifstream in(ecuio_operation::journal_path(b.folder.str(), 0x7E8),
                     std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

/* The journal a flash on the bench left, as the next connect reads it */
ecuio_operation::journal_record
record_after(const ecuio_sim_flash_faults& faults, const Flash& f = sim_flash())
{
    Bench b;
    arm(b, faults);
    b.start(flash_made(f));
    EXPECT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_RECOVERY_NEEDED) << v.words;
    ecuio_operation::journal_record j;
    std::string why;
    EXPECT_EQ(ecuio_operation::journal_load(b.folder.str(), 0x7E8, &j, &why),
              ecuio_operation::journal_found::unfinished)
        << why;
    return j;
}

ecuio_operation::journal_record silent_record(const Flash& f = sim_flash())
{
    ecuio_sim_flash_faults silent;
    silent.silent = true;
    return record_after(silent, f);
}

/* A bench that connects to a module left part-written, its journal put
 * and the sector at erased left erased. A record is made before the bench:
 * a bench's end closes the consent door every bench shares. */
void reconnect(Bench& b, const ecuio_operation::journal_record& j,
               uint32_t erased, ecuio_sim_flash_faults faults = {},
               const Flash& f = sim_flash(), const char* store = nullptr)
{
    b.journals = true;
    put_journal(b, j.head, j.mark);
    faults.boot = true;
    faults.boot_erased = erased;
    arm(b, faults);
    b.start(flash_made(f), ECUIO_ENGINE_PDU_MAX, store);
}

ecuio_status_t recover(Bench& b, const char* copy = nullptr)
{
    const ecuio_operation_request_t q =
        request(kFlags | ECUIO_OPERATION_RECOVER);
    return ecuio_session_operation_start_from(b.s, &q, b.folder.str().c_str(),
                                              copy);
}

size_t keys_sent(const Bench& b)
{
    size_t n = 0;
    for (const Bytes& q : b.sent) {
        n += q.size() >= 2 && q[0] == 0x27 && (q[1] & 0x01) == 0 ? 1u : 0u;
    }
    return n;
}

} // namespace

TEST(SessionRecoveryTest, ARecoveryWritesEveryJournalSectorAgain)
{
    ecuio_operation::journal_record j = silent_record();
    j.mark.written = 1;
    j.mark.erased = 2;
    Bench b;
    ASSERT_NO_FATAL_FAILURE(reconnect(b, j, 1));
    ASSERT_EQ(recovery_of(b).state, ECUIO_RECOVERY_NEEDED);
    ASSERT_EQ(recover(b), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(recovery_of(b).attempts, 1u);
    EXPECT_NE(read_journal(b).find("\"recoveries\": 1"), std::string::npos)
        << "counted before anything is sent";
    EXPECT_NE(read_journal(b).find("\"state\": \"writing\""),
              std::string::npos);
    EXPECT_EQ(b.state().sectors, 2u);
    const ecuio_operation_state_t v = b.until_ended(20000);
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_RECOVERED) << v.words;
    EXPECT_STREQ(v.words, "");
    EXPECT_EQ(count_routine(b, 0xFF00), 2u) << "the erase included, each";
    EXPECT_EQ(b.region(kCode, 2 * kSector), image_two_sectors());
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::none);
    const ecuio_module_recovery_t r = recovery_of(b);
    EXPECT_EQ(r.state, ECUIO_RECOVERY_RECOVERED);
    EXPECT_EQ(r.sectors_written, 2u);
    EXPECT_NE(ecuio_session_clear_diagnostics(b.s, 0, 0), ECUIO_ERR_IN_USE)
        << "the hold is lifted";
}

TEST(SessionRecoveryTest, AnOverwriteSafePlanSkipsTheSectorsWritten)
{
    Flash f = sim_flash();
    f.flash_extra = R"(, "overwrite_safe": true)";
    ecuio_operation::journal_record j = silent_record(f);
    j.mark.written = 1;
    j.mark.erased = 2;
    Bench b;
    ASSERT_NO_FATAL_FAILURE(reconnect(b, j, 1, {}, f));
    ASSERT_EQ(recover(b), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(b.state().sectors, 1u);
    const ecuio_operation_state_t v = b.until_ended(20000);
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_RECOVERED) << v.words;
    EXPECT_EQ(count_routine(b, 0xFF00), 1u) << "the second sector alone";
    EXPECT_EQ(b.region(kCode, 2 * kSector), image_two_sectors());
}

TEST(SessionRecoveryTest, TheJournalsPinsMustBeTheSessions)
{
    const ecuio_operation::journal_record base = silent_record();
    struct Case {
        const char* words;
        void (*change)(ecuio_operation::journal_head&);
    };
    const Case cases[] = {
        {"the flash was begun by another plugin: recover the module with "
         "the plugin that began it",
         [](ecuio_operation::journal_head& h) {
             h.fingerprint = std::string(64, 'c');
         }},
        {"the flash was begun by another plan: recover the module with the "
         "plan that began it",
         [](ecuio_operation::journal_head& h) { h.operation = "other"; }},
        {"the flash was begun with other images: recover the module with "
         "the images it began with",
         [](ecuio_operation::journal_head& h) {
             h.images[0] = std::string(64, 'd');
         }},
        {"the flash was begun with other images: recover the module with "
         "the images it began with",
         [](ecuio_operation::journal_head& h) {
             h.images.push_back(h.images[0]);
         }},
        {"the journal's sectors aren't this plan's",
         [](ecuio_operation::journal_head& h) {
             h.sectors[0].address = 0x00010000;
         }},
        {"the journal's sectors aren't this plan's",
         [](ecuio_operation::journal_head& h) { h.sectors[0].bytes = 8192; }},
        {"the journal's sectors aren't this plan's",
         [](ecuio_operation::journal_head& h) { h.sectors[0].address += 16; }},
        {"the journal's sectors aren't this plan's",
         [](ecuio_operation::journal_head& h) { h.sectors[1] = h.sectors[0]; }},
        {"the journal's sectors aren't this plan's",
         [](ecuio_operation::journal_head& h) {
             std::swap(h.sectors[0], h.sectors[1]);
         }},
    };
    for (const Case& c : cases) {
        ecuio_operation::journal_record j = base;
        c.change(j.head);
        Bench b;
        ASSERT_NO_FATAL_FAILURE(reconnect(b, j, 0));
        EXPECT_EQ(recover(b), ECUIO_ERR_INVALID_ARGUMENT) << c.words;
        EXPECT_STREQ(ecuio_last_error_detail(), c.words);
        EXPECT_EQ(recovery_of(b).attempts, 0u) << "nothing counted";
        EXPECT_EQ(read_journal(b).find("recoveries"), std::string::npos);
    }
}

TEST(SessionRecoveryTest, ARecoveryIsAFlashsWithNoCopyAndAJournal)
{
    const ecuio_operation::journal_record j = silent_record();
    {
        Bench none;
        arm(none);
        ASSERT_NO_FATAL_FAILURE(none.start(flash_made()));
        EXPECT_EQ(recover(none), ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_STREQ(ecuio_last_error_detail(),
                     "the folder holds no unfinished flash of this module: "
                     "there is nothing to recover");
    }
    Bench b;
    ASSERT_NO_FATAL_FAILURE(reconnect(b, j, 0));
    EXPECT_EQ(recover(b, "copy.havnedump"), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "a recovery is a flash's, with no copy");
    EXPECT_EQ(start(b), ECUIO_ERR_IN_USE) << "no flash but a recovery";
}

TEST(SessionRecoveryTest,
     ALockedBootloaderEndsTheAttemptWithNoKeyAndKeepsTheCount)
{
    const ecuio_operation::journal_record j = silent_record();
    ecuio_sim_flash_faults locked;
    locked.locked = true;
    Bench b;
    ASSERT_NO_FATAL_FAILURE(reconnect(b, j, 0, locked));
    ASSERT_EQ(recover(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_UNLOCK_REFUSED) << v.words;
    EXPECT_EQ(v.nrc, 0x36);
    EXPECT_EQ(b.count(0x27), 1u) << "one seed asked, never again";
    EXPECT_EQ(keys_sent(b), 0u);
    EXPECT_EQ(count_routine(b, 0xFF00), 0u);
    EXPECT_NE(std::string(v.words).find("still part-written"),
              std::string::npos)
        << v.words;
    const ecuio_module_recovery_t r = recovery_of(b);
    EXPECT_EQ(r.state, ECUIO_RECOVERY_NEEDED);
    EXPECT_EQ(r.attempts, 1u);
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::unfinished);
    EXPECT_EQ(start(b), ECUIO_ERR_IN_USE) << "still held";
    EXPECT_EQ(recover(b), ECUIO_ERR_TOO_SOON) << "the lockout's delay runs";
    EXPECT_EQ(recovery_of(b).attempts, 1u) << "a refused start counts none";
    EXPECT_NE(read_journal(b).find("\"recoveries\": 1,"), std::string::npos)
        << read_journal(b);

    ecuio_operation::journal_record again;
    std::string why;
    ASSERT_EQ(
        ecuio_operation::journal_load(b.folder.str(), 0x7E8, &again, &why),
        ecuio_operation::journal_found::unfinished);
    Bench next;
    ASSERT_NO_FATAL_FAILURE(reconnect(next, again, 0));
    EXPECT_EQ(recovery_of(next).attempts, 1u) << "kept across the reconnect";
    ASSERT_EQ(recover(next), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(recovery_of(next).attempts, 2u);
    EXPECT_EQ(next.until_ended(20000).end, ECUIO_OPERATION_END_RECOVERED);
}

TEST(SessionRecoveryTest, AStormBeforeTheFloorEndsCleanlyAndKeepsTheHold)
{
    const ecuio_operation::journal_record j = silent_record();
    ecuio_sim_flash_faults storm;
    storm.pending_ms = ECUIO_SIM_FLASH_PENDING_MOST_MS;
    Bench b;
    ASSERT_NO_FATAL_FAILURE(reconnect(b, j, 0, storm));
    ASSERT_EQ(recover(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    EXPECT_NE(v.end, ECUIO_OPERATION_END_RECOVERED);
    EXPECT_NE(v.end, ECUIO_OPERATION_END_RECOVERY_NEEDED) << v.words;
    EXPECT_EQ(count_routine(b, 0xFF00), 0u);
    EXPECT_NE(std::string(v.words).find("still part-written"),
              std::string::npos)
        << v.words;
    EXPECT_EQ(recovery_of(b).state, ECUIO_RECOVERY_NEEDED);
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::unfinished);
    EXPECT_EQ(start(b), ECUIO_ERR_IN_USE);
}

TEST(SessionRecoveryTest, ARecoveryLostPastTheFloorNeedsAnother)
{
    const ecuio_operation::journal_record j = silent_record();
    ecuio_sim_flash_faults silent;
    silent.silent = true;
    Bench b;
    ASSERT_NO_FATAL_FAILURE(reconnect(b, j, 0, silent));
    ASSERT_EQ(recover(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_RECOVERY_NEEDED) << v.words;
    EXPECT_EQ(recovery_of(b).state, ECUIO_RECOVERY_NEEDED);
    EXPECT_EQ(recovery_of(b).attempts, 1u);
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::unfinished);
    EXPECT_EQ(start(b), ECUIO_ERR_IN_USE);
}

TEST(SessionRecoveryTest, AFlashFailedInTheSessionIsRecoveredInIt)
{
    Bench b;
    ecuio_sim_flash_faults silent;
    silent.silent = true;
    arm(b, silent);
    ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
    ASSERT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(b.until_ended(20000).end, ECUIO_OPERATION_END_RECOVERY_NEEDED);
    const ecuio_module_recovery_t r = recovery_of(b);
    EXPECT_EQ(r.state, ECUIO_RECOVERY_NEEDED);
    EXPECT_EQ(r.sectors, 2u);
    EXPECT_EQ(r.attempts, 0u);
    b.node.s.flash.faults.silent = false;
    b.node.s.flash.dead = false;
    ASSERT_EQ(recover(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_RECOVERED) << v.words;
    EXPECT_EQ(b.region(kCode, 2 * kSector), image_two_sectors());
    EXPECT_EQ(recovery_of(b).state, ECUIO_RECOVERY_RECOVERED);
}

TEST(SessionRecoveryTest, ACloseBeforeTheRecoveryWritesKeepsTheJournal)
{
    const ecuio_operation::journal_record j = silent_record();
    Bench b;
    ASSERT_NO_FATAL_FAILURE(reconnect(b, j, 0));
    ASSERT_EQ(recover(b), ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_close(b.s), ECUIO_OK);
    const ecuio_operation_state_t v = b.state();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_CANCELED);
    EXPECT_STREQ(v.words, "the session closed before the recovery began "
                          "writing: the module is still part-written");
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::unfinished);
}

TEST(SessionRecoveryTest, AHostKilledAtEachStepIsRecoveredFromTheJournalItLeft)
{
    struct Left {
        std::string at;
        std::string journal;
        bool held;
        uint32_t erased; /* the sim's sector left erased */
    };
    std::vector<Left> left;
    {
        Bench b;
        arm(b);
        ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
        uint32_t erases = 0;
        b.script = [&](const Bytes& q, size_t, Answer*) {
            if (q.size() > 4 && q[0] == 0x31 && q[2] == 0xFF && q[3] == 0x00) {
                left.push_back({"erase " + std::to_string(erases),
                                read_journal(b), true, erases});
                ++erases;
            } else if (q.size() == 4 && q[0] == 0x31 && q[2] == 0xFF &&
                       q[3] == 0x01) {
                left.push_back(
                    {"check", read_journal(b), true, ECUIO_SIM_FLASH_SECTORS});
            }
            return false;
        };
        ASSERT_EQ(start(b), ECUIO_OK) << ecuio_last_error_detail();
        left.insert(left.begin(), {"opened", read_journal(b), false, 0});
        ASSERT_EQ(b.until_ended(20000).end, ECUIO_OPERATION_END_WRITTEN);
        left.push_back({"done", read_journal(b), false, 0});
    }
    ASSERT_EQ(left.size(), 5u);
    for (const Left& l : left) {
        SCOPED_TRACE(l.at);
        Bench b;
        b.journals = true;
        put_journal(b, l.journal);
        ecuio_sim_flash_faults f;
        f.boot = l.held;
        f.boot_erased = l.erased;
        arm(b, f);
        ASSERT_NO_FATAL_FAILURE(b.start(flash_made()));
        const ecuio_module_recovery_t r = recovery_of(b);
        if (!l.held) {
            EXPECT_EQ(r.state, ECUIO_RECOVERY_NONE);
            continue;
        }
        EXPECT_EQ(r.state, ECUIO_RECOVERY_NEEDED) << "a blank module is held";
        EXPECT_EQ(r.flags & ECUIO_RECOVERY_NOTHING_ERASED, 0u);
        ASSERT_EQ(recover(b), ECUIO_OK) << ecuio_last_error_detail();
        const ecuio_operation_state_t v = b.until_ended(20000);
        EXPECT_EQ(v.end, ECUIO_OPERATION_END_RECOVERED) << v.words;
        EXPECT_EQ(b.region(kCode, 2 * kSector), image_two_sectors());
        EXPECT_EQ(recovery_of(b).state, ECUIO_RECOVERY_RECOVERED);
    }
}

TEST(SessionRecoveryTest,
     AFlashOrRecoveryInAnotherFolderThanTheJournalsIsRefused)
{
    const ecuio_operation::journal_record j = silent_record();
    Bench b;
    ASSERT_NO_FATAL_FAILURE(reconnect(b, j, 0));
    dump_file_test::Folder other("-other");
    constexpr const char* kWords = "this session keeps its flash journals in "
                                   "another folder: flash and recover in "
                                   "that one";
    const ecuio_operation_request_t q =
        request(kFlags | ECUIO_OPERATION_RECOVER);
    EXPECT_EQ(ecuio_session_operation_start_from(b.s, &q, other.str().c_str(),
                                                 nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(), kWords);
    const ecuio_operation_request_t plain = request(kFlags);
    EXPECT_EQ(ecuio_session_operation_start_from(b.s, &plain,
                                                 other.str().c_str(), nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(), kWords);
    const std::string same = b.folder.str() + "/";
    EXPECT_EQ(
        ecuio_session_operation_start_from(b.s, &q, same.c_str(), nullptr),
        ECUIO_OK)
        << "the same folder, written another way: "
        << ecuio_last_error_detail();
    EXPECT_EQ(b.until_ended(20000).end, ECUIO_OPERATION_END_RECOVERED);
}

TEST(SessionRecoveryTest, AMarkThatCantBeSavedCountsNoAttempt)
{
    const ecuio_operation::journal_record j = silent_record();
    Bench b;
    const std::string store = b.folder.str() + "/security.json";
    ASSERT_NO_FATAL_FAILURE(reconnect(b, j, 0, {}, sim_flash(), store.c_str()));
    const std::string before = read_journal(b);
    ecuio_security::store_test_fail_at = 1;
    EXPECT_EQ(recover(b), ECUIO_ERR_LOAD_FAILED) << ecuio_last_error_detail();
    ecuio_security::store_test_fail_at = 0;
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::unfinished);
    EXPECT_EQ(read_journal(b), before) << "put back as it was";
    EXPECT_EQ(recovery_of(b).attempts, 0u);
    ASSERT_EQ(recover(b), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(recovery_of(b).attempts, 1u);
    EXPECT_EQ(b.until_ended(20000).end, ECUIO_OPERATION_END_RECOVERED);
}

TEST(SessionRecoveryTest, ARecoveryWhoseJournalCantCloseSaysSo)
{
    const ecuio_operation::journal_record j = silent_record();
    Bench b;
    ASSERT_NO_FATAL_FAILURE(reconnect(b, j, 0));
    b.script = [&](const Bytes& q, size_t, Answer*) {
        if (q.size() == 4 && q[0] == 0x31 && q[2] == 0xFF && q[3] == 0x01) {
            ecuio_operation::journal_test_fail_at = 1;
        }
        return false;
    };
    ASSERT_EQ(recover(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended(20000);
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_RECOVERED);
    EXPECT_EQ(std::string(v.words).rfind(
                  "recovered and checked, but the flash journal can't be "
                  "written: ",
                  0),
              0u)
        << v.words;
    EXPECT_EQ(journal_of(b), ecuio_operation::journal_found::unfinished);
}
