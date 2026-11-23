// File/layer: tests/test_core_conversation.cpp | test, C++17, Qt-free
// Testing: UNIT: CoreConversationTest (test_core)
//
// The public calls of a bound module (core.h; core_conversation.cpp):
// the report's copy, the store with its key (the module's serial, the
// car's VIN, else the label), and the unlock's refusals, TOO_SOON's two
// causes told apart by the time left alone.

#include "conversation_session_support.h"

#include "../core/src/security_store.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

using conversation_session::Bench;
using conversation_session::Bound;
using conversation_session::DoorOpen;
using conversation_session::GuestShape;
using conversation_session::made;
using conversation_session::make_session;

namespace {

struct TempStore {
    std::string path;
    explicit TempStore(const char* name)
        : path(testing::TempDir() + std::string(name))
    {
        std::remove(path.c_str());
    }
    ~TempStore() { std::remove(path.c_str()); }
};

std::string text_of(const std::string& path)
{
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char b[4096];
        size_t n = 0;
        while ((n = std::fread(b, 1, sizeof b, f)) > 0) {
            out.append(b, n);
        }
        std::fclose(f);
    }
    return out;
}

std::string hex16(uint64_t v)
{
    char b[17];
    std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(v));
    return b;
}

/* Channel 0's identity report holding one item */
void identity_item(ecuio_session_t* s, const uint8_t (&request)[3],
                   uint8_t length, uint8_t outcome, const char* text)
{
    ecuio_channel_identity_t& id = s->identity[0];
    ecuio_identity_item_t& it = id.list[id.items++];
    std::memcpy(it.request, request, 3);
    it.length = length;
    it.outcome = outcome;
    std::snprintf(it.text, sizeof it.text, "%s", text);
    it.text_length = static_cast<uint8_t>(std::strlen(it.text));
}

const uint8_t kSerial[3] = {0x22, 0xF1, 0x8C};
const uint8_t kVin[3] = {0x09, 0x02, 0x00};

/* A bound session, its bench past begin, with a store at path and label */
void bound_with_store(Bound* b, Bench* bench, const GuestShape& shape,
                      const std::string& path, const char* label)
{
    ASSERT_NO_FATAL_FAILURE(make_session(b, made(shape)));
    if (!path.empty()) {
        ASSERT_EQ(
            ecuio_session_security_store(b->session.get(), path.c_str(), label),
            ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ASSERT_EQ(ecuio_conversation_bind(b->session.get()), ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_NO_FATAL_FAILURE(bench->start(b->session.get()));
    bench->passes(8);
}

uint32_t generation_of(ecuio_session_t* s)
{
    ecuio_channel_conversation_t c{};
    EXPECT_EQ(ecuio_session_get_channel_conversation(s, 0, &c), ECUIO_OK);
    return c.generation;
}

} // namespace

/* Nothing bound: the report is NONE with its rx, the unlock refused in
 * words, a channel past the modules INVALID_ARGUMENT */
TEST(CoreConversationTest, ASessionWithNothingBoundAnswersNone)
{
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made()));
    ecuio_channel_conversation_t c{};
    c.reserved[0] = 7;
    ASSERT_EQ(ecuio_session_get_channel_conversation(b.session.get(), 0, &c),
              ECUIO_OK);
    EXPECT_EQ(c.state, ECUIO_CONVERSATION_NONE);
    EXPECT_EQ(c.rx_id, 0x7E8u);
    EXPECT_EQ(c.reserved[0], 0u);
    EXPECT_EQ(ecuio_session_get_channel_conversation(b.session.get(), 99, &c),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_channel_conversation(nullptr, 0, &c),
              ECUIO_ERR_NULL_ARGUMENT);
    uint32_t wait = 5;
    EXPECT_EQ(ecuio_session_security_unlock(b.session.get(), 0, 0x01, 0, &wait),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "no plugin's code talks on this channel");
    EXPECT_EQ(wait, 0u);
    ecuio_channel_support_t sup{};
    ASSERT_EQ(ecuio_session_get_channel_support(b.session.get(), 0, &sup),
              ECUIO_OK);
    EXPECT_EQ(sup.flags & ECUIO_SUPPORT_CONVERSATION, 0u);
}

/* The unlock through the public call: the report says unlocked at its
 * level, the support report carries the flag, NOT_KEPT with no store */
