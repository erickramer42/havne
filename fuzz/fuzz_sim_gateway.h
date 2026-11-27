#ifndef FUZZ_SIM_GATEWAY_H
#define FUZZ_SIM_GATEWAY_H

#include "sim_ecu.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace fuzz_sim_gateway {

inline bool is_gated(ecuio_sim_route r)
{
    return r == ecuio_sim_route::gated || r == ecuio_sim_route::gated_silent;
}

inline bool nrc_ok(uint8_t nrc, ecuio_sim_route r)
{
    return nrc == 0 ||
           (nrc >= 0x10 && nrc <= 0xFE && nrc != 0x78 &&
            (r == ecuio_sim_route::refuse || r == ecuio_sim_route::gated));
}

inline void check_keying(const ecuio_sim_gateway_config& g)
{
    const ecuio_sim_keying& k = g.keying;
    switch (k.kind) {
    case ecuio_sim_keying_kind::made_up:
        break;
    case ecuio_sim_keying_kind::pairs:
        if (k.pair_count == 0 || k.pair_count > ECUIO_SIM_SECURITY_PAIRS_MAX) {
            abort();
        }
        for (uint32_t i = 0; i < k.pair_count; ++i) {
            if (k.pairs[i].bytes != 4 && k.pairs[i].bytes != 16) {
                abort();
            }
        }
        break;
    case ecuio_sim_keying_kind::xor_mask:
    case ecuio_sim_keying_kind::add_mask:
        if (k.mask_len == 0 || k.mask_len > sizeof k.mask) {
            abort();
        }
        break;
    }
    if (g.delay_us < 0 || g.delay_us > ECUIO_SIM_SA_DELAY_MAX_MS * 1000 ||
        (g.delay_given && (!g.sa_given || g.delay_us == 0)) ||
        (!g.sa_given &&
         (g.never || k.kind != ecuio_sim_keying_kind::made_up))) {
        abort();
    }
}

inline void check_entries(const ecuio_sim_gateway_config& g)
{
    for (uint8_t c = 0; c < ECUIO_SIM_GATEWAY_CLASSES; ++c) {
        if (!nrc_ok(g.cls_nrc[c], g.cls[c]) ||
            (is_gated(g.cls[c]) && !g.sa_given)) {
            abort();
        }
    }
    for (uint32_t i = 0; i < g.entry_count; ++i) {
        const ecuio_sim_route_entry& e = g.entries[i];
        bool behind = e.reply_id == 0;
        for (uint32_t b = 0; b < g.behind_count; ++b) {
            behind = behind || g.behind[b] == e.reply_id;
        }
        if (!behind || !nrc_ok(e.nrc, e.outcome) ||
            (is_gated(e.outcome) && !g.sa_given) ||
            (e.service ? e.key < 0x01 || e.key > 0xBF
                       : e.key >= ECUIO_SIM_GATEWAY_CLASSES)) {
            abort();
        }
    }
}

inline void check_config(const ecuio_sim_config& config)
{
    const ecuio_sim_gateway_config& g = config.gateway;
    if (!g.given) {
        if (g.behind_count != 0 || g.entry_count != 0 || g.sa_given) {
            abort();
        }
        return;
    }
    if (g.request_id == 0 || g.reply_id == 0 || g.request_id > 0x7FFu ||
        g.reply_id > 0x7FFu || g.request_id == g.reply_id ||
        g.request_id == 0x7DFu || g.reply_id == 0x7DFu ||
        g.behind_count > ECUIO_SIM_BEHIND_MAX ||
        g.entry_count > ECUIO_SIM_ROUTES_MAX || (g.unsuppressed && !g.aloud)) {
        abort();
    }
    for (uint32_t b = 0; b < g.behind_count; ++b) {
        for (uint32_t c = 0; c < b; ++c) {
            if (g.behind[b] > 0x7FFu || g.behind[b] == g.behind[c]) {
                abort();
            }
        }
    }
    check_keying(g);
    check_entries(g);
}

struct model {
    int64_t now_us = 0;
    int64_t until_us = 0;
    uint64_t gated = 0;
    uint64_t routed = 0;
    uint64_t unlocks = 0;
    uint64_t too_early = 0;
    uint32_t turn[2] = {};
    uint32_t pair_out = 0;
    uint32_t module = 0;
};

inline bool lapses(const ecuio_sim_session& s, int64_t now_us)
{
    return (s.extended || s.programming) &&
           now_us - s.heard_us >= ECUIO_SIM_S3_US;
}

