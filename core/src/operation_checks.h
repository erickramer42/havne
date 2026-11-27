#ifndef ECUIO_OPERATION_CHECKS_H
#define ECUIO_OPERATION_CHECKS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

constexpr bool ecuio_check_allowed(uint8_t sid, uint8_t pid)
{
    return sid == 0x01 &&
           (pid == 0x0C || pid == 0x0D || pid == 0x40 || pid == 0x42);
}

namespace ecuio_operation {

constexpr uint32_t kDumpWarnLowMv = 11800;
constexpr uint32_t kDumpStopMv = 11000;
constexpr uint32_t kVoltsWarnHighMv = 15000;
constexpr int64_t kVoltsEveryUs = 2000000;
constexpr int64_t kVoltsLateUs = 5000000;

/* The battery's readings at the adapter, or the engine module's 01 42 */
struct volts {
    bool first = false; /* the start's reading is settled, got or not */
    bool has = false;
    bool from_ecu = false;
    bool low_warn = false;
    bool high_warn = false;
    bool dropped = false;
    uint32_t mv = 0;
    uint32_t min_mv = 0;
};

/* running: the start's reading is in, so this one is the run's */
inline void volts_take(volts& v, uint32_t mv, bool running)
{
    v.mv = mv;
    v.min_mv = v.has ? (std::min)(v.min_mv, mv) : mv;
    v.has = true;
    v.high_warn = v.high_warn || mv > kVoltsWarnHighMv;
    if (!running) {
        v.low_warn = mv < kDumpWarnLowMv;
    } else if (mv < kDumpStopMv) {
        v.dropped = true;
    }
}

enum class check_stage : uint8_t { rpm, kmh, volts_listed, volts, done };
enum class check_end : uint8_t { none, engine_running, moving };

struct checks {
    uint32_t modules[2] = {};
    uint32_t module_count = 0;
    uint32_t at_module = 0;
    check_stage at = check_stage::rpm;
    check_end result = check_end::none;
    bool want_volts = false;
    bool has_rpm = false;
    bool has_kmh = false;
    bool has_volts = false;
    uint32_t rpm = 0;
    uint32_t kmh = 0;
    uint32_t volts_mv = 0;
};

struct check_request {
    uint32_t module = 0;
    uint8_t bytes[2] = {};
};

/* engine UINT32_MAX for none; the target is asked after it */
inline void check_begin(checks& k, uint32_t engine, uint32_t target,
                        bool want_volts)
{
    k = checks{};
    k.want_volts = want_volts;
    if (engine != UINT32_MAX && engine != target) {
        k.modules[k.module_count++] = engine;
    }
    k.modules[k.module_count++] = target;
}

inline void check_stage_unread(checks& k)
{
    k.at_module = 0;
    switch (k.at) {
    case check_stage::rpm:
        k.at = check_stage::kmh;
        break;
    case check_stage::kmh:
        k.at = k.want_volts ? check_stage::volts_listed : check_stage::done;
        break;
    default:
        k.at = check_stage::done;
        break;
    }
}

inline void check_next_module(checks& k)
{
    if (k.at == check_stage::volts) {
        k.at = check_stage::volts_listed;
    }
    if (++k.at_module >= k.module_count) {
        check_stage_unread(k);
    }
}

inline bool check_next(const checks& k, check_request& out)
{
    if (k.at == check_stage::done) {
        return false;
    }
    out.module = k.modules[k.at_module];
    out.bytes[0] = 0x01;
    out.bytes[1] = k.at == check_stage::rpm            ? uint8_t{0x0C}
                   : k.at == check_stage::kmh          ? uint8_t{0x0D}
                   : k.at == check_stage::volts_listed ? uint8_t{0x40}
                                                       : uint8_t{0x42};
    return true;
}

inline void check_give_up(checks& k)
{
    k.at = check_stage::done;
}

inline void check_silence(checks& k)
{
    if (k.at != check_stage::done) {
        check_next_module(k);
    }
}

inline void check_reply(checks& k, const uint8_t* d, size_t n)
{
    if (k.at == check_stage::done) {
        return;
    }
    const bool ours = n >= 3 && d[0] == 0x41;
    if (ours && k.at == check_stage::rpm && n >= 4 && d[1] == 0x0C) {
        const uint32_t raw = (uint32_t{d[2]} << 8) | d[3];
        k.has_rpm = true;
        k.rpm = (raw + 3) / 4;
        if (raw > 0) {
            k.result = check_end::engine_running;
            k.at = check_stage::done;
        } else {
            check_stage_unread(k);
        }
    } else if (ours && k.at == check_stage::kmh && d[1] == 0x0D) {
        k.has_kmh = true;
        k.kmh = d[2];
        if (d[2] > 0) {
            k.result = check_end::moving;
            k.at = check_stage::done;
        } else {
            check_stage_unread(k);
        }
    } else if (ours && k.at == check_stage::volts_listed && n >= 6 &&
               d[1] == 0x40 && (d[2] & 0x40) != 0) {
        k.at = check_stage::volts;
    } else if (ours && k.at == check_stage::volts && n >= 4 && d[1] == 0x42) {
        k.has_volts = true;
        k.volts_mv = (uint32_t{d[2]} << 8) | d[3];
        k.at = check_stage::done;
    } else {
        check_next_module(k);
    }
}

} // namespace ecuio_operation

#endif /* ECUIO_OPERATION_CHECKS_H */
