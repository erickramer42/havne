// File/layer: tests/sim_memory_support.h | test-only, C++17, Qt-free, every
//             platform; header-only
// Testing: UNIT: SimMemoryTest, FakeMemoryTest (test_obd2)
//
// What the simulated memory's cases share (core/src/sim_memory.h): a
// read's request and its expected bytes, one ECU's memory by itself, a
// node in its session, the unbound sim and the fakes' ECU.

#ifndef ECUIO_TESTS_SIM_MEMORY_SUPPORT_H
#define ECUIO_TESTS_SIM_MEMORY_SUPPORT_H

#include "ecuio/internal.h"
#include "ecuio/sim.h"
#include "fake_ecus.h"
#include "sim_ecu.h"
#include "sim_memory.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sim_memory_test {

using Bytes = std::vector<uint8_t>;

inline constexpr uint32_t kBase = ECUIO_SIM_MEMORY_ADDRESS;
inline constexpr uint32_t kSize = ECUIO_SIM_MEMORY_BYTES;
inline constexpr uint32_t kVinAt = ECUIO_SIM_MEMORY_VIN_ADDRESS;

/* 23 <ALFID> <address> <size>, the address in a bytes and the size in s */
inline Bytes read_of(uint64_t address, uint64_t size, int a = 4, int s = 2)
{
    Bytes req = {0x23, static_cast<uint8_t>((s << 4) | a)};
    for (int i = a - 1; i >= 0; --i) {
        req.push_back(static_cast<uint8_t>(address >> (8 * i)));
    }
    for (int i = s - 1; i >= 0; --i) {
        req.push_back(static_cast<uint8_t>(size >> (8 * i)));
    }
    return req;
}

/* What a read of size bytes at address answers with no fault: 63 and
 * the region's bytes, vin at its place */
inline Bytes data_of(uint32_t address, uint32_t size, const char* vin)
{
    ecuio_sim_memory m;
    m.vin = vin;
    Bytes want = {0x63};
    for (uint32_t i = 0; i < size; ++i) {
        want.push_back(ecuio_sim_memory_byte(m, address + i));
    }
    return want;
}

inline Bytes key_for(uint8_t level)
{
    const ecuio_sim_security_level* l = ecuio_sim_security_find(level);
    Bytes req = {0x27, static_cast<uint8_t>(level + 1)};
    req.resize(2u + l->bytes);
    ecuio_sim_security_key(l->seed, l->bytes, req.data() + 2);
    return req;
}

inline Bytes nrc(uint8_t service, uint8_t code)
{
    return Bytes{0x7F, service, code};
}

/* One ECU's memory, unlocked in the extended session unless a case says */
struct Ecu {
    ecuio_sim_memory m;
    ecuio_sim_security s;
    bool extended = true;

    Ecu()
    {
        m.vin = ECUIO_SIM_DEFAULT_VIN;
        s.unlocked = 0x01;
    }

    Bytes ask(const Bytes& req)
    {
        std::vector<uint8_t> out(ECUIO_SIM_MEMORY_REPLY_MAX + 8, 0xEE);
        const uint32_t n = ecuio_sim_memory_answer(
            m, s, extended, req.data(), static_cast<uint32_t>(req.size()),
            out.data());
        EXPECT_LE(n, ECUIO_SIM_MEMORY_REPLY_MAX);
        EXPECT_EQ(out[ECUIO_SIM_MEMORY_REPLY_MAX], 0xEE) << "wrote past out";
        return Bytes(out.begin(), out.begin() + n);
    }
};

inline const Bytes kEntered = {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4};
inline const Bytes kHardware = {0x22, 0xD2, 0x20};
inline const Bytes kSoftware = {0x22, 0xD2, 0x21};
inline const Bytes kNoDid = {0x7F, 0x22, 0x31};

