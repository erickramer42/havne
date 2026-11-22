// File/layer: tests/test_core_merge.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: MergeSessionTest (test_core)
//
// ecuio_signal_file_merge where test_core holds the seam (ADR-0065
// section 9): every allocation of a merge failed once, and a session on
// a merged set decoding a value from each part.

#include "core_test_support.h"

#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>

#include <string>

using namespace core_test;

void fail_new_on_this_thread(unsigned long n);
unsigned long news_on_this_thread();

namespace {

const char* const kFirst = R"({"havne_signals": 2, "name": "First",
 "source": {"name": "Havne's tests", "license": "CC-BY-SA-4.0"},
 "extends": "obd2",
 "ecus": [{"id": "pcm", "rx": "7E8"}, {"id": "tcm", "rx": "7E9", "name": "Transaxle"}],
 "signals": [
 {"key": "oil", "name": "Oil", "read": {"service": "22", "number": "D100"},
  "field": {"byte": 0}, "unit": "deg C"},
 {"key": "gear", "name": "Gear", "kind": "enum", "ecu": "tcm",
  "read": {"service": "22", "number": "D200"}, "field": {"byte": 0},
  "values": {"0": "Park", "1": "Drive"}},
 {"key": "old", "name": "Old", "ecu": "tcm",
  "read": {"service": "22", "number": "D201"}, "field": {"byte": 0}},
 {"key": "double", "name": "Double", "kind": "computed", "expr": "oil * 2"}]})";

const char* const kSecond = R"({"havne_signals": 3, "name": "Second",
 "source": {"name": "Havne's tests"}, "needs": ["oil", "absent"],
 "ecus": [{"id": "trans", "rx": "7E9"}],
 "signals": [
 {"key": "fresh", "name": "Fresh", "read": {"service": "22", "number": "D201"},
  "field": {"byte": 0}},
 {"key": "sum", "name": "Sum", "kind": "computed", "expr": "oil + fresh"},
 {"key": "lost", "name": "Lost", "kind": "computed", "expr": "absent + 1"}]})";

ecuio_signal_file_t* loaded(const char* text)
{
    ecuio_signal_file_t* f = nullptr;
    EXPECT_EQ(ecuio_signal_file_load(text, std::strlen(text), &f), ECUIO_OK)
        << ecuio_last_error_detail();
    return f;
}

std::string text_of(const ecuio_signal_file_t* f)
{
    size_t length = 0;
    const char* text = ecuio_signal_file_text(f, &length);
    return std::string(text, length);
}

} // namespace

/* Every allocation of a merge failed in turn: the call answers OK with
 * the whole set or OUT_OF_MEMORY with none, and throws nothing */
TEST(MergeSessionTest, EveryAllocationFailedOnceIsOutOfMemoryOrTheWholeSet)
{
    ecuio_signal_file_t* first = loaded(kFirst);
    ecuio_signal_file_t* second = loaded(kSecond);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    const ecuio_signal_file_t* parts[] = {first, second};

    ecuio_signal_file_t* whole = nullptr;
    const unsigned long before = news_on_this_thread();
    ASSERT_EQ(ecuio_signal_file_merge(parts, 2, nullptr, &whole), ECUIO_OK)
        << ecuio_last_error_detail();
    const unsigned long news = news_on_this_thread() - before;
    const std::string text = text_of(whole);
    const uint32_t notes = ecuio_signal_file_note_count(whole);
    EXPECT_EQ(notes, 2u) << "a replacement and a signal left out";
    ecuio_signal_file_free(whole);
    ASSERT_GT(news, 100u);

    unsigned long refused = 0;
    for (unsigned long n = 1; n <= news; ++n) {
        ecuio_signal_file_t* out = reinterpret_cast<ecuio_signal_file_t*>(1);
        ecuio_status_t rc = ECUIO_OK;
        bool threw = false;
        fail_new_on_this_thread(n);
        try {
            rc = ecuio_signal_file_merge(parts, 2, nullptr, &out);
        } catch (...) {
            threw = true;
        }
        fail_new_on_this_thread(0);
        ASSERT_FALSE(threw) << "the merge threw at allocation " << n;
        if (rc == ECUIO_OK) {
            ASSERT_NE(out, nullptr) << "at " << n;
            ASSERT_EQ(text_of(out), text) << "at " << n;
            ASSERT_EQ(ecuio_signal_file_note_count(out), notes) << "at " << n;
            ecuio_signal_file_free(out);
        } else {
            ++refused;
            ASSERT_EQ(rc, ECUIO_ERR_OUT_OF_MEMORY) << "at " << n;
            ASSERT_EQ(out, nullptr) << "at " << n;
        }
    }
    EXPECT_GT(refused, news / 2) << "most allocations are needed";
    ecuio_signal_file_free(first);
    ecuio_signal_file_free(second);
}