inline bool suppressed(uint8_t nrc)
{
    static const uint8_t kCodes[] = {0x11, 0x12, 0x31, 0x7E, 0x7F};
    return memchr(kCodes, nrc, sizeof kCodes) != nullptr;
}

inline void check_verdict(const ecuio_sim_gateway_config& g, ecuio_sim_route r,
                          uint8_t nrc, bool open, bool functional,
                          uint8_t service, const ecuio_sim_gateway_verdict& v)
{
    using fate = ecuio_sim_gateway_fate;
    fate want = fate::drop;
    uint8_t code = nrc;
    switch (r) {
    case ecuio_sim_route::forward:
        want = fate::forward;
        break;
    case ecuio_sim_route::gated:
        want = open ? fate::forward : fate::withhold;
        code = nrc != 0 ? nrc : 0x33;
        break;
    case ecuio_sim_route::gated_silent:
        want = open ? fate::forward : fate::drop;
        break;
    case ecuio_sim_route::refuse:
        want = fate::withhold;
        code = nrc != 0 ? nrc : 0x22;
        break;
    case ecuio_sim_route::silent:
        break;
    }
    if (want == fate::withhold && functional &&
        (!g.aloud || (suppressed(code) && !g.unsuppressed))) {
        want = fate::drop;
    }
    if (v.fate != want ||
        (want == fate::withhold &&
         (v.out[0] != 0x7F || v.out[1] != service || v.out[2] != code))) {
        abort();
    }
}

inline void route_one(const ecuio_sim_gateway_config& g, ecuio_sim_session& gw,
                      ecuio_sim_gateway_tally& t, model& m, uint32_t id,
                      const uint8_t* req, bool functional, std::string* out)
{
    const ecuio_sim_session was = gw;
    const bool lapsed = lapses(was, m.now_us);
    const bool unlocked = !lapsed && was.security.unlocked != 0;
    uint8_t nrc = 0;
    const ecuio_sim_route r = ecuio_sim_gateway_outcome(g, id, req[0], &nrc);
    const bool counted = is_gated(r) && unlocked;
    m.gated += counted ? 1 : 0;
    const bool relocks = counted && g.relock != 0 && m.gated % g.relock == 0;
    const ecuio_sim_gateway_verdict v =
        ecuio_sim_gateway_route(g, gw, &t.gated, id, req, functional, m.now_us);
    ecuio_sim_gateway_count_route(t, v, was.security.unlocked != 0, gw);
    ++m.routed;
    check_verdict(g, r, nrc, unlocked && !relocks, functional, req[0], v);
    const bool locks = lapsed || relocks;
    const bool heard = g.keep_any && v.fate == ecuio_sim_gateway_fate::forward;
    if (gw.security.unlocked != (locks ? 0 : was.security.unlocked) ||
        gw.security.seed_out != (locks ? 0 : was.security.seed_out) ||
        gw.security.wrong != was.security.wrong ||
        gw.security.delayed != was.security.delayed ||
        gw.extended != (!lapsed && was.extended) ||
        gw.heard_us != (heard ? m.now_us : was.heard_us) ||
        t.gated != m.gated) {
        abort();
    }
    out->push_back(static_cast<char>(v.fate));
    out->append(reinterpret_cast<const char*>(v.out), sizeof v.out);
}

inline uint32_t pair_for(const ecuio_sim_keying& k, uint8_t bytes, uint32_t nth)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < k.pair_count; ++i) {
        count += k.pairs[i].bytes == bytes ? 1 : 0;
    }
    for (uint32_t i = 0, n = 0; i < k.pair_count && count != 0; ++i) {
        if (k.pairs[i].bytes == bytes && n++ == nth % count) {
            return i;
        }
    }
    return k.pair_count;
}

inline void key_for(const ecuio_sim_keying& k,
                    const ecuio_sim_security_level& l, uint32_t pair,
                    uint8_t* want)
{
    switch (k.kind) {
    case ecuio_sim_keying_kind::made_up:
        ecuio_sim_security_key(l.seed, l.bytes, want);
        return;
    case ecuio_sim_keying_kind::pairs:
        memcpy(want, k.pairs[pair].key, l.bytes);
        return;
    case ecuio_sim_keying_kind::xor_mask:
    case ecuio_sim_keying_kind::add_mask:
        break;
    }
    for (uint32_t i = 0; i < l.bytes; ++i) {
        const uint8_t mask = k.mask[i % k.mask_len];
        want[i] = static_cast<uint8_t>(k.kind == ecuio_sim_keying_kind::add_mask
                                           ? l.seed[i] + mask
                                           : l.seed[i] ^ mask);
    }
}

