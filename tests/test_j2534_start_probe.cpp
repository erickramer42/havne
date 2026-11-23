// File/layer: tests/test_j2534_start_probe.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534ProbeTest (test_j2534_stream): the start's
//          emissions probe on the fake DLL (request_probe.h, ADR-0065
//          step 5's RANGE), its switch on for each case

#include "test_j2534_stream_support.h"
#include "start_probe_seam.h"

#ifdef _WIN32

namespace {

class J2534ProbeTest : public J2534AdoptionTest
{
protected:
    StartProbe probe_;
    ecuio_session_t* session = nullptr;

    void TearDown() override
    {
        if (session != nullptr) {
            EXPECT_EQ(ecuio_session_close(session), ECUIO_OK);
            ecuio_session_destroy(session);
            session = nullptr;
        }
        J2534AdoptionTest::TearDown();
    }

    void create(const char* query)
    {
        char uri[512];
        snprintf(uri, sizeof uri, "j2534://%s%s", FAKE_J2534_DLL_PATH, query);
        ASSERT_EQ(ecuio_session_create(uri, &session), ECUIO_OK)
            << uri << ": " << ecuio_last_error_detail();
    }

    uint32_t reasons(uint32_t c) const
    {
        ecuio_channel_support_t sup{};
        EXPECT_EQ(ecuio_session_get_channel_support(session, c, &sup),
                  ECUIO_OK);
        return sup.open_reasons;
    }
};

} // namespace

/* The adapter's first write is the functional 01 00, at attach, before
 * the worker starts; the engine ECU's answer makes it RANGE */
TEST_F(J2534ProbeTest, TheProbeIsOneFunctionalWriteAtTheStart)
{
    ASSERT_NO_FATAL_FAILURE(create("?identify=0&diagnose=0"));
    unsigned long writes = 0;
    get_write_count(&writes);
    EXPECT_EQ(writes, 1u) << "one request, written before the start";
    unsigned char wbuf[64] = {};
    unsigned long wlen = 0;
    unsigned long wflags = 0;
    get_recorded_write(0, wbuf, &wlen, &wflags);
    ASSERT_EQ(wlen, 6u);
    EXPECT_EQ(wbuf[2], 0x07u);
    EXPECT_EQ(wbuf[3], 0xDFu);
    EXPECT_EQ(wbuf[4], 0x01u);
    EXPECT_EQ(wbuf[5], 0x00u);
    EXPECT_NE(reasons(0) & ECUIO_MODULE_REASON_RANGE, 0u);
}

/* A set that doesn't declare the engine ECU gains it as a module, its
 * requests to the physical ID; ?filter= joins nothing */
TEST_F(J2534ProbeTest, AnUndeclaredAnswererJoinsTheTable)
{
    ASSERT_NO_FATAL_FAILURE(
        create("?profile=uds-generic&did=7B8:D801&identify=0&diagnose=0"));
    ecuio_session_info_t info{};
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    ASSERT_EQ(info.module_count, 2u);
    ecuio_channel_decl_t m{};
    ASSERT_EQ(ecuio_session_get_module(session, 1, &m), ECUIO_OK);
    EXPECT_EQ(m.rx_id, 0x7E8u);
    EXPECT_EQ(m.tx_id, 0x7E0u);
    EXPECT_NE(reasons(1) & ECUIO_MODULE_REASON_RANGE, 0u);
    EXPECT_EQ(reasons(0) & ECUIO_MODULE_REASON_RANGE, 0u);
    ecuio_session_destroy(session);
    session = nullptr;

    ASSERT_NO_FATAL_FAILURE(create("?filter=0x7B8&identify=0&diagnose=0"));
    ASSERT_EQ(ecuio_session_get_info(session, &info), ECUIO_OK);
    EXPECT_EQ(info.module_count, 1u);
}

#endif /* _WIN32 */
