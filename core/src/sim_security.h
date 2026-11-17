// File/layer: core/src/sim_security.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimSecurityTest (test_obd2)
//
// The simulated ECU's security access (ADR-0066 section 2.7), Service
// 27 from the service byte on: a seed for a level, a key checked, a
// lockout and its delay, all the caller's state and the caller's clock.
// Everything here is made up for the simulated car: the seeds are fixed
// numbers and the key is the seed's bytes in reverse order, each XORed
// with a letter of "HAVNE". It is no manufacturer's algorithm and
// protects nothing. sim_ecu.h's engine ECU and the fakes' one ECU
// (tests/fake_ecus.h) both answer from it.

#ifndef ECUIO_SIM_SECURITY_H
#define ECUIO_SIM_SECURITY_H

#include <cstdint>
#include <cstring>

/* A level: its seed request's sub-function (odd; the key's is the next),
 * and its seed, as long as its key */
struct ecuio_sim_security_level {
    uint8_t level;
    uint8_t bytes;
    uint8_t seed[16];
};

/* Level 01 has 4 bytes; level 03 has 16, so its key's request is longer
 * than a CAN frame */
constexpr ecuio_sim_security_level ECUIO_SIM_SECURITY_LEVELS[] = {
    {0x01, 4, {0x5A, 0x17, 0xC3, 0x08}},
    {0x03,
     16,
     {0x11, 0x92, 0x23, 0xA4, 0x35, 0xB6, 0x47, 0xC8, 0x59, 0xDA, 0x6B, 0xEC,
      0x7D, 0xFE, 0x8F, 0x10}},
};

/* The wrong key that locks: the third answers 36 and starts the delay */
constexpr uint8_t ECUIO_SIM_SECURITY_WRONG_MAX = 3;
/* The delay a lockout runs for, on the ECU's own clock */
constexpr int64_t ECUIO_SIM_SECURITY_DELAY_US = 10000000;

/* The DID that answers only once a level is unlocked (7F 22 33 before
 * it, in the extended session), two made-up bytes */
constexpr uint16_t ECUIO_SIM_GATED_DID = 0xD210;
constexpr uint8_t ECUIO_SIM_GATED_DID_DATA[2] = {0x4B, 0x2D};

/* The codes this header answers with */
constexpr uint8_t ECUIO_SIM_NRC_SUBFUNCTION = 0x12;
constexpr uint8_t ECUIO_SIM_NRC_LENGTH = 0x13;
constexpr uint8_t ECUIO_SIM_NRC_SEQUENCE = 0x24;
constexpr uint8_t ECUIO_SIM_NRC_DENIED = 0x33;
constexpr uint8_t ECUIO_SIM_NRC_INVALID_KEY = 0x35;
constexpr uint8_t ECUIO_SIM_NRC_ATTEMPTS = 0x36;
constexpr uint8_t ECUIO_SIM_NRC_DELAY = 0x37;
constexpr uint8_t ECUIO_SIM_NRC_SESSION = 0x7F;

/* One ECU's security state */
struct ecuio_sim_security {
    uint8_t seed_out = 0; /* the level whose seed is out, 0 for none */
    uint8_t unlocked = 0; /* the level unlocked, 0 while locked */
    uint8_t wrong = 0;    /* wrong keys counted toward the lockout */
    bool delayed = false; /* a lockout's delay runs until delay_until_us */
    int64_t delay_until_us = 0;
};

inline const ecuio_sim_security_level* ecuio_sim_security_find(uint8_t level)
{
    for (const ecuio_sim_security_level& l : ECUIO_SIM_SECURITY_LEVELS) {
        if (l.level == level) {
            return &l;
        }
    }
    return nullptr;
}

/* The key a seed of n bytes asks for: the made-up transform */
inline void ecuio_sim_security_key(const uint8_t* seed, uint32_t n,
                                   uint8_t* key)
{
    constexpr char word[] = "HAVNE";
    for (uint32_t i = 0; i < n; ++i) {
        key[i] = static_cast<uint8_t>(seed[n - 1 - i] ^
                                      static_cast<uint8_t>(word[i % 5]));
    }
}

/* The diagnostic session changed, or lapsed: the unlock and a seed out
 * go with it; the wrong keys counted and a delay under way stay */
inline void ecuio_sim_security_lock(ecuio_sim_security& s)
{
    s.seed_out = 0;
    s.unlocked = 0;
}

