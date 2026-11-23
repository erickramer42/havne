// File/layer: core/src/sim_gateway.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimGatewayTest (test_obd2), fuzz_sim
//
// The simulated gateway's configuration (ADR-0046's appendix "a gateway
// node"): its IDs, the modules behind it, what it does with each class of
// service or service byte a module behind it is asked, its knobs, and the
// grammar of gateway=, behind=, route=, sa=, sa_delay= and gwopt=.
// Invented for test
// infrastructure and modeled on no vehicle; its default IDs are made up.

#ifndef ECUIO_SIM_GATEWAY_H
#define ECUIO_SIM_GATEWAY_H

#include "sim_faults.h"   /* ecuio_sim_fault_detail::hex_digit */
#include "sim_security.h" /* ecuio_sim_keying */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

/* gateway=1's pair: made up, the reply 8 above the request so an
 * ELM327's automatic receive filter takes it after AT SH 6F0 */
constexpr uint32_t ECUIO_SIM_GATEWAY_REQUEST_ID = 0x6F0u;
constexpr uint32_t ECUIO_SIM_GATEWAY_REPLY_ID = 0x6F8u;

constexpr uint32_t ECUIO_SIM_BEHIND_MAX = 32;
/* route= entries past the gateway's own table of classes */
constexpr uint32_t ECUIO_SIM_ROUTES_MAX = 64;
constexpr uint8_t ECUIO_SIM_GATEWAY_CLASSES = 5;

/* What the gateway does with a request for a module behind it; the
 * numbers are sim.h's ECUIO_SIM_ROUTE_* */
enum class ecuio_sim_route : uint8_t {
    forward = 1,
    refuse = 2, /* 7F <service> 22, or the entry's code */
    silent = 3,
    gated = 4,       /* refused 33 (or the entry's code) until unlocked */
    gated_silent = 5 /* silent until unlocked */
};

constexpr uint8_t ECUIO_SIM_ROUTE_REFUSE_NRC = 0x22;
/* securityAccessDenied, what the engine ECU's D210 answers while locked */
constexpr uint8_t ECUIO_SIM_ROUTE_GATED_NRC = 0x33;
/* sa_delay='s most, in ms */
constexpr int64_t ECUIO_SIM_SA_DELAY_MAX_MS = 600000;

/* A request's class by its service byte: reads 0, session 1, write 2,
 * prog 3, other 4 */
inline uint8_t ecuio_sim_gateway_class(uint8_t service)
{
    /* Each class's services, zero past its last */
    static const uint8_t kMembers[4][12] = {
        {0x01, 0x02, 0x03, 0x06, 0x07, 0x09, 0x0A, 0x19, 0x21, 0x22, 0x23,
         0x24},
        {0x10, 0x27, 0x28, 0x3E, 0x85},
        {0x04, 0x08, 0x14, 0x2C, 0x2E, 0x2F, 0x31, 0x3D},
        {0x11, 0x34, 0x35, 0x36, 0x37, 0x38}};
    for (uint8_t c = 0; c < 4 && service != 0; ++c) {
        for (const uint8_t member : kMembers[c]) {
            if (member == service) {
                return c;
            }
        }
    }
    return 4;
}

inline const char* ecuio_sim_gateway_class_name(uint8_t c)
{
    static const char* const kNames[ECUIO_SIM_GATEWAY_CLASSES] = {
        "reads", "session", "write", "prog", "other"};
    return c < ECUIO_SIM_GATEWAY_CLASSES ? kNames[c] : "";
}

inline const char* ecuio_sim_route_name(ecuio_sim_route r)
{
    switch (r) {
    case ecuio_sim_route::forward:
        return "forward";
    case ecuio_sim_route::refuse:
        return "refuse";
    case ecuio_sim_route::silent:
        return "silent";
    case ecuio_sim_route::gated:
        return "gated";
    case ecuio_sim_route::gated_silent:
        break;
    }
    return "gated-silent";
}

