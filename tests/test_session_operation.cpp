// File/layer: tests/test_session_operation.cpp | test, C++17, Qt-free
// Testing: UNIT: SessionOperationTest (test_core)
//
// A run of a plugin's operation in a session (ecuio/operation.h;
// core_operation.cpp, request_operation.h): the copy it keeps, what goes
// on the wire while it runs, the start's refusals in their order, the
// unlock it makes or doesn't, and what every other end leaves (nothing).

#include "session_operation_support.h"

#include <ecuio/dump.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using engine_talk_test::Bytes;
using engine_talk_test::kKeep;
using engine_talk_test::kRow;
using session_operation::Answer;
using session_operation::Bench;
using session_operation::kHighAt;
using session_operation::kHighBytes;
using session_operation::kLowAt;
using session_operation::kLowBytes;
using session_operation::made;
using session_operation::PlanShape;
using session_operation::request;
using sim_memory_test::nrc;

const std::vector<std::string> kNoFiles;

/* The run's own services: the checks', the unlock's, an identifier's,
 * the memory's */
bool of_the_run(const Bytes& q)
{
    return q != kRow &&
           (q[0] == 0x01 || q[0] == 0x27 || q[0] == 0x22 || q[0] == 0x23 ||
            q[0] == 0x35 || q[0] == 0x36 || q[0] == 0x37);
}

TEST(SessionOperationTest, ARunKeepsTheCopyItsRecordDescribes)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ecuio_operation_state_t v = b.state();
    EXPECT_EQ(v.generation, 0u);
    EXPECT_EQ(v.phase, ECUIO_OPERATION_PHASE_NONE);
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    v = b.state();
    EXPECT_EQ(v.generation, 1u);
    EXPECT_EQ(v.phase, ECUIO_OPERATION_PHASE_CHECKING);
    EXPECT_EQ(v.bytes_total, 2u * (kLowBytes + kHighBytes));
    v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(v.phase, ECUIO_OPERATION_PHASE_DONE);
    EXPECT_EQ(v.bytes_done, v.bytes_total);
    EXPECT_STREQ(v.words, "");
    ASSERT_EQ(b.files(), std::vector<std::string>{v.name});

    const dump_file_test::Dump copy =
        dump_file_test::open_dump(b.folder.at(v.name));
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(ecuio_dump_verify(copy.get(), nullptr, nullptr, nullptr, 0),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_dump_info_t info{};
    info.struct_size = sizeof info;
    ASSERT_EQ(ecuio_dump_get_info(copy.get(), &info), ECUIO_OK);
    EXPECT_EQ(info.image_bytes, kLowBytes + kHighBytes);
    EXPECT_EQ(info.rx_id, 0x7E8u);
    EXPECT_EQ(info.tx_id, 0x7E0u);
    EXPECT_STREQ(info.plugin_id, "talker");
    EXPECT_STREQ(info.operation, "copy");
    EXPECT_STREQ(info.scheme, "j2534");
    EXPECT_EQ(info.unlocks, 1u);
    EXPECT_EQ(info.passes, 2u);
    Bytes image(kLowBytes + kHighBytes);
    size_t count = 0;
    ASSERT_EQ(
        ecuio_dump_read(copy.get(), 0, image.data(), image.size(), &count),
        ECUIO_OK);
    ASSERT_EQ(count, image.size());
    Bytes want = b.region(kLowAt, kLowBytes);
    const Bytes high = b.region(kHighAt, kHighBytes);
    want.insert(want.end(), high.begin(), high.end());
    EXPECT_EQ(image, want);
    ecuio_dump_ident_t ident{};
    ident.struct_size = sizeof ident;
    ASSERT_EQ(ecuio_dump_get_ident(copy.get(), 1, &ident), ECUIO_OK);
    EXPECT_EQ(ident.did, 0xD221u);
    EXPECT_EQ(ident.role, ECUIO_DUMP_ROLE_SOFTWARE);
    EXPECT_GT(ident.length, 0u);
}

/* The record's seconds are the run's own, from its start */
TEST(SessionOperationTest, TheRecordsSecondsCountFromTheRunsStart)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    /* Readings first, for less than the node's S3: the bench answers
     * the rotation itself, so the node hears none of it */
    for (int i = 0; i < 3; ++i) {
        b.now += 1000000;
        b.pass();
    }
    const int64_t began = b.now;
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    const dump_file_test::Dump copy =
        dump_file_test::open_dump(b.folder.at(v.name));
    ASSERT_NE(copy, nullptr);
    ecuio_dump_info_t info{};
    info.struct_size = sizeof info;
    ASSERT_EQ(ecuio_dump_get_info(copy.get(), &info), ECUIO_OK);
    EXPECT_LE(info.seconds, static_cast<uint32_t>((b.now - began) / 1000000));
}

