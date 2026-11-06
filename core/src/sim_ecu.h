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
// own made-up DIDs (the appendix of 2026-11-04). replay://: a
// recording's replies at the ECU's time since it started
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
#include "recording_stretch.h"
#include "sim_recording.h"
#include "uri_query.h"

#include "absolute_path.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

enum class ecuio_sim_source : uint8_t { mock, replay };
enum class ecuio_sim_values : uint8_t { drive, canned };

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

/* A VIN of the VIN alphabet that decodes (model year 2021, its check
 * digit right) under a manufacturer code NHTSA doesn't list, so the
 * simulated car is no maker's */
constexpr char ECUIO_SIM_DEFAULT_VIN[] = "ZZZFAKE11M0000001";

/* Service 09 item 0A, 20 bytes, NUL-padded */
constexpr char ECUIO_SIM_ECU_NAME[] = "ECM-HavneSim";

/* The ECU's IDs: the obd2 engine ECU's, and the functional request */
constexpr uint32_t ECUIO_SIM_FUNCTIONAL_ID = 0x7DFu;
constexpr uint32_t ECUIO_SIM_REQUEST_ID = 0x7E0u;
constexpr uint32_t ECUIO_SIM_REPLY_ID = 0x7E8u;
/* modules=2: a second module, a transmission's, on the next pair */
constexpr uint32_t ECUIO_SIM_MODULE_REQUEST_ID = 0x7E1u;
constexpr uint32_t ECUIO_SIM_MODULE_REPLY_ID = 0x7E9u;

/* The second module's DIDs (Service 22) and their data: made up for the
 * simulated car, no maker's. D100 fluid temperature (A - 40, 90 deg C),
 * D101 the gear (3), D102 the module's name as text, a segmented reply */
struct ecuio_sim_module_did {
    uint16_t did;
    uint8_t len;
    uint8_t data[16];
};
constexpr ecuio_sim_module_did ECUIO_SIM_MODULE_DIDS[] = {
    {0xD100, 1, {0x82}},
    {0xD101, 1, {0x03}},
    {0xD102, 12, {'T', 'C', 'M', '-', 'H', 'a', 'v', 'n', 'e', 'S', 'i', 'm'}},
};

/* The longest reply: a recording's, ISO-TP's longest message (the mock's
 * is a pack of six PIDs, the longest PID 13 bytes) */
constexpr uint32_t ECUIO_SIM_REPLY_MAX = ECUIO_SIM_RECORDING_REPLY_MAX;
static_assert(1 + 6 * (1 + 13) <= ECUIO_SIM_REPLY_MAX, "the mock's pack fits");

/* One tick of the ECU's clock: the drive cycle's step */
constexpr int64_t ECUIO_SIM_TICK_US = 50000;

struct ecuio_sim_config {
    ecuio_sim_source source = ecuio_sim_source::mock;
    ecuio_sim_values values = ecuio_sim_values::drive;
    char vin[ECUIO_VIN_LEN + 1] = {};
    bool vin_given = false; /* replay: 09 02 answers it, not the car's */
    ecuio_sim_faults faults;
    bool second_module = false; /* mock: modules=2 */
    /* mock: profile=, a registered profile whose set's DIDs the ECUs
     * answer (sim_dids.h), a view into the URI parsed (valid while it
     * is); empty for none */
    const char* profile = nullptr;
    size_t profile_len = 0;
    /* replay: the recording's path, a view into the URI parsed (valid
     * while it is), and whether its time goes round its span */
    const char* path = nullptr;
    size_t path_len = 0;
    bool loop = false;
    ecuio_stretch::stretch stretch; /* replay: from=, to= */
};

struct ecuio_sim_ecu {
    ecuio_sim_config config;
    /* replay: the recording loaded (the caller's, outliving the ECU) */
    const ecuio_sim_recording* recording = nullptr;
    int64_t time_us = 0;        /* since the ECU started, by its ticks */
    ecuio_mock::DriveCycle car; /* values=drive: stepped by the bus side */
    float engine_s = 0.0f;      /* time since the engine last started */
    bool cleared = false;       /* a 04 was obeyed */
    uint32_t singles = 0;       /* replies counted for the faults */
    uint32_t segmented = 0;
    uint32_t module_singles = 0; /* the second module's, counted apart */
    uint32_t module_segmented = 0;
};

