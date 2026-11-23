// File/layer: tests/fake_j2534_state.h | Fake J2534 vendor DLL, HARNESS tier;
//             built in BOTH trees
// Testing: the state and helpers of fake_j2534*.cpp (see fake_j2534.cpp)

#pragma once

#include <windows.h>

#include "ecuio/j2534_ptypes.h"
#include "fake_control.h"
#include "fake_ecus.h"
#include "fake_j2534_queue.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <cstdlib>

#define FAKE_FILTER_DEPTH 32
#define FAKE_CONFIG_DEPTH 16
#define STATUS_NOERROR ECUIO_PT_STATUS_NOERROR
#define STATUS_FAILED  ECUIO_PT_STATUS_FAILED
#define ERR_EXCEEDED_LIMIT 0x0CL /* J2534-1: no filter or message ID left */

namespace fake_j2534 {

struct FilterRecord {
    unsigned long filter_id;
    unsigned long filter_type;
    unsigned char mask[12];
    unsigned char pattern[12];
    unsigned char flow_control[12];
    unsigned long mask_len;
};

inline int g_handle = 0;                       /* 1 between Open and Close */

/* The VX as measured (docs/bench-sessions/2026-10-22.md): its reply
 * latency is the engine ECU's median from indication to reply; with
 * nothing on the bus to
 * acknowledge, it accepted three writes per connect, then blocked
 * each later one 500-515 ms and failed it. */
constexpr unsigned long kVxReplyDelayMs = 4;
/* A VX read with a timeout of 0 holds about 50 ms, whenever its reply
 * arrives: in the test car's frame log a reply read that
 * way reached the host a median 63 ms after its write, with the reply
 * on the adapter's clock a median 11.1 ms after its request's TX
 * indication (docs/bench-sessions/2026-11-04-two-ecu-car.md) */
constexpr unsigned long kVxZeroTimeoutHoldMs = 50;
/* A VX read asked to wait less than 10 ms holds about 10 ms unless its
 * count is met first: on the test car a call asked to wait 1 ms for the
 * rest of a batch held a median 9.8 ms, 90 % under 10.4, and nearly
 * always returned nothing (docs/bench-sessions/2026-11-05-two-ecu-car-rerun.md) */
constexpr unsigned long kVxShortReadHoldMs = 10;
constexpr unsigned long kVxUnackedDepth = 3;
constexpr DWORD kVxRefusedWriteMs = 500;
constexpr unsigned long kVxIndication = ECUIO_PT_TX_MSG_TYPE |
                                        ECUIO_PT_TX_INDICATION;
/* What the VX queued after each refused write: the request echoed with
 * RxStatus 0x201. Bit 0x200 isn't a v04.04 RxStatus bit; its meaning is
 * unknown. */
constexpr unsigned long kVxRefusedEcho = 0x201;
inline unsigned long g_unacked = 0;            /* writes accepted since connect */

/* PassThruGetLastError's text: the last refusal a knob forced. */
inline char g_last_error[ECUIO_PT_TEXT_LEN] = "fake_j2534: no error";

/* PassThruReadVersion's answers. */
constexpr char kFirmwareVersion[] = "fake-fw 1.0";
constexpr char kDllVersion[] = "fake_j2534 0.3";
constexpr char kApiVersion[] = "04.04";

/* IoCtl config table: SET_CONFIG stores, GET_CONFIG reads back.
 * Unknown parameters GET back as 0xFFFFFFFF ("vendor default"). */
inline unsigned long g_cfg_param[FAKE_CONFIG_DEPTH];
inline unsigned long g_cfg_value[FAKE_CONFIG_DEPTH];
inline unsigned long g_cfg_len = 0;

/* Filter bookkeeping: a started filter takes the first free slot (a
 * stopped one's too), filter_id 0 marking a free one */
inline FilterRecord g_filters[FAKE_FILTER_DEPTH];
inline unsigned long g_filter_seq = 0;
inline FilterRecord g_last_filter;              /* the last started: its pattern
                                          * heads the echo and the replies */
/* One-shot ioctl stall: env-armed sleep on
 * the FIRST PassThruIoctl only: a vendor op that outlives the
 * client's 3s transact timeout, while every later op answers
 * instantly, isolating the recovery path under test. */
inline bool g_ioctl_delay_fired = false;
/* The same for the FIRST PassThruReadMsgs: a read answered past the
 * client's deadline for it, with whatever the queue then holds. */
inline bool g_read_delay_fired = false;

/* The IDs PassThruOpen and PassThruConnect hand out: device 1 and
 * channel 0x55, or 0 for both under ECUIO_FAKE_ZERO_IDS=1, since J2534
 * doesn't reserve 0. The device-scoped calls (READ_VBATT,
 * PassThruReadVersion) answer only the device ID. */
constexpr unsigned long kDeviceId = 1;
constexpr unsigned long kChannelId = 0x55;
inline unsigned long g_device_id = kDeviceId;
inline unsigned long g_channel_id = kChannelId;

/* A knob's value: the control page's when it holds the name, else the
 * environment's. False when neither sets it, or it doesn't fit buf. */
inline bool read_knob(const char* name, char* buf, DWORD cap)
{
    fake_control::Page* page = fake_control::attach();
    if (page != nullptr && fake_control::get(page, name, buf, cap)) {
        return buf[0] != '\0';
    }
    const DWORD n = GetEnvironmentVariableA(name, buf, cap);
    return n > 0 && n < cap;
}

/* The status a refusal knob forces on call, 0 when unset. A refusal
 * also sets the text PassThruGetLastError returns. */
inline long refusal(const char* knob, const char* call)
{
    char buf[32] = {};
    if (!read_knob(knob, buf, sizeof(buf))) {
        return 0;
    }
    const long code = strtol(buf, nullptr, 0);
    if (code != 0) {
        std::lock_guard<std::mutex> lk(g_lock);
        snprintf(g_last_error, sizeof(g_last_error),
                 "fake_j2534: %s refused (%s)", call, knob);
    }
    return code;
}

inline bool knob_on(const char* name)
{
    char buf[8] = {};
    return read_knob(name, buf, sizeof(buf)) && buf[0] == '1';
}

inline unsigned long knob_ms(const char* name, unsigned long fallback)
{
    char buf[32] = {};
    if (!read_knob(name, buf, sizeof(buf))) {
        return fallback;
    }
    const unsigned long ms = strtoul(buf, nullptr, 10);
    return (ms <= 60000) ? ms : fallback;
}

/* Whether this process is a surrogate (its exe is
 * ecuio-j2534-surrogate*.exe), the only host ECUIO_FAKE_EXIT_ON_READ
 * may end. */
inline bool in_a_surrogate()
{
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return false;
    }
    const char* name = strrchr(path, '\\');
    name = (name != nullptr) ? name + 1 : path;
    return _strnicmp(name, "ecuio-j2534-surrogate", 21) == 0;
}

