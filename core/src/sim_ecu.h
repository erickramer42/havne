// File/layer: core/src/sim_ecu.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimEcuTest, SimRecordingTest (test_obd2)
//
// What the simulated ECU answers (ADR-0046): the engine ECU, from the
// service byte on, its framing (ISO-TP) and its bus left to sim.cpp.
// mock://: Service 01 comes from the mock's drive cycle (values=drive)
// or the fakes' canned readings (values=canned, sim_canned.h); the
// diagnostics are the mock's (mock_ecu.h's tables, the fakes' codes,
// monitors and freeze frame) in both; identification is the VIN the URI
// gives and an ECU name. modules=2 adds a second module's answers, its
// own made-up DIDs (the appendix of 2026-11-04). The engine ECU also has
// an extended session (ADR-0060): 10 03 enters it, 3E 00 keeps it, S3 of
// silence leaves it, and DIDs D200-D203 answer only in it. In that
// session it has security access too (ADR-0066 section 2.7,
// sim_security.h): 27 unlocks a level, and DID D210 answers once one is.
// sessions=1 gives a profile's modules the extended session as well (no
// security access), so a session can be held on many modules at once.
// replay://: a recording's replies at the ECU's time since it started
// (sim_recording.h). Faults break every Nth reply, as ADR-0041 section
// 4's ECU does, and pending puts a reply off behind a 7F <service> 78.

#ifndef ECUIO_SIM_ECU_H
#define ECUIO_SIM_ECU_H

#include "ecuio/core.h"
#include "ecuio/profile.h" /* ECUIO_PROFILE_NAME_MAX */
#include "ecuio/vin.h"
#include "mock_drive_cycle.h"
#include "mock_ecu.h"
#include "sim_canned.h"
#include "sim_faults.h"
#include "recording_stretch.h"
#include "sim_recording.h"
#include "sim_security.h"
#include "sim_timing.h"
#include "uri_query.h"

#include "absolute_path.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sim_types.h"
#include "sim_pids.h"
#include "sim_session.h"
#include "sim_engine.h"
#include "sim_module.h"

/* The fate of a module's next reply from its periods f, counting it in
 * c: bitmaps are never broken, single-frame and segmented replies are
 * counted apart, and the two frame faults take segmented replies only */
inline ecuio_sim_fate ecuio_sim_next_fate(const ecuio_sim_faults& f,
                                          ecuio_sim_counts& c,
                                          const uint8_t* req, uint32_t req_len,
                                          uint32_t reply_len)
{
    if (req_len == 2 && req[0] == 0x01 &&
        ecuio_sim_detail::bitmap_pid(req[1])) {
        return ecuio_sim_fate::intact;
    }
    const bool segmented = reply_len > 7;
    const uint32_t k = ++(segmented ? c.segmented : c.singles);
    const auto every = [k](uint32_t n) { return n != 0 && k % n == 0; };
    if (every(f.silent)) {
        return ecuio_sim_fate::silent;
    }
    if (every(f.late)) {
        return ecuio_sim_fate::late;
    }
    if (every(f.pending)) {
        return ecuio_sim_fate::pending;
    }
    if (segmented && every(f.bad_sequence)) {
        return ecuio_sim_fate::bad_sequence;
    }
    if (segmented && every(f.stalled)) {
        return ecuio_sim_fate::stalled;
    }
    return ecuio_sim_fate::intact;
}

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
        } else {
            std::snprintf(why, why_cap, "values: drive or canned");
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

/* faults= */
inline taken take_faults(const ecuio_uri::param& p, ecuio_sim_config* out,
                         char* why, size_t why_cap)
{
    if (!p.is("faults")) {
        return taken::other;
    }
    return ecuio_sim_parse_faults(p.val, p.val_len, &out->faults, &out->aimed,
                                  why, why_cap)
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

/* One key, through each group in turn: a key belongs to one group only,
 * so their order changes nothing */
inline bool take_key(const ecuio_uri::param& p, ecuio_sim_config* out,
                     char* why, size_t why_cap)
{
    using reader =
        taken (*)(const ecuio_uri::param&, ecuio_sim_config*, char*, size_t);
    static const reader kReaders[] = {take_mock_shape, take_profile,
                                      take_vin,        take_replay_span,
                                      take_faults,     take_timing};
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
 *          [&timing]
 *   replay://<absolute path>[?loop][&from][&to][&vin][&faults][&timing]
 * where timing is delay=, fc=, cf=, seed=, busy= and bus= (sim_timing.h)
 * (ADR-0046 sections 3 and 7, and the appendix "a second module"). why
 * says what's wrong (a line). */
inline ecuio_sim_uri_result ecuio_sim_parse_uri(const char* uri,
                                                ecuio_sim_config* out,
                                                char* why, size_t why_cap)
{
    using namespace ecuio_sim_uri_detail;
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
        "values", "modules", "sessions", "profile", "vin", "faults", "delay",
        "fc",     "cf",      "seed",     "busy",    "bus", nullptr};
    static const char* const kReplayKeys[] = {
        "loop", "from", "to",   "vin",  "faults", "delay",
        "fc",   "cf",   "seed", "busy", "bus",    nullptr};
    if (const ecuio_uri::param* p =
            ecuio_uri::first_unknown(q, replay ? kReplayKeys : kMockKeys)) {
        std::snprintf(why, why_cap,
                      "the simulated ECU takes no query key '%.*s' for %s, "
                      "only %s, vin, faults, delay, fc, cf, seed, busy and bus",
                      static_cast<int>(p->key_len < 40 ? p->key_len : 40),
                      p->key, replay ? "replay://" : "mock://",
                      replay ? "loop, from, to"
                             : "values, modules, sessions, profile");
        return ecuio_sim_uri_result::bad;
    }
    for (size_t i = 0; i < q.count; ++i) {
        if (!take_key(q.params[i], out, why, why_cap)) {
            return ecuio_sim_uri_result::bad;
        }
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

#endif /* ECUIO_SIM_ECU_H */
