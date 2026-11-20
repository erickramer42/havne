// File/layer: core/src/security_access.h | internal, C++17, Qt-free, every
//             platform; pure: no clock, no file, no bus
// Testing: UNIT: SecurityAccessTest (test_obd2)
//
// One module's security-access attempts (ADR-0066 section 2.3): which
// level is asked, what each end of a seed and of a key counts, and how
// long the module is left alone after a failure. It sends nothing and
// reads no clock: the conversation tells it each step with the session's
// time, and it answers what to do next.
//
// The rules:
//   - One attempt at a time, and one ask is one attempt: nothing here
//     asks again.
//   - Every end of a key that was offered, but its positive reply, is one
//     failed attempt and starts the level's delay: a refusal of any code,
//     no answer, and a write that failed (the key may have reached the
//     module). A key whose window ended before its turn never touched the
//     bus and counts nothing.
//   - A seed's ends count nothing; 7F 27 36 and 7F 27 37 (the module's
//     own delay runs) start the delay here too.
//   - A level unlocked stays so until the session is lost.
//   - An attempt that ends before its key is offered counts nothing.
//
// Counting once too many only makes the next ask wait; once too few lets
// a module's own lockout come closer unseen. So every doubt counts.
//
// The conversation reads a talk's end before it tells of a lost session,
// so a key's own end decides wherever there is one; a session lost with a
// key still on its way counts that key as failed.

#ifndef ECUIO_SECURITY_ACCESS_H
#define ECUIO_SECURITY_ACCESS_H

#include "ecuio/plugin.h"
#include "request_talk.h"

#include <cstdint>

