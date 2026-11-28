// File/layer: core/src/sim_uri.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimEcuTest, SimGatewayTest (the URI), fuzz_sim
//
// The simulated ECU's URI: each key's reading and ecuio_sim_parse_uri.

#ifndef ECUIO_SIM_URI_H
#define ECUIO_SIM_URI_H

#include "sim_types.h"
#include "uri_query.h"
#include "absolute_path.h"
#include "recording_stretch.h"
#include "sim_faults.h"
#include "sim_timing.h"
#include <cstdio>
#include <cstring>
#include <string>

/* ---- The URI ---------------------------------------------------------- */

enum class ecuio_sim_uri_result : uint8_t { ok, bad };

namespace ecuio_sim_uri_detail {

/* A key's reading: not this group's key, taken, or refused with why */
enum class taken : uint8_t { other, ok, bad };

inline bool value_is(const ecuio_uri::param& p, const char* v)
{
    return p.val_len == std::strlen(v) &&
           std::strncmp(p.val, v, p.val_len) == 0;
}

/* The scheme and, for replay://, the recording's path: *body is what
 * follows the scheme */
inline bool take_source(const char* uri, ecuio_sim_config* out,
                        const char** body, char* why, size_t why_cap)
{
    if (std::strncmp(uri, "replay://", 9) == 0) {
        out->source = ecuio_sim_source::replay;
        *body = uri + 9;
        const char* q = std::strchr(*body, '?');
        out->path = *body;
        out->path_len =
            q != nullptr ? static_cast<size_t>(q - *body) : std::strlen(*body);
        const std::string path(*body, out->path_len);
#ifdef _WIN32
        const bool absolute = ecuio_is_absolute_path(path.c_str());
#else
        const bool absolute = !path.empty() && path[0] == '/';
#endif
        if (path.empty()) {
            std::snprintf(why, why_cap,
                          "replay:// needs a recording's "
                          "folder or its frames.csv");
            return false;
        }
        if (!absolute) {
            std::snprintf(why, why_cap,
                          "the recording's path must be absolute");
            return false;
        }
    } else if (std::strncmp(uri, "mock://", 7) == 0) {
        *body = uri + 7;
    } else {
        std::snprintf(why, why_cap,
                      "the simulated ECU takes mock:// or replay://");
        return false;
    }
    return true;
}

/* values=, modules= and sessions= */
inline taken take_mock_shape(const ecuio_uri::param& p, ecuio_sim_config* out,
                             char* why, size_t why_cap)
{
    if (p.is("values")) {
        if (value_is(p, "drive")) {
            out->values = ecuio_sim_values::drive;
        } else if (value_is(p, "canned")) {
            out->values = ecuio_sim_values::canned;
        } else if (value_is(p, "parked")) {
            out->values = ecuio_sim_values::parked;
        } else {
            std::snprintf(why, why_cap, "values: drive, canned or parked");
            return taken::bad;
        }
        return taken::ok;
    }
    if (p.is("modules")) {
        if (!value_is(p, "1") && !value_is(p, "2")) {
            std::snprintf(why, why_cap,
                          "modules: 1 (the engine ECU) or 2 (and a "
                          "second module on 0x7E1/0x7E9)");
            return taken::bad;
        }
        out->second_module = value_is(p, "2");
        return taken::ok;
    }
    if (p.is("sessions")) {
        if (!value_is(p, "0") && !value_is(p, "1")) {
            std::snprintf(why, why_cap,
                          "sessions: 0 or 1 (a profile's modules have the "
                          "extended session too)");
            return taken::bad;
        }
        out->sessions = value_is(p, "1");
        return taken::ok;
    }
    return taken::other;
}

/* profile= */
inline taken take_profile(const ecuio_uri::param& p, ecuio_sim_config* out,
                          char* why, size_t why_cap)
{
    if (!p.is("profile")) {
        return taken::other;
    }
    /* The registry's grammar (profile.h): 1-63 of [a-z0-9._-] */
    bool ok = p.val_len >= 1 && p.val_len <= ECUIO_PROFILE_NAME_MAX;
    for (size_t k = 0; ok && k < p.val_len; ++k) {
        const char c = p.val[k];
        ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
             c == '_' || c == '-';
    }
    if (!ok) {
        std::snprintf(why, why_cap,
                      "profile: a registered profile's name, 1 to "
                      "%d of a-z, 0-9, '.', '_' and '-'",
                      ECUIO_PROFILE_NAME_MAX);
        return taken::bad;
    }
    out->profile = p.val;
    out->profile_len = p.val_len;
    return taken::ok;
}

/* vin= */
inline taken take_vin(const ecuio_uri::param& p, ecuio_sim_config* out,
                      char* why, size_t why_cap)
{
    if (!p.is("vin")) {
        return taken::other;
    }
    char vin[ECUIO_VIN_LEN + 1] = {};
    ecuio_vin_info_t info;
    if (p.val_len == ECUIO_VIN_LEN) {
        std::memcpy(vin, p.val, ECUIO_VIN_LEN);
    }
    if (vin[0] == '\0' || ecuio_vin_decode(vin, 0, &info) != ECUIO_OK) {
        std::snprintf(why, why_cap,
                      "vin: 17 digits and capital letters, never I, "
                      "O or Q");
        return taken::bad;
    }
    std::memcpy(out->vin, vin, sizeof vin);
    out->vin_given = true;
    return taken::ok;
}

/* loop=, from= and to= */
inline taken take_replay_span(const ecuio_uri::param& p, ecuio_sim_config* out,
                              char* why, size_t why_cap)
{
    if (p.is("loop")) {
        if (!value_is(p, "0") && !value_is(p, "1")) {
            std::snprintf(why, why_cap, "loop: 0 or 1");
            return taken::bad;
        }
        out->loop = value_is(p, "1");
        return taken::ok;
    }
    if (p.is("from") || p.is("to")) {
        if (!ecuio_stretch::take(p, &out->stretch)) {
            std::snprintf(why, why_cap,
                          "from and to: the recording's seconds, as the "
                          "viewer and havne-cli view show them, with at "
                          "most three decimals");
            return taken::bad;
        }
        return taken::ok;
    }
    return taken::other;
}

/* faults= and pending_ms= */
inline taken take_faults(const ecuio_uri::param& p, ecuio_sim_config* out,
                         char* why, size_t why_cap)
{
    if (p.is("pending_ms")) {
        return ecuio_sim_parse_pending_ms(p.val, p.val_len, &out->pending_us,
                                          &out->pending_service, why, why_cap)
                   ? taken::ok
                   : taken::bad;
    }
    if (!p.is("faults")) {
        return taken::other;
    }
    return ecuio_sim_parse_faults(p.val, p.val_len, &out->faults, &out->aimed,
                                  why, why_cap)
               ? taken::ok
               : taken::bad;
}

inline taken take_codes(const ecuio_uri::param& p, ecuio_sim_config* out,
                        char* why, size_t why_cap)
{
    if (!p.is("codes")) {
        return taken::other;
    }
    return ecuio_sim_parse_codes(p.val, p.val_len, &out->codes, why, why_cap)
               ? taken::ok
               : taken::bad;
}

/* delay=, fc=, cf=, seed=, busy= and bus= (sim_timing.h) */
inline taken take_timing(const ecuio_uri::param& p, ecuio_sim_config* out,
                         char* why, size_t why_cap)
{
    using ecuio_sim_timing::knob;
    bool ok = true;
    if (p.is("delay") || p.is("fc") || p.is("cf") || p.is("busy")) {
        const knob what = p.is("delay") ? knob::delay
                          : p.is("fc")  ? knob::fc
                          : p.is("cf")  ? knob::cf
                                        : knob::busy;
        ok = ecuio_sim_timing::parse_key(what, p.val, p.val_len, &out->timing,
                                         why, why_cap);
    } else if (p.is("seed")) {
        ok = ecuio_sim_timing::parse_seed(p.val, p.val_len, &out->timing, why,
                                          why_cap);
    } else if (p.is("bus")) {
        ok = ecuio_sim_timing::parse_bus(p.val, p.val_len, &out->timing, why,
                                         why_cap);
    } else {
        return taken::other;
    }
    return ok ? taken::ok : taken::bad;
}

/* gateway=, behind=, route=, sa=, sa_delay= and gwopt= (sim_gateway.h) */
inline taken take_gateway(const ecuio_uri::param& p, ecuio_sim_config* out,
                          char* why, size_t why_cap)
{
    using parse =
        bool (*)(const char*, size_t, ecuio_sim_gateway_config*, char*, size_t);
    parse read = p.is("gateway")    ? ecuio_sim_parse_gateway
                 : p.is("behind")   ? ecuio_sim_parse_behind
                 : p.is("route")    ? ecuio_sim_parse_route
                 : p.is("gwopt")    ? ecuio_sim_parse_gwopt
                 : p.is("sa")       ? ecuio_sim_parse_sa
                 : p.is("sa_delay") ? ecuio_sim_parse_sa_delay
                                    : nullptr;
    if (read == nullptr) {
        return taken::other;
    }
    return read(p.val, p.val_len, &out->gateway, why, why_cap) ? taken::ok
                                                               : taken::bad;
}

/* mem= (sim_memory.h) */
inline taken take_memory(const ecuio_uri::param& p, ecuio_sim_config* out,
                         char* why, size_t why_cap)
{
    if (!p.is("mem")) {
        return taken::other;
    }
    return ecuio_sim_parse_memory(p.val, p.val_len, &out->memory, why, why_cap)
               ? taken::ok
               : taken::bad;
}

inline taken take_kernel(const ecuio_uri::param& p, ecuio_sim_config* out,
                         char* why, size_t why_cap)
{
    if (!p.is("kernel")) {
        return taken::other;
    }
    if (ecuio_sim_parse_kernel(p.val, p.val_len, &out->kernel,
                               &out->kernel_faults)) {
        return taken::ok;
    }
    std::snprintf(why, why_cap,
                  "kernel takes 1, or once, storm, silent and refuse, "
                  "comma-separated");
    return taken::bad;
}

inline taken take_flash(const ecuio_uri::param& p, ecuio_sim_config* out,
                        char* why, size_t why_cap)
{
    if (!p.is("flash")) {
        return taken::other;
    }
    if (ecuio_sim_parse_flash(p.val, p.val_len, &out->flash,
                              &out->flash_faults)) {
        return taken::ok;
    }
    std::snprintf(why, why_cap,
                  "flash takes 1, or counter, general, volts, check, erase, "
                  "silent, boot[:<sectors>], locked and pending:<ms>, "
                  "comma-separated");
    return taken::bad;
}

/* One key, through each group in turn: a key belongs to one group only,
 * so their order changes nothing */
inline bool take_key(const ecuio_uri::param& p, ecuio_sim_config* out,
                     char* why, size_t why_cap)
{
    using reader =
        taken (*)(const ecuio_uri::param&, ecuio_sim_config*, char*, size_t);
    static const reader kReaders[] = {
        take_mock_shape, take_profile, take_vin,    take_replay_span,
        take_faults,     take_codes,   take_timing, take_gateway,
        take_memory,     take_kernel,  take_flash};
    for (const reader r : kReaders) {
        const taken t = r(p, out, why, why_cap);
        if (t != taken::other) {
            return t == taken::ok;
        }
    }
    return true; /* the key list was checked first */
}

} // namespace ecuio_sim_uri_detail