inline void check_seed(const ecuio_sim_keying& k,
                       const ecuio_sim_security_level& l, uint8_t held,
                       const ecuio_sim_session& gw, model& m,
                       const uint8_t* seed)
{
    uint8_t want[sizeof l.seed] = {};
    if (held != l.level) {
        if (k.kind == ecuio_sim_keying_kind::pairs) {
            m.pair_out =
                pair_for(k, l.bytes, m.turn[l.level == 0x01 ? 0 : 1]++);
            memcpy(want, k.pairs[m.pair_out].seed, l.bytes);
        } else {
            memcpy(want, l.seed, l.bytes);
        }
    }
    if (memcmp(seed, want, l.bytes) != 0 ||
        gw.security.seed_out != (held == l.level ? 0 : l.level)) {
        abort();
    }
}

inline void check_refused_27(const ecuio_sim_gateway_config& g,
                             const ecuio_sim_session& gw, model& m, uint8_t nrc)
{
    if (nrc == ECUIO_SIM_NRC_DELAY) {
        if (m.now_us >= m.until_us) {
            abort();
        }
        ++m.too_early;
    }
    if (nrc == ECUIO_SIM_NRC_ATTEMPTS) {
        m.until_us = m.now_us + (g.delay_us != 0 ? g.delay_us
                                                 : ECUIO_SIM_SECURITY_DELAY_US);
        if (gw.security.delay_until_us != m.until_us) {
            abort();
        }
    }
}

inline void check_27(const ecuio_sim_gateway_config& g,
                     const ecuio_sim_session& was, const ecuio_sim_session& gw,
                     model& m, const uint8_t* req, size_t len,
                     const uint8_t* reply, uint32_t n)
{
    const bool lapsed = lapses(was, m.now_us);
    const bool in = was.extended && !lapsed;
    const uint8_t held = lapsed ? 0 : was.security.unlocked;
    const uint8_t sub = len >= 2 ? req[1] : 0;
    const ecuio_sim_security_level* l =
        sub == 0 ? nullptr
                 : ecuio_sim_security_find(
                       static_cast<uint8_t>(sub % 2 == 0 ? sub - 1 : sub));
    const bool level = l != nullptr &&
                       (g.keying.kind != ecuio_sim_keying_kind::pairs ||
                        pair_for(g.keying, l->bytes, 0) != g.keying.pair_count);
    const bool refused = n == 3 && reply[0] == 0x7F;
    if (in && level && m.now_us < m.until_us &&
        !(refused && reply[2] == ECUIO_SIM_NRC_DELAY)) {
        abort();
    }
    uint8_t want[sizeof ecuio_sim_security_level::seed] = {};
    const bool asked = in && level && sub % 2 == 0 &&
                       (lapsed ? 0 : was.security.seed_out) == l->level &&
                       len == 2u + l->bytes;
    if (asked) {
        key_for(g.keying, *l, m.pair_out, want);
    }
    const bool right = asked && memcmp(req + 2, want, l->bytes) == 0;
    if (refused) {
        if (right && !g.never && m.now_us >= m.until_us) {
            abort();
        }
        check_refused_27(g, gw, m, reply[2]);
        return;
    }
    if (n < 2 || !in || !level || reply[1] != sub) {
        abort();
    }
    if (sub % 2 != 0) {
        check_seed(g.keying, *l, held, gw, m, reply + 2);
        return;
    }
    if (g.never || n != 2 || !right || gw.security.unlocked != l->level) {
        abort();
    }
    ++m.unlocks;
}