/* One route= entry outside the gateway's table of classes: a service
 * byte for the whole gateway, or a class or service for one module */
struct ecuio_sim_route_entry {
    bool service = false; /* key is a service byte, else a class */
    uint8_t key = 0;
    uint32_t reply_id = 0; /* the module's, 0 for the whole gateway */
    ecuio_sim_route outcome = ecuio_sim_route::forward;
    uint8_t nrc = 0; /* 0: the outcome's default */
};

struct ecuio_sim_gateway_config {
    bool given = false;        /* gateway= */
    bool default_pair = false; /* gateway=1 */
    uint32_t request_id = 0;
    uint32_t reply_id = 0;
    bool behind_given = false;
    uint32_t behind[ECUIO_SIM_BEHIND_MAX] = {};
    uint32_t behind_count = 0;
    bool route_given = false;
    /* Each class's outcome for the whole gateway, and its code */
    ecuio_sim_route cls[ECUIO_SIM_GATEWAY_CLASSES] = {
        ecuio_sim_route::forward, ecuio_sim_route::forward,
        ecuio_sim_route::forward, ecuio_sim_route::forward,
        ecuio_sim_route::forward};
    uint8_t cls_nrc[ECUIO_SIM_GATEWAY_CLASSES] = {};
    ecuio_sim_route_entry entries[ECUIO_SIM_ROUTES_MAX];
    uint32_t entry_count = 0;
    bool gwopt_given = false;
    bool aloud = false;        /* gwopt=aloud */
    bool unsuppressed = false; /* gwopt=unsuppressed */
    bool keep_any = false;     /* gwopt=keep-any */
    /* sa=: the gateway's security access, how its seeds and keys are
     * made, never (no key is right), and sa_delay= (0 for 10 s) */
    bool sa_given = false;
    ecuio_sim_keying keying;
    bool never = false;
    bool delay_given = false;
    int64_t delay_us = 0;
    /* faults=relock's period, the gateway's own (plan_sim), 0 for none */
    uint32_t relock = 0;
};

namespace ecuio_sim_gateway_detail {

/* An 11-bit ID of 1 to 3 hex digits */
inline bool id_of(const char* s, size_t n, uint32_t* out)
{
    uint32_t id = 0;
    if (n == 0 || n > 3) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const int v = ecuio_sim_fault_detail::hex_digit(s[i]);
        if (v < 0) {
            return false;
        }
        id = id * 16 + static_cast<uint32_t>(v);
    }
    *out = id;
    return id <= 0x7FFu;
}

/* Exactly two hex digits */
inline bool byte_of(const char* s, size_t n, uint8_t* out)
{
    if (n != 2) {
        return false;
    }
    const int hi = ecuio_sim_fault_detail::hex_digit(s[0]);
    const int lo = ecuio_sim_fault_detail::hex_digit(s[1]);
    if (hi < 0 || lo < 0) {
        return false;
    }
    *out = static_cast<uint8_t>(hi * 16 + lo);
    return true;
}

inline bool named(const char* s, size_t n, const char* name)
{
    return n == std::strlen(name) && std::strncmp(s, name, n) == 0;
}

/* Calls item(s, n) for each comma-separated item, false at the first it
 * refuses or at an empty one */
template <typename Item>
bool each_item(const char* text, size_t len, Item&& item)
{
    size_t at = 0;
    if (len == 0 || text[len - 1] == ',') {
        return false;
    }
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != ',') {
            ++end;
        }
        if (end == at || !item(text + at, end - at)) {
            return false;
        }
        at = end + 1;
    }
    return true;
}

inline int clip(size_t n)
{
    return static_cast<int>(n < 40 ? n : 40);
}

/* An outcome by its name in route= */
inline bool outcome_of(const char* s, size_t n, ecuio_sim_route* out)
{
    for (uint8_t i = 1;
         i <= static_cast<uint8_t>(ecuio_sim_route::gated_silent); ++i) {
        const auto r = static_cast<ecuio_sim_route>(i);
        if (named(s, n, ecuio_sim_route_name(r))) {
            *out = r;
            return true;
        }
    }
    return false;
}

