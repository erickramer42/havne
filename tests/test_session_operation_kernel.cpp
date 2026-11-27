#include "plugin_kernel_support.h"
#include "session_operation_support.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

using namespace session_operation;
using kernel_test::Kernel;

conversation_test::Made kernel_made(const Kernel& k = {})
{
    conversation_test::Made m = made();
    m.operations = k.json();
    m.files_extra = R"(, "kernel.bin": {"kind": "bin", "base64": ")" +
                    conversation_test::base64(k.blob) + "\"}";
    return m;
}

ecuio_status_t start_kernel(Bench& b,
                            uint32_t flags = ECUIO_OPERATION_MAY_UNLOCK)
{
    const ecuio_operation_request_t q = request(flags);
    return ecuio_session_operation_start(b.s, &q, b.folder.str().c_str());
}

} // namespace

TEST(SessionOperationTest, AKernelReadNeedsJ2534OrVcan)
{
    Bench elm;
    elm.uri = "elm327://bench";
    ASSERT_NO_FATAL_FAILURE(elm.start(kernel_made()));
    EXPECT_EQ(start_kernel(elm), ECUIO_ERR_NOT_IMPLEMENTED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "this plan writes to the module, which needs a J2534 "
                 "adapter: an ELM327 sends no request longer than a frame");

    Bench car;
    car.uri = "socketcan://can0";
    ASSERT_NO_FATAL_FAILURE(car.start(kernel_made()));
    EXPECT_EQ(start_kernel(car), ECUIO_ERR_NOT_IMPLEMENTED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "this plan writes to the module, which Havne does over "
                 "J2534 only: SocketCAN runs it on vcan alone");

    Bench vcan;
    vcan.uri = "socketcan://vcan0";
    vcan.write_max = 0;
    ASSERT_NO_FATAL_FAILURE(vcan.start(kernel_made()));
    vcan.s->no_car_bus = true;
    EXPECT_EQ(start_kernel(vcan), ECUIO_ERR_INVALID_ARGUMENT)
        << "past the link, to the adapter's bound";
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the helper's download is asked in 11 bytes: this adapter "
                 "carries no write that long");

    Bench dump;
    dump.uri = "elm327://bench";
    ASSERT_NO_FATAL_FAILURE(dump.start());
    ASSERT_EQ(dump.begin(), ECUIO_OK) << "a dump's link is any";
    EXPECT_EQ(dump.until_ended().end, ECUIO_OPERATION_END_SAVED);
}

TEST(SessionOperationTest, AKernelReadNeedsNoBatterySupportNorASource)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start(kernel_made()));
    b.s->battery.source = false;
    b.battery_source = false;
    ASSERT_EQ(start_kernel(b), ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_operation_cancel(b.s), ECUIO_OK);
    EXPECT_EQ(b.until_ended().end, ECUIO_OPERATION_END_CANCELED);
    EXPECT_TRUE(b.files().empty());
}

TEST(SessionOperationTest, TheQuieterAndItsKeepAliveGoToTheFunctionalId)
{
    Bench b;
    ASSERT_NO_FATAL_FAILURE(b.start(kernel_made()));
    ASSERT_EQ(start_kernel(b), ECUIO_OK) << ecuio_last_error_detail();
    for (int i = 0; i < 4000 && b.functional.size() < 3 &&
                    b.state().end == ECUIO_OPERATION_END_NONE;
         ++i) {
        b.pass();
    }
    ASSERT_GE(b.functional.size(), 1u) << b.state().words;
    EXPECT_EQ(b.functional[0].first, 0x7DFu);
    EXPECT_EQ(b.functional[0].second, (Bytes{0x10, 0x82}));
    EXPECT_GT(b.count(0x01), 0u);
    EXPECT_EQ(b.count(0x01, b.functional_after[0]), 0u)
        << "after the checks' reads in the default session";
    for (size_t i = 1; i < b.functional.size(); ++i) {
        EXPECT_EQ(b.functional[i].second, (Bytes{0x3E, 0x80})) << i;
        const int64_t gap = b.functional_at[i] - b.functional_at[i - 1];
        EXPECT_GE(gap, 2000 * engine_talk_test::kMs) << i;
        EXPECT_LT(gap, 2000 * engine_talk_test::kMs + 2 * ECUIO_ENGINE_GAP_US +
                           100 * engine_talk_test::kMs)
            << i;
    }
    const size_t heard = b.functional.size();
    ASSERT_EQ(ecuio_session_operation_cancel(b.s), ECUIO_OK);
    (void)b.until_ended();
    const int64_t ended = b.now;
    for (int i = 0; i < 400; ++i) {
        b.pass();
    }
    EXPECT_GT(b.now - ended, 3000 * engine_talk_test::kMs);
    EXPECT_EQ(b.functional.size(), heard) << "none once the run has ended";
}

