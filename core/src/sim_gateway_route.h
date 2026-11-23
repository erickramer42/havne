// File/layer: core/src/sim_gateway_route.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: UNIT: SimGatewayTest (test_obd2), fuzz_sim
//
// The simulated gateway at work (ADR-0046's appendix "a gateway node"):
// its verdict on a request for a module behind it, judged before the
// module's busy check, answer, draw and fate, so a request it withholds
// never reaches the module; its answer on its own request ID; and its
// counts and the view ecuio_sim_get_gateway copies out.

#ifndef ECUIO_SIM_GATEWAY_ROUTE_H
#define ECUIO_SIM_GATEWAY_ROUTE_H

#include "ecuio/sim.h"
#include "sim_gateway.h"
#include "sim_session.h"
#include "sim_types.h"

#include <cstdint>
#include <cstring>

static_assert(static_cast<uint8_t>(ecuio_sim_route::forward) ==
                      ECUIO_SIM_ROUTE_FORWARD &&
                  static_cast<uint8_t>(ecuio_sim_route::refuse) ==
                      ECUIO_SIM_ROUTE_REFUSE &&
                  static_cast<uint8_t>(ecuio_sim_route::silent) ==
                      ECUIO_SIM_ROUTE_SILENT &&
                  static_cast<uint8_t>(ecuio_sim_route::gated) ==
                      ECUIO_SIM_ROUTE_GATED &&
                  static_cast<uint8_t>(ecuio_sim_route::gated_silent) ==
                      ECUIO_SIM_ROUTE_GATED_SILENT,
              "the routes are sim.h's numbers");
static_assert(ECUIO_SIM_GATEWAY_CLASSES == ECUIO_SIM_CLASS_COUNT &&
                  ECUIO_SIM_CLASS_COUNT <= 8,
              "the classes fit ecuio_sim_gateway_t's arrays");

enum class ecuio_sim_gateway_fate : uint8_t {
    forward,  /* the module answers */
    withhold, /* the gateway answers for it: out */
    drop      /* nobody answers */
};

struct ecuio_sim_gateway_verdict {
    ecuio_sim_gateway_fate fate = ecuio_sim_gateway_fate::forward;
    uint8_t out[3] = {}; /* withhold's 7F <service> <code> */
};

/* The codes ISO 14229-1 section 7.5 has a server keep to itself on a
 * functional request */
inline bool ecuio_sim_gateway_suppressed(uint8_t nrc)
{
    return nrc == 0x11 || nrc == 0x12 || nrc == 0x31 || nrc == 0x7E ||
           nrc == 0x7F;
}

/* An outcome as it stands: a gated one forwards once the gateway is
 * unlocked, else refuses (33 unless its entry names a code) or, gated
 * silent, drops; a refusal's code its default when *nrc is 0 */
inline ecuio_sim_route ecuio_sim_gateway_now(ecuio_sim_route r, bool unlocked,
                                             uint8_t* nrc)
{
    switch (r) {
    case ecuio_sim_route::gated:
        if (unlocked) {
            return ecuio_sim_route::forward;
        }
        *nrc = *nrc != 0 ? *nrc : ECUIO_SIM_ROUTE_GATED_NRC;
        return ecuio_sim_route::refuse;
    case ecuio_sim_route::gated_silent:
        return unlocked ? ecuio_sim_route::forward : ecuio_sim_route::silent;
    case ecuio_sim_route::refuse:
        *nrc = *nrc != 0 ? *nrc : ECUIO_SIM_ROUTE_REFUSE_NRC;
        break;
    case ecuio_sim_route::forward:
    case ecuio_sim_route::silent:
        break;
    }
    return r;
}

/* The gateway's verdict on req (len at least 1) for the module answering
 * on reply_id, gw its session at the ECU's time now_us: S3 lapses it
 * first (locking it), a gated class follows its lock, and with keep_any
 * a request let through keeps the session. Each request an unlocked
 * gated route meets is counted in *gated; with g.relock (faults=relock)
 * every relock-th locks the gateway again first and is withheld as the
 * route is while locked. A refusal goes on the module's reply ID; on a
 * functional request a withheld one is silent unless aloud, and aloud
 * keeps the suppressed codes silent unless unsuppressed. */
