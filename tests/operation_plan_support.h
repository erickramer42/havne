// File/layer: tests/operation_plan_support.h | test-only, C++17, Qt-free,
//             every platform; header-only
// Testing: UNIT and HARNESS: what SessionOperationTest and the transports'
//          operation cases share: a plugin whose one operation copies part
//          of the simulated ECU's memory, a run waited to its end through
//          the public calls, and a saved copy's image
//
// Every address and value is the simulated ECU's, made up.

#ifndef ECUIO_TESTS_OPERATION_PLAN_SUPPORT_H
#define ECUIO_TESTS_OPERATION_PLAN_SUPPORT_H

#include "conversation_session_support.h"
#include "sim_memory.h" /* core/src */

#include <ecuio/core.h>
#include <ecuio/dump.h>
#include <ecuio/operation.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace session_operation {

using conversation_session::GuestShape;

inline constexpr uint32_t kLowAt = 0x00010000; /* a read's range */
inline constexpr uint32_t kLowBytes = 2048;
inline constexpr uint32_t kHighAt = 0x00011000; /* an upload's */
inline constexpr uint32_t kHighBytes = 4096;

/* What a case changes in the plan */
struct PlanShape {
    std::string chunk = "256";
    std::string address_bytes = "4";
    std::string size_bytes = "2";
    bool upload = true;
    bool ident = true;
    std::string level = "01";
};

/* A plan whose every request fits one frame: the read alone, asked in 7
 * bytes */
inline PlanShape one_frame_plan()
{
    PlanShape p;
    p.address_bytes = "3";
    p.size_bytes = "1";
    p.chunk = "128";
    p.upload = false;
    return p;
}

/* conversation_session's plugin in format 3 with the operation tier: one
 * plan, "copy", on its one ECU at level 01 */
inline conversation_test::Made made(const GuestShape& guest = {},
                                    const PlanShape& plan = {})
{
    conversation_test::Made m = conversation_session::made(guest);
    m.format = "3";
    m.tiers = R"(["data", "conversation", "operation"])";
    m.publisher = R"({"name": "Made-up Tools", "contact": "c", )"
                  R"("license": "MIT", "terms": "Use at your own risk."})";
    std::string steps =
        R"([{"step": "read", "name": "low", "session": "extended", )"
        R"("address": "00010000", "bytes": 2048, "address_bytes": )" +
        plan.address_bytes + R"(, "size_bytes": )" + plan.size_bytes +
        R"(, "chunk": )" + plan.chunk + "}";
    if (plan.upload) {
        steps += R"(, {"step": "upload", "name": "high", "session": )"
                 R"("extended", "address": "00011000", "bytes": 4096, )"
                 R"("address_bytes": 4, "size_bytes": 4, "dfi": "00"})";
    }
    m.operations =
        R"([{"id": "copy", "kind": "dump", "title": "Copy", "ecu": "pcm", )"
        R"("level": ")" +
        plan.level + R"(", )" +
        std::string(plan.ident ? R"("ident": [{"did": "D220", "role": )"
                                 R"("hardware"}, {"did": "D221", "role": )"
                                 R"("software"}], )"
                               : "") +
        R"("steps": )" + steps + "]}]";
    return m;
}

inline ecuio_operation_request_t request(uint32_t flags = 0)
{
    ecuio_operation_request_t q{};
    q.struct_size = sizeof q;
    q.flags = flags;
    return q;
}

/* Waits until channel 0's module is talking (the worker has run its
 * begin), 10 s at most: a start before that is refused */
inline void talking(const ecuio_session_t* s)
{
    ecuio_channel_conversation_t c{};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        ASSERT_EQ(ecuio_session_get_channel_conversation(s, 0, &c), ECUIO_OK);
        if (c.state == ECUIO_CONVERSATION_TALKING) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    FAIL() << "the module never talked: state " << c.state;
}

/* A started session's run once it has ended, or its state after seconds */
inline ecuio_operation_state_t ended(const ecuio_session_t* s, int seconds = 60)
{
    ecuio_operation_state_t v{};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    do {
        v = ecuio_operation_state_t{};
        v.struct_size = sizeof v;
        EXPECT_EQ(ecuio_session_get_operation(s, &v), ECUIO_OK);
        if (v.end != ECUIO_OPERATION_END_NONE) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return v;
}

/* The image of the copy at path, read through the public reader after
 * its verify; empty with a failure for a file that isn't a whole copy */
inline std::vector<uint8_t> image_at(const std::string& path)
{
    ecuio_dump_t* raw = nullptr;
    EXPECT_EQ(ecuio_dump_open(path.c_str(), &raw), ECUIO_OK)
        << ecuio_last_error_detail();
    if (raw == nullptr) {
        return {};
    }
    EXPECT_EQ(ecuio_dump_verify(raw, nullptr, nullptr, nullptr, 0), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_dump_info_t info{};
    info.struct_size = sizeof info;
    EXPECT_EQ(ecuio_dump_get_info(raw, &info), ECUIO_OK);
    std::vector<uint8_t> image(static_cast<size_t>(info.image_bytes));
    size_t count = 0;
    EXPECT_EQ(ecuio_dump_read(raw, 0, image.data(), image.size(), &count),
              ECUIO_OK);
    EXPECT_EQ(count, image.size());
    ecuio_dump_close(raw);
    return image;
}

/* What the simulated ECU's memory holds from address on, vin at its
 * place: the fakes' and the sim's alike */
inline std::vector<uint8_t> memory_of(const char* vin, uint32_t address,
                                      uint32_t bytes)
{
    ecuio_sim_memory m;
    m.vin = vin;
    std::vector<uint8_t> out;
    for (uint32_t i = 0; i < bytes; ++i) {
        out.push_back(ecuio_sim_memory_byte(m, address + i));
    }
    return out;
}

} // namespace session_operation

#endif /* ECUIO_TESTS_OPERATION_PLAN_SUPPORT_H */
