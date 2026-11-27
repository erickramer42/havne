#include "../core/src/operation_checks.h"
#include "../core/src/request_talk.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using ecuio_operation::check_begin;
using ecuio_operation::check_end;
using ecuio_operation::check_next;
using ecuio_operation::check_reply;
using ecuio_operation::check_request;
using ecuio_operation::check_silence;
using ecuio_operation::checks;

using Bytes = std::vector<uint8_t>;

/* The request next, as module and PID; 0xFF for none */
std::pair<uint32_t, uint8_t> next_of(const checks& k)
{
    check_request q;
    if (!check_next(k, q)) {
        return {UINT32_MAX, 0xFF};
    }
    EXPECT_EQ(q.bytes[0], 0x01);
    return {q.module, q.bytes[1]};
}

void reply(checks& k, const Bytes& d)
{
    check_reply(k, d.data(), d.size());
}

TEST(OperationChecksTest, OnlyTheFourReadingsPassTheDoor)
{
    for (unsigned sid = 0; sid < 256; ++sid) {
        for (unsigned pid = 0; pid < 256; ++pid) {
            const bool want = sid == 0x01 && (pid == 0x0C || pid == 0x0D ||
                                              pid == 0x40 || pid == 0x42);
            EXPECT_EQ(ecuio_check_allowed(static_cast<uint8_t>(sid),
                                          static_cast<uint8_t>(pid)),
                      want)
                << sid << " " << pid;
        }
    }
    ecuio_engine_talk_state t;
    ecuio_key_window w;
    const uint8_t rpm[] = {0x01, 0x0C};
    const uint8_t clear[] = {0x04, 0x00};
    ecuio_talk_request q;
    q.kind = ecuio_talk_kind::check;
    q.pdu = clear;
    q.len = 2;
    q.echo = 2;
    EXPECT_EQ(ecuio_engine_talk(t, w, 60, q, 0), ecuio_talk_offer::refused);
    q.pdu = rpm;
    q.echo = 1;
    EXPECT_EQ(ecuio_engine_talk(t, w, 60, q, 0), ecuio_talk_offer::refused);
    q.echo = 2;
    const uint8_t pack[] = {0x01, 0x0C, 0x0D};
    q.pdu = pack;
    q.len = 3;
    EXPECT_EQ(ecuio_engine_talk(t, w, 60, q, 0), ecuio_talk_offer::refused)
        << "one PID a check";
    q.pdu = rpm;
    q.len = 2;
    EXPECT_EQ(ecuio_engine_talk(t, w, 60, q, 0), ecuio_talk_offer::taken);
    q.kind = ecuio_talk_kind::plain;
    ecuio_engine_talk_state plain;
    EXPECT_EQ(ecuio_engine_talk(plain, w, 60, q, 0), ecuio_talk_offer::refused)
        << "service 01 isn't the guest's";
}

TEST(OperationChecksTest, TheEngineModuleIsAskedBeforeTheTarget)
{
    checks k;
    check_begin(k, 3, 1, false);
    EXPECT_EQ(next_of(k), std::make_pair(3u, uint8_t{0x0C}));
    check_silence(k);
    EXPECT_EQ(next_of(k), std::make_pair(1u, uint8_t{0x0C}));
    reply(k, {0x7F, 0x01, 0x12});
    EXPECT_EQ(next_of(k), std::make_pair(3u, uint8_t{0x0D}))
        << "neither answered: no rpm, the speed next";
    EXPECT_FALSE(k.has_rpm);
    reply(k, {0x41, 0x0D, 0x00});
    EXPECT_TRUE(k.has_kmh);
    EXPECT_EQ(next_of(k).second, 0xFF);
    EXPECT_EQ(k.result, check_end::none);
}

TEST(OperationChecksTest, TheTargetAloneWhenItIsTheEngineModule)
{
    for (const uint32_t engine : {1u, UINT32_MAX}) {
        checks k;
        check_begin(k, engine, 1, false);
        EXPECT_EQ(next_of(k), std::make_pair(1u, uint8_t{0x0C}));
        check_silence(k);
        EXPECT_EQ(next_of(k), std::make_pair(1u, uint8_t{0x0D}));
    }
}

