// File/layer: tests/fake_j2534.cpp | Fake J2534 vendor DLL, HARNESS tier;
//             built in BOTH trees
// Testing: exercised via LoadLibraryEx by test_j2534_load/_stream and
//          via the surrogate child (either arch)
// NOTE: InjectFrame/ReadMsgs share a mutex-guarded FIFO — injections
//      from the test thread race safely against the polling worker.
//      Connect params are recorded so tests can assert that URI
//      parsing reached PassThruConnect.
//
// BEHAVIOR:
//  * WriteMsgs: records the message (hook-observable). Response
//    generation is two-mode:
//      - Mode-01 request [4-byte ID BE][01][PID] (exactly 6 bytes, the
//        adapter-framed logical message): response
//        [rx_id BE][41][PID][canned], values from g_obd2_canned
//        (SetFakeObd2CannedValueExternal overrides per-test). NO PCI
//        byte in either direction — the fake models the adapter side
//        of ISO-TP, and the transport's 4-byte header strip lands the
//        bridge on the obd2 set's mux-at-offset-0 grammar.
//      - Everything else: the legacy deterministic echo (byte 0
//        +0x40), preserving existing framing tests.
//    ECUIO_FAKE_WRITE_STATUS fails every write with the given status,
//    recording nothing and echoing nothing.
//  * IoCtl: config table (SET stores, GET reads back), fixed VBATT,
//    CLEAR_* accepted. Unknown ids FAIL like a strict vendor.
//  * Disconnect: counted; ECUIO_FAKE_DISCONNECT_DELAY_MS sleeps
//    before each one (a slow vendor teardown).
//  * Filters: registration bookkeeping + last-invocation recording.
//    Filter-aware echo prepends the filter pattern (4-byte BE CAN ID).
//    SetFilterGating controls ONLY ReadMsgs dropping; the echo shape
//    is unconditional once a filter lands.
//  * ReadMsgs, by default, returns at most one message per call,
//    whatever NumMsgs and Timeout ask for, and STATUS_NOERROR with zero
//    messages on an empty queue. ECUIO_FAKE_READ_STATUS forces a
//    status with no messages; ECUIO_FAKE_UNTOUCHED_COUNT_STATUS
//    forces a status with the caller's count and buffer left as
//    passed (a sloppy vendor; see ecuio_trusted_count);
//    ECUIO_FAKE_SPEC_READ_SEMANTICS=1 switches to J2534-1 batch and
//    empty statuses.
//  * ExtraDataIndex = DataSize (v4.04 no-extra-data convention).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ecuio/j2534_ptypes.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <cstdlib>

#define FAKE_QUEUE_DEPTH 16
#define FAKE_WRITE_DEPTH 16
#define FAKE_FILTER_DEPTH 8
#define FAKE_CONFIG_DEPTH 16
#define STATUS_NOERROR ECUIO_PT_STATUS_NOERROR
#define STATUS_FAILED  ECUIO_PT_STATUS_FAILED