/* The simulated ECU's URI into *out:
 *   mock://<label>[?values][&modules][&sessions][&profile][&vin][&faults]
 *          [&timing][&gateway][&behind][&route][&sa][&sa_delay][&gwopt]
 *          [&mem][&kernel][&flash]
 *   replay://<absolute path>[?loop][&from][&to][&vin][&faults][&timing]
 * where timing is delay=, fc=, cf=, seed=, busy= and bus= (sim_timing.h)
 * (ADR-0046 sections 3 and 7, and the appendix "a second module"). why
 * says what's wrong (a line). */
inline ecuio_sim_uri_result ecuio_sim_parse_uri(const char* uri,
                                                ecuio_sim_config* out,
                                                char* why, size_t why_cap)
{
    using namespace ecuio_sim_uri_detail;
    // The gateway's route entries set their own outcome (forward)
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    *out = ecuio_sim_config{};
    std::memcpy(out->vin, ECUIO_SIM_DEFAULT_VIN, sizeof ECUIO_SIM_DEFAULT_VIN);
    const char* body = nullptr;
    if (!take_source(uri, out, &body, why, why_cap)) {
        return ecuio_sim_uri_result::bad;
    }
    ecuio_uri_query q;
    const ecuio_uri::split_result split =
        ecuio_uri::split(ecuio_uri::query_of(body), &q);
    if (split != ecuio_uri::split_result::ok) {
        std::snprintf(why, why_cap, "the query isn't key=value pairs");
        return ecuio_sim_uri_result::bad;
    }
    const bool replay = out->source == ecuio_sim_source::replay;
    static const char* const kMockKeys[] = {
        "values", "modules",    "sessions", "profile", "vin",   "faults",
        "codes",  "pending_ms", "delay",    "fc",      "cf",    "seed",
        "busy",   "bus",        "gateway",  "behind",  "route", "gwopt",
        "sa",     "sa_delay",   "mem",      "kernel",  "flash", nullptr};
    static const char* const kReplayKeys[] = {
        "loop",  "from", "to", "vin",  "faults", "codes", "pending_ms",
        "delay", "fc",   "cf", "seed", "busy",   "bus",   nullptr};
    static const char* const kGatewayKeys[] = {
        "gateway", "behind", "route", "gwopt", "sa", "sa_delay", nullptr};
    const ecuio_uri::param* p =
        ecuio_uri::first_unknown(q, replay ? kReplayKeys : kMockKeys);
    bool gateway_key = false;
    for (const char* const* k = kGatewayKeys; p != nullptr && *k != nullptr;
         ++k) {
        gateway_key = gateway_key || p->is(*k);
    }
    if (replay && gateway_key) {
        std::snprintf(why, why_cap,
                      "a gateway is mock://'s: a replay ECU answers as the "
                      "engine ECU alone, with no module to put behind one");
        return ecuio_sim_uri_result::bad;
    }
    if (p != nullptr) {
        std::snprintf(
            why, why_cap,
            "the simulated ECU takes no query key '%.*s' for %s, "
            "only %s, vin, faults, codes, pending_ms, delay, fc, cf, seed, "
            "busy and bus",
            static_cast<int>(p->key_len < 40 ? p->key_len : 40), p->key,
            replay ? "replay://" : "mock://",
            replay ? "loop, from, to"
                   : "values, modules, sessions, profile, "
                     "gateway, behind, route, sa, sa_delay, gwopt, "
                     "mem, kernel, flash");
        return ecuio_sim_uri_result::bad;
    }
    for (size_t i = 0; i < q.count; ++i) {
        if (!take_key(q.params[i], out, why, why_cap)) {
            return ecuio_sim_uri_result::bad;
        }
    }
    if (!ecuio_sim_gateway_judge(out->gateway, why, why_cap)) {
        return ecuio_sim_uri_result::bad;
    }
    if (out->profile_len != 0 && out->second_module) {
        std::snprintf(why, why_cap,
                      "profile and modules=2 both name a second module: the "
                      "profile's channels are the modules");
        return ecuio_sim_uri_result::bad;
    }
    if (out->sessions && out->profile_len == 0) {
        std::snprintf(why, why_cap,
                      "sessions=1 gives a profile's modules the extended "
                      "session: it needs profile=");
        return ecuio_sim_uri_result::bad;
    }
    if (!ecuio_stretch::ordered(out->stretch)) {
        std::snprintf(why, why_cap, "to must come after from");
        return ecuio_sim_uri_result::bad;
    }
    return ecuio_sim_uri_result::ok;
}

#endif /* ECUIO_SIM_URI_H */