inline ecuio_sim_gateway_verdict ecuio_sim_gateway_route(
    const ecuio_sim_gateway_config& g, ecuio_sim_session& gw, uint64_t* gated,
    uint32_t reply_id, const uint8_t* req, bool functional, int64_t now_us)
{
    ecuio_sim_session_lapse(gw, now_us);
    ecuio_sim_gateway_verdict v;
    uint8_t nrc = 0;
    const ecuio_sim_route configured =
        ecuio_sim_gateway_outcome(g, reply_id, req[0], &nrc);
    if ((configured == ecuio_sim_route::gated ||
         configured == ecuio_sim_route::gated_silent) &&
        gw.security.unlocked != 0) {
        ++*gated;
        if (g.relock != 0 && *gated % g.relock == 0) {
            ecuio_sim_security_lock(gw.security);
        }
    }
    const ecuio_sim_route r =
        ecuio_sim_gateway_now(configured, gw.security.unlocked != 0, &nrc);
    switch (r) {
    case ecuio_sim_route::forward:
        if (g.keep_any) {
            gw.heard_us = now_us;
        }
        return v;
    case ecuio_sim_route::refuse:
        break;
    case ecuio_sim_route::silent:
    case ecuio_sim_route::gated:
    case ecuio_sim_route::gated_silent:
        v.fate = ecuio_sim_gateway_fate::drop;
        return v;
    }
    const bool quiet =
        functional &&
        (!g.aloud || (ecuio_sim_gateway_suppressed(nrc) && !g.unsuppressed));
    v.fate =
        quiet ? ecuio_sim_gateway_fate::drop : ecuio_sim_gateway_fate::withhold;
    ecuio_sim_detail::refuse(req[0], nrc, v.out);
    return v;
}

/* The gateway's answer to a physical request on its own request ID at
 * the ECU's time now_us, into out, its length: 10 01, 10 03, 3E 00 and
 * 3E 80 (silent) as any node's session answers them, 27 from its
 * security access (sa=) or refused 11 without one, every other service
 * 7F <service> 11 */
inline uint32_t ecuio_sim_gateway_answer(const ecuio_sim_gateway_config& g,
                                         ecuio_sim_session& s, int64_t now_us,
                                         const uint8_t* req, uint32_t len,
                                         uint8_t* out)
{
    if (len == 0) {
        return 0;
    }
    bool handled = false;
    const uint32_t n = ecuio_sim_session_answer(s, true, g.sa_given, now_us,
                                                req, len, out, &handled);
    return handled ? n : ecuio_sim_detail::refuse(req[0], 0x11, out);
}

/* The gateway's counts since create (ecuio_sim_gateway_t's) */
struct ecuio_sim_gateway_tally {
    uint64_t forwarded = 0;
    uint64_t refused = 0;
    uint64_t dropped = 0;
    uint64_t seeds = 0;
    uint64_t keys = 0;
    uint64_t wrong_keys = 0;
    uint64_t lockouts = 0;
    uint64_t unlocks = 0;
    uint64_t relocks = 0;
    uint64_t too_early = 0;
    uint64_t gated = 0; /* through an unlocked gated route: relock's count */
};

/* A verdict counted by its fate, and an unlock its S3 lapse lost (was:
 * unlocked before it) */
inline void ecuio_sim_gateway_count_route(ecuio_sim_gateway_tally& t,
                                          const ecuio_sim_gateway_verdict& v,
                                          bool was, const ecuio_sim_session& gw)
{
    switch (v.fate) {
    case ecuio_sim_gateway_fate::forward:
        ++t.forwarded;
        break;
    case ecuio_sim_gateway_fate::withhold:
        ++t.refused;
        break;
    case ecuio_sim_gateway_fate::drop:
        ++t.dropped;
        break;
    }
    t.relocks += was && gw.security.unlocked == 0 ? 1 : 0;
}

/* The gateway's answer out (n bytes) to req counted: a seed, a key
 * judged right or wrong, a lockout, a 27 refused for its delay, and an
 * unlock the request's session change lost (was: unlocked before it) */