/* One tick (kTickS of the ECU's time): its clock runs; the mock's drive
 * cycle steps, and the run time since the engine started counts while it
 * runs */
inline void ecuio_sim_tick(ecuio_sim_ecu& e)
{
    e.time_us += ECUIO_SIM_TICK_US;
    if (e.recording != nullptr) {
        return;
    }
    const bool was = e.car.running;
    e.car.step();
    if (!e.car.running) {
        e.engine_s = 0.0f;
    } else {
        e.engine_s = was ? e.engine_s + ecuio_mock::kTickS : 0.0f;
    }
}

namespace ecuio_sim_detail {

inline uint8_t u8(float v)
{
    return static_cast<uint8_t>(v <= 0.0f ? 0 : v >= 255.0f ? 255 : v + 0.5f);
}

inline void put16(uint8_t* out, uint16_t v)
{
    out[0] = static_cast<uint8_t>(v >> 8);
    out[1] = static_cast<uint8_t>(v);
}

/* The PIDs the drive cycle answers, data PIDs only */
constexpr uint8_t kDrivePids[] = {0x04, 0x05, 0x0C, 0x0D, 0x0F, 0x10,
                                  0x11, 0x1F, 0x24, 0x42, 0x46, 0x5C};

/* The diagnostics' PIDs, in both modes: readiness, and what a clear
 * resets (their values before one: the fakes') */
constexpr uint8_t kDiagPids[] = {0x01, 0x21, 0x30, 0x31, 0x41, 0x4D, 0x4E};

inline bool listed(const uint8_t* list, size_t n, uint8_t pid)
{
    for (size_t i = 0; i < n; ++i) {
        if (list[i] == pid) {
            return true;
        }
    }
    return false;
}

/* A data PID the ECU answers (not a bitmap) */
inline bool has_data_pid(const ecuio_sim_ecu& e, uint8_t pid)
{
    if (listed(kDiagPids, sizeof kDiagPids, pid)) {
        return true;
    }
    if (e.config.values == ecuio_sim_values::canned) {
        return ecuio_sim_canned_pid(pid) != nullptr;
    }
    return listed(kDrivePids, sizeof kDrivePids, pid);
}

inline bool bitmap_pid(uint8_t pid)
{
    return pid % 0x20 == 0;
}

/* A bitmap PID is supported when it's 00, or when a data PID past it
 * is (J1979: each bitmap's last bit names the next) */
inline bool has_pid(const ecuio_sim_ecu& e, unsigned pid)
{
    if (pid > 0xFF) {
        return false;
    }
    if (!bitmap_pid(static_cast<uint8_t>(pid))) {
        return has_data_pid(e, static_cast<uint8_t>(pid));
    }
    if (pid == 0) {
        return true;
    }
    for (unsigned p = pid + 1; p <= 0xFF; ++p) {
        if (!bitmap_pid(static_cast<uint8_t>(p)) &&
            has_data_pid(e, static_cast<uint8_t>(p))) {
            return true;
        }
    }
    return false;
}

/* A mock table's reply to the request, its length, or 0 */
inline uint32_t from_table(const ecuio_sim_ecu& e, const uint8_t* req,
                           uint32_t len, uint8_t* out)
{
    const auto find = [&](const auto& table) -> uint32_t {
        for (const ecuio_mock_answer& a : table) {
            if (a.request_len == len && std::memcmp(a.request, req, len) == 0) {
                std::memcpy(out, a.reply, a.reply_len);
                return a.reply_len;
            }
        }
        return 0;
    };
    return e.cleared ? find(ECUIO_MOCK_CLEARED_ANSWERS)
                     : find(ECUIO_MOCK_ANSWERS);
}

/* One Service 01 PID's data into out, its length; 0 when the ECU lacks
 * it */
inline uint32_t pid_data(const ecuio_sim_ecu& e, uint8_t pid, uint8_t* out)
{
    if (!has_pid(e, pid)) {
        return 0;
    }
    if (bitmap_pid(pid)) {
        std::memset(out, 0, 4);
        for (unsigned k = 1; k <= 32; ++k) {
            if (has_pid(e, pid + k)) {
                out[(k - 1) / 8] = static_cast<uint8_t>(
                    out[(k - 1) / 8] | (0x80u >> ((k - 1) % 8)));
            }
        }
        return 4;
    }
    if (pid == 0x01 || pid == 0x41) {
        const uint8_t req[2] = {0x01, pid};
        uint8_t reply[8];
        const uint32_t n = from_table(e, req, 2, reply);
        std::memcpy(out, reply + 2, n - 2);
        return n - 2;
    }
    /* What a clear resets: 42 km and 60 min with the MIL on, 20
     * warm-ups, 1234 km and 3000 min since the last clear (the fakes') */
    const auto reset = [&](uint32_t n, uint16_t v) -> uint32_t {
        const uint16_t value = e.cleared ? 0 : v;
        if (n == 1) {
            out[0] = static_cast<uint8_t>(value);
        } else {
            put16(out, value);
        }
        return n;
    };
    switch (pid) {
    case 0x21:
        return reset(2, 42);
    case 0x30:
        return reset(1, 20);
    case 0x31:
        return reset(2, 1234);
    case 0x4D:
        return reset(2, 60);
    case 0x4E:
        return reset(2, 3000);
    default:
        break;
    }
    if (e.config.values == ecuio_sim_values::canned) {
        const ecuio_sim_canned* c = ecuio_sim_canned_pid(pid);
        std::memcpy(out, c->data, c->len);
        return c->len;
    }
    const ecuio_mock::DriveCycle& car = e.car;
    switch (pid) {
    case 0x04: /* calculated load, A * 100 / 255 */
        out[0] = u8(car.load_pct * 255.0f / 100.0f);
        return 1;
    case 0x05: /* A - 40 */
        out[0] = u8(car.coolant_c + 40.0f);
        return 1;
    case 0x0C: /* (256A + B) / 4 */
        put16(out, ecuio_mock::clamp_u16(car.rpm * 4.0f));
        return 2;
    case 0x0D:
        out[0] = u8(car.speed_kmh);
        return 1;
    case 0x0F:
        out[0] = u8(car.iat_c + 40.0f);
        return 1;
    case 0x10: /* (256A + B) / 100 */
        put16(out, ecuio_mock::clamp_u16(car.maf_gs * 100.0f));
        return 2;
    case 0x11:
        out[0] = u8(car.throttle_pct * 255.0f / 100.0f);
        return 1;
    case 0x1F: /* seconds since the engine started */
        put16(out, ecuio_mock::clamp_u16(e.engine_s));
        return 2;
    case 0x24: /* lambda, (256A + B) * 2 / 65536; the voltage isn't
                  modeled: a steady 2.50 V, (256C + D) * 8 / 65536 */
        put16(out, ecuio_mock::clamp_u16(car.lambda * 32768.0f));
        out[2] = 0x50;
        out[3] = 0x00;
        return 4;
    case 0x42: /* module voltage, (256A + B) / 1000: charging while the
                  engine runs, the battery's own when it doesn't */
        put16(out, car.running ? 14100 : 12600);
        return 2;
    case 0x46: /* ambient, A - 40: the cold start's air */
        out[0] = u8(ecuio_mock::kColdStartC + 40.0f);
        return 1;
    case 0x5C:
        out[0] = u8(car.oil_c + 40.0f);
        return 1;
    default:
        return 0;
    }
}

inline uint32_t refuse(uint8_t service, uint8_t nrc, uint8_t* out)
{
    out[0] = 0x7F;
    out[1] = service;
    out[2] = nrc;
    return 3;
}

} // namespace ecuio_sim_detail

