// File/layer: tests/test_session_operation_record.cpp | test_core, Qt-free
// Testing: UNIT: SessionOperationTest's recording cases: the operation
//          lines a recording takes at a run's start and end

#include "session_operation_support.h"

#include <ecuio/recording.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace session_operation;

namespace {

/* A recording of a bench's session */
struct Recording {
    std::filesystem::path root;
    ecuio_recorder_t* rec = nullptr;
    char folder[1024] = {};

    void attach(Bench& b)
    {
        root = std::filesystem::u8path(testing::TempDir()) /
               temp_name("operation-recording");
        std::filesystem::remove_all(root);
        const std::string root_text = root.u8string();
        ecuio_record_options_t o{};
        o.root_utf8 = root_text.c_str();
        ASSERT_EQ(ecuio_recorder_open(&o, &rec), ECUIO_OK);
        uint32_t len = 0;
        ASSERT_EQ(ecuio_recorder_get_folder(rec, folder, sizeof folder, &len),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_record(b.s, rec), ECUIO_OK)
            << ecuio_last_error_detail();
    }

    /* The session destroyed and the recording closed: each operation
     * line's words, in order, and events.log whole into *all */
    std::vector<std::string> lines(Bench& b, std::string* all = nullptr)
    {
        b.bound.session.reset();
        b.s = nullptr;
        EXPECT_EQ(ecuio_recorder_close(rec, nullptr), ECUIO_OK);
        std::stringstream text;
        {
            std::ifstream in(std::filesystem::u8path(folder) / "events.log",
                             std::ios::binary);
            text << in.rdbuf();
        }
        std::string t = text.str();
        t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
        std::vector<std::string> out;
        const std::string key = "  operation  ";
        for (size_t at = t.find(key); at != std::string::npos;
             at = t.find(key, at + 1)) {
            const size_t from = at + key.size();
            out.push_back(t.substr(from, t.find('\n', from) - from));
        }
        if (all != nullptr) {
            *all = t;
        }
        std::filesystem::remove_all(root);
        return out;
    }
};

const std::string kStarted = "dump,copy,0x7E8,6144,started";

} // namespace

/* A recording says when a run started and how it ended, by the plan's
 * id, the module and the image's size; never by the copy's name, which
 * is the module's contents' checksum */
TEST(SessionOperationTest, ARecordingNamesTheRunsStartAndEnd)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start());
    Recording r;
    ASSERT_NO_FATAL_FAILURE(r.attach(b));
    ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    std::string events;
    EXPECT_EQ(
        r.lines(b, &events),
        (std::vector<std::string>{kStarted, "dump,copy,0x7E8,6144,saved"}));
    EXPECT_EQ(events.find(v.name), std::string::npos);
    EXPECT_EQ(events.find(std::string(v.name).substr(13, 16)),
              std::string::npos)
        << "the checksum's digits";
}

TEST(SessionOperationTest, ARecordingNamesEachOtherEnd)
{
    {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        Recording r;
        ASSERT_NO_FATAL_FAILURE(r.attach(b));
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        b.until_phase(ECUIO_OPERATION_PHASE_READING);
        EXPECT_EQ(ecuio_session_operation_cancel(b.s), ECUIO_OK);
        EXPECT_EQ(b.until_ended().end, ECUIO_OPERATION_END_CANCELED);
        EXPECT_EQ(r.lines(b), (std::vector<std::string>{
                                  kStarted, "dump,copy,0x7E8,6144,canceled"}));
    }
    {
        /* the module refuses the first read */
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        b.script = [](const Bytes& q, size_t, Answer* a) {
            if (q[0] != 0x23) {
                return false;
            }
            a->frames = {Bytes{0x7F, 0x23, 0x31}};
            return true;
        };
        Recording r;
        ASSERT_NO_FATAL_FAILURE(r.attach(b));
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_EQ(b.until_ended().end, ECUIO_OPERATION_END_REFUSED);
        EXPECT_EQ(r.lines(b), (std::vector<std::string>{
                                  kStarted, "dump,copy,0x7E8,6144,refused"}));
    }
}

/* A close mid-run ends the run's line canceled; a run the worker never
 * began has no line at all */
TEST(SessionOperationTest, ARecordingNamesARunTheCloseEnded)
{
    {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        Recording r;
        ASSERT_NO_FATAL_FAILURE(r.attach(b));
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        b.until_phase(ECUIO_OPERATION_PHASE_READING);
        ecuio_conversations_end(b.s, b.e.get());
        EXPECT_EQ(r.lines(b), (std::vector<std::string>{
                                  kStarted, "dump,copy,0x7E8,6144,canceled"}));
    }
    {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start());
        Recording r;
        ASSERT_NO_FATAL_FAILURE(r.attach(b));
        ASSERT_EQ(b.begin(), ECUIO_OK) << ecuio_last_error_detail();
        ecuio_conversations_end(b.s, b.e.get());
        EXPECT_EQ(r.lines(b), std::vector<std::string>{});
    }
}