/* While a run is under way the session sends the run's requests and its
 * keep-alive, nothing else; the readings go on after it */
TEST(SessionOperationTest, ARunHoldsEverythingElseOffTheWire)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    EXPECT_GT(b.count(0x22), 0u) << "the rotation reads before the run";
    const size_t from = b.sent.size();
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    /* From the start to the run's last request (the hold ends there,
     * while the copy is sealed) */
    size_t to = from;
    for (size_t i = from; i < b.sent.size(); ++i) {
        to = of_the_run(b.sent[i]) ? i + 1 : to;
    }
    for (size_t i = from; i < to; ++i) {
        const Bytes& q = b.sent[i];
        EXPECT_TRUE(of_the_run(q) || q == kKeep) << "request " << i;
    }
    /* The order: the engine's and the vehicle's speed, one unlock, the
     * identifiers, each range twice, the identifiers again */
    std::vector<Bytes> run;
    for (size_t i = from; i < to; ++i) {
        if (of_the_run(b.sent[i])) {
            run.push_back(b.sent[i]);
        }
    }
    ASSERT_GT(run.size(), 8u);
    EXPECT_EQ(run[0], (Bytes{0x01, 0x0C}));
    EXPECT_EQ(run[1], (Bytes{0x01, 0x0D}));
    EXPECT_EQ(run[2], (Bytes{0x27, 0x01}));
    EXPECT_EQ(run[3][0], 0x27);
    EXPECT_EQ(run[3][1], 0x02);
    EXPECT_EQ(run[4], sim_memory_test::kHardware);
    EXPECT_EQ(run[5], sim_memory_test::kSoftware);
    EXPECT_EQ(run[6], sim_memory_test::read_of(kLowAt, 256));
    EXPECT_EQ(run[run.size() - 2], sim_memory_test::kHardware);
    EXPECT_EQ(run.back(), sim_memory_test::kSoftware);
    EXPECT_EQ(b.count(0x27, from), 2u) << "one seed and its key";
    EXPECT_EQ(b.count(0x23, from), 2u * (kLowBytes / 256));
    EXPECT_EQ(b.count(0x35, from), 2u) << "the upload opened once a reading";
    EXPECT_EQ(b.count(0x37, from), 2u);
    b.passes(6);
    EXPECT_EQ(b.sent.back(), kRow) << "the rotation goes on after the run";
}

/* Each refusal of the start, in the order the checks run, with nothing
 * left in the folder and nothing sent for it */
