// File/layer: core/src/sim_faults.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: UNIT: SimEcuTest, SimAskTest (test_obd2); fuzz_sim
//
// The simulated ECU's faults (ADR-0046, ADR-0041 section 4): which
// replies the URI's faults= breaks, how each is broken, and the
// grammar that reads them. A fault is every module's, each on its own
// count, or aimed at one module by its reply ID (ADR-0065 section 10).

#ifndef ECUIO_SIM_FAULTS_H
#define ECUIO_SIM_FAULTS_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

/* Every Nth reply of its kind, 0 = never (ADR-0041 section 4) */
struct ecuio_sim_faults {
    uint32_t silent = 0;
    uint32_t late = 0;
    uint32_t bad_sequence = 0; /* segmented replies only */
    uint32_t stalled = 0;      /* segmented replies only */
    uint32_t pending = 0;      /* 7F <service> 78 first, the reply later */
};

constexpr uint32_t ECUIO_SIM_FAULT_MAX = 10000;

/* A reply's fate */
enum class ecuio_sim_fate : uint8_t {
    intact,
    silent,
    late,
    bad_sequence,
    stalled,
    pending
};

/* Past the request engine's wait (100 ms), before the next request's */
constexpr int64_t ECUIO_SIM_LATE_US = 130000;
/* Past N_Cr (150 ms) after the frame before */
constexpr int64_t ECUIO_SIM_STALLED_US = 160000;
/* "Response pending": the reply this long after its 7F <service> 78,
 * past the request engine's 100 ms wait and an ELM327's AT ST timeout
 * (ATST19, 100 ms), well inside P2* (5 s), so only a tester that keeps
 * waiting after the 78 gets it */
constexpr int64_t ECUIO_SIM_PENDING_US = 300000;

/* A fault by its name in faults= */
enum class ecuio_sim_fault_kind : uint8_t {
    silent,
    late,
    bad_sequence,
    stalled,
    pending
};

/* One fault aimed at one module by its reply ID (faults=late:3@7EA) */
struct ecuio_sim_aim {
    uint32_t reply_id = 0;
    ecuio_sim_fault_kind kind = ecuio_sim_fault_kind::silent;
    uint32_t every = 0;
};

constexpr uint32_t ECUIO_SIM_AIMED_MAX = 32;

/* The faults aimed at modules, in the URI's order */
struct ecuio_sim_aims {
    uint32_t count = 0;
    ecuio_sim_aim items[ECUIO_SIM_AIMED_MAX];
};

/* One module's replies counted for its faults, single-frame and
 * segmented apart */
struct ecuio_sim_counts {
    uint32_t singles = 0;
    uint32_t segmented = 0;
};

inline const char* ecuio_sim_fault_name(ecuio_sim_fault_kind k)
{
    switch (k) {
    case ecuio_sim_fault_kind::silent:
        return "silent";
    case ecuio_sim_fault_kind::late:
        return "late";
    case ecuio_sim_fault_kind::bad_sequence:
        return "bad_sequence";
    case ecuio_sim_fault_kind::stalled:
        return "stalled";
    case ecuio_sim_fault_kind::pending:
        break;
    }
    return "pending";
}

inline uint32_t& ecuio_sim_fault_slot(ecuio_sim_faults& f,
                                      ecuio_sim_fault_kind k)
{
    switch (k) {
    case ecuio_sim_fault_kind::silent:
        return f.silent;
    case ecuio_sim_fault_kind::late:
        return f.late;
    case ecuio_sim_fault_kind::bad_sequence:
        return f.bad_sequence;
    case ecuio_sim_fault_kind::stalled:
        return f.stalled;
    case ecuio_sim_fault_kind::pending:
        break;
    }
    return f.pending;
}

/* The periods of the module answering on reply_id: every module's, each
 * fault aimed at it replacing that fault, a later aim an earlier one */
inline ecuio_sim_faults ecuio_sim_faults_for(ecuio_sim_faults all,
                                             const ecuio_sim_aims& aims,
                                             uint32_t reply_id)
{
    for (uint32_t i = 0; i < aims.count; ++i) {
        if (aims.items[i].reply_id == reply_id) {
            ecuio_sim_fault_slot(all, aims.items[i].kind) = aims.items[i].every;
        }
    }
    return all;
}