/* A route= key: a class by name, or a request's service byte 01-BF */
inline bool aim_of(const char* s, size_t n, ecuio_sim_route_entry* e)
{
    for (uint8_t c = 0; c < ECUIO_SIM_GATEWAY_CLASSES; ++c) {
        if (named(s, n, ecuio_sim_gateway_class_name(c))) {
            e->service = false;
            e->key = c;
            return true;
        }
    }
    e->service = true;
    return byte_of(s, n, &e->key) && e->key >= 0x01 && e->key <= 0xBF;
}

/* One route= item, <aim>:<outcome>[.<NRC>][@<reply ID>]; false with why */
inline bool route_item(const char* s, size_t n, ecuio_sim_route_entry* e,
                       char* why, size_t why_cap)
{
    const char* colon = static_cast<const char*>(std::memchr(s, ':', n));
    const size_t at = colon != nullptr ? static_cast<size_t>(colon - s) : n;
    size_t end = at + 1;
    while (end < n && s[end] != '.' && s[end] != '@') {
        ++end;
    }
    if (colon == nullptr || !aim_of(s, at, e) ||
        !outcome_of(s + at + 1, end - at - 1, &e->outcome)) {
        std::snprintf(why, why_cap,
                      "route: '%.*s' isn't <class or service>:<outcome>"
                      "[.<NRC>][@<reply ID>], the class reads, session, "
                      "write, prog or other, or a service byte 01-BF in "
                      "hex, the outcome forward, refuse, silent, gated or "
                      "gated-silent",
                      clip(n), s);
        return false;
    }
    e->nrc = 0;
    if (end < n && s[end] == '.') {
        const size_t nrc_at = end + 1;
        end = nrc_at;
        while (end < n && s[end] != '@') {
            ++end;
        }
        if (!byte_of(s + nrc_at, end - nrc_at, &e->nrc) || e->nrc < 0x10 ||
            e->nrc > 0xFE || e->nrc == 0x78 ||
            (e->outcome != ecuio_sim_route::refuse &&
             e->outcome != ecuio_sim_route::gated)) {
            std::snprintf(why, why_cap,
                          "route: '%.*s': only refuse and gated take a code, "
                          ".<NRC>, two hex digits from 10 to FE but 78 (a "
                          "pending reply is faults=pending's)",
                          clip(n), s);
            return false;
        }
    }
    e->reply_id = 0;
    if (end < n &&
        (!id_of(s + end + 1, n - end - 1, &e->reply_id) || e->reply_id == 0)) {
        std::snprintf(why, why_cap,
                      "route: '%.*s': @ names a module behind the gateway "
                      "by its 11-bit reply ID in hex",
                      clip(n), s);
        return false;
    }
    return true;
}

} // namespace ecuio_sim_gateway_detail

/* gateway=<request ID>:<reply ID> (11-bit hex) or gateway=1 (6F0:6F8);
 * false with why. Neither ID 0x7DF, nor the two the same. */
inline bool ecuio_sim_parse_gateway(const char* text, size_t len,
                                    ecuio_sim_gateway_config* g, char* why,
                                    size_t why_cap)
{
    using namespace ecuio_sim_gateway_detail;
    g->given = true;
    g->default_pair = named(text, len, "1");
    if (g->default_pair) {
        g->request_id = ECUIO_SIM_GATEWAY_REQUEST_ID;
        g->reply_id = ECUIO_SIM_GATEWAY_REPLY_ID;
        return true;
    }
    const char* colon = static_cast<const char*>(std::memchr(text, ':', len));
    const size_t at =
        colon != nullptr ? static_cast<size_t>(colon - text) : len;
    if (colon == nullptr || !id_of(text, at, &g->request_id) ||
        !id_of(text + at + 1, len - at - 1, &g->reply_id)) {
        std::snprintf(why, why_cap,
                      "gateway: <request ID>:<reply ID>, each 11-bit hex "
                      "(gateway=6F0:6F8), or 1 for 6F0:6F8");
        return false;
    }
    if (g->request_id == 0x7DFu || g->reply_id == 0x7DFu ||
        g->request_id == g->reply_id) {
        std::snprintf(why, why_cap,
                      "gateway: %X:%X: the request and reply IDs differ, and "
                      "neither is 7DF, the functional request's",
                      g->request_id, g->reply_id);
        return false;
    }
    return true;
}