TEST(SessionOperationTest, TheStartRefusesInWords)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    ecuio_session_t* s = b.s;
    const std::string folder = b.folder.str();
    const size_t sent = b.sent.size();
    ecuio_operation_request_t q = request(ECUIO_OPERATION_MAY_UNLOCK);
    EXPECT_EQ(ecuio_session_operation_start(nullptr, &q, folder.c_str()),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_operation_start(s, nullptr, folder.c_str()),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_session_operation_start(s, &q, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    const auto refused = [&](ecuio_status_t status, const char* words) {
        EXPECT_EQ(ecuio_session_operation_start(s, &q, folder.c_str()), status)
            << words;
        EXPECT_STREQ(ecuio_last_error_detail(), words);
        EXPECT_EQ(b.files(), kNoFiles) << words;
        EXPECT_EQ(b.state().generation, 0u) << words;
    };
    q.struct_size = sizeof q - 1;
    refused(ECUIO_ERR_INVALID_ARGUMENT,
            "the request's struct_size is smaller than the struct");
    q = request(0x80000000u);
    refused(ECUIO_ERR_INVALID_ARGUMENT,
            "the request sets a flag this Havne doesn't know");
    q = request();
    q.reserved[4] = 1;
    refused(ECUIO_ERR_INVALID_ARGUMENT,
            "the request's reserved words are zero");
    q = request(ECUIO_OPERATION_MAY_UNLOCK);
    s->started = false;
    refused(ECUIO_ERR_INVALID_ARGUMENT, "a run starts on a started session");
    s->started = true;
    q.operation = 1;
    refused(ECUIO_ERR_INVALID_ARGUMENT, "the session's plugin declares 1 "
                                        "operations: there is no operation 1");
    q.operation = 0;
    s->info.flags &= ~ECUIO_SESSION_INFO_REQUESTS; /* mock://, replay:// */
    refused(ECUIO_ERR_INVALID_ARGUMENT, "this session asks nothing: a run "
                                        "needs a transport that sends "
                                        "requests");
    s->info.flags |= ECUIO_SESSION_INFO_REQUESTS;
    s->module_tx[0] = 0x7E1;
    refused(ECUIO_ERR_INVALID_ARGUMENT,
            "the operation's module isn't one this session talks to");
    s->module_tx[0] = 0x7E0;
    /* Bound behind the door and the door shut since: no consent stands */
    ecuio_test_conversation_binding(0);
    refused(ECUIO_ERR_CONSENT_REQUIRED,
            "no consent was given to the plugin whose plan this is");
    s->consent_applied = true;
    q = request(ECUIO_OPERATION_MAY_UNLOCK, false);
    refused(ECUIO_ERR_INVALID_ARGUMENT,
            "confirm the vehicle is parked first: ignition on, not Ready; "
            "engine off by the key, not by start-stop; in P or neutral with "
            "the parking brake on");
    /* With a consent and no door, a start goes past the gates */
    q = request(0);
    refused(ECUIO_ERR_INVALID_ARGUMENT, "unlock level 01 first, or start the "
                                        "run with the unlock allowed");
    q = request(ECUIO_OPERATION_MAY_UNLOCK);
    s->consent_applied = false;
    ecuio_test_conversation_binding(1);
    q.flags = ECUIO_OPERATION_PARKED;
    refused(ECUIO_ERR_INVALID_ARGUMENT, "unlock level 01 first, or start the "
                                        "run with the unlock allowed");
    q.flags = ECUIO_OPERATION_MAY_UNLOCK | ECUIO_OPERATION_PARKED;
    /* A folder that isn't there: no path in the words */
    EXPECT_EQ(ecuio_session_operation_start(s, &q, b.folder.at("none").c_str()),
              ECUIO_ERR_LOAD_FAILED);
    const std::string words = ecuio_last_error_detail();
    EXPECT_EQ(words.rfind("the copy can't be started: ", 0), 0u) << words;
    EXPECT_EQ(words.find(folder), std::string::npos) << "no path";
    EXPECT_EQ(b.state().generation, 0u);
    b.passes(4);
    EXPECT_EQ(b.count(0x27), 0u) << "no refusal asked a seed";
    EXPECT_GE(b.sent.size(), sent);
    /* One run at a time */
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(b.begin(), ECUIO_ERR_IN_USE);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "a run is under way in this session");
    b.passes(3);
    EXPECT_EQ(b.begin(), ECUIO_ERR_IN_USE);
    EXPECT_EQ(b.until_ended().end, ECUIO_OPERATION_END_SAVED);
    /* A closed session */
    EXPECT_EQ(ecuio_session_close(s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_operation_start(s, &q, folder.c_str()),
              ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(b.state().end, ECUIO_OPERATION_END_SAVED)
        << "the last run's end stays readable";

    /* Before the worker has run the module's begin, no code talks yet */
    Bench early;
    ASSERT_NO_FATAL_FAILURE(
        early.start(made(), ECUIO_ENGINE_PDU_MAX, nullptr, false));
    EXPECT_EQ(early.begin(), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the module's code isn't talking on this channel");
    EXPECT_EQ(early.files(), kNoFiles);
    EXPECT_EQ(early.state().generation, 0u);
}

/* A request the adapter can't send whole is refused before any seed */
TEST(SessionOperationTest, ARequestTooLongForTheAdapterIsRefusedBeforeASeed)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start(made(), ECUIO_ENGINE_PDU_SINGLE));
    EXPECT_EQ(b.begin(), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "step \"low\" is asked in 8 bytes: it doesn't fit one "
                 "request on this adapter (7 bytes)");
    b.passes(6);
    EXPECT_EQ(b.count(0x27), 0u);
    EXPECT_EQ(b.files(), kNoFiles);

    /* An upload's first request is a byte longer than a read's of the
     * same numbers: 35, the data format, then the read's bytes */
    Bench ten;
    ASSERT_NO_FATAL_FAILURE(ten.start(made(), 10));
    EXPECT_EQ(ten.begin(), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "step \"high\" is asked in 11 bytes: it doesn't fit one "
                 "request on this adapter (10 bytes)");
    Bench eleven;
    ASSERT_NO_FATAL_FAILURE(eleven.start(made(), 11));
    EXPECT_EQ(eleven.begin(), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(eleven.until_ended().end, ECUIO_OPERATION_END_SAVED);

    /* One that fits a frame runs there */
    Bench fits;
    ASSERT_NO_FATAL_FAILURE(
        fits.start(made(session_operation::GuestShape(),
                        session_operation::one_frame_plan()),
                   ECUIO_ENGINE_PDU_SINGLE));
    ASSERT_EQ(fits.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = fits.until_ended();
    EXPECT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(fits.count(0x23), 2u * (kLowBytes / 128));
}

} // namespace