TEST(CoreConversationTest, TheUnlockCallUnlocksAndTheReportSaysSo)
{
    DoorOpen open;
    Bound b;
    Bench bench;
    ASSERT_NO_FATAL_FAILURE(bound_with_store(&b, &bench, {}, "", nullptr));
    ecuio_channel_conversation_t c{};
    ASSERT_EQ(ecuio_session_get_channel_conversation(b.session.get(), 0, &c),
              ECUIO_OK);
    EXPECT_EQ(c.state, ECUIO_CONVERSATION_TALKING);
    EXPECT_EQ(c.session, 3);
    EXPECT_EQ(c.flags, ECUIO_CONVERSATION_NOT_KEPT);
    ecuio_channel_support_t sup{};
    ASSERT_EQ(ecuio_session_get_channel_support(b.session.get(), 0, &sup),
              ECUIO_OK);
    EXPECT_NE(sup.flags & ECUIO_SUPPORT_CONVERSATION, 0u);

    EXPECT_EQ(ecuio_session_security_unlock(b.session.get(), 0, 0x01,
                                            c.generation + 1, nullptr),
              ECUIO_ERR_REPORT_CHANGED);
    EXPECT_EQ(ecuio_session_security_unlock(b.session.get(), 0, 0x03,
                                            c.generation, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the plugin declares no security level 03 for this module");
    ASSERT_EQ(ecuio_session_security_unlock(b.session.get(), 0, 0x01,
                                            c.generation, nullptr),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_session_security_unlock(b.session.get(), 0, 0x01,
                                            c.generation, nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "an unlock is under way on this module");
    bench.until_word_ended(c.words_ended);
    ASSERT_EQ(ecuio_session_get_channel_conversation(b.session.get(), 0, &c),
              ECUIO_OK);
    EXPECT_EQ(c.unlock, ECUIO_UNLOCK_UNLOCKED);
    EXPECT_EQ(c.level, 0x01);
    EXPECT_EQ(c.failed, 0u);
    EXPECT_EQ(c.delay_left_ms, 0u);
}

/* A key past the adapter's one request (ELM327's 7 bytes: 2 + key) is
 * refused up front, nothing sent */
TEST(CoreConversationTest, AKeyPastTheAdaptersRequestIsRefusedUpFront)
{
    DoorOpen open;
    Bound b;
    Bench bench;
    ASSERT_NO_FATAL_FAILURE(bound_with_store(&b, &bench, {}, "", nullptr));
    ecuio_session_talk& t = *b.session->talk;
    t.bound[0].talk_bound.store(5); /* 2 + 4 bytes don't fit */
    EXPECT_EQ(ecuio_session_security_unlock(b.session.get(), 0, 0x01,
                                            generation_of(b.session.get()),
                                            nullptr),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "level 01's key of 4 bytes doesn't fit one request on this "
                 "adapter (5 bytes)");
    bench.passes(6);
    EXPECT_EQ(bench.heard({0x27, 0x01}), 0u);
}

/* TOO_SOON's two causes by the time left alone: the identity still being
 * read is 0, the delay after a refused key above 0 */
TEST(CoreConversationTest, TooSoonTellsTheIdentityFromTheDelayByItsTime)
{
    DoorOpen open;
    TempStore file("core_conversation_too_soon.json");
    GuestShape wrong;
    wrong.flip = 0x01;
    Bound b;
    Bench bench;
    ASSERT_NO_FATAL_FAILURE(
        bound_with_store(&b, &bench, wrong, file.path, "  My Car "));
    identity_item(b.session.get(), kSerial, 3, ECUIO_IDENTITY_UNASKED, "");
    uint32_t wait = 9;
    EXPECT_EQ(ecuio_session_security_unlock(b.session.get(), 0, 0x01,
                                            generation_of(b.session.get()),
                                            &wait),
              ECUIO_ERR_TOO_SOON);
    EXPECT_EQ(wait, 0u);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the module's identity is still being read: ask again "
                 "shortly");

    b.session->identity[0].list[0].outcome = ECUIO_IDENTITY_REFUSED;
    const uint32_t before = generation_of(b.session.get());
    ecuio_channel_conversation_t c{};
    ASSERT_EQ(ecuio_session_get_channel_conversation(b.session.get(), 0, &c),
              ECUIO_OK);
    ASSERT_EQ(
        ecuio_session_security_unlock(b.session.get(), 0, 0x01, before, &wait),
        ECUIO_OK)
        << ecuio_last_error_detail();
    bench.until_word_ended(c.words_ended);
    ASSERT_EQ(ecuio_session_get_channel_conversation(b.session.get(), 0, &c),
              ECUIO_OK);
    ASSERT_EQ(c.unlock, ECUIO_UNLOCK_FAILED);
    EXPECT_EQ(c.failed, 1u);
    EXPECT_GT(c.delay_left_ms, 0u);
    EXPECT_EQ(ecuio_session_security_unlock(b.session.get(), 0, 0x01,
                                            c.generation, &wait),
              ECUIO_ERR_TOO_SOON);
    EXPECT_GT(wait, 0u);
    EXPECT_LE(wait, 1000u);
    EXPECT_NE(std::string(ecuio_last_error_detail())
                  .find("the module's delay after a refused key runs"),
              std::string::npos);
    /* the label, trimmed and folded, named the module */
    EXPECT_NE(text_of(file.path).find(
                  hex16(ecuio_security::store_vehicle_of("label:my car"))),
              std::string::npos)
        << text_of(file.path);
}

/* The store's key is the module's serial where it answers one, else the
 * car's VIN; with neither and no label the unlock is refused in words */
TEST(CoreConversationTest, TheStoresKeyIsTheSerialThenTheVinThenTheLabel)
{
    DoorOpen open;
    struct Row {
        const char* serial; /* NULL: refused */
        const char* vin;    /* NULL: none */
        const char* label;
        const char* identity; /* NULL: refused, no name */
    };
    const Row rows[] = {
        {"SN0042", "1FAFAKE10M0000001", "car", "serial:SN0042"},
        {nullptr, "1FAFAKE10M0000001", "car", "vin:1FAFAKE10M0000001"},
        {nullptr, nullptr, "Car", "label:car"},
        {nullptr, nullptr, "", nullptr},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.identity == nullptr ? "none" : row.identity);
        TempStore file("core_conversation_key.json");
        Bound b;
        Bench bench;
        ASSERT_NO_FATAL_FAILURE(
            bound_with_store(&b, &bench, {}, file.path, row.label));
        identity_item(b.session.get(), kSerial, 3,
                      row.serial ? ECUIO_IDENTITY_ANSWERED
                                 : ECUIO_IDENTITY_REFUSED,
                      row.serial ? row.serial : "");
        if (row.vin != nullptr) {
            identity_item(b.session.get(), kVin, 2, ECUIO_IDENTITY_ANSWERED,
                          row.vin);
        }
        const ecuio_status_t rc = ecuio_session_security_unlock(
            b.session.get(), 0, 0x01, generation_of(b.session.get()), nullptr);
        if (row.identity == nullptr) {
            EXPECT_EQ(rc, ECUIO_ERR_INVALID_ARGUMENT);
            EXPECT_STREQ(ecuio_last_error_detail(),
                         "the security store names a module by its serial, "
                         "the car's VIN or a label: this module gave no "
                         "serial, the car no VIN, and no label was given");
            continue;
        }
        ASSERT_EQ(rc, ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_NE(text_of(file.path).find(
                      hex16(ecuio_security::store_vehicle_of(row.identity))),
                  std::string::npos)
            << text_of(file.path);
    }
}

/* The store call: before start only, a file that doesn't read refused
 * in words naming no path */
TEST(CoreConversationTest, TheStoreIsNamedBeforeStartAndReadThere)
{
    TempStore file("core_conversation_bad.json");
    if (FILE* f = std::fopen(file.path.c_str(), "wb")) {
        std::fputs("{not a store", f);
        std::fclose(f);
    }
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made()));
    EXPECT_EQ(ecuio_session_security_store(b.session.get(), nullptr, ""),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(
        ecuio_session_security_store(b.session.get(), file.path.c_str(), ""),
        ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(std::string(ecuio_last_error_detail()).find(file.path),
              std::string::npos);
    EXPECT_STRNE(ecuio_last_error_detail(), "");
    b.session->started = true;
    std::remove(file.path.c_str());
    EXPECT_EQ(
        ecuio_session_security_store(b.session.get(), file.path.c_str(), ""),
        ECUIO_ERR_INVALID_ARGUMENT);
    b.session->started = false;
}