inline void answer_one(const ecuio_sim_gateway_config& g, ecuio_sim_session& gw,
                       ecuio_sim_gateway_tally& t, model& m, const uint8_t* req,
                       size_t len, std::string* out)
{
    const ecuio_sim_session was = gw;
    uint8_t reply[ECUIO_SIM_REPLY_MAX + 16];
    memset(reply, 0xEE, sizeof reply);
    const uint32_t n = ecuio_sim_gateway_answer(
        g, gw, m.now_us, req, static_cast<uint32_t>(len), reply);
    ecuio_sim_gateway_count_answer(t, was.security.unlocked != 0, gw, req,
                                   static_cast<uint32_t>(len), reply, n);
    if (n > ECUIO_SIM_REPLY_MAX || reply[ECUIO_SIM_REPLY_MAX] != 0xEE ||
        (n > 0 && (reply[0] == 0x7F ? n != 3 || reply[1] != req[0]
                                    : reply[0] != (req[0] | 0x40))) ||
        gw.heard_us != m.now_us ||
        (n > 0 && reply[0] == 0x50 && gw.security.unlocked != 0)) {
        abort();
    }
    const bool own =
        req[0] == 0x10 || req[0] == 0x3E || (req[0] == 0x27 && g.sa_given);
    if (!own && (n != 3 || reply[0] != 0x7F || reply[2] != 0x11)) {
        abort();
    }
    if (req[0] == 0x27 && g.sa_given) {
        check_27(g, was, gw, m, req, len, reply, n);
    }
    out->append(reinterpret_cast<const char*>(reply), n);
}

inline void check_view(const ecuio_sim_gateway_config& g,
                       const ecuio_sim_session& gw,
                       const ecuio_sim_gateway_tally& t, const model& m)
{
    ecuio_sim_gateway_t v;
    ecuio_sim_gateway_view(g, gw, t, m.now_us, &v);
    const bool unlocked = gw.security.unlocked != 0;
    if (((v.flags & ECUIO_SIM_GATEWAY_UNLOCKED) != 0) != unlocked ||
        ((v.flags & ECUIO_SIM_GATEWAY_DELAYED) != 0) !=
            (m.now_us < m.until_us) ||
        v.forwarded + v.refused + v.dropped != m.routed ||
        v.unlocks != m.unlocks || v.too_early != m.too_early) {
        abort();
    }
    for (uint8_t c = 0; c < ECUIO_SIM_GATEWAY_CLASSES; ++c) {
        if (is_gated(g.cls[c]) &&
            (v.now[c] == ECUIO_SIM_ROUTE_FORWARD) != unlocked) {
            abort();
        }
    }
}

// 1sssssmm: s x 500 ms on, module m; 0ofnnnnn: own ID, functional, n bytes
inline void run(const ecuio_sim_gateway_config& g, const uint8_t* data,
                size_t size, std::string* out)
{
    ecuio_sim_session gw;
    gw.security.keying = &g.keying;
    gw.security.never = g.never;
    if (g.delay_us != 0) {
        gw.security.delay_us = g.delay_us;
    }
    ecuio_sim_gateway_tally t;
    model m;
    size_t at = 0;
    while (at < size) {
        const uint8_t h = data[at++];
        if (h & 0x80) {
            m.module = h & 3u;
            m.now_us += static_cast<int64_t>((h >> 2) & 0x1F) * 500000;
            continue;
        }
        const size_t len = h & 0x1Fu;
        if (len > size - at) {
            break;
        }
        const uint8_t* req = data + at;
        if (len > 0 && (h & 0x20)) {
            for (uint32_t b = 0; b < g.behind_count; ++b) {
                route_one(g, gw, t, m, g.behind[b], req, true, out);
            }
        } else if (len > 0 && (h & 0x40)) {
            answer_one(g, gw, t, m, req, len, out);
        } else if (len > 0) {
            route_one(g, gw, t, m, g.behind[m.module % g.behind_count], req,
                      false, out);
        }
        check_view(g, gw, t, m);
        m.now_us += 10000;
        at += len;
    }
}

// 'G', the URI's length, the URI, then run()'s items
inline void fuzz(const uint8_t* data, size_t size)
{
    if (size < 2 || data[1] > size - 2) {
        return;
    }
    std::string text(reinterpret_cast<const char*>(data + 2), data[1]);
    for (char& c : text) {
        c = c == '\0' ? 'x' : c;
    }
    ecuio_sim_config config;
    char why[256];
    if (ecuio_sim_parse_uri(text.c_str(), &config, why, sizeof why) !=
            ecuio_sim_uri_result::ok ||
        !config.gateway.given || config.gateway.behind_count == 0 ||
        !ecuio_sim_gateway_relock(&config.gateway, config.faults, config.aimed,
                                  why, sizeof why)) {
        return;
    }
    check_config(config);
    std::string a;
    std::string b;
    run(config.gateway, data + 2 + data[1], size - 2 - data[1], &a);
    run(config.gateway, data + 2 + data[1], size - 2 - data[1], &b);
    if (a != b) {
        abort();
    }
}

} // namespace fuzz_sim_gateway

#endif /* FUZZ_SIM_GATEWAY_H */