/* The ECU's reply to req (len bytes, the service byte first) into out
 * (ECUIO_SIM_REPLY_MAX bytes), its length; 0 for none. functional: the
 * request came on 0x7DF, where ISO 14229-1 has an ECU keep its negative
 * replies 11, 12 and 31 to itself. The mock's ECU clears on a 04; a
 * recording answers from its replies at the ECU's time. */
inline uint32_t ecuio_sim_answer(ecuio_sim_ecu& e, const uint8_t* req,
                                 uint32_t len, bool functional, uint8_t* out)
{
    using namespace ecuio_sim_detail;
    if (len == 0) {
        return 0;
    }
    const uint8_t service = req[0];
    uint32_t n = 0;
    if (e.recording != nullptr) {
        n = ecuio_sim_recording_answer(
            *e.recording, e.time_us, req, len,
            e.config.vin_given ? e.config.vin : nullptr, out);
    } else if (service == 0x01 && len == 2) {
        out[0] = 0x41;
        out[1] = req[1];
        const uint32_t m = pid_data(e, req[1], out + 2);
        n = m > 0 ? 2 + m : refuse(0x01, 0x12, out);
    } else if (service == 0x01 && len >= 3 && len <= 7) {
        /* J1979's pack: each PID it has, in the order asked */
        n = 1;
        out[0] = 0x41;
        for (uint32_t k = 1; k < len; ++k) {
            uint8_t data[13];
            const uint32_t m = pid_data(e, req[k], data);
            if (m > 0) {
                out[n] = req[k];
                std::memcpy(out + n + 1, data, m);
                n += 1 + m;
            }
        }
        if (n == 1) {
            return 0; /* none it has: no answer */
        }
    } else if (service == 0x04 && len == 1) {
        e.cleared = true;
        out[0] = 0x44;
        n = 1;
    } else if ((service == 0x03 || service == 0x07 || service == 0x0A ||
                service == 0x02) &&
               len <= 3) {
        n = from_table(e, req, len, out);
        if (n == 0) {
            n = refuse(service, 0x12, out);
        }
    } else if (service == 0x09 && len == 2) {
        out[0] = 0x49;
        out[1] = req[1];
        switch (req[1]) {
        case 0x00: /* items 02 and 0A */
            out[2] = 0x40;
            out[3] = 0x40;
            out[4] = 0x00;
            out[5] = 0x00;
            n = 6;
            break;
        case 0x02:
            out[2] = 0x01;
            std::memcpy(out + 3, e.config.vin, ECUIO_VIN_LEN);
            n = 3 + ECUIO_VIN_LEN;
            break;
        case 0x0A:
            out[2] = 0x01;
            std::memset(out + 3, 0, 20);
            std::memcpy(out + 3, ECUIO_SIM_ECU_NAME,
                        sizeof ECUIO_SIM_ECU_NAME - 1);
            n = 3 + 20;
            break;
        default:
            n = refuse(0x09, 0x12, out);
            break;
        }
    } else if (service == 0x22) {
        n = refuse(0x22, 0x31, out);
    } else {
        n = refuse(service, 0x11, out);
    }
    if (functional && n == 3 && out[0] == 0x7F &&
        (out[2] == 0x11 || out[2] == 0x12 || out[2] == 0x31)) {
        return 0;
    }
    return n;
}