/* The VX's clock: microseconds, 32 bits, wrapping. */
inline unsigned long vendor_stamp(long long steady_us)
{
    return static_cast<unsigned long>(steady_us & 0xFFFFFFFFLL);
}

/* Moves the messages a test queued on the control page into the RX
 * FIFO (mutex held by caller), RxStatus verbatim. */
inline void take_posted_locked()
{
    fake_control::Page* page = fake_control::attach();
    if (page == nullptr) {
        return;
    }
    fake_control::Message posted[fake_control::kInboxDepth];
    const uint32_t n = fake_control::take(page, posted,
                                          fake_control::kInboxDepth);
    for (uint32_t i = 0; i < n; ++i) {
        enqueue_rx_locked(posted[i].data, posted[i].size,
                          posted[i].timestamp, posted[i].rx_status);
    }
}

/* Matches the most recent active filter against a frame's first
 * mask_len bytes (the pattern is applied with the mask: classic
 * filter semantics). Returns true if the frame passes. The mask is
 * assumed to be zero-padded beyond the meaningful bytes. */
inline bool filter_matches(const unsigned char* frame_data, unsigned long frame_size,
                    const FilterRecord& f)
{
    if (frame_size < f.mask_len) {
        return false;
    }
    for (unsigned long i = 0; i < f.mask_len; ++i) {
        if ((frame_data[i] & f.mask[i]) != (f.pattern[i] & f.mask[i])) {
            return false;
        }
    }
    return true;
}