TEST(OperationChecksTest, AnyEngineSpeedAboveZeroIsRunning)
{
    checks k;
    check_begin(k, UINT32_MAX, 0, false);
    reply(k, {0x41, 0x0C, 0x00, 0x01});
    EXPECT_EQ(k.result, check_end::engine_running);
    EXPECT_EQ(k.rpm, 1u) << "a quarter rpm is one, never zero";
    EXPECT_EQ(next_of(k).second, 0xFF);
    check_begin(k, UINT32_MAX, 0, false);
    reply(k, {0x41, 0x0C, 0x00, 0x00});
    EXPECT_EQ(k.result, check_end::none);
    reply(k, {0x41, 0x0D, 0x01});
    EXPECT_EQ(k.result, check_end::moving);
    EXPECT_EQ(k.kmh, 1u);
}

TEST(OperationChecksTest, AnAnswerOfTheWrongShapeIsNoReading)
{
    checks k;
    check_begin(k, UINT32_MAX, 0, false);
    reply(k, {0x41, 0x0C, 0x05});
    EXPECT_FALSE(k.has_rpm);
    EXPECT_EQ(next_of(k).second, 0x0D);
    reply(k, {0x41, 0x0C, 0x05});
    EXPECT_FALSE(k.has_kmh);
    EXPECT_EQ(k.result, check_end::none);
}

TEST(OperationChecksTest, TheEcusVoltageIsAskedOnlyWhereItIsListed)
{
    checks k;
    check_begin(k, 2, 0, true);
    reply(k, {0x41, 0x0C, 0x00, 0x00});
    reply(k, {0x41, 0x0D, 0x00});
    EXPECT_EQ(next_of(k), std::make_pair(2u, uint8_t{0x40}));
    reply(k, {0x41, 0x40, 0xBF, 0xFF, 0xFF, 0xFF});
    EXPECT_EQ(next_of(k), std::make_pair(0u, uint8_t{0x40}))
        << "the engine module lists no 42";
    reply(k, {0x41, 0x40, 0x40, 0x00, 0x00, 0x00});
    EXPECT_EQ(next_of(k), std::make_pair(0u, uint8_t{0x42}));
    reply(k, {0x41, 0x42, 0x31, 0x38});
    EXPECT_TRUE(k.has_volts);
    EXPECT_EQ(k.volts_mv, 12600u);
    EXPECT_EQ(next_of(k).second, 0xFF);
}

TEST(OperationChecksTest, AVoltageUnansweredAsksTheNextModulesList)
{
    checks k;
    check_begin(k, 2, 0, true);
    reply(k, {0x41, 0x0C, 0x00, 0x00});
    reply(k, {0x41, 0x0D, 0x00});
    reply(k, {0x41, 0x40, 0x40, 0x00, 0x00, 0x00});
    EXPECT_EQ(next_of(k), std::make_pair(2u, uint8_t{0x42}));
    check_silence(k);
    EXPECT_EQ(next_of(k), std::make_pair(0u, uint8_t{0x40}));
    check_silence(k);
    EXPECT_EQ(next_of(k).second, 0xFF);
    EXPECT_FALSE(k.has_volts);
}

TEST(OperationChecksTest, NoVoltageIsAskedWhenTheAdapterReadsOne)
{
    checks k;
    check_begin(k, UINT32_MAX, 0, false);
    reply(k, {0x41, 0x0C, 0x00, 0x00});
    reply(k, {0x41, 0x0D, 0x00});
    EXPECT_EQ(next_of(k).second, 0xFF);
}

} // namespace

TEST(OperationChecksTest, AKernelReadsBatteryIsJudgedAsADumpsAtItsStart)
{
    using ecuio_operation::volts;
    using ecuio_operation::volts_take;
    volts v;
    v.write = true;
    v.ram_only = true;
    volts_take(v, 11790, false);
    EXPECT_TRUE(v.low_warn) << "the dump's warn under 11.8 V";
    volts_take(v, 11400, false);
    EXPECT_TRUE(v.low_warn);
    EXPECT_FALSE(v.refused_low) << "no write's floor";
    volts_take(v, 11800, false);
    EXPECT_FALSE(v.low_warn) << "nor the write's warn under 12.0 V";
    volts_take(v, 16100, false);
    EXPECT_TRUE(v.refused_high) << "a 24 V system refused, as for a write";
    v.refused_high = false;
    volts_take(v, 10999, true);
    EXPECT_FALSE(v.dropped) << "a sag never ends it";

    volts write;
    write.write = true;
    volts_take(write, 11900, false);
    EXPECT_TRUE(write.low_warn) << "a write's warn under 12.0 V";
    volts_take(write, 11400, false);
    EXPECT_TRUE(write.refused_low) << "a write's start under 11.5 V";
}