/* The merged set registers under its profile, and a session on it
 * decodes obd2's signal, each part's, and one computed across them */
TEST(MergeSessionTest, ASessionOnAMergedSetDecodesEachPartsSignals)
{
    ecuio_signal_file_t* first = loaded(kFirst);
    ecuio_signal_file_t* second = loaded(kSecond);
    const ecuio_signal_file_t* parts[] = {first, second};
    ecuio_signal_file_t* merged = nullptr;
    ASSERT_EQ(ecuio_signal_file_merge(parts, 2, nullptr, &merged), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_signal_file_free(first);
    ecuio_signal_file_free(second); /* the result keeps nothing of them */

    const char* profile = ecuio_signal_file_profile(merged);
    ASSERT_EQ(
        ecuio_profile_register(profile, ecuio_signal_file_set(merged), nullptr),
        ECUIO_OK);
    const std::string uri = std::string("mock://merged?profile=") + profile;
    ecuio_session_t* session = nullptr;
    ASSERT_EQ(ecuio_session_create(uri.c_str(), &session), ECUIO_OK)
        << ecuio_last_error_detail();
    ValuesCapture cap;
    ASSERT_EQ(ecuio_register_values_callback(session, values_capture_cb, &cap),
              ECUIO_OK);
    const ecuio_frame_t rpm =
        value_frame(ECUIO_OBD2_ECU_ARB_ID, {0x41, 0x0C, 0x0F, 0xA0}, 1000000);
    const ecuio_frame_t oil =
        value_frame(0x7E8, {0x62, 0xD1, 0x00, 50}, 1100000);
    const ecuio_frame_t gear =
        value_frame(0x7E9, {0x62, 0xD2, 0x00, 1}, 1200000);
    const ecuio_frame_t fresh =
        value_frame(0x7E9, {0x62, 0xD2, 0x01, 7}, 1300000);
    for (const ecuio_frame_t* f : {&rpm, &oil, &gear, &fresh}) {
        ecuio_session_dispatch_callbacks(session, f);
    }
    ASSERT_EQ(cap.batches.size(), 4u);
    const auto id_of = [merged](const char* key) {
        const ecuio_signal_def_t* def = ecuio_signal_file_find(merged, key);
        return def != nullptr ? def->id : uint16_t{0};
    };
    ASSERT_NE(value_of(cap.batches[0], ECUIO_OBD2_SIG_RPM_ID), nullptr);
    EXPECT_EQ(value_of(cap.batches[0], ECUIO_OBD2_SIG_RPM_ID)->value, 1000.0);
    ASSERT_NE(value_of(cap.batches[1], id_of("oil")), nullptr);
    EXPECT_EQ(value_of(cap.batches[1], id_of("oil"))->value, 50.0);
    ASSERT_NE(value_of(cap.batches[1], id_of("double")), nullptr);
    EXPECT_EQ(value_of(cap.batches[1], id_of("double"))->value, 100.0);
    ASSERT_NE(value_of(cap.batches[2], id_of("gear")), nullptr);
    EXPECT_EQ(value_of(cap.batches[2], id_of("gear"))->value, 1.0);
    /* Second's signal stands where First's "old" did, under both keys */
    EXPECT_EQ(id_of("old"), id_of("fresh"));
    ASSERT_NE(value_of(cap.batches[3], id_of("fresh")), nullptr);
    EXPECT_EQ(value_of(cap.batches[3], id_of("fresh"))->value, 7.0);
    ASSERT_NE(value_of(cap.batches[3], id_of("sum")), nullptr)
        << "computed in one part from both";
    EXPECT_EQ(value_of(cap.batches[3], id_of("sum"))->value, 57.0);
    EXPECT_EQ(ecuio_signal_file_find(merged, "lost"), nullptr);

    ecuio_session_destroy(session);
    EXPECT_EQ(ecuio_profile_unregister(profile), ECUIO_OK);
    ecuio_signal_file_free(merged);
}
