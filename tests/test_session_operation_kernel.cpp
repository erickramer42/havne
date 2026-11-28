#include "plugin_kernel_support.h"
#include "session_operation_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

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

TEST(SessionOperationTest, AKernelReadDownloadsStartsCopiesAndLeaves)
{
    Kernel k;
    k.blob.resize(2000);
    ASSERT_EQ(ecuio_sim_kernel_image(k.blob.data(), 2000), 2000u);
    Bench b;
    b.node.s.kernel.on = true;
    ASSERT_NO_FATAL_FAILURE(b.start(kernel_made(k)));
    ASSERT_EQ(start_kernel(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    ASSERT_FALSE(b.functional.empty());
    EXPECT_EQ(b.functional[0].second, (Bytes{0x10, 0x82}));
    EXPECT_EQ(b.node.s.kernel.downloads, 1u);
    EXPECT_EQ(b.node.s.kernel.starts, 1u);
    EXPECT_EQ(b.count(0x34), 1u);
    EXPECT_EQ(b.count(0x36, 0), 2u + 2u * 4u)
        << "two blocks down, four up twice";
    EXPECT_EQ(b.count(0x11), 0u) << "no reset of any kind";
    size_t exit = 0;
    for (size_t i = 0; i < b.sent.size(); ++i) {
        exit = b.sent[i] == Bytes{0x37} ? i : exit;
    }
    ASSERT_LT(exit + 1, b.sent.size());
    EXPECT_EQ(b.sent[exit + 1], (Bytes{0x10, 0x01}))
        << "the run's last request is its leave";
    const std::vector<std::string> names = b.files();
    ASSERT_EQ(names.size(), 1u);
    const std::vector<uint8_t> image =
        image_at((b.folder.path / names[0]).generic_u8string());
    EXPECT_EQ(image, memory_of(ECUIO_SIM_DEFAULT_VIN, 0x00080000, 4096))
        << "the code region, read through the helper";
}

TEST(SessionOperationTest, AHelpersDfiIsAClaimAndItsStartsStatusIsShown)
{
    Kernel k;
    k.blob.resize(2000);
    ASSERT_EQ(ecuio_sim_kernel_image(k.blob.data(), 2000), 2000u);
    ecuio_sim_kernel_scramble(k.blob.data(), 2000);
    k.download_extra = R"(, "dfi": "01")";
    k.routine_extra = R"(, "option": "A55A0FF0")";
    Bench b;
    b.node.s.kernel.on = true;
    ASSERT_NO_FATAL_FAILURE(b.start(kernel_made(k)));
    ASSERT_EQ(start_kernel(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_NE(v.checks & ECUIO_OPERATION_CHECK_DFI_CLAIMED, 0u);
    EXPECT_EQ(v.routine_status_given, 1);
    EXPECT_EQ(v.routine_status, ECUIO_SIM_KERNEL_STATUS);
    const ecuio_run_lines& l = b.s->run_lines;
    std::vector<std::string> lines(l.text, l.text + l.count);
    EXPECT_NE(std::find(lines.begin(), lines.end(),
                        "run helper  dfi 01: the plugin's claim, unchecked"),
              lines.end());
    EXPECT_NE(
        std::find(lines.begin(), lines.end(), "run helper  started, status 10"),
        lines.end());

    Bench plain;
    plain.node.s.kernel.on = true;
    Kernel bare;
    bare.blob.resize(2000);
    ASSERT_EQ(ecuio_sim_kernel_image(bare.blob.data(), 2000), 2000u);
    ASSERT_NO_FATAL_FAILURE(plain.start(kernel_made(bare)));
    ASSERT_EQ(start_kernel(plain), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t w = plain.until_ended();
    ASSERT_EQ(w.end, ECUIO_OPERATION_END_SAVED) << w.words;
    EXPECT_EQ(w.checks & ECUIO_OPERATION_CHECK_DFI_CLAIMED, 0u);
    EXPECT_EQ(w.routine_status_given, 0);
    EXPECT_EQ(w.routine_status, 0);
    for (uint32_t i = 0; i < plain.s->run_lines.count; ++i) {
        EXPECT_EQ(
            std::string(plain.s->run_lines.text[i]).rfind("run helper", 0),
            std::string::npos)
            << plain.s->run_lines.text[i];
    }
}

TEST(SessionOperationTest, AStartLongerThanTheAdaptersWritesIsRefused)
{
    Kernel k;
    k.routine_extra = R"(, "option": "00112233445566778899AABBCCDDEEFF")";
    Bench vcan;
    vcan.uri = "socketcan://vcan0";
    vcan.write_max = 19;
    ASSERT_NO_FATAL_FAILURE(vcan.start(kernel_made(k)));
    vcan.s->no_car_bus = true;
    EXPECT_EQ(start_kernel(vcan), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the helper's start is asked in 20 bytes: this adapter "
                 "carries no write that long");
}

TEST(SessionOperationTest, EachStartOfAHelperThatEndsShowsAndLogsItsStatus)
{
    Kernel k;
    k.blob.resize(2000);
    ASSERT_EQ(ecuio_sim_kernel_image(k.blob.data(), 2000), 2000u);
    k.stays = "false";
    Bench b;
    b.node.s.kernel.on = true;
    b.node.s.kernel.faults.once = true;
    uint8_t status = 0x10;
    b.script = [&](const Bytes& q, size_t, Answer* a) {
        if (q.empty() || q[0] != 0x31) {
            return false;
        }
        Bytes reply = b.node.ask(q);
        if (reply.size() >= 4 && reply[0] == 0x71) {
            reply.resize(4);
            reply.push_back(status);
            status = 0x20;
        }
        a->frames = {reply};
        return true;
    };
    ASSERT_NO_FATAL_FAILURE(b.start(kernel_made(k)));
    ASSERT_EQ(start_kernel(b), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t v = b.until_ended();
    ASSERT_EQ(v.end, ECUIO_OPERATION_END_SAVED) << v.words;
    EXPECT_EQ(b.node.s.kernel.starts, 2u) << "a start for each pass";
    EXPECT_EQ(v.routine_status_given, 1);
    EXPECT_EQ(v.routine_status, 0x20) << "the latest start's";
    const ecuio_run_lines& l = b.s->run_lines;
    std::vector<std::string> lines(l.text, l.text + l.count);
    for (const char* want :
         {"run helper  started, status 10", "run helper  started, status 20"}) {
        EXPECT_NE(std::find(lines.begin(), lines.end(), want), lines.end())
            << want;
    }

    Bench refused;
    refused.node.s.kernel.on = true;
    refused.node.s.kernel.faults.once = true;
    status = 0x10;
    size_t uploads = 0;
    refused.script = [&](const Bytes& q, size_t, Answer* a) {
        if (!q.empty() && q[0] == 0x35 && ++uploads == 2) {
            a->frames = {Bytes{0x7F, 0x35, 0x22}};
            return true;
        }
        if (q.empty() || q[0] != 0x31) {
            return false;
        }
        Bytes reply = refused.node.ask(q);
        if (reply.size() >= 4 && reply[0] == 0x71) {
            reply.resize(4);
            reply.push_back(status);
            status = 0x20;
        }
        a->frames = {reply};
        return true;
    };
    ASSERT_NO_FATAL_FAILURE(refused.start(kernel_made(k)));
    ASSERT_EQ(start_kernel(refused), ECUIO_OK) << ecuio_last_error_detail();
    const ecuio_operation_state_t w = refused.until_ended();
    ASSERT_EQ(w.end, ECUIO_OPERATION_END_REFUSED) << w.words;
    EXPECT_EQ(w.routine_status, 0x20)
        << "the second start's, though nothing else moved before the end";
}