inline Bytes ident_of(uint16_t did, const char* text)
{
    Bytes want = {0x62, static_cast<uint8_t>(did >> 8),
                  static_cast<uint8_t>(did & 0xFF)};
    want.insert(want.end(), text, text + std::strlen(text));
    return want;
}

/* A node's session and the engine ECU behind it, as sim.cpp's node
 * answers: the session's services first, then the engine ECU's */
struct Node {
    ecuio_sim_ecu ecu;
    ecuio_sim_session s;
    int64_t now = 0;
    bool handled = false;

    explicit Node(bool memory = true)
    {
        std::memcpy(ecu.config.vin, ECUIO_SIM_DEFAULT_VIN,
                    sizeof ECUIO_SIM_DEFAULT_VIN);
        s.memory.vin = memory ? ecu.config.vin : nullptr;
    }

    Bytes ask(const Bytes& req, bool functional = false)
    {
        std::vector<uint8_t> out(ECUIO_SIM_REPLY_MAX);
        uint32_t n = 0;
        handled = false;
        if (!functional) {
            n = ecuio_sim_session_answer(s, true, true, now, req.data(),
                                         static_cast<uint32_t>(req.size()),
                                         out.data(), &handled);
        }
        if (!handled) {
            n = ecuio_sim_answer(ecu, s, req.data(),
                                 static_cast<uint32_t>(req.size()), functional,
                                 out.data());
        }
        return Bytes(out.begin(), out.begin() + n);
    }

    void unlock()
    {
        ask({0x27, 0x01});
        EXPECT_EQ(ask(key_for(0x01)), (Bytes{0x67, 0x02}));
    }
};

struct Unbound {
    ecuio_sim_t* sim = nullptr;
    int64_t t = 0;

    explicit Unbound(const char* uri)
    {
        EXPECT_EQ(ecuio_sim_create_unbound(uri, 0, &sim), ECUIO_OK)
            << uri << ": " << ecuio_last_error_detail();
    }
    ~Unbound() { ecuio_sim_destroy(sim); }
    Unbound(const Unbound&) = delete;
    Unbound& operator=(const Unbound&) = delete;

    /* The one reply to req on id at the case's time, empty for none */
    Bytes ask(uint32_t id, const Bytes& req, uint32_t* flags = nullptr)
    {
        std::vector<ecuio_sim_reply_t> r(ECUIO_SIM_MAX_REPLIES);
        uint32_t n = 0;
        EXPECT_EQ(ecuio_sim_ask(sim, id, req.data(),
                                static_cast<uint32_t>(req.size()), t, r.data(),
                                ECUIO_SIM_MAX_REPLIES, &n),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_LE(n, 1u);
        if (flags != nullptr) {
            *flags = n == 0 ? 0 : r[0].flags;
        }
        return n == 0 ? Bytes{} : Bytes(r[0].data, r[0].data + r[0].length);
    }

    void unlock()
    {
        EXPECT_EQ(ask(0x7E0, {0x10, 0x03}), kEntered);
        ask(0x7E0, {0x27, 0x01});
        EXPECT_EQ(ask(0x7E0, key_for(0x01)), (Bytes{0x67, 0x02}));
    }
};

/* The fakes' state is the process's: each case starts and ends clean */
struct FakeMemoryTest : ::testing::Test {
    void SetUp() override { clean(); }
    void TearDown() override { clean(); }

    static void clean()
    {
        fake_ecu::set_module_dids(nullptr);
        fake_ecu::reset_sessions();
        fake_ecu::reset_security();
    }

    static Bytes ask(uint32_t rx, const Bytes& req)
    {
        std::vector<uint8_t> out(fake_ecu::kReplyMax);
        size_t n = 0;
        if (!fake_ecu::answer_for(rx, req.data(), req.size(), -1, out.data(),
                                  &n)) {
            return Bytes{};
        }
        return Bytes(out.begin(), out.begin() + static_cast<long>(n));
    }
};

} // namespace sim_memory_test

#endif /* ECUIO_TESTS_SIM_MEMORY_SUPPORT_H */
