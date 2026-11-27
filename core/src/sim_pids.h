// File/layer: core/src/sim_pids.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: SimEcuTest, SimRecordingTest (test_obd2)
//
// The simulated ECU's PID answers: which data PIDs the engine ECU has and
// what a PID reads (the drive cycle's or the fakes' canned values).

#ifndef ECUIO_SIM_PIDS_H
#define ECUIO_SIM_PIDS_H

#include "sim_types.h"
#include "mock_ecu.h"
#include "mock_drive_cycle.h"
#include "sim_canned.h"
#include <cstdint>
#include <cstring>

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
constexpr uint8_t kDrivePids[] = {0x04, 0x05, 0x0B, 0x0C, 0x0D,
                                  0x0F, 0x10, 0x11, 0x1F, 0x24,
                                  0x33, 0x42, 0x44, 0x46, 0x5C};

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
    if (e.config.values != ecuio_sim_values::drive) {
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
    if (e.config.values == ecuio_sim_values::parked) {
        if (pid == 0x0C) {
            put16(out, 0);
            return 2;
        }
        if (pid == 0x0D) {
            out[0] = 0;
            return 1;
        }
        if (pid == 0x42) {
            put16(out, 12600);
            return 2;
        }
    }
    if (e.config.values != ecuio_sim_values::drive) {
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
    case 0x0B: /* MAP, A kPa: a byte, so boost past 255 kPa absolute reads 255 */
        out[0] = u8(car.map_kpa);
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
    case 0x33: /* baro, A kPa */
        out[0] = u8(car.baro_kpa);
        return 1;
    case 0x42: /* module voltage, (256A + B) / 1000: the drive cycle's
                  battery, charging while the engine runs */
        put16(out, ecuio_mock::clamp_u16(car.battery_v * 1000.0f));
        return 2;
    case 0x44: /* commanded lambda, (256A + B) * 2 / 65536 */
        put16(out, ecuio_mock::clamp_u16(car.commanded_lambda * 32768.0f));
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
} // namespace ecuio_sim_detail

#endif /* ECUIO_SIM_PIDS_H */