/* The second module's reply (modules=2, on 0x7E1, answering on 0x7E9)
 * to req into out, its length; 0 for none. Service 22 from its own DIDs,
 * 7F 22 31 for one it lacks, 7F <service> 11 for any other service; a
 * functional request's refusals unsaid, as the engine ECU's */
inline uint32_t ecuio_sim_module_answer(const uint8_t* req, uint32_t len,
                                        bool functional, uint8_t* out)
{
    using namespace ecuio_sim_detail;
    if (len == 0) {
        return 0;
    }
    uint32_t n = 0;
    if (req[0] == 0x22) {
        n = refuse(0x22, 0x31, out);
        if (len == 3) {
            const uint16_t did = static_cast<uint16_t>((req[1] << 8) | req[2]);
            for (const ecuio_sim_module_did& d : ECUIO_SIM_MODULE_DIDS) {
                if (d.did == did) {
                    out[0] = 0x62;
                    out[1] = req[1];
                    out[2] = req[2];
                    std::memcpy(out + 3, d.data, d.len);
                    n = 3u + d.len;
                    break;
                }
            }
        }
    } else {
        n = refuse(req[0], 0x11, out);
    }
    if (functional && out[0] == 0x7F) {
        return 0;
    }
    return n;
}

/* The fate of the next reply, counting it: bitmaps are never broken,
 * single-frame and segmented replies are counted apart (and each
 * module's apart from the other's: module 1 is the second), and the two
 * frame faults take segmented replies only */
