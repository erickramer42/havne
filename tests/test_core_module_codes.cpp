#include "core_test_support.h"
#include "fake_diagnostics.h"

#include "ecuio/core.h"

#include <gtest/gtest.h>

#include <string>

using namespace core_test;

namespace {

bool read_complete(ecuio_session_t* s, uint32_t c, uint32_t id,
                   ecuio_module_codes_t* r)
{
    return wait_for(
        [&] {
            return ecuio_session_get_module_codes(s, c, r) == ECUIO_OK &&
                   r->read_id >= id &&
                   (r->flags & ECUIO_MODULE_CODES_COMPLETE) != 0;
        },
        3000);
}

} // namespace

TEST(CoreTest, ReadModuleCodesRefusesWhatItCant)
{
    uint32_t id = 0;
    EXPECT_EQ(ecuio_session_read_module_codes(nullptr, 0, 0, &id),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    ecuio_session_info_t info;
    ASSERT_EQ(ecuio_session_get_info(s, &info), ECUIO_OK);
    EXPECT_EQ(ecuio_session_read_module_codes(s, info.module_count, 0, &id),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("modules"),
              std::string::npos);
    ecuio_module_codes_t r{};
    EXPECT_EQ(ecuio_session_get_module_codes(s, info.module_count, &r),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_session_get_module_codes(s, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    ASSERT_EQ(ecuio_session_get_module_codes(s, 0, &r), ECUIO_OK);
    EXPECT_EQ(r.flags, ECUIO_MODULE_CODES_ASKABLE) << "nothing read yet";
    EXPECT_EQ(r.rx_id, 0x7E8u);
    ecuio_session_close(s);
    EXPECT_EQ(ecuio_session_read_module_codes(s, 0, 0, &id),
              ECUIO_ERR_SESSION_CLOSED);
    ecuio_session_destroy(s);

    TempRecording rec(paced_rows(3));
    const std::string replay = rec.uri("?profile=obd2");
    ASSERT_EQ(ecuio_session_create(replay.c_str(), &s), ECUIO_OK);
    EXPECT_EQ(ecuio_session_read_module_codes(s, 0, 0, &id),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find("sends no requests"),
              std::string::npos);
    ecuio_session_destroy(s);
}

TEST(CoreTest, TheCallersMaskAndReadIdReachTheWorker)
{
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://demo", &s), ECUIO_OK);
    uint32_t first = 0;
    ASSERT_EQ(ecuio_session_read_module_codes(s, 0, 0, &first), ECUIO_OK);
    EXPECT_EQ(first, 1u);
    ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    ecuio_module_codes_t r{};
    ASSERT_TRUE(read_complete(s, 0, first, &r));
    EXPECT_EQ(r.read_id, 1u);
    EXPECT_EQ(r.mask, 0xAF) << "0 reads the default";
    uint32_t second = 0;
    ASSERT_EQ(ecuio_session_read_module_codes(s, 0, 0xFF, &second), ECUIO_OK);
    EXPECT_EQ(second, 2u);
    ASSERT_TRUE(read_complete(s, 0, second, &r));
    EXPECT_EQ(r.read_id, 2u);
    EXPECT_EQ(r.mask, 0xFF);
    ecuio_session_destroy(s);
}

TEST(MockTest, TheMocksModuleCodesAreTheFakesAndCompleteAtOnce)
{
    for (const char* uri : {"mock://demo", "mock://demo?diagnose=0"}) {
        SCOPED_TRACE(uri);
        ecuio_session_t* s = nullptr;
        ASSERT_EQ(ecuio_session_create(uri, &s), ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
        uint32_t id = 0;
        ASSERT_EQ(ecuio_session_read_module_codes(s, 0, 0, &id), ECUIO_OK);
        ecuio_module_codes_t r{};
        ASSERT_TRUE(read_complete(s, 0, id, &r));
        EXPECT_EQ(r.rx_id, 0x7E8u);
        EXPECT_NE(r.flags & ECUIO_MODULE_CODES_ASKABLE, 0u);
        EXPECT_NE(r.flags & ECUIO_MODULE_CODES_FORMAT, 0u);
        EXPECT_EQ(r.generation, 3u);
        fake_diagnostics::expect_module_codes(r, 0xAF);
        ecuio_session_destroy(s);
    }
}