/* behind=<reply ID>[,...]: the modules behind the gateway, at most
 * ECUIO_SIM_BEHIND_MAX, each once; false with why */
inline bool ecuio_sim_parse_behind(const char* text, size_t len,
                                   ecuio_sim_gateway_config* g, char* why,
                                   size_t why_cap)
{
    using namespace ecuio_sim_gateway_detail;
    g->behind_given = true;
    g->behind_count = 0;
    bool full = false;
    bool twice = false;
    const bool ok = each_item(text, len, [&](const char* s, size_t n) {
        uint32_t id = 0;
        if (!id_of(s, n, &id) || id == 0) {
            return false;
        }
        for (uint32_t i = 0; i < g->behind_count; ++i) {
            twice = twice || g->behind[i] == id;
        }
        full = g->behind_count == ECUIO_SIM_BEHIND_MAX;
        if (twice || full) {
            return false;
        }
        g->behind[g->behind_count++] = id;
        return true;
    });
    if (!ok) {
        std::snprintf(why, why_cap,
                      twice  ? "behind: names a module twice"
                      : full ? "behind: more than 32 modules"
                             : "behind: the modules' reply IDs, 11-bit hex, "
                               "comma-separated (behind=7E9)");
    }
    return ok;
}

/* route=<class or service>:<outcome>[.<NRC>][@<reply ID>][,...]; a later
 * entry for the same aim replaces the earlier, a second route= key adds;
 * false with why */
inline bool ecuio_sim_parse_route(const char* text, size_t len,
                                  ecuio_sim_gateway_config* g, char* why,
                                  size_t why_cap)
{
    using namespace ecuio_sim_gateway_detail;
    g->route_given = true;
    bool said = false;
    const bool ok = each_item(text, len, [&](const char* s, size_t n) {
        ecuio_sim_route_entry e;
        said = !route_item(s, n, &e, why, why_cap);
        if (said) {
            return false;
        }
        if (!e.service && e.reply_id == 0) {
            g->cls[e.key] = e.outcome;
            g->cls_nrc[e.key] = e.nrc;
            return true;
        }
        for (uint32_t i = 0; i < g->entry_count; ++i) {
            ecuio_sim_route_entry& was = g->entries[i];
            if (was.service == e.service && was.key == e.key &&
                was.reply_id == e.reply_id) {
                was = e;
                return true;
            }
        }
        if (g->entry_count == ECUIO_SIM_ROUTES_MAX) {
            std::snprintf(why, why_cap,
                          "route: more than %u entries for a service or a "
                          "module",
                          ECUIO_SIM_ROUTES_MAX);
            said = true;
            return false;
        }
        g->entries[g->entry_count++] = e;
        return true;
    });
    if (!ok && !said) {
        std::snprintf(why, why_cap, "route: an empty entry");
    }
    return ok;
}

/* gwopt=aloud,unsuppressed (a comma list); false with why */
inline bool ecuio_sim_parse_gwopt(const char* text, size_t len,
                                  ecuio_sim_gateway_config* g, char* why,
                                  size_t why_cap)
{
    using namespace ecuio_sim_gateway_detail;
    g->gwopt_given = true;
    const bool ok = each_item(text, len, [&](const char* s, size_t n) {
        if (named(s, n, "aloud")) {
            g->aloud = true;
        } else if (named(s, n, "keep-any")) {
            g->keep_any = true;
        } else if (named(s, n, "unsuppressed")) {
            g->unsuppressed = true;
        } else {
            return false;
        }
        return true;
    });
    if (!ok) {
        std::snprintf(why, why_cap,
                      "gwopt: keep-any (any request let through keeps the "
                      "gateway's session), aloud (a withheld request on 7DF "
                      "refused for each module) and unsuppressed (with "
                      "aloud, ISO 14229-1's suppressed codes said too), "
                      "comma-separated");
    }
    return ok;
}