inline ecuio_sim_fate ecuio_sim_next_fate(ecuio_sim_ecu& e, const uint8_t* req,
                                          uint32_t req_len, uint32_t reply_len,
                                          int module = 0)
{
    if (req_len == 2 && req[0] == 0x01 &&
        ecuio_sim_detail::bitmap_pid(req[1])) {
        return ecuio_sim_fate::intact;
    }
    const bool segmented = reply_len > 7;
    uint32_t& count = module == 0
                          ? (segmented ? e.segmented : e.singles)
                          : (segmented ? e.module_segmented : e.module_singles);
    const uint32_t k = ++count;
    const ecuio_sim_faults& f = e.config.faults;
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

/* faults=<name>:<n>[,...] into *out; false with why (a line) */
inline bool ecuio_sim_parse_faults(const char* text, size_t len,
                                   ecuio_sim_faults* out, char* why,
                                   size_t why_cap)
{
    *out = ecuio_sim_faults{};
    size_t at = 0;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != ',') {
            ++end;
        }
        const char* item = text + at;
        const size_t item_len = end - at;
        size_t colon = 0;
        while (colon < item_len && item[colon] != ':') {
            ++colon;
        }
        uint32_t* slot = nullptr;
        const auto named = [&](const char* name) {
            return colon == std::strlen(name) &&
                   std::strncmp(item, name, colon) == 0;
        };
        if (named("silent")) {
            slot = &out->silent;
        } else if (named("late")) {
            slot = &out->late;
        } else if (named("bad_sequence")) {
            slot = &out->bad_sequence;
        } else if (named("stalled")) {
            slot = &out->stalled;
        } else if (named("pending")) {
            slot = &out->pending;
        }
        uint32_t n = 0;
        bool digits = colon + 1 < item_len;
        for (size_t i = colon + 1; digits && i < item_len; ++i) {
            const char c = item[i];
            if (c < '0' || c > '9' || n > ECUIO_SIM_FAULT_MAX) {
                digits = false;
            } else {
                n = n * 10 + static_cast<uint32_t>(c - '0');
            }
        }
        if (slot == nullptr || colon == item_len || !digits || n == 0 ||
            n > ECUIO_SIM_FAULT_MAX) {
            std::snprintf(why, why_cap,
                          "faults: '%.*s' isn't <fault>:<n>, the fault one of "
                          "silent, late, pending, bad_sequence or stalled and "
                          "n from 1 to %u",
                          static_cast<int>(item_len < 40 ? item_len : 40), item,
                          ECUIO_SIM_FAULT_MAX);
            return false;
        }
        *slot = n;
        at = end + 1;
    }
    if (len == 0 || text[len - 1] == ',') {
        std::snprintf(why, why_cap, "faults: names none");
        return false;
    }
    return true;
}

enum class ecuio_sim_uri_result : uint8_t { ok, bad };

/* The simulated ECU's URI into *out:
 *   mock://<label>[?values][&modules][&profile][&vin][&faults]
 *   replay://<absolute path>[?loop][&from][&to][&vin][&faults]
 * (ADR-0046 sections 3 and 7, and the appendix "a second module"). why
 * says what's wrong (a line). */