namespace ecuio_security {

/* The most levels one module's machine holds: a manifest's whole list */
constexpr uint32_t kMaxLevels = ECUIO_PLUGIN_MAX_LEVELS;

/* The negative replies that say the module's own delay runs: its
 * attempts are used up, or the delay hasn't passed */
constexpr uint8_t kNrcExceededAttempts = 0x36;
constexpr uint8_t kNrcDelayNotExpired = 0x37;

/* The UTC seconds a stored failure's age is short by at most: both ends
 * are whole seconds, each cut down */
constexpr int64_t kStoredSecondSlackUs = 1000000;

/* One level of one module: what the manifest declares, and its state */
struct level_state {
    uint8_t level = 0;         /* the seed request's sub-function, odd */
    uint8_t seed_bytes = 0;    /* the seed's declared length */
    uint8_t key_bytes = 0;     /* the key's */
    bool unlocked = false;     /* until the session is lost */
    uint32_t failed = 0;       /* failed attempts since the last right key */
    int64_t delay_us = 0;      /* the least wait after a failure */
    int64_t window_us = 0;     /* the seed's window_us for its key */
    int64_t wait_until_us = 0; /* session time the delay ends; 0: none */
};

/* Where the attempt in flight stands */
enum class phase : uint8_t {
    idle,       /* none */
    seed_asked, /* ask said go: the seed is offered or out */
    key_wanted, /* the seed came: the module's code computes the key */
    key_sent,   /* the key is offered or out: its end counts */
};

struct module_access {
    level_state levels[kMaxLevels];
    uint32_t count = 0;
    phase at = phase::idle;
    uint32_t in_flight = 0; /* the level's index while at isn't idle */
};

/* Adds a level as its manifest declares it (delay and window in
 * milliseconds); false when the machine is full or holds the level, and
 * for a seed or a key of no bytes (a seed of none would read as all
 * zeroes: unlocked) */
inline bool add_level(module_access& a, uint8_t level, uint8_t seed_bytes,
                      uint8_t key_bytes, uint32_t delay_ms, uint32_t window_ms)
{
    if (a.count >= kMaxLevels || seed_bytes == 0 || key_bytes == 0) {
        return false;
    }
    for (uint32_t k = 0; k < a.count; ++k) {
        if (a.levels[k].level == level) {
            return false;
        }
    }
    level_state& l = a.levels[a.count++];
    l.level = level;
    l.seed_bytes = seed_bytes;
    l.key_bytes = key_bytes;
    l.delay_us = int64_t{delay_ms} * 1000;
    l.window_us = int64_t{window_ms} * 1000;
    return true;
}

/* The index of a level, or count when the machine holds none such */
inline uint32_t find_level(const module_access& a, uint8_t level)
{
    for (uint32_t k = 0; k < a.count; ++k) {
        if (a.levels[k].level == level) {
            return k;
        }
    }
    return a.count;
}

/* How much of a delay of delay_us is left at utc_now for a failure the
 * store dates utc_last (whole seconds, 0: none): never under 0 and never
 * over the whole delay, so a clock set back costs one delay at most. The
 * age is taken a second short, since both seconds are cut down. */
inline int64_t stored_delay_left_us(int64_t delay_us, int64_t utc_last,
                                    int64_t utc_now)
{
    if (utc_last == 0) {
        return 0;
    }
    if (utc_now < utc_last) {
        return delay_us;
    }
    /* Past the delay and its second: none left, and no product to
     * overflow. Up to there the age is the delay at most. */
    if (utc_now - utc_last > delay_us / 1000000 + 1) {
        return 0;
    }
    const int64_t age_us =
        (utc_now - utc_last) * 1000000 - kStoredSecondSlackUs;
    const int64_t left = delay_us - age_us;
    return left > delay_us ? delay_us : left;
}

/* A level's count and delay as the store kept them, at session time
 * now_us: failed attempts, and the delay left by the UTC seconds. False,
 * and nothing changes, for an index the machine doesn't hold (find_level's
 * answer for no such level). */
inline bool restore_level(module_access& a, uint32_t index, uint32_t failed,
                          int64_t utc_last, int64_t utc_now, int64_t now_us)
{
    if (index >= a.count) {
        return false;
    }
    level_state& l = a.levels[index];
    l.failed = failed;
    const int64_t left = stored_delay_left_us(l.delay_us, utc_last, utc_now);
    l.wait_until_us = left > 0 ? now_us + left : 0;
    return true;
}

/* What ask answers. All but go send nothing and start no attempt. */
enum class ask_answer : uint8_t {
    go,           /* the seed may be offered: index and window_us say which */
    no_level,     /* the module declares no such level */
    key_too_long, /* the key's request is past this transport's bound */
    busy,         /* an attempt is under way */
    delayed,      /* the delay runs: left_us of it */
    unlocked,     /* the level is unlocked in this session */
};

struct asked {
    ask_answer answer = ask_answer::no_level;
    uint32_t index = 0;
    int64_t left_us = 0;
};

/* The caller's word for level at now_us; pdu_max is the transport's bound
 * for one request on this channel, which the key's (27, its sub-function,
 * key_bytes) must fit. The refusals are checked in the enum's order. */
inline asked ask(module_access& a, uint8_t level, uint32_t pdu_max,
                 int64_t now_us)
{
    asked r;
    r.index = find_level(a, level);
    if (r.index >= a.count) {
        r.answer = ask_answer::no_level;
        return r;
    }
    level_state& l = a.levels[r.index];
    if (uint32_t{2} + l.key_bytes > pdu_max) {
        r.answer = ask_answer::key_too_long;
    } else if (a.at != phase::idle) {
        r.answer = ask_answer::busy;
    } else if (now_us < l.wait_until_us) {
        r.answer = ask_answer::delayed;
        r.left_us = l.wait_until_us - now_us;
    } else if (l.unlocked) {
        r.answer = ask_answer::unlocked;
    } else {
        r.answer = ask_answer::go;
        l.wait_until_us = 0;
        a.at = phase::seed_asked;
        a.in_flight = r.index;
    }
    return r;
}

/* What the seed's end comes to */
enum class seed_answer : uint8_t {
    key,      /* hand the seed to the module's code for its key */
    unlocked, /* all zeroes: unlocked already, no key; the conversation
                 ends the channel's window */
    ended,    /* the attempt is over, nothing counted */
    no_seed,  /* no seed was asked: nothing changes */
};

/* The seed talk settled at now_us: its end and code, and for a positive
 * reply the seed's bytes (what follows 67 and the sub-function) */
inline seed_answer seed(module_access& a, ecuio_talk_end end, uint8_t nrc,
                        const uint8_t* bytes, uint32_t len, int64_t now_us)
{
    if (a.at != phase::seed_asked) {
        return seed_answer::no_seed;
    }
    level_state& l = a.levels[a.in_flight];
    a.at = phase::idle;
    if (end != ecuio_talk_end::answered) {
        if (end == ecuio_talk_end::refused &&
            (nrc == kNrcExceededAttempts || nrc == kNrcDelayNotExpired)) {
            l.wait_until_us = now_us + l.delay_us;
        }
        return seed_answer::ended;
    }
    if (len != l.seed_bytes || bytes == nullptr) {
        return seed_answer::ended;
    }
    bool zero = true;
    for (uint32_t k = 0; k < len; ++k) {
        zero = zero && bytes[k] == 0;
    }
    if (zero) {
        l.unlocked = true;
        return seed_answer::unlocked;
    }
    a.at = phase::key_wanted;
    return seed_answer::key;
}

/* The module's code returned a key of len bytes: true when it is the
 * declared length and may be offered. Any other length ends the attempt
 * with nothing sent and nothing counted. */
inline bool key(module_access& a, uint32_t len)
{
    if (a.at != phase::key_wanted) {
        return false;
    }
    if (len != a.levels[a.in_flight].key_bytes) {
        a.at = phase::idle;
        return false;
    }
    return true;
}

/* The door took the key: from here its end counts */
inline void key_offered(module_access& a)
{
    if (a.at == phase::key_wanted) {
        a.at = phase::key_sent;
    }
}

/* The attempt ends before its key was offered (the seed's offer refused,
 * a fault in the module's code, the door refusing the key): nothing
 * counted. False, and nothing changes, once the key is offered: only its
 * end closes the attempt then. */
inline bool drop(module_access& a)
{
    if (a.at == phase::key_sent) {
        return false;
    }
    a.at = phase::idle;
    return true;
}

/* One failed attempt on l at now_us: counted, never past the count's
 * range, and the delay runs from now */
inline void count_failure(level_state& l, int64_t now_us)
{
    if (l.failed < UINT32_MAX) {
        ++l.failed;
    }
    l.wait_until_us = now_us + l.delay_us;
}

/* What the key's end comes to */
enum class key_answer : uint8_t {
    unlocked,    /* 67: the level is open, its count zero */
    failed,      /* one failed attempt, the delay runs */
    not_counted, /* the key never touched the bus */
    no_key,      /* no key was offered: nothing changes */
};

/* The key talk settled at now_us */
inline key_answer key_end(module_access& a, ecuio_talk_end end, int64_t now_us)
{
    if (a.at != phase::key_sent || end == ecuio_talk_end::none) {
        return key_answer::no_key;
    }
    level_state& l = a.levels[a.in_flight];
    a.at = phase::idle;
    if (end == ecuio_talk_end::answered) {
        l.unlocked = true;
        l.failed = 0; /* no delay runs: ask ended it at go */
        return key_answer::unlocked;
    }
    if (end == ecuio_talk_end::lapsed) {
        return key_answer::not_counted;
    }
    count_failure(l, now_us);
    return key_answer::failed;
}

/* The channel's session was entered again at now_us: every level is
 * locked, and the attempt in flight ends. True when that counted a
 * failure: a key was on its way and its end hadn't been told. */
inline bool session_lost(module_access& a, int64_t now_us)
{
    for (uint32_t k = 0; k < a.count; ++k) {
        a.levels[k].unlocked = false;
    }
    const bool counted = a.at == phase::key_sent;
    if (counted) {
        count_failure(a.levels[a.in_flight], now_us);
    }
    a.at = phase::idle;
    return counted;
}

} // namespace ecuio_security

#endif /* ECUIO_SECURITY_ACCESS_H */