inline void ecuio_sim_gateway_count_answer(ecuio_sim_gateway_tally& t, bool was,
                                           const ecuio_sim_session& gw,
                                           const uint8_t* req, uint32_t len,
                                           const uint8_t* out, uint32_t n)
{
    t.relocks += was && gw.security.unlocked == 0 ? 1 : 0;
    if (len < 2 || req[0] != 0x27 || n < 2) {
        return;
    }
    const bool key = req[1] % 2 == 0;
    if (out[0] == 0x67) {
        t.seeds += key ? 0 : 1;
        t.keys += key ? 1 : 0;
        t.unlocks += key ? 1 : 0;
        return;
    }
    if (n != 3 || out[0] != 0x7F) {
        return;
    }
    if (out[2] == ECUIO_SIM_NRC_INVALID_KEY ||
        out[2] == ECUIO_SIM_NRC_ATTEMPTS) {
        ++t.keys;
        ++t.wrong_keys;
        t.lockouts += out[2] == ECUIO_SIM_NRC_ATTEMPTS ? 1 : 0;
    }
    t.too_early += out[2] == ECUIO_SIM_NRC_DELAY ? 1 : 0;
}

/* The gateway's view: its config g, its session gw and counts t at the
 * ECU's time now_us, into *out */
inline void ecuio_sim_gateway_view(const ecuio_sim_gateway_config& g,
                                   const ecuio_sim_session& gw,
                                   const ecuio_sim_gateway_tally& t,
                                   int64_t now_us, ecuio_sim_gateway_t* out)
{
    const ecuio_sim_security& sec = gw.security;
    ecuio_sim_gateway_t v;
    std::memset(&v, 0, sizeof v);
    v.request_id = g.request_id;
    v.reply_id = g.reply_id;
    const bool delayed = sec.delayed && now_us < sec.delay_until_us;
    const struct {
        bool on;
        uint32_t flag;
    } flags[] = {{gw.extended, ECUIO_SIM_GATEWAY_EXTENDED},
                 {sec.unlocked != 0, ECUIO_SIM_GATEWAY_UNLOCKED},
                 {delayed, ECUIO_SIM_GATEWAY_DELAYED},
                 {g.never, ECUIO_SIM_GATEWAY_NEVER},
                 {g.sa_given, ECUIO_SIM_GATEWAY_SECURITY},
                 {g.keep_any, ECUIO_SIM_GATEWAY_KEEP_ANY},
                 {g.aloud, ECUIO_SIM_GATEWAY_ALOUD},
                 {g.unsuppressed, ECUIO_SIM_GATEWAY_UNSUPPRESSED}};
    for (const auto& f : flags) {
        v.flags |= f.on ? f.flag : 0u;
    }
    v.unlocked_level = sec.unlocked;
    v.wrong = sec.wrong;
    for (uint8_t c = 0; c < ECUIO_SIM_GATEWAY_CLASSES; ++c) {
        v.route[c] = static_cast<uint8_t>(g.cls[c]);
        v.route_nrc[c] = g.cls_nrc[c];
        uint8_t nrc = g.cls_nrc[c];
        v.now[c] = static_cast<uint8_t>(
            ecuio_sim_gateway_now(g.cls[c], sec.unlocked != 0, &nrc));
    }
    v.delayed_until_us = sec.delayed ? sec.delay_until_us : 0;
    v.forwarded = t.forwarded;
    v.refused = t.refused;
    v.dropped = t.dropped;
    v.seeds = t.seeds;
    v.keys = t.keys;
    v.wrong_keys = t.wrong_keys;
    v.lockouts = t.lockouts;
    v.unlocks = t.unlocks;
    v.relocks = t.relocks;
    v.too_early = t.too_early;
    v.overrides = g.entry_count;
    *out = v;
}

/* Entry index of g's overrides (below g.entry_count), its now by the
 * lock (unlocked), into *out */
inline void ecuio_sim_gateway_override_of(const ecuio_sim_gateway_config& g,
                                          uint32_t index, bool unlocked,
                                          ecuio_sim_gateway_override_t* out)
{
    const ecuio_sim_route_entry& e = g.entries[index];
    ecuio_sim_gateway_override_t o;
    std::memset(&o, 0, sizeof o);
    o.reply_id = e.reply_id;
    o.service = e.service ? 1 : 0;
    o.key = e.key;
    o.route = static_cast<uint8_t>(e.outcome);
    o.nrc = e.nrc;
    uint8_t nrc = e.nrc;
    o.now =
        static_cast<uint8_t>(ecuio_sim_gateway_now(e.outcome, unlocked, &nrc));
    *out = o;
}

#endif /* ECUIO_SIM_GATEWAY_ROUTE_H */