namespace ecuio_sim_gateway_detail {

/* Hex bytes, an even count of digits, into out (at most cap); their
 * count, 0 for any other shape */
inline uint8_t hex_bytes(const char* s, size_t n, uint8_t* out, size_t cap)
{
    if (n == 0 || n % 2 != 0 || n / 2 > cap) {
        return 0;
    }
    for (size_t i = 0; i < n; i += 2) {
        if (!byte_of(s + i, 2, &out[i / 2])) {
            return 0;
        }
    }
    return static_cast<uint8_t>(n / 2);
}

/* A delay in ms, 1 to ECUIO_SIM_SA_DELAY_MAX_MS, as microseconds */
inline bool delay_of(const char* s, size_t n, int64_t* us)
{
    int64_t ms = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9' || ms > ECUIO_SIM_SA_DELAY_MAX_MS) {
            return false;
        }
        ms = ms * 10 + (s[i] - '0');
    }
    *us = ms * 1000;
    return n > 0 && ms >= 1 && ms <= ECUIO_SIM_SA_DELAY_MAX_MS;
}

/* key:<seed hex>:<key hex>[,<seed hex>:<key hex>...]'s pairs, after
 * "key:" */
inline bool pairs_of(const char* text, size_t len, ecuio_sim_keying* k)
{
    k->pair_count = 0;
    return each_item(text, len, [k](const char* s, size_t n) {
        const char* colon = static_cast<const char*>(std::memchr(s, ':', n));
        if (colon == nullptr || k->pair_count == ECUIO_SIM_SECURITY_PAIRS_MAX) {
            return false;
        }
        ecuio_sim_security_pair& p = k->pairs[k->pair_count];
        const size_t at = static_cast<size_t>(colon - s);
        p.bytes = hex_bytes(s, at, p.seed, sizeof p.seed);
        const uint8_t key =
            hex_bytes(colon + 1, n - at - 1, p.key, sizeof p.key);
        if ((p.bytes != 4 && p.bytes != 16) || key != p.bytes) {
            return false;
        }
        ++k->pair_count;
        return true;
    });
}

} // namespace ecuio_sim_gateway_detail

/* sa=key | key:<seed hex>:<key hex>[,...] | xor:<hex> | add:<hex> |
 * never[:<delay ms>]: the gateway's security access; false with why */
inline bool ecuio_sim_parse_sa(const char* text, size_t len,
                               ecuio_sim_gateway_config* g, char* why,
                               size_t why_cap)
{
    using namespace ecuio_sim_gateway_detail;
    g->sa_given = true;
    g->never = false;
    g->keying = ecuio_sim_keying{};
    ecuio_sim_keying& k = g->keying;
    const auto starts = [&](const char* word) {
        const size_t n = std::strlen(word);
        return len > n && std::strncmp(text, word, n) == 0;
    };
    bool ok = true;
    if (named(text, len, "key")) {
        k.kind = ecuio_sim_keying_kind::made_up;
    } else if (named(text, len, "never")) {
        g->never = true;
    } else if (starts("never:")) {
        g->never = true;
        g->delay_given = true;
        ok = delay_of(text + 6, len - 6, &g->delay_us);
    } else if (starts("key:")) {
        k.kind = ecuio_sim_keying_kind::pairs;
        ok = pairs_of(text + 4, len - 4, &k);
    } else if (starts("xor:") || starts("add:")) {
        k.kind = text[0] == 'x' ? ecuio_sim_keying_kind::xor_mask
                                : ecuio_sim_keying_kind::add_mask;
        k.mask_len = hex_bytes(text + 4, len - 4, k.mask, sizeof k.mask);
        ok = k.mask_len != 0;
    } else {
        ok = false;
    }
    if (!ok) {
        std::snprintf(why, why_cap,
                      "sa: key (the simulated car's made-up transform), "
                      "key:<seed hex>:<key hex>[,...] (fixed pairs of 4 or "
                      "16 bytes each, at most %u), xor:<hex> or add:<hex> "
                      "(1 to 16 bytes over the seed), or never[:<delay ms>]",
                      ECUIO_SIM_SECURITY_PAIRS_MAX);
    }
    return ok;
}