TEST(SessionOperationTest, AKernelReadGoesOnWithNoReadingAtTheAdapter)
{
    Bench b;
    b.battery = [](uint32_t) { return 0u; };
    ASSERT_NO_FATAL_FAILURE(b.start(kernel_made()));
    ASSERT_EQ(start_kernel(b), ECUIO_OK) << ecuio_last_error_detail();
    for (int i = 0; i < 400 && b.functional.empty() &&
                    b.state().end == ECUIO_OPERATION_END_NONE;
         ++i) {
        b.pass();
    }
    EXPECT_NE(b.state().end, ECUIO_OPERATION_END_VOLTAGE_LOW)
        << b.state().words;
    EXPECT_FALSE(b.functional.empty()) << "past the checks to the quieter";
    (void)ecuio_session_operation_cancel(b.s);
    (void)b.until_ended();
}

/* The worker's half of the functional sends, stepped by hand: the
 * quieter written, then the keep-alive every keepalive_ms */
TEST(SessionOperationTest, TheKeepAliveGoesEveryKeepaliveMsUntilTheEnd)
{
    for (const uint32_t tx : {0x7E0u, 0x18DA10F1u}) {
        Bench b;
        ASSERT_NO_FATAL_FAILURE(b.start(kernel_made()));
        b.s->module_tx[0] = tx;
        ecuio_operation::plan p;
        p.has_kernel = true;
        p.helper.quiet_session = 0x82;
        p.helper.quiet_keepalive = 0x80;
        p.helper.keepalive_ms = 2000;
        p.steps.resize(1);
        auto o = std::make_unique<ecuio_operation::session_run>();
        o->p = &p;
        ecuio_operation::begin(o->r, p);
        o->r.asked = true;
        o->quiet_out = true;
        std::vector<int64_t> at;
        int64_t now = 1000000;
        for (; now < 1000000 + 9000 * engine_talk_test::kMs;
             now += 100 * engine_talk_test::kMs) {
            ecuio_operation::functional_step(b.s, *o, b.e.get(), now);
            if (b.e->aside_due) {
                b.e->aside_due = false;
                EXPECT_EQ(Bytes(b.e->aside, b.e->aside + 2),
                          (Bytes{0x3E, 0x80}));
                EXPECT_EQ(b.e->aside_tx, tx > 0x7FF ? 0x18DB33F1u : 0x7DFu);
                at.push_back(now);
            }
            if (at.empty() && now == 1000000) {
                EXPECT_EQ(o->r.at, ecuio_operation::run_stage::enter)
                    << "the quieter written settles the machine";
            }
        }
        ASSERT_EQ(at.size(), 4u) << "at 2, 4, 6 and 8 s";
        EXPECT_EQ(at[0], 1000000 + 2000 * engine_talk_test::kMs);
        for (size_t i = 1; i < at.size(); ++i) {
            EXPECT_EQ(at[i] - at[i - 1], 2000 * engine_talk_test::kMs);
        }
        ecuio_operation::stop(o->r, ecuio_operation::run_end::canceled);
        for (int i = 0; i < 50; ++i, now += 100 * engine_talk_test::kMs) {
            ecuio_operation::functional_step(b.s, *o, b.e.get(), now);
        }
        EXPECT_FALSE(b.e->aside_due) << "none once the run has ended";
    }
}