namespace ecuio_sim_fault_detail {

inline bool kind_of(const char* s, size_t n, ecuio_sim_fault_kind* k)
{
    for (uint8_t i = 0;
         i <= static_cast<uint8_t>(ecuio_sim_fault_kind::pending); ++i) {
        const auto kind = static_cast<ecuio_sim_fault_kind>(i);
        const char* name = ecuio_sim_fault_name(kind);
        if (n == std::strlen(name) && std::strncmp(s, name, n) == 0) {
            *k = kind;
            return true;
        }
    }
    return false;
}

inline int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* One item, <fault>:<n>[@<reply ID>], into *out, *aimed when it names a
 * module; false for any other shape */
inline bool parse_item(const char* s, size_t n, ecuio_sim_aim* out, bool* aimed)
{
    size_t at = 0;
    while (at < n && s[at] != ':') {
        ++at;
    }
    if (at == n || !kind_of(s, at, &out->kind)) {
        return false;
    }
    ++at;
    uint32_t every = 0;
    size_t digits = 0;
    for (; at < n && s[at] >= '0' && s[at] <= '9'; ++at, ++digits) {
        if (every > ECUIO_SIM_FAULT_MAX) {
            return false;
        }
        every = every * 10 + static_cast<uint32_t>(s[at] - '0');
    }
    if (digits == 0 || every == 0 || every > ECUIO_SIM_FAULT_MAX) {
        return false;
    }
    out->every = every;
    *aimed = at < n;
    if (!*aimed) {
        return true;
    }
    if (s[at] != '@') {
        return false;
    }
    uint32_t id = 0;
    size_t hex = 0;
    for (++at; at < n; ++at, ++hex) {
        const int v = hex_digit(s[at]);
        if (v < 0 || hex == 3) {
            return false;
        }
        id = id * 16 + static_cast<uint32_t>(v);
    }
    out->reply_id = id;
    return hex > 0 && id <= 0x7FFu;
}

} // namespace ecuio_sim_fault_detail

/* faults=<name>:<n>[@<reply ID>][,...]: every module's periods into
 * *out, the aimed ones joining *aims, so a second faults= key adds to the
 * first as the timing keys do (a later item in place of an earlier);
 * false with why (a line) */
inline bool ecuio_sim_parse_faults(const char* text, size_t len,
                                   ecuio_sim_faults* out, ecuio_sim_aims* aims,
                                   char* why, size_t why_cap)
{
    size_t at = 0;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != ',') {
            ++end;
        }
        const char* item = text + at;
        const size_t item_len = end - at;
        ecuio_sim_aim a;
        bool aimed = false;
        if (!ecuio_sim_fault_detail::parse_item(item, item_len, &a, &aimed)) {
            std::snprintf(why, why_cap,
                          "faults: '%.*s' isn't <fault>:<n>[@<reply ID>], the "
                          "fault one of silent, late, pending, bad_sequence or "
                          "stalled, n from 1 to %u and the reply ID 11-bit hex",
                          static_cast<int>(item_len < 40 ? item_len : 40), item,
                          ECUIO_SIM_FAULT_MAX);
            return false;
        }
        if (!aimed) {
            ecuio_sim_fault_slot(*out, a.kind) = a.every;
        } else if (aims->count == ECUIO_SIM_AIMED_MAX) {
            std::snprintf(why, why_cap,
                          "faults: more than %u faults aimed at a module",
                          ECUIO_SIM_AIMED_MAX);
            return false;
        } else {
            aims->items[aims->count++] = a;
        }
        at = end + 1;
    }
    if (len == 0 || text[len - 1] == ',') {
        std::snprintf(why, why_cap, "faults: names none");
        return false;
    }
    return true;
}

/* Whether every aim names a reply ID one of a sim's ECUs answers on
 * (ids, count of them); false with why naming the first that doesn't
 * and the first 8 of ids */
inline bool ecuio_sim_aims_land(const ecuio_sim_aims& aims, const uint32_t* ids,
                                uint32_t count, char* why, size_t why_cap)
{
    for (uint32_t i = 0; i < aims.count; ++i) {
        const ecuio_sim_aim& a = aims.items[i];
        bool lands = false;
        for (uint32_t k = 0; k < count && !lands; ++k) {
            lands = ids[k] == a.reply_id;
        }
        if (lands) {
            continue;
        }
        size_t n = 0;
        const auto add = [&](int wrote) {
            if (wrote > 0) {
                n += static_cast<size_t>(wrote);
            }
            if (n >= why_cap) {
                n = why_cap - 1;
            }
        };
        add(std::snprintf(why, why_cap,
                          "faults: %s:%u@%X aims at a module the simulated "
                          "car hasn't; it answers on",
                          ecuio_sim_fault_name(a.kind), a.every, a.reply_id));
        for (uint32_t k = 0; k < count && k < 8; ++k) {
            add(std::snprintf(why + n, why_cap - n, "%s %X", k == 0 ? "" : ",",
                              ids[k]));
        }
        if (count > 8) {
            add(std::snprintf(why + n, why_cap - n, " and %u more", count - 8));
        }
        return false;
    }
    return true;
}

#endif /* ECUIO_SIM_FAULTS_H */