/* sa_delay=<ms>: the lockout's delay, 1 to 600000; false with why */
inline bool ecuio_sim_parse_sa_delay(const char* text, size_t len,
                                     ecuio_sim_gateway_config* g, char* why,
                                     size_t why_cap)
{
    g->delay_given = true;
    if (!ecuio_sim_gateway_detail::delay_of(text, len, &g->delay_us)) {
        std::snprintf(why, why_cap,
                      "sa_delay: the lockout's delay in ms, 1 to %lld",
                      static_cast<long long>(ECUIO_SIM_SA_DELAY_MAX_MS));
        return false;
    }
    return true;
}

/* The gateway's keys together, once the URI is read: false with why */
inline bool ecuio_sim_gateway_judge(const ecuio_sim_gateway_config& g,
                                    char* why, size_t why_cap)
{
    if (!g.given && (g.behind_given || g.route_given || g.gwopt_given ||
                     g.sa_given || g.delay_given)) {
        std::snprintf(why, why_cap,
                      "behind, route, sa, sa_delay and gwopt are a "
                      "gateway's: they need gateway=");
        return false;
    }
    if (g.delay_given && !g.sa_given) {
        std::snprintf(why, why_cap,
                      "sa_delay is the lockout's delay of the gateway's "
                      "security access: it needs sa=");
        return false;
    }
    bool gated = false;
    for (const ecuio_sim_route r : g.cls) {
        gated = gated || r == ecuio_sim_route::gated ||
                r == ecuio_sim_route::gated_silent;
    }
    for (uint32_t i = 0; i < g.entry_count; ++i) {
        gated = gated || g.entries[i].outcome == ecuio_sim_route::gated ||
                g.entries[i].outcome == ecuio_sim_route::gated_silent;
    }
    if (gated && !g.sa_given) {
        std::snprintf(why, why_cap,
                      "route: a gated class needs the gateway's security "
                      "access, sa=");
        return false;
    }
    if (g.unsuppressed && !g.aloud) {
        std::snprintf(why, why_cap,
                      "gwopt: unsuppressed says ISO 14229-1's suppressed "
                      "codes aloud: it needs aloud");
        return false;
    }
    for (uint32_t i = 0; i < g.entry_count; ++i) {
        const uint32_t id = g.entries[i].reply_id;
        bool behind = id == 0;
        for (uint32_t k = 0; k < g.behind_count && !behind; ++k) {
            behind = g.behind[k] == id;
        }
        if (!behind) {
            std::snprintf(why, why_cap,
                          "route: @%X names no module behind the gateway "
                          "(behind=)",
                          id);
            return false;
        }
    }
    return true;
}

/* Whether the gateway's IDs are none of the sim's other nodes' (their
 * request and reply IDs, count each) and each module behind it answers
 * on one of reply_ids but the engine ECU's; false with why */
