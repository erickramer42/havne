// File/layer: tests/test_core_conversation.cpp | test, C++17, Qt-free
// Testing: UNIT: CoreConversationTest (test_core)
//
// The public calls of a bound module (core.h; core_conversation.cpp):
// the report's copy, the store with its key (the module's serial, the
// car's VIN, else the label), and the unlock's refusals, TOO_SOON's two
// causes told apart by the time left alone; and a recording's
// conversation line, read back and kept by the folder export.

#include "conversation_session_support.h"

#include "../core/src/security_store.h"
#include "../core/src/sha256.h"
#include "ecuio/recording.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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

/* A delay the store dates from an earlier session runs on: the first
 * unlock is TOO_SOON with its time, and the module's machine takes the
 * stored count */
TEST(CoreConversationTest, AStoredDelayIsRestoredAtTheFirstUnlock)
{
    DoorOpen open;
    TempStore file("core_conversation_restore.json");
    const long long now = static_cast<long long>(std::time(nullptr));
    if (FILE* f = std::fopen(file.path.c_str(), "wb")) {
        std::fprintf(f,
                     "{\"havne_security_store\": 1, \"entries\": [{\"vehicle\": "
                     "\"%s\", \"rx\": \"7E8\", \"level\": 1, \"failed\": 2, "
                     "\"last_utc\": %lld, \"pending_utc\": 0}]}",
                     hex16(ecuio_security::store_vehicle_of("label:car")).c_str(),
                     now);
        std::fclose(f);
    }
    Bound b;
    Bench bench;
    ASSERT_NO_FATAL_FAILURE(bound_with_store(&b, &bench, {}, file.path, "Car"));
    uint32_t wait = 0;
    EXPECT_EQ(ecuio_session_security_unlock(b.session.get(), 0, 0x01,
                                            generation_of(b.session.get()),
                                            &wait),
              ECUIO_ERR_TOO_SOON);
    EXPECT_GT(wait, 0u);
    EXPECT_LE(wait, 1000u);
    bench.passes(2);
    const ecuio_bound_channel& bound = b.session->talk->bound[0];
    ASSERT_EQ(bound.level_count, 1u);
    EXPECT_EQ(bound.levels[0].failed, 2u) << "the machine took the stored count";
    EXPECT_GT(bound.levels[0].delay_left_us, 0);
    EXPECT_EQ(bench.heard({0x27, 0x01}), 0u);
}

/* Consent is a create-time fact: the door opened after create binds
 * nothing, and the session doesn't ask as one with requests */
TEST(CoreConversationTest, ConsentIsTakenAtCreate)
{
    Bound b;
    ASSERT_NO_FATAL_FAILURE(make_session(&b, made()));
    DoorOpen open;
    EXPECT_FALSE(ecuio_session_may_talk(b.session.get()));
    ASSERT_EQ(ecuio_conversation_bind(b.session.get()), ECUIO_OK);
    EXPECT_EQ(b.session->talk, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "plugin talker's conversation part isn't in use: not enabled");
}

/* After close the unlock is SESSION_CLOSED, and the report keeps its
 * last state; a fault reads in the report by its code */
TEST(CoreConversationTest, AfterCloseTheUnlockIsRefusedAndTheReportStays)
{
    DoorOpen open;
    GuestShape faults;
    faults.begin = 7;
    Bound b;
    Bench bench;
    ASSERT_NO_FATAL_FAILURE(bound_with_store(&b, &bench, faults, "", nullptr));
    ecuio_channel_conversation_t c{};
    ASSERT_EQ(ecuio_session_get_channel_conversation(b.session.get(), 0, &c),
              ECUIO_OK);
    EXPECT_EQ(c.state, ECUIO_CONVERSATION_FAULTED);
    EXPECT_EQ(c.fault, ECUIO_CONVERSATION_FAULT_BEGIN);
    ASSERT_EQ(ecuio_session_close(b.session.get()), ECUIO_OK);
    EXPECT_EQ(ecuio_session_security_unlock(b.session.get(), 0, 0x01,
                                            c.generation, nullptr),
              ECUIO_ERR_SESSION_CLOSED);
    ecuio_channel_conversation_t after{};
    ASSERT_EQ(ecuio_session_get_channel_conversation(b.session.get(), 0,
                                                     &after),
              ECUIO_OK);
    EXPECT_EQ(after.state, ECUIO_CONVERSATION_FAULTED);
    EXPECT_EQ(after.generation, c.generation);
}

/* A recording of a bound session names the part that talked, its
 * module's hash and the channels bound, in channel order; the reader
 * flags it, and the folder export keeps the line */