/* The ECU's answer to a 27 request (req[0] is 0x27, len at least 1) at
 * its time now_us, into out (18 bytes at most); the length. extended:
 * the node is in the extended session, the only one with security
 * access. The refusals in the order they are judged: outside the
 * extended session 7F (ISO 14229-1 judges the service in the active
 * session before a request's length); a request with no sub-function
 * 13; a sub-function that is no level's 12; a lockout's delay running
 * 37; a key with no seed out for its level 24; a seed request longer
 * than two bytes, or a key of the wrong length, 13; a wrong key 35, the
 * ECUIO_SIM_SECURITY_WRONG_MAX-th 36, which starts the delay (delay_us). After the delay one more wrong
 * key locks again; a right key zeroes the count. A seed asked for a
 * level already unlocked is all zeroes (ISO 14229-1). Only the seed
 * asked for last is out: a seed request, a zero one too, ends the wait
 * for another level's key. A key is asked for once: right or wrong, its
 * seed is spent. */
inline uint32_t ecuio_sim_security_answer(ecuio_sim_security& s, bool extended,
                                          int64_t delay_us, int64_t now_us,
                                          const uint8_t* req, uint32_t len,
                                          uint8_t* out)
{
    const auto refuse = [out](uint8_t nrc) -> uint32_t {
        out[0] = 0x7F;
        out[1] = 0x27;
        out[2] = nrc;
        return 3;
    };
    if (!extended) {
        return refuse(ECUIO_SIM_NRC_SESSION);
    }
    if (len < 2) {
        return refuse(ECUIO_SIM_NRC_LENGTH);
    }
    const uint8_t sub = req[1];
    const bool key = sub % 2 == 0;
    const ecuio_sim_security_level* l =
        sub == 0 ? nullptr
                 : ecuio_sim_security_find(
                       static_cast<uint8_t>(key ? sub - 1 : sub));
    if (l == nullptr) {
        return refuse(ECUIO_SIM_NRC_SUBFUNCTION);
    }
    if (s.delayed && now_us < s.delay_until_us) {
        return refuse(ECUIO_SIM_NRC_DELAY);
    }
    s.delayed = false;
    out[0] = 0x67;
    out[1] = sub;
    if (!key) {
        if (len != 2) {
            return refuse(ECUIO_SIM_NRC_LENGTH);
        }
        if (s.unlocked == l->level) {
            std::memset(out + 2, 0, l->bytes);
            s.seed_out = 0;
        } else {
            std::memcpy(out + 2, l->seed, l->bytes);
            s.seed_out = l->level;
        }
        return 2u + l->bytes;
    }
    if (s.seed_out != l->level) {
        return refuse(ECUIO_SIM_NRC_SEQUENCE);
    }
    if (len != 2u + l->bytes) {
        return refuse(ECUIO_SIM_NRC_LENGTH);
    }
    uint8_t want[sizeof l->seed];
    ecuio_sim_security_key(l->seed, l->bytes, want);
    s.seed_out = 0;
    if (std::memcmp(req + 2, want, l->bytes) == 0) {
        s.unlocked = l->level;
        s.wrong = 0;
        return 2;
    }
    if (++s.wrong < ECUIO_SIM_SECURITY_WRONG_MAX) {
        return refuse(ECUIO_SIM_NRC_INVALID_KEY);
    }
    s.wrong = ECUIO_SIM_SECURITY_WRONG_MAX - 1;
    s.delayed = true;
    s.delay_until_us = now_us + delay_us;
    return refuse(ECUIO_SIM_NRC_ATTEMPTS);
}

/* The gated DID's answer to 22 D2 10 into out, its length: its data once
 * a level is unlocked, 7F 22 33 while locked in the extended session;
 * 0 outside it, where the caller answers as for any DID it lacks */
inline uint32_t ecuio_sim_security_did(const ecuio_sim_security& s,
                                       bool extended, uint8_t* out)
{
    if (!extended) {
        return 0;
    }
    if (s.unlocked == 0) {
        out[0] = 0x7F;
        out[1] = 0x22;
        out[2] = ECUIO_SIM_NRC_DENIED;
        return 3;
    }
    out[0] = 0x62;
    out[1] = static_cast<uint8_t>(ECUIO_SIM_GATED_DID >> 8);
    out[2] = static_cast<uint8_t>(ECUIO_SIM_GATED_DID & 0xFF);
    std::memcpy(out + 3, ECUIO_SIM_GATED_DID_DATA,
                sizeof ECUIO_SIM_GATED_DID_DATA);
    return 3 + sizeof ECUIO_SIM_GATED_DID_DATA;
}

#endif /* ECUIO_SIM_SECURITY_H */