inline bool ecuio_sim_gateway_lands(const ecuio_sim_gateway_config& g,
                                    const uint32_t* request_ids,
                                    const uint32_t* reply_ids, uint32_t count,
                                    char* why, size_t why_cap)
{
    if (!g.given) {
        return true;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t ids[2] = {request_ids[i], reply_ids[i]};
        for (const uint32_t id : ids) {
            if (id != g.request_id && id != g.reply_id) {
                continue;
            }
            std::snprintf(why, why_cap,
                          g.default_pair
                              ? "gateway=1's %X:%X meets a module's ID %X: "
                                "give the gateway's IDs (gateway=<request "
                                "ID>:<reply ID>)"
                              : "gateway: %X:%X meets a module's ID %X",
                          g.request_id, g.reply_id, id);
            return false;
        }
    }
    for (uint32_t b = 0; b < g.behind_count; ++b) {
        const uint32_t id = g.behind[b];
        bool lands = false;
        for (uint32_t i = 1; i < count && !lands; ++i) {
            lands = reply_ids[i] == id;
        }
        if (lands) {
            continue;
        }
        size_t n = 0;
        const auto add = [&](int wrote) {
            n += wrote > 0 ? static_cast<size_t>(wrote) : 0;
            n = n >= why_cap ? why_cap - 1 : n;
        };
        add(std::snprintf(why, why_cap,
                          id == reply_ids[0]
                              ? "behind: the engine ECU (%X) can't be behind "
                                "the gateway; the modules answer on"
                              : "behind: %X is no module of the simulated "
                                "car's; the modules answer on",
                          id));
        for (uint32_t i = 1; i < count && i <= 8; ++i) {
            add(std::snprintf(why + n, why_cap - n, "%s %X", i == 1 ? "" : ",",
                              reply_ids[i]));
        }
        if (count == 1) {
            add(std::snprintf(why + n, why_cap - n,
                              " none (modules=2 or profile= adds them)"));
        } else if (count > 9) {
            add(std::snprintf(why + n, why_cap - n, " and %u more", count - 9));
        }
        return false;
    }
    return true;
}

/* Whether faults=relock is the gateway's alone (with no @, or aimed at
 * its reply ID), and its period into g->relock; false with why */
inline bool ecuio_sim_gateway_relock(ecuio_sim_gateway_config* g,
                                     const ecuio_sim_faults& all,
                                     const ecuio_sim_aims& aims, char* why,
                                     size_t why_cap)
{
    bool aimed = false;
    for (uint32_t i = 0; i < aims.count; ++i) {
        const ecuio_sim_aim& a = aims.items[i];
        if (a.kind != ecuio_sim_fault_kind::relock) {
            continue;
        }
        aimed = true;
        if (g->given && a.reply_id != g->reply_id) {
            std::snprintf(why, why_cap,
                          "faults: relock:%u@%X aims at a module: relock is "
                          "the gateway's alone (%X)",
                          a.every, a.reply_id, g->reply_id);
            return false;
        }
    }
    if (!g->given && (aimed || all.relock != 0)) {
        std::snprintf(why, why_cap,
                      "faults: relock locks a gateway again: it needs "
                      "gateway=");
        return false;
    }
    g->relock =
        g->given ? ecuio_sim_faults_for(all, aims, g->reply_id).relock : 0;
    return true;
}

/* What the gateway does with service asked of the module on reply_id,
 * its code in *nrc (0 for the outcome's default): a service entry for
 * the module, then a class entry for it, then a service entry for the
 * whole gateway, then the gateway's class */
inline ecuio_sim_route
ecuio_sim_gateway_outcome(const ecuio_sim_gateway_config& g, uint32_t reply_id,
                          uint8_t service, uint8_t* nrc)
{
    const uint8_t c = ecuio_sim_gateway_class(service);
    const ecuio_sim_route_entry* best = nullptr;
    int rank = 3;
    for (uint32_t i = 0; i < g.entry_count; ++i) {
        const ecuio_sim_route_entry& e = g.entries[i];
        const bool hits = e.service ? e.key == service : e.key == c;
        if (!hits || (e.reply_id != 0 && e.reply_id != reply_id)) {
            continue;
        }
        const int r = e.reply_id != 0 ? (e.service ? 0 : 1) : 2;
        if (r < rank) {
            rank = r;
            best = &e;
        }
    }
    if (best != nullptr) {
        *nrc = best->nrc;
        return best->outcome;
    }
    *nrc = g.cls_nrc[c];
    return g.cls[c];
}

#endif /* ECUIO_SIM_GATEWAY_H */