namespace fake_j2534 {

struct FrameSlot {
    unsigned char data[64];
    unsigned long size;
    unsigned long timestamp_us;
    unsigned long rx_status;   /* replayed verbatim into PASSTHRU_MSG.RxStatus */
};

struct WriteRecord {
    unsigned char data[64];
    unsigned long size;
    unsigned long tx_flags;
};

struct FilterRecord {
    unsigned long filter_id;
    unsigned long filter_type;
    unsigned char mask[12];
    unsigned char pattern[12];
    unsigned char flow_control[12];
    unsigned long mask_len;
};

std::mutex g_lock;
int g_handle = 0;                       /* 1 between Open and Close */
FrameSlot g_queue[FAKE_QUEUE_DEPTH];
unsigned long g_queue_len = 0;

/* Write recording + response generation */
WriteRecord g_writes[FAKE_WRITE_DEPTH];
unsigned long g_write_count = 0;         /* total, monotonic */

/* IoCtl config table: SET_CONFIG stores, GET_CONFIG reads back.
 * Unknown parameters GET back as 0xFFFFFFFF ("vendor default"). */
unsigned long g_cfg_param[FAKE_CONFIG_DEPTH];
unsigned long g_cfg_value[FAKE_CONFIG_DEPTH];
unsigned long g_cfg_len = 0;

/* Filter bookkeeping */
FilterRecord g_filters[FAKE_FILTER_DEPTH];
unsigned long g_filter_seq = 0;
FilterRecord g_last_filter;              /* raw last StartMsgFilter call */
bool g_filter_gating_enabled = false;    /* SetFilterGating toggle — controls
                                          * ReadMsgs dropping ONLY, not echo */
/* One-shot ioctl stall: env-armed sleep on
 * the FIRST PassThruIoctl only — a vendor op that outlives the
 * client's 3s transact timeout, while every later op answers
 * instantly, isolating the recovery path under test. */
bool g_ioctl_delay_fired = false;

/* Last PassThruConnect invocation, recorded BEFORE the validity
 * check — even a refused connect shows what was asked, which is
 * what the query-param tests assert against. */
unsigned long g_last_connect_baud = 0;
unsigned long g_last_connect_flags = 0;
unsigned long g_last_connect_protocol = 0;

/* Last StopMsgFilter invocation, for detach-ordering assertion. */
unsigned long g_last_stopped_filter_id = 0;

/* Disconnect counter: a successful client disconnect must clear the
 * child's channel so teardown never issues a SECOND vendor disconnect.
 * Monotonic, read via GetDisconnectCount — in-proc instances only. The
 * child's instance is a separate process (see the OP_INJECT note
 * above), so this hook can't see it; a surrogate test would count the
 * child's OP_DISCONNECT lines in the session log
 * (ecuio_j2534_sur_log_path) instead. */
unsigned long g_disconnect_count = 0;

/* The threads that last ran PassThruOpen and PassThruDisconnect (0 =
 * never), read via GetLastCallThreadIds: the bridge tests check that
 * neither runs on the GUI thread. In-proc instances only, like the
 * disconnect count. */
unsigned long g_open_thread_id = 0;
unsigned long g_disconnect_thread_id = 0;

/* ---- OBD-II canned response data (HARNESS only, ADR-0013) ----
 * Standard PIDs that express as simple byte-extraction + scale/offset.
 * Each default is a plausible reading on its own, not one consistent
 * engine state: RPM 2000, speed 0, load ~10%, throttle ~8%, coolant
 * and IAT 20 degC, oil 80 degC, fuel ~25%, voltage ~13.9 V, MAF
 * 1.00 g/s. Oil reads warmer than coolant and IAT so a test can tell
 * the A - 40 temperatures apart. Overridable per-test through
 * SetFakeObd2CannedValueExternal (same mutex discipline as all
 * other module globals). */
struct Obd2CannedValues {
    uint8_t pid_04_engine_load;
    uint8_t pid_05_coolant_temp;
    uint8_t pid_0C_rpm_high;
    uint8_t pid_0C_rpm_low;
    uint8_t pid_0D_speed;
    uint8_t pid_0F_iat;
    uint8_t pid_11_throttle;
    uint8_t pid_2F_fuel_level;
    uint8_t pid_42_volt_high;
    uint8_t pid_42_volt_low;
    uint8_t pid_10_maf_high;
    uint8_t pid_10_maf_low;
    uint8_t pid_5C_oil_temp;
};

Obd2CannedValues g_obd2_canned;

/* Enqueue into the shared RX FIFO (mutex held by caller). Drops on
 * full exactly like InjectFrame — documented, keeps counts honest. */
void enqueue_rx_locked(const unsigned char* data, unsigned long size,
                       unsigned long timestamp_us, unsigned long rx_status)
{
    if (g_queue_len >= FAKE_QUEUE_DEPTH) {
        return;
    }
    FrameSlot& slot = g_queue[g_queue_len++];
    memcpy(slot.data, data, size);
    slot.size = size;
    slot.timestamp_us = timestamp_us;
    slot.rx_status = rx_status;
}

/* Reset canned OBD-II values to the cold-idle defaults. Called once
 * per module load from PassThruOpen (function-local static guards the
 * once-ness); per-test overrides via SetFakeObd2CannedValueExternal
 * survive subsequent opens because the reset never re-runs. */
void reset_obd2_canned()
{
    g_obd2_canned.pid_04_engine_load = 25;   /* 25 * 100/255 ~= 9.8% */
    g_obd2_canned.pid_05_coolant_temp = 60;  /* 60 - 40 = 20 degC */
    g_obd2_canned.pid_0C_rpm_high = 0x1F;    /* 0x1F40 * 0.25 = 2000 RPM */
    g_obd2_canned.pid_0C_rpm_low = 0x40;
    g_obd2_canned.pid_0D_speed = 0;          /* 0 km/h */
    g_obd2_canned.pid_0F_iat = 60;           /* 20 degC */
    g_obd2_canned.pid_11_throttle = 20;      /* ~= 7.8% */
    g_obd2_canned.pid_2F_fuel_level = 64;    /* ~= 25% tank */
    g_obd2_canned.pid_42_volt_high = 0x36;   /* 0x3630 = 13872 mV */
    g_obd2_canned.pid_42_volt_low = 0x30;
    g_obd2_canned.pid_10_maf_high = 0x00;  /* 0x0064 * 0.01 = 1.00 g/s */
    g_obd2_canned.pid_10_maf_low = 0x64;
    g_obd2_canned.pid_5C_oil_temp = 0x78;  /* 120 - 40 = 80 degC */
}

/* Matches the most recent active filter against a frame's first
 * mask_len bytes (the pattern is applied with the mask — classic
 * filter semantics). Returns true if the frame passes. The mask is
 * assumed to be zero-padded beyond the meaningful bytes. */
bool filter_matches(const unsigned char* frame_data, unsigned long frame_size,
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

/* Build an OBD-II Mode-01 response.
 * Caller (WriteMsgs, mutex HELD) has verified: exactly 6 bytes
 * [tx_id BE][0x01][PID] and an active filter. Output:
 * [rx_id BE from the filter pattern][0x41][PID][canned data] — NO
 * PCI BYTE in either direction. The fake models the ADAPTER side of
 * ISO-TP: this queue content is what a conforming adapter delivers
 * from ReadMsgs, and the transport's header strip lands the bridge
 * on [41][PID][A][B]... — the obd2 set's mux-at-offset-0 grammar.
 * Response length varies per PID. */
void build_obd2_response(const unsigned char* req, unsigned long req_len,
                         unsigned char* resp, unsigned long* resp_len)
{
    (void)req_len;   /* shape pre-validated by the caller */
    const uint8_t pid = req[5];

    resp[0] = g_last_filter.pattern[0];
    resp[1] = g_last_filter.pattern[1];
    resp[2] = g_last_filter.pattern[2];
    resp[3] = g_last_filter.pattern[3];
    resp[4] = 0x41;   /* Mode 01 + 0x40 response */
    resp[5] = pid;    /* PID echo */

    switch (pid) {
        case 0x00:  /* Supported PIDs bitmask (all zero for v1) */
            resp[6] = resp[7] = resp[8] = resp[9] = 0;
            *resp_len = 10;
            break;
        case 0x04: resp[6] = g_obd2_canned.pid_04_engine_load;  *resp_len = 7;  break;
        case 0x05: resp[6] = g_obd2_canned.pid_05_coolant_temp; *resp_len = 7;  break;
        case 0x0C: resp[6] = g_obd2_canned.pid_0C_rpm_high;
                   resp[7] = g_obd2_canned.pid_0C_rpm_low;      *resp_len = 8;  break;
        case 0x0D: resp[6] = g_obd2_canned.pid_0D_speed;        *resp_len = 7;  break;
        case 0x0F: resp[6] = g_obd2_canned.pid_0F_iat;          *resp_len = 7;  break;
        case 0x11: resp[6] = g_obd2_canned.pid_11_throttle;      *resp_len = 7;  break;
        case 0x2F: resp[6] = g_obd2_canned.pid_2F_fuel_level;    *resp_len = 7;  break;
        case 0x42: resp[6] = g_obd2_canned.pid_42_volt_high;
                   resp[7] = g_obd2_canned.pid_42_volt_low;     *resp_len = 8;  break;
        case 0x10: resp[6] = g_obd2_canned.pid_10_maf_high;
                   resp[7] = g_obd2_canned.pid_10_maf_low;      *resp_len = 8;  break;
        case 0x5C: resp[6] = g_obd2_canned.pid_5C_oil_temp;     *resp_len = 7;  break;
        default:    /* Unsupported PID — bare grammatical echo */
            *resp_len = 6;
            break;
    }
}

/* Override canned response values for a specific PID. */
void SetFakeObd2CannedValue(unsigned char pid, unsigned char* data,
                            unsigned long len)
{
    if (data == nullptr || len == 0) return;
    std::lock_guard<std::mutex> lk(g_lock);
    switch (pid) {
        case 0x04: if (len >= 1) g_obd2_canned.pid_04_engine_load = data[0]; break;
        case 0x05: if (len >= 1) g_obd2_canned.pid_05_coolant_temp = data[0]; break;
        case 0x0C: if (len >= 2) { g_obd2_canned.pid_0C_rpm_high = data[0];
                                   g_obd2_canned.pid_0C_rpm_low = data[1]; } break;
        case 0x0D: if (len >= 1) g_obd2_canned.pid_0D_speed = data[0]; break;
        case 0x0F: if (len >= 1) g_obd2_canned.pid_0F_iat = data[0]; break;
        case 0x11: if (len >= 1) g_obd2_canned.pid_11_throttle = data[0]; break;
        case 0x2F: if (len >= 1) g_obd2_canned.pid_2F_fuel_level = data[0]; break;
        case 0x42: if (len >= 2) { g_obd2_canned.pid_42_volt_high = data[0];
                                   g_obd2_canned.pid_42_volt_low = data[1]; } break;
        case 0x10: if (len >= 2) { g_obd2_canned.pid_10_maf_high = data[0];
                                   g_obd2_canned.pid_10_maf_low = data[1]; } break;
        case 0x5C: if (len >= 1) g_obd2_canned.pid_5C_oil_temp = data[0]; break;
    }
}

/* Get current canned value for a PID (for test assertions). */
void GetFakeObd2CannedValue(unsigned char pid, unsigned char* out_data,
                            unsigned long* out_len)
{
    if (out_data == nullptr || out_len == nullptr) return;
    std::lock_guard<std::mutex> lk(g_lock);
    switch (pid) {
        case 0x04: *out_len = 1; out_data[0] = g_obd2_canned.pid_04_engine_load; break;
        case 0x05: *out_len = 1; out_data[0] = g_obd2_canned.pid_05_coolant_temp; break;
        case 0x0C: *out_len = 2; out_data[0] = g_obd2_canned.pid_0C_rpm_high;
                                    out_data[1] = g_obd2_canned.pid_0C_rpm_low; break;
        case 0x0D: *out_len = 1; out_data[0] = g_obd2_canned.pid_0D_speed; break;
        case 0x0F: *out_len = 1; out_data[0] = g_obd2_canned.pid_0F_iat; break;
        case 0x11: *out_len = 1; out_data[0] = g_obd2_canned.pid_11_throttle; break;
        case 0x2F: *out_len = 1; out_data[0] = g_obd2_canned.pid_2F_fuel_level; break;
        case 0x42: *out_len = 2; out_data[0] = g_obd2_canned.pid_42_volt_high;
                                    out_data[1] = g_obd2_canned.pid_42_volt_low; break;
        case 0x10: *out_len = 2; out_data[0] = g_obd2_canned.pid_10_maf_high;
                                   out_data[1] = g_obd2_canned.pid_10_maf_low; break;
        case 0x5C: *out_len = 1; out_data[0] = g_obd2_canned.pid_5C_oil_temp; break;
        default: *out_len = 0; break;
    }
}

} // namespace fake_j2534

extern "C" __declspec(dllexport)
long __stdcall PassThruOpen(void* pName, unsigned long* pDeviceID)
{
    (void)pName;
    if (pDeviceID == nullptr) {
        return STATUS_FAILED;
    }

    /* Initialize canned OBD values ONCE per module load (before the
     * lock so a test armed on another thread never observes the
     * half-reset state; once-only means per-test overrides through
     * SetFakeObd2CannedValueExternal survive later re-opens). */
    static bool canned_initialized = false;
    if (!canned_initialized) {
        fake_j2534::reset_obd2_canned();
        canned_initialized = true;
    }

    /* Test-only open blocking: ECUIO_FAKE_OPEN_DELAY_MS
     * makes PassThruOpen sleep before doing anything — the blocked-
     * vendor-open mode. Armed via ENVIRONMENT, not a hook: the child
     * inherits the parent's environment, so an x64 test arms the x86
     * child's fake with zero protocol surface. */
    {
        char buf[32] = {};
        const DWORD n = GetEnvironmentVariableA("ECUIO_FAKE_OPEN_DELAY_MS",
                                                 buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            const DWORD delay = static_cast<DWORD>(strtoul(buf, nullptr, 10));
            if (delay > 0 && delay <= 60000) Sleep(delay);
        }
    }

    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    fake_j2534::g_handle = 1;
    fake_j2534::g_open_thread_id = GetCurrentThreadId();
    *pDeviceID = 1;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruClose(unsigned long DeviceID)
{
    (void)DeviceID;
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    fake_j2534::g_handle = 0;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruConnect(unsigned long DeviceID,
                               unsigned long ProtocolID,
                               unsigned long Flags,
                               unsigned long BaudRate,
                               unsigned long* pChannelID)
{
    (void)DeviceID;
    {
        std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
        fake_j2534::g_last_connect_protocol = ProtocolID;
        fake_j2534::g_last_connect_flags = Flags;
        fake_j2534::g_last_connect_baud = BaudRate;
    }
    if (pChannelID == nullptr) {
        return STATUS_FAILED;
    }
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    if (fake_j2534::g_handle != 1) {
        return STATUS_FAILED;
    }
    /* Distinct from the device id (1) ON PURPOSE: if device and channel
     * were both 1, a caller passing the channel where J2534 wants the
     * device (READ_VBATT) would still pass. 0x55 is arbitrary, nonzero,
     * and unrelated to any protocol constant. */
    *pChannelID = 0x55;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruDisconnect(unsigned long ChannelID)
{
    (void)ChannelID;

    /* Test-only slow teardown: ECUIO_FAKE_DISCONNECT_DELAY_MS makes
     * every PassThruDisconnect sleep first, the slow-vendor teardown
     * an async disconnect must keep off the GUI thread. Re-read per
     * call and slept outside the lock, like the open delay. */
    {
        char buf[32] = {};
        const DWORD n = GetEnvironmentVariableA("ECUIO_FAKE_DISCONNECT_DELAY_MS",
                                                 buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            const DWORD delay = static_cast<DWORD>(strtoul(buf, nullptr, 10));
            if (delay > 0 && delay <= 60000) Sleep(delay);
        }
    }

    /* Tolerant by design: a real vendor's second-disconnect behavior
     * is vendor swamp we don't model — the COUNT is the contract
     * observable, not the return code. */
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    fake_j2534::g_disconnect_count++;
    fake_j2534::g_disconnect_thread_id = GetCurrentThreadId();
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruReadMsgs(unsigned long ChannelID,
                                PASSTHRU_MSG* pMsg,
                                unsigned long* pNumMsgs,
                                unsigned long Timeout)
{
    (void)ChannelID; (void)Timeout;
    if (pMsg == nullptr || pNumMsgs == nullptr) {
        return STATUS_FAILED;
    }

    /* #11 test knob (HARNESS): ECUIO_FAKE_READ_STATUS forces this
     * call to return the given vendor status with zero messages —
     * 0x09 (ERR_TIMEOUT) for quiet-bus, a hard code (e.g. 0x07) for
     * the dead-adapter path. Persistent while set and re-read per
     * call, so in-proc tests toggle it with SetEnvironmentVariableA
     * between phases; the surrogate child only sees the value it
     * inherited at spawn (same as ECUIO_FAKE_OPEN_DELAY_MS). Checked
     * before the queue, so a simulated dead adapter delivers nothing.
     * It always zeroes the count. Timeout WITH delivered messages is
     * spec mode's job, below. */
    {
        char buf[32] = {};
        const DWORD n = GetEnvironmentVariableA("ECUIO_FAKE_READ_STATUS",
                                                 buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            const long code = strtol(buf, nullptr, 0);
            if (code != 0) {
                *pNumMsgs = 0;
                return code;
            }
        }
    }

    /* The sloppy vendor: unlike ECUIO_FAKE_READ_STATUS above, this
     * returns the given hard status WITHOUT touching *pNumMsgs or the
     * message array — the caller's request count stays DIRTY and its
     * buffer keeps whatever the caller put there (zeros, or the
     * surrogate child's previous batch contents). Mirrors the VX,
     * which reports "status 7, written 1" on a failed write
     * (docs/bench-sessions/2026-10-20.md). Re-read per call:
     * in-proc tests toggle live, the surrogate child only sees the
     * spawn-time value. */
    {
        char uc[32] = {};
        const DWORD un = GetEnvironmentVariableA(
            "ECUIO_FAKE_UNTOUCHED_COUNT_STATUS", uc, sizeof(uc));
        if (un > 0 && un < sizeof(uc)) {
            const long code = strtol(uc, nullptr, 0);
            if (code != 0) {
                return code;   /* *pNumMsgs and buffer left untouched */
            }
        }
    }

    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);

    /* SPEC-CONFORMANT READ MODE: when
     * ECUIO_FAKE_SPEC_READ_SEMANTICS is 1, model the J2534-1 rule
     * for a conforming vendor: a read asking for N messages with a
     * nonzero timeout returns min(N, available), and if fewer than
     * requested, ERR_TIMEOUT TOGETHER WITH those messages. (The VX
     * departs from it: it returns STATUS_NOERROR with a short batch.) Timeout 0
     * on an empty queue returns ERR_BUFFER_EMPTY. The call returns at
     * once; the timeout isn't waited out. Filter gating is not
     * modeled here (the knobs are never combined in tests).
     * Off by default: the exact-count stream tests pin the legacy
     * single-message delivery. */
    {
        char spec[32] = {};
        const DWORD sn = GetEnvironmentVariableA(
            "ECUIO_FAKE_SPEC_READ_SEMANTICS", spec, sizeof(spec));
        if (sn > 0 && sn < sizeof(spec) && spec[0] == '1') {
            const unsigned long requested = *pNumMsgs;
            unsigned long avail = fake_j2534::g_queue_len;
            if (avail > requested) avail = requested;
            for (unsigned long i = 0; i < avail; ++i) {
                const fake_j2534::FrameSlot& slot = fake_j2534::g_queue[i];
                memcpy(pMsg[i].Data, slot.data, slot.size);
                pMsg[i].DataSize       = slot.size;
                pMsg[i].ProtocolID     = ECUIO_PT_PROTOCOL_ISO15765;
                pMsg[i].RxStatus       = slot.rx_status;
                pMsg[i].Timestamp      = slot.timestamp_us;
                pMsg[i].ExtraDataIndex  = slot.size;
            }
            for (unsigned long i = avail; i < fake_j2534::g_queue_len; ++i) {
                fake_j2534::g_queue[i - avail] = fake_j2534::g_queue[i];
            }
            fake_j2534::g_queue_len -= avail;
            *pNumMsgs = avail;
            if (avail < requested && Timeout > 0) {
                return ECUIO_PT_STATUS_TIMEOUT;
            }
            if (avail == 0 && Timeout == 0) {
                return ECUIO_PT_STATUS_BUFFER_EMPTY;
            }
            return STATUS_NOERROR;
        }
    }

    /* FILTER GATING (SetFilterGating-controlled): model the device's
     * filter hardware — non-matching frames are DISCARDED, never
     * parked. Drain rejected frames from the queue head until a
     * matching frame is found (deliver it) or the queue empties.
     * Returning "0 messages" while leaving a rejected frame at the
     * head is HEAD-OF-LINE BLOCKING: the polling worker reads the
     * same parked frame forever and the stream stalls at whatever
     * arrived before it. FilterGatingDropsNonMatchingFrames exists to
     * catch exactly this. Gating enabled with NO active filters
     * drains everything (spec: an unfiltered ISO 15765 channel
     * receives nothing). */
    if (fake_j2534::g_filter_gating_enabled) {
        bool have_match_at_head = false;
        while (fake_j2534::g_queue_len > 0) {
            bool match = false;
            for (unsigned long i = 0; i < fake_j2534::g_filter_seq &&
                                      i < FAKE_FILTER_DEPTH; ++i) {
                if (fake_j2534::g_filters[i].filter_id == 0) {
                    continue;
                }
                if (fake_j2534::filter_matches(fake_j2534::g_queue[0].data,
                                               fake_j2534::g_queue[0].size,
                                               fake_j2534::g_filters[i])) {
                    match = true;
                    break;
                }
            }
            if (match) {
                have_match_at_head = true;
                break;
            }
            /* Discard the non-matching head (FIFO shift) */
            for (unsigned long i = 1; i < fake_j2534::g_queue_len; ++i) {
                fake_j2534::g_queue[i - 1] = fake_j2534::g_queue[i];
            }
            fake_j2534::g_queue_len--;
        }
        if (!have_match_at_head) {
            *pNumMsgs = 0;
            return STATUS_NOERROR;
        }
    } else if (fake_j2534::g_queue_len == 0) {
        *pNumMsgs = 0;   /* quiet bus — idle, not an error */
        return STATUS_NOERROR;
    }

    /* Deliver the queue head */
    const fake_j2534::FrameSlot& slot = fake_j2534::g_queue[0];
    memcpy(pMsg->Data, slot.data, slot.size);
    pMsg->DataSize      = slot.size;
    pMsg->ProtocolID    = ECUIO_PT_PROTOCOL_ISO15765;
    pMsg->RxStatus      = slot.rx_status;
    pMsg->Timestamp     = slot.timestamp_us;
    pMsg->ExtraDataIndex = slot.size;   /* v4.04: == DataSize when none */

    for (unsigned long i = 1; i < fake_j2534::g_queue_len; ++i) {
        fake_j2534::g_queue[i - 1] = fake_j2534::g_queue[i];
    }
    fake_j2534::g_queue_len--;

    *pNumMsgs = 1;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruWriteMsgs(unsigned long ChannelID,
                                 PASSTHRU_MSG* pMsg,
                                 unsigned long* pNumMsgs,
                                 unsigned long Timeout)
{
    (void)ChannelID; (void)Timeout;
    if (pMsg == nullptr || pNumMsgs == nullptr) {
        return STATUS_FAILED;
    }

    /* ECUIO_FAKE_WRITE_STATUS
     * makes every WriteMsgs return the given vendor status WITHOUT
     * recording the message or generating a response — the VX quiet-
     * bus shape: writes fail (no ACK on the bus) while reads stay
     * clean. Like ECUIO_FAKE_UNTOUCHED_COUNT_STATUS on the read side,
     * *pNumMsgs is left as passed (1), the VX's "status 7, written 1",
     * so a write path that read the count before the status would be
     * caught. Unrecorded writes don't move GetWriteCount. Re-read per
     * call: in-proc tests toggle live; the child sees only the
     * spawn-time value. */
    {
        char wb[32] = {};
        const DWORD wn = GetEnvironmentVariableA("ECUIO_FAKE_WRITE_STATUS",
                                                 wb, sizeof(wb));
        if (wn > 0 && wn < sizeof(wb)) {
            const long code = strtol(wb, nullptr, 0);
            if (code != 0) {
                return code;   /* not recorded, no echo enqueued */
            }
        }
    }

    if (*pNumMsgs != 1) {
        return STATUS_FAILED;   /* the relay sends one at a time — strict */
    }

    unsigned long n = pMsg->DataSize;
    if (n > 64) {
        return STATUS_FAILED;   /* beyond the frame ABI cap — not shaped */
    }

    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);

    /* Record (bounded — the count stays monotonic so overflow-during-
     * test is detectable via GetWriteCount > depth with index clamped). */
    if (fake_j2534::g_write_count < FAKE_WRITE_DEPTH) {
        fake_j2534::WriteRecord& w =
            fake_j2534::g_writes[fake_j2534::g_write_count];
        memcpy(w.data, pMsg->Data, n);
        w.size = n;
        w.tx_flags = pMsg->TxFlags;
    }
    fake_j2534::g_write_count++;

    /* RESPONSE GENERATION.
     * Mode-01 request: EXACTLY 6 bytes, req[4]==0x01 — the
     * adapter-framed logical message. With an active filter, emit
     * [rx_id BE][41][PID][canned]; without one there is no rx_id to
     * address a response to, so the legacy echo applies. Everything
     * else keeps the deterministic echo semantics. */
    if (n > 0) {
        const unsigned char* req = pMsg->Data;
        unsigned char echo[64];
        unsigned long echo_len;

        const bool has_filter = (fake_j2534::g_last_filter.filter_id != 0)
                                 && (n >= 4);

        if (n == 6 && req[4] == 0x01 && has_filter) {
            fake_j2534::build_obd2_response(req, n, echo, &echo_len);
        } else if (has_filter) {
            memcpy(echo, fake_j2534::g_last_filter.pattern, 4);
            memcpy(echo + 4, req + 4, n - 4);
            echo[4] = static_cast<unsigned char>(req[4] + 0x40);
            echo_len = 4 + (n - 4);
        } else {
            memcpy(echo, req, n);
            echo[0] = static_cast<unsigned char>(req[0] + 0x40);
            echo_len = n;
        }

        static unsigned long resp_ts = 0x1000;
        resp_ts += 1;
        fake_j2534::enqueue_rx_locked(echo, echo_len, resp_ts, 0);
    }

    return STATUS_NOERROR;   /* *pNumMsgs stays 1 — fully written */
}

extern "C"
long __stdcall PassThruIoctl(unsigned long ChannelID,
                             long IoControlID,
                             void* pInput,
                             void* pOutput)
{
    (void)ChannelID;

    /* One-shot env-armed stall. The flag is taken under the lock and
     * the sleep outside it, so other callers of the fake (which share
     * the mutex) are never blocked by the stall. */
    bool do_stall = false;
    char stall_buf[32] = {};
    {
        const DWORD n = GetEnvironmentVariableA("ECUIO_FAKE_IOCTL_DELAY_MS",
                                                stall_buf, sizeof(stall_buf));
        if (n > 0 && n < sizeof(stall_buf)) {
            std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
            if (!fake_j2534::g_ioctl_delay_fired) {
                fake_j2534::g_ioctl_delay_fired = true;
                do_stall = true;
            }
        }
    }
    if (do_stall) {
        const DWORD delay =
            static_cast<DWORD>(strtoul(stall_buf, nullptr, 10));
        if (delay > 0 && delay <= 60000) Sleep(delay);
    }

    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);

    switch (IoControlID) {
    case ECUIO_PT_IOCTL_GET_CONFIG: {
        if (pInput == nullptr) {
            return STATUS_FAILED;
        }
        SCONFIG_LIST* list = static_cast<SCONFIG_LIST*>(pInput);
        if (list == nullptr || list->ConfigPtr == nullptr ||
            list->NumOfParams == 0 ||
            list->NumOfParams > FAKE_CONFIG_DEPTH) {
            return STATUS_FAILED;
        }
        for (unsigned long i = 0; i < list->NumOfParams; ++i) {
            unsigned long value = 0xFFFFFFFF;   /* "vendor default" */
            for (unsigned long j = 0; j < fake_j2534::g_cfg_len; ++j) {
                if (fake_j2534::g_cfg_param[j] ==
                    list->ConfigPtr[i].Parameter) {
                    value = fake_j2534::g_cfg_value[j];
                    break;
                }
            }
            list->ConfigPtr[i].Value = value;
        }
        return STATUS_NOERROR;
    }
    case ECUIO_PT_IOCTL_SET_CONFIG: {
        if (pInput == nullptr) {
            return STATUS_FAILED;
        }
        SCONFIG_LIST* list = static_cast<SCONFIG_LIST*>(pInput);
        if (list == nullptr || list->ConfigPtr == nullptr ||
            list->NumOfParams == 0 ||
            list->NumOfParams > FAKE_CONFIG_DEPTH) {
            return STATUS_FAILED;
        }
        for (unsigned long i = 0; i < list->NumOfParams; ++i) {
            bool stored = false;
            for (unsigned long j = 0; j < fake_j2534::g_cfg_len; ++j) {
                if (fake_j2534::g_cfg_param[j] ==
                    list->ConfigPtr[i].Parameter) {
                    fake_j2534::g_cfg_value[j] = list->ConfigPtr[i].Value;
                    stored = true;
                    break;
                }
            }
            if (!stored && fake_j2534::g_cfg_len < FAKE_CONFIG_DEPTH) {
                fake_j2534::g_cfg_param[fake_j2534::g_cfg_len] =
                    list->ConfigPtr[i].Parameter;
                fake_j2534::g_cfg_value[fake_j2534::g_cfg_len] =
                    list->ConfigPtr[i].Value;
                fake_j2534::g_cfg_len++;
            }
        }
        return STATUS_NOERROR;
    }
    case ECUIO_PT_IOCTL_READ_VBATT:
        /* Contract pin: v04.04 passes the DeviceID
         * to READ_VBATT. This fake's PassThruOpen returns device 1 and
         * PassThruConnect returns channel 0x55 — a caller passing the
         * channel FAILS here instead of passing by coincidence. */
        if (ChannelID != 1) {
            return STATUS_FAILED;
        }
        if (pOutput != nullptr) {
            *static_cast<unsigned long*>(pOutput) = 13800;  /* 13.8 V in mV */
        }
        return STATUS_NOERROR;
    case ECUIO_PT_IOCTL_CLEAR_TX_BUFFER:
    case ECUIO_PT_IOCTL_CLEAR_RX_BUFFER:
    case ECUIO_PT_IOCTL_CLEAR_MSG_FILTERS:
        return STATUS_NOERROR;
    default:
        return STATUS_FAILED;   /* strict vendor: unknown = failure */
    }
}

extern "C" __declspec(dllexport)
long __stdcall PassThruStartMsgFilter(unsigned long ChannelID,
                                      unsigned long FilterType,
                                      PASSTHRU_MSG* pMaskMsg,
                                      PASSTHRU_MSG* pPatternMsg,
                                      PASSTHRU_MSG* pFlowControlMsg,
                                      unsigned long* pFilterID)
{
    (void)ChannelID;
    if (pMaskMsg == nullptr || pPatternMsg == nullptr || pFilterID == nullptr) {
        return STATUS_FAILED;
    }
    if (FilterType != ECUIO_PT_PASS_FILTER &&
        FilterType != ECUIO_PT_BLOCK_FILTER &&
        FilterType != ECUIO_PT_FLOW_CONTROL_FILTER) {
        return STATUS_FAILED;
    }
    if (FilterType == ECUIO_PT_FLOW_CONTROL_FILTER &&
        pFlowControlMsg == nullptr) {
        return STATUS_FAILED;
    }
    if (pMaskMsg->DataSize != pPatternMsg->DataSize ||
        pMaskMsg->DataSize == 0 ||
        pMaskMsg->DataSize > 12) {
        return STATUS_FAILED;
    }

    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);

    fake_j2534::FilterRecord rec = {};
    rec.filter_id = ++fake_j2534::g_filter_seq;
    rec.filter_type = FilterType;
    rec.mask_len = pMaskMsg->DataSize;
    memcpy(rec.mask, pMaskMsg->Data, pMaskMsg->DataSize);
    memcpy(rec.pattern, pPatternMsg->Data, pPatternMsg->DataSize);
    if (pFlowControlMsg != nullptr && pFlowControlMsg->DataSize > 0 &&
        pFlowControlMsg->DataSize <= 12) {
        memcpy(rec.flow_control, pFlowControlMsg->Data,
               pFlowControlMsg->DataSize);
    }
    if (fake_j2534::g_filter_seq <= FAKE_FILTER_DEPTH) {
        fake_j2534::g_filters[fake_j2534::g_filter_seq - 1] = rec;
    }
    fake_j2534::g_last_filter = rec;

    *pFilterID = rec.filter_id;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruStopMsgFilter(unsigned long ChannelID,
                                      unsigned long FilterID)
{
    (void)ChannelID;
    if (FilterID == 0) {
        return STATUS_FAILED;
    }
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    /* Record the invocation for detach-ordering tests */
    fake_j2534::g_last_stopped_filter_id = FilterID;
    for (unsigned long i = 0; i < fake_j2534::g_filter_seq &&
                              i < FAKE_FILTER_DEPTH; ++i) {
        if (fake_j2534::g_filters[i].filter_id == FilterID) {
            fake_j2534::g_filters[i].filter_id = 0;   /* tombstone */
            return STATUS_NOERROR;
        }
    }
    return STATUS_FAILED;   /* unknown/already-stopped — strict vendor */
}

/* ================= Test hooks (TEST_HOOKS_ENABLED only) ============== */
#if defined(TEST_HOOKS_ENABLED) || defined(_DEBUG)

extern "C" __declspec(dllexport)
void InjectFrame(const unsigned char* data, unsigned long size,
                 unsigned long timestamp_us,
                 unsigned long is_extended,
                 unsigned long channel_id)
{
    (void)channel_id;
    if (data == nullptr || size == 0 || size > 64) {
        return;
    }

    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    fake_j2534::enqueue_rx_locked(data, size, timestamp_us,
                                  is_extended ? ECUIO_PT_CAN_29BIT_ID : 0ul);
}

extern "C" __declspec(dllexport)
void GetBufferState(unsigned char** out_buffer, unsigned long* out_size)
{
    if (out_buffer == nullptr || out_size == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    if (fake_j2534::g_queue_len > 0) {
        *out_buffer = fake_j2534::g_queue[0].data;
        *out_size   = fake_j2534::g_queue[0].size;
    } else {
        *out_buffer = nullptr;
        *out_size = 0;
    }
}

extern "C" __declspec(dllexport)
int GetCurrentHandle(void)
{
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    return fake_j2534::g_handle;
}

extern "C" __declspec(dllexport)
void GetLastConnectParams(unsigned long* out_baud,
                          unsigned long* out_flags,
                          unsigned long* out_protocol)
{
    if (out_baud == nullptr || out_flags == nullptr ||
        out_protocol == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    *out_baud = fake_j2534::g_last_connect_baud;
    *out_flags = fake_j2534::g_last_connect_flags;
    *out_protocol = fake_j2534::g_last_connect_protocol;
}

/* ---- Write, filter and disconnect hooks ---- */

/* Total PassThruWriteMsgs messages accepted (monotonic, exceeds the
 * recording depth on overflow — index clamp in GetRecordedWrite). */
extern "C" __declspec(dllexport)
void GetWriteCount(unsigned long* out_count)
{
    if (out_count == nullptr) return;
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    *out_count = fake_j2534::g_write_count;
}

/* Recorded write by index (0-based, arrival order). Outputs zeroed
 * when index >= recorded depth (overflow) — count tells the story. */
extern "C" __declspec(dllexport)
void GetRecordedWrite(unsigned long index,
                     unsigned char* out_data,
                     unsigned long* out_size,
                     unsigned long* out_tx_flags)
{
    if (out_data == nullptr || out_size == nullptr || out_tx_flags == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    *out_size = 0;
    *out_tx_flags = 0;
    memset(out_data, 0, 64);
    if (index < fake_j2534::g_write_count && index < FAKE_WRITE_DEPTH) {
        const fake_j2534::WriteRecord& w = fake_j2534::g_writes[index];
        memcpy(out_data, w.data, w.size);
        *out_size = w.size;
        *out_tx_flags = w.tx_flags;
    }
}

/* Raw last PassThruStartMsgFilter invocation (even a refused one —
 * recorded after validation in this fake, so only well-formed calls
 * land here; refused shapes return before recording, matching the
 * strict-vendor posture). Copies mask_len bytes of mask and pattern
 * into caller buffers (>= 12 bytes each). */
extern "C" __declspec(dllexport)
void GetLastFilterStart(unsigned long* out_filter_id,
                       unsigned long* out_filter_type,
                       unsigned char* out_mask,
                       unsigned char* out_pattern,
                       unsigned long* out_len)
{
    if (out_filter_id == nullptr || out_filter_type == nullptr ||
        out_mask == nullptr || out_pattern == nullptr || out_len == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    *out_filter_id = fake_j2534::g_last_filter.filter_id;
    *out_filter_type = fake_j2534::g_last_filter.filter_type;
    memcpy(out_mask, fake_j2534::g_last_filter.mask, 12);
    memcpy(out_pattern, fake_j2534::g_last_filter.pattern, 12);
    *out_len = fake_j2534::g_last_filter.mask_len;
}

/* Record of the last StopMsgFilter invocation (for detach-ordering
 * assertion — StopMsgFilter called exactly once with the registered
 * ID before disconnect). Zero means no stop was ever called. */
extern "C" __declspec(dllexport)
void GetLastFilterStop(unsigned long* out_filter_id)
{
    if (out_filter_id == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    *out_filter_id = fake_j2534::g_last_stopped_filter_id;
}

/* Monotonic PassThruDisconnect count: explicit
 * client-disconnect-then-close must total exactly 1. In-proc
 * instances only; for the child, see the session-log note on
 * g_disconnect_count. */
extern "C" __declspec(dllexport)
unsigned long GetDisconnectCount(void)
{
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    return fake_j2534::g_disconnect_count;
}

/* GetLastCallThreadIds: the Win32 thread IDs that last ran
 * PassThruOpen and PassThruDisconnect, 0 for never. In-proc only. */
extern "C" __declspec(dllexport)
void GetLastCallThreadIds(unsigned long* open_tid, unsigned long* disconnect_tid)
{
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    if (open_tid != nullptr) *open_tid = fake_j2534::g_open_thread_id;
    if (disconnect_tid != nullptr) *disconnect_tid = fake_j2534::g_disconnect_thread_id;
}

/* SetFilterGating(enable): when armed, ReadMsgs drops frames whose
 * first mask_len bytes don't match any active filter (spec receive-
 * requires-filter semantics). Does NOT affect WriteMsgs' echo shape
 * (that's unconditional once a filter is registered). The x64 test
 * process cannot reach into the x86 child's fake instance to arm this
 * (the OP_INJECT gap); filter-aware echo works regardless, making
 * cross-arch parity achievable without arming. */
extern "C" __declspec(dllexport)
void SetFilterGating(int enable)
{
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    fake_j2534::g_filter_gating_enabled = (enable != 0);
}

/* ---- OBD-II canned-value hooks (ADR-0013, HARNESS) ---- */

/* Override canned response values for a specific PID. Test-side
 * configuration so HARNESS tests can assert exact decoded values. */
extern "C" __declspec(dllexport)
void SetFakeObd2CannedValueExternal(unsigned char pid, unsigned char* data,
                                    unsigned long len)
{
    fake_j2534::SetFakeObd2CannedValue(pid, data, len);
}

/* Get current canned value for a PID (for test assertions). */
extern "C" __declspec(dllexport)
void GetFakeObd2CannedValueExternal(unsigned char pid, unsigned char* out_data,
                                    unsigned long* out_len)
{
    fake_j2534::GetFakeObd2CannedValue(pid, out_data, out_len);
}

#endif // TEST_HOOKS_ENABLED || _DEBUG