TEST(CoreConversationTest, ARecordingNamesThePartThatTalked)
{
    namespace fs = std::filesystem;
    DoorOpen open;
    const conversation_test::Made m = made();
    Bound bound;
    make_session(&bound, m);
    std::string ids;
    for (size_t c = 0; c < bound.session->module_rx.size(); ++c) {
        const uint32_t rx = bound.session->module_rx[c];
        if ((rx == 0x7E8 || rx == 0x7E9 || rx == 0x7EA) &&
            bound.session->module_tx[c] == rx - 8) {
            char id[16];
            std::snprintf(id, sizeof id, "%s0x%X", ids.empty() ? "" : " ",
                          rx);
            ids += id;
        }
    }
    const std::string line =
        "conversation  talker,1.2.0," +
        ecuio_sha256::hex_of(m.module.data(), m.module.size()) + "," + ids;
    const fs::path root =
        fs::u8path(testing::TempDir()) / "conversation_recording";
    fs::remove_all(root);
    const std::string root_text = root.u8string();
    ecuio_record_options_t o{};
    o.root_utf8 = root_text.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK)
        << ecuio_last_error_detail();
    char folder[1024] = {};
    uint32_t len = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(rec, folder, sizeof folder, &len),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(bound.session.get(), rec), ECUIO_OK)
        << ecuio_last_error_detail();
    bound.session.reset();
    ASSERT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
    const auto events_of = [](const fs::path& f) {
        std::ifstream in(f / "events.log", std::ios::binary);
        std::stringstream all;
        all << in.rdbuf();
        std::string text = all.str(); /* CRLF on Windows */
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        return text;
    };
    EXPECT_NE(events_of(fs::u8path(folder)).find("\n" + line + "\n"),
              std::string::npos)
        << line << "\n"
        << events_of(fs::u8path(folder));
    ecuio_recording_t* r = nullptr;
    ASSERT_EQ(ecuio_recording_open(folder, nullptr, nullptr, &r), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_recording_info_t info{};
    EXPECT_EQ(ecuio_recording_get_info(r, &info), ECUIO_OK);
    EXPECT_NE(info.flags & ECUIO_RECORDING_INFO_CONVERSATION, 0u);
    ecuio_recording_close(r);
    const std::string copy = (root / "copy").u8string();
    ecuio_export_options_t x{};
    x.format = ECUIO_EXPORT_FOLDER;
    x.folder_utf8 = folder;
    x.out_path_utf8 = copy.c_str();
    ASSERT_EQ(ecuio_export(&x, nullptr), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_NE(events_of(fs::u8path(copy)).find("\n" + line + "\n"),
              std::string::npos);
    fs::remove_all(root);
}

/* A session whose part talks on none of its channels records no line */
TEST(CoreConversationTest, ARecordingOfAnUnboundSessionHasNoConversationLine)
{
    namespace fs = std::filesystem;
    DoorOpen open;
    Bound bound;
    make_session(&bound, made(), "mock://x");
    const fs::path root =
        fs::u8path(testing::TempDir()) / "conversation_unbound";
    fs::remove_all(root);
    const std::string root_text = root.u8string();
    ecuio_record_options_t o{};
    o.root_utf8 = root_text.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
    char folder[1024] = {};
    uint32_t len = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(rec, folder, sizeof folder, &len),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(bound.session.get(), rec), ECUIO_OK);
    bound.session.reset();
    ASSERT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
    ecuio_recording_t* r = nullptr;
    ASSERT_EQ(ecuio_recording_open(folder, nullptr, nullptr, &r), ECUIO_OK);
    ecuio_recording_info_t info{};
    EXPECT_EQ(ecuio_recording_get_info(r, &info), ECUIO_OK);
    EXPECT_EQ(info.flags & ECUIO_RECORDING_INFO_CONVERSATION, 0u);
    ecuio_recording_close(r);
    fs::remove_all(root);
}

namespace {

/* frames.csv and events.log of a recording of b's session, which is
 * handed each of frames on 0x7E8 and then destroyed */
void recorded(Bound& b, const char* name,
              const std::vector<std::vector<uint8_t>>& frames,
              std::string* rows, std::string* events)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::u8path(testing::TempDir()) / name;
    fs::remove_all(root);
    const std::string root_text = root.u8string();
    ecuio_record_options_t o{};
    o.root_utf8 = root_text.c_str();
    ecuio_recorder_t* rec = nullptr;
    ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
    char folder[1024] = {};
    uint32_t len = 0;
    ASSERT_EQ(ecuio_recorder_get_folder(rec, folder, sizeof folder, &len),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_record(b.session.get(), rec), ECUIO_OK);
    auto f = std::make_unique<ecuio_frame_t>();
    for (const std::vector<uint8_t>& bytes : frames) {
        *f = ecuio_frame_t{};
        f->arbitration_id = 0x7E8;
        f->flags = ECUIO_FRAME_FLAG_REASSEMBLED;
        f->data_length = static_cast<uint16_t>(bytes.size());
        std::memcpy(f->data, bytes.data(), bytes.size());
        ecuio_session_dispatch_callbacks(b.session.get(), f.get());
    }
    b.session.reset();
    ASSERT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
    const auto text_of_file = [&](const char* file) {
        std::ifstream in(fs::u8path(folder) / file, std::ios::binary);
        std::stringstream all;
        all << in.rdbuf();
        std::string t = all.str(); /* CRLF on Windows */
        t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
        return t;
    };
    *rows = text_of_file("frames.csv");
    *events = text_of_file("events.log");
    fs::remove_all(root);
}

} // namespace