/* The ID the fake's ECU answers a request on (fake_ecu::reply_id), into
 * out (4 bytes, big-endian); any other ID falls back to the last
 * filter's pattern. */
inline void reply_header(const unsigned char* req, unsigned char* out)
{
    const uint32_t tx = (uint32_t{req[0]} << 24) | (uint32_t{req[1]} << 16) |
                        (uint32_t{req[2]} << 8) | uint32_t{req[3]};
    const uint32_t rx = fake_ecu::reply_id(tx);
    if (rx == 0) {
        memcpy(out, g_last_filter.pattern, 4);
        return;
    }
    out[0] = static_cast<unsigned char>(rx >> 24);
    out[1] = static_cast<unsigned char>(rx >> 16);
    out[2] = static_cast<unsigned char>(rx >> 8);
    out[3] = static_cast<unsigned char>(rx);
}

/* A request's negative reply: [reply ID][7F][service][code]. */
inline void build_negative(const unsigned char* req, unsigned char code,
                    unsigned char* resp, unsigned long* resp_len)
{
    reply_header(req, resp);
    resp[4] = 0x7F;
    resp[5] = req[4];
    resp[6] = code;
    *resp_len = 7;
}

/* An OBD-II Service 01, 09 or 0x22 response into resp, which holds
 * ECUIO_PT_MSG_DATA_SIZE bytes: [reply ID BE] and the fake's ECU's
 * answer (fake_ecu::answer): NO PCI BYTE in either direction. The fake
 * models the ADAPTER side of ISO-TP: this queue content is what a
 * conforming adapter delivers from ReadMsgs, and the transport's header
 * strip lands the bridge on [41][PID][A][B]...: the obd2 set's
 * mux-at-offset-0 grammar.
 * Caller (WriteMsgs, mutex HELD) has verified: [tx_id BE][0x01][PID] or
 * [tx_id BE][0x09][item] (6 bytes), [tx_id BE][0x01] and two to six
 * PIDs (7 to 11 bytes), [tx_id BE][0x22][DID] or
 * [tx_id BE][0x02][PID][frame] (7 bytes), or [tx_id BE][service] for
 * Services 03, 04, 07 and 0x0A (5 bytes), and an active filter. False
 * when nothing answers. */
inline bool build_obd2_response(const unsigned char* req, unsigned long req_len,
                         long unsupported_nrc,
                         unsigned char* resp, unsigned long* resp_len)
{
    static_assert(4 + fake_ecu::kReplyMax <= ECUIO_PT_MSG_DATA_SIZE,
                  "the longest answer fills PASSTHRU_MSG.Data");
    size_t n = 0;
    const uint32_t tx = (uint32_t{req[0]} << 24) | (uint32_t{req[1]} << 16) |
                        (uint32_t{req[2]} << 8) | uint32_t{req[3]};
    /* A module of ECUIO_FAKE_MODULE_DIDS answers from its own rows */
    if (!fake_ecu::answer_for(fake_ecu::reply_id(tx), req + 4, req_len - 4,
                              unsupported_nrc, resp + 4, &n)) {
        return false;
    }
    reply_header(req, resp);
    *resp_len = static_cast<unsigned long>(4 + n);
    return true;
}

} // namespace fake_j2534