inline ecuio_sim_uri_result ecuio_sim_parse_uri(const char* uri,
                                                ecuio_sim_config* out,
                                                char* why, size_t why_cap)
{
    *out = ecuio_sim_config{};
    std::memcpy(out->vin, ECUIO_SIM_DEFAULT_VIN, sizeof ECUIO_SIM_DEFAULT_VIN);
    const char* body = nullptr;
    if (std::strncmp(uri, "replay://", 9) == 0) {
        out->source = ecuio_sim_source::replay;
        body = uri + 9;
        const char* q = std::strchr(body, '?');
        out->path = body;
        out->path_len =
            q != nullptr ? static_cast<size_t>(q - body) : std::strlen(body);
        const std::string path(body, out->path_len);
#ifdef _WIN32
        const bool absolute = ecuio_is_absolute_path(path.c_str());
#else
        const bool absolute = !path.empty() && path[0] == '/';
#endif
        if (path.empty()) {
            std::snprintf(why, why_cap,
                          "replay:// needs a recording's "
                          "folder or its frames.csv");
            return ecuio_sim_uri_result::bad;
        }
        if (!absolute) {
            std::snprintf(why, why_cap,
                          "the recording's path must be absolute");
            return ecuio_sim_uri_result::bad;
        }
    } else if (std::strncmp(uri, "mock://", 7) == 0) {
        body = uri + 7;
    } else {
        std::snprintf(why, why_cap,
                      "the simulated ECU takes mock:// or replay://");
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
    static const char* const kMockKeys[] = {"values", "modules", "profile",
                                            "vin",    "faults",  nullptr};
    static const char* const kReplayKeys[] = {"loop", "from",   "to",
                                              "vin",  "faults", nullptr};
    if (const ecuio_uri::param* p =
            ecuio_uri::first_unknown(q, replay ? kReplayKeys : kMockKeys)) {
        std::snprintf(why, why_cap,
                      "the simulated ECU takes no query key '%.*s' for %s, "
                      "only %s, vin and faults",
                      static_cast<int>(p->key_len < 40 ? p->key_len : 40),
                      p->key, replay ? "replay://" : "mock://",
                      replay ? "loop, from, to" : "values, modules, profile");
        return ecuio_sim_uri_result::bad;
    }
    for (size_t i = 0; i < q.count; ++i) {
        const ecuio_uri::param& p = q.params[i];
        const auto value_is = [&p](const char* v) {
            return p.val_len == std::strlen(v) &&
                   std::strncmp(p.val, v, p.val_len) == 0;
        };
        if (p.is("values")) {
            if (value_is("drive")) {
                out->values = ecuio_sim_values::drive;
            } else if (value_is("canned")) {
                out->values = ecuio_sim_values::canned;
            } else {
                std::snprintf(why, why_cap, "values: drive or canned");
                return ecuio_sim_uri_result::bad;
            }
        } else if (p.is("modules")) {
            if (!value_is("1") && !value_is("2")) {
                std::snprintf(why, why_cap,
                              "modules: 1 (the engine ECU) or 2 (and a "
                              "second module on 0x7E1/0x7E9)");
                return ecuio_sim_uri_result::bad;
            }
            out->second_module = value_is("2");
        } else if (p.is("profile")) {
            /* The registry's grammar (profile.h): 1-63 of [a-z0-9._-] */
            bool ok = p.val_len >= 1 && p.val_len <= ECUIO_PROFILE_NAME_MAX;
            for (size_t k = 0; ok && k < p.val_len; ++k) {
                const char c = p.val[k];
                ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                     c == '.' || c == '_' || c == '-';
            }
            if (!ok) {
                std::snprintf(why, why_cap,
                              "profile: a registered profile's name, 1 to "
                              "%d of a-z, 0-9, '.', '_' and '-'",
                              ECUIO_PROFILE_NAME_MAX);
                return ecuio_sim_uri_result::bad;
            }
            out->profile = p.val;
            out->profile_len = p.val_len;
        } else if (p.is("vin")) {
            char vin[ECUIO_VIN_LEN + 1] = {};
            ecuio_vin_info_t info;
            if (p.val_len == ECUIO_VIN_LEN) {
                std::memcpy(vin, p.val, ECUIO_VIN_LEN);
            }
            if (vin[0] == '\0' || ecuio_vin_decode(vin, 0, &info) != ECUIO_OK) {
                std::snprintf(why, why_cap,
                              "vin: 17 digits and capital letters, never I, "
                              "O or Q");
                return ecuio_sim_uri_result::bad;
            }
            std::memcpy(out->vin, vin, sizeof vin);
            out->vin_given = true;
        } else if (p.is("loop")) {
            if (!value_is("0") && !value_is("1")) {
                std::snprintf(why, why_cap, "loop: 0 or 1");
                return ecuio_sim_uri_result::bad;
            }
            out->loop = value_is("1");
        } else if (p.is("from") || p.is("to")) {
            if (!ecuio_stretch::take(p, &out->stretch)) {
                std::snprintf(why, why_cap,
                              "from and to: the recording's seconds, as the "
                              "viewer and havne-cli view show them, with at "
                              "most three decimals");
                return ecuio_sim_uri_result::bad;
            }
        } else if (p.is("faults")) {
            if (!ecuio_sim_parse_faults(p.val, p.val_len, &out->faults, why,
                                        why_cap)) {
                return ecuio_sim_uri_result::bad;
            }
        }
    }
    if (out->profile_len != 0 && out->second_module) {
        std::snprintf(why, why_cap,
                      "profile and modules=2 both name a second module: the "
                      "profile's channels are the modules");
        return ecuio_sim_uri_result::bad;
    }
    if (!ecuio_stretch::ordered(out->stretch)) {
        std::snprintf(why, why_cap, "to must come after from");
        return ecuio_sim_uri_result::bad;
    }
    return ecuio_sim_uri_result::ok;
}

#endif /* ECUIO_SIM_ECU_H */