/* On a bound channel a seed's bytes past the sub-function are zeroes in
 * frames.csv, the frame flagged, and events.log says their length and
 * SHA-256; any other reply, and the same seed on a session whose part
 * talks on no channel, is recorded whole */
TEST(CoreConversationTest, ASecurityAccessIsCutFromTheRecording)
{
    DoorOpen open;
    const std::vector<std::vector<uint8_t>> frames = {
        {0x67, 0x01, 0x11, 0x22, 0x33, 0x44}, {0x62, 0xF4, 0x0D, 0x5A}};
    std::string rows;
    std::string events;
    {
        Bound bound;
        make_session(&bound, made());
        ASSERT_NO_FATAL_FAILURE(
            recorded(bound, "conversation_cut", frames, &rows, &events));
    }
    EXPECT_NE(rows.find(",0x7E8,0x500,67 01 00 00 00 00\n"), std::string::npos)
        << rows;
    EXPECT_EQ(rows.find("11 22 33 44"), std::string::npos) << rows;
    EXPECT_NE(rows.find(",0x7E8,0x100,62 F4 0D 5A\n"), std::string::npos);
    EXPECT_NE(events.find("  cut  0x7E8 67 01: 4 bytes cut, SHA-256 "
                          "1a835ed8734f86355ca5b835d824d486993aabf1913cd3a011b"
                          "7446c0514b7c9\n"),
              std::string::npos)
        << events;
    {
        Bound bound;
        make_session(&bound, made(), "mock://x");
        ASSERT_NO_FATAL_FAILURE(
            recorded(bound, "conversation_uncut", frames, &rows, &events));
    }
    EXPECT_NE(rows.find(",0x7E8,0x100,67 01 11 22 33 44\n"), std::string::npos)
        << rows;
    EXPECT_EQ(events.find("  cut  "), std::string::npos);
}

/* A module's memory stays out of a recording as a seed does: a read's
 * answer is zeroes past its service, a transfer's past its block counter,
 * each with its cut line; a refusal and a request are recorded whole */
TEST(CoreConversationTest, AMemoryReadsAnswerIsCutFromTheRecording)
{
    DoorOpen open;
    const std::vector<std::vector<uint8_t>> frames = {
        {0x63, 0x11, 0x22, 0x33, 0x44},
        {0x76, 0x01, 0x11, 0x22, 0x33, 0x44},
        {0x7F, 0x23, 0x31},
        {0x75, 0x20, 0x01, 0x02}};
    std::string rows;
    std::string events;
    {
        Bound bound;
        make_session(&bound, made());
        ASSERT_NO_FATAL_FAILURE(
            recorded(bound, "memory_cut", frames, &rows, &events));
    }
    EXPECT_NE(rows.find(",0x7E8,0x500,63 00 00 00 00\n"), std::string::npos)
        << rows;
    EXPECT_NE(rows.find(",0x7E8,0x500,76 01 00 00 00 00\n"), std::string::npos)
        << rows;
    EXPECT_EQ(rows.find("11 22 33 44"), std::string::npos) << rows;
    EXPECT_NE(rows.find(",0x7E8,0x100,7F 23 31\n"), std::string::npos);
    EXPECT_NE(rows.find(",0x7E8,0x100,75 20 01 02\n"), std::string::npos);
    const std::string sum =
        ": 4 bytes cut, SHA-256 "
        "1a835ed8734f86355ca5b835d824d486993aabf1913cd3a011b7446c0514b7c9\n";
    EXPECT_NE(events.find("  cut  0x7E8 63" + sum), std::string::npos)
        << events;
    EXPECT_NE(events.find("  cut  0x7E8 76 01" + sum), std::string::npos)
        << events;
    {
        Bound bound;
        make_session(&bound, made(), "mock://x");
        ASSERT_NO_FATAL_FAILURE(
            recorded(bound, "memory_uncut", frames, &rows, &events));
    }
    EXPECT_NE(rows.find(",0x7E8,0x100,63 11 22 33 44\n"), std::string::npos)
        << rows;
    EXPECT_EQ(events.find("  cut  "), std::string::npos);
}
