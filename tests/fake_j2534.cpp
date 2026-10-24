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
//      - A request, with a filter registered: Service 01 or 09
//        [4-byte ID BE][01|09][PID] (exactly 6 bytes, the adapter-framed
//        logical message) or Service 0x22 [ID][22][DID] (7 bytes). The
//        ECU the request is addressed to answers: [reply ID BE][41][PID]
//        [canned] from g_obd2_canned (SetFakeObd2CannedValueExternal
//        overrides per-test) or its supported-PID bitmap;
//        [ID][49][item][01][data] for Service 09's VIN (02), CALID
//        (04), CVN (06) and ECU name (0A), synthetic values marked FAKE;
//        or [ID][62][DID][value] for the DIDs it knows: 0x1E1C (2 bytes),
//        the identification DIDs 0xF187 to 0xF191 as FAKE text, and
//        three long replies, 0xF1A0 (200 bytes), 0xF1A1 (4092, so
//        the reply is ISO 15765-2's longest, 4095 bytes) and 0xF1A2
//        (4121, filling PASSTHRU_MSG.Data: longer than a frame holds).
//        Any other PID, item or DID gets a negative reply, 7F 01 12
//        (what the first vehicle answered for PID 0x5C), 7F 09 12 or
//        7F 22 31 (so 0xF194 and 0xF195 are refused). The reply ID
//        is the request's plus 8 (0x7E0 ->
//        0x7E8; the functional 0x7DF is answered from 0x7E8) or, 29-bit,
//        18DA<tester><ecu> for 18DA<ecu><tester>. NO PCI byte in either
//        direction — the fake models the adapter side of ISO-TP, and
//        the transport's 4-byte header strip lands the bridge on the
//        obd2 set's mux-at-offset-0 grammar.
//      - Everything else: the legacy deterministic echo (byte 0
//        +0x40), preserving existing framing tests.
//    ECUIO_FAKE_WRITE_STATUS fails every write with the given status,
//    recording nothing and echoing nothing; ECUIO_FAKE_WRITE_ZERO_COUNT=1
//    answers every write STATUS_NOERROR with a count of 0, the same
//    way. ECUIO_FAKE_REPLY_DELAY_MS
//    holds each response back that long after the write (default 0);
//    ECUIO_FAKE_PENDING=1 answers each request with "response pending"
//    (7F <service> 78) at once, ahead of that reply.
//    ECUIO_FAKE_UNSUPPORTED_NRC sets the code of the negative reply to
//    an unsupported PID or DID (hex or decimal), or 0 for no reply at
//    all. The supported-PID bitmaps (PIDs 0x00, 0x20, 0x40) mark the
//    canned PIDs, so the chain ends at 0x40.
//  * IoCtl: config table (SET stores, GET reads back), fixed VBATT
//    (13800 mV), CLEAR_* accepted. Unknown ids FAIL like a strict vendor.
//  * ReadVersion: fixed firmware, DLL and API strings (kFirmwareVersion
//    and friends) for the device PassThruOpen handed out. The legacy
//    fixture (fake_j2534_legacy.def) doesn't export it.
//  * IDs: device 1 and channel 0x55, or 0 for both under
//    ECUIO_FAKE_ZERO_IDS=1 (J2534 doesn't reserve 0).
//  * Refusals: ECUIO_FAKE_OPEN_STATUS, ECUIO_FAKE_CONNECT_STATUS,
//    ECUIO_FAKE_FILTER_STATUS, ECUIO_FAKE_VBATT_STATUS and
//    ECUIO_FAKE_VERSION_STATUS make PassThruOpen, PassThruConnect,
//    PassThruStartMsgFilter, READ_VBATT and PassThruReadVersion return
//    that status, and PassThruGetLastError then describes the refusal.
//  * Disconnect: counted; ECUIO_FAKE_DISCONNECT_DELAY_MS sleeps
//    before each one (a slow vendor teardown).
//  * Filters: registration bookkeeping + last-invocation recording.
//    Filter-aware echo prepends the filter pattern (4-byte BE CAN ID).
//    SetFilterGating controls ONLY ReadMsgs dropping; the echo shape
//    is unconditional once a filter lands.
//  * Knobs (ECUIO_FAKE_*) are read on every call: from the control
//    page (fake_control.h) when it holds the name, else from the
//    environment. The page reaches the surrogate child's instance
//    mid-session, which the child's spawn-time environment can't. It
//    also carries messages a test queues for ReadMsgs, and the fake
//    publishes its successful writes, the process that opened it, and
//    its disconnects and closes by the IDs it handed out.
//  * ReadMsgs, by default, returns at most one message per call,
//    whatever NumMsgs and Timeout ask for, and STATUS_NOERROR with zero
//    messages on an empty queue. ECUIO_FAKE_READ_STATUS forces a
//    status with no messages; ECUIO_FAKE_UNTOUCHED_COUNT_STATUS
//    forces a status with the caller's count and buffer left as
//    passed (a sloppy vendor; see ecuio_trusted_count);
//    ECUIO_FAKE_SPEC_READ_SEMANTICS=1 switches to J2534-1 batches.
//    ECUIO_FAKE_READ_DELAY_MS stalls the first read after load, once.
//  * ECUIO_FAKE_VX=1 models the OBDX Pro VX as measured
//    (docs/bench-sessions/2026-10-22.md and its vehicle sheet): batch
//    reads held for their timeout, a short batch with STATUS_NOERROR
//    and an empty read with ERR_BUFFER_EMPTY; after each accepted
//    write a TX indication (RxStatus 0x09, the 4-byte CAN ID), and the
//    reply 4 ms later. ECUIO_FAKE_NO_ACK=1 adds the bus with no other
//    node: three writes accepted per connect, then each blocks about
//    500 ms and fails ERR_FAILED, echoed with RxStatus 0x201.
//  * A queued message has an arrival time; reads see only those that
//    have arrived, in arrival order.
//  * ExtraDataIndex = DataSize (v4.04 no-extra-data convention).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ecuio/j2534_ptypes.h"
#include "fake_control.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
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
    unsigned char data[ECUIO_PT_MSG_DATA_SIZE];
    unsigned long size;
    unsigned long timestamp_us;
    unsigned long rx_status;   /* replayed verbatim into PASSTHRU_MSG.RxStatus */
    long long arrives_us;      /* steady clock; reads skip it until then */
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
std::condition_variable g_arrived;      /* a message was queued */
int g_handle = 0;                       /* 1 between Open and Close */
FrameSlot g_queue[FAKE_QUEUE_DEPTH];    /* in arrival order */
unsigned long g_queue_len = 0;

/* The VX as measured on 2026-10-22: its reply latency is the engine
 * ECU's median from indication to reply; with nothing on the bus to
 * acknowledge, it accepted three writes per connect, then blocked
 * each later one 500-515 ms and failed it. */
constexpr unsigned long kVxReplyDelayMs = 4;
constexpr unsigned long kVxUnackedDepth = 3;
constexpr DWORD kVxRefusedWriteMs = 500;
constexpr unsigned long kVxIndication = ECUIO_PT_TX_MSG_TYPE |
                                        ECUIO_PT_TX_INDICATION;
/* What the VX queued after each refused write: the request echoed with
 * RxStatus 0x201. Bit 0x200 isn't a v04.04 RxStatus bit; its meaning is
 * unknown. */
constexpr unsigned long kVxRefusedEcho = 0x201;
unsigned long g_unacked = 0;            /* writes accepted since connect */

/* PassThruGetLastError's text: the last refusal a knob forced. */
char g_last_error[ECUIO_PT_TEXT_LEN] = "fake_j2534: no error";

/* PassThruReadVersion's answers. */
constexpr char kFirmwareVersion[] = "fake-fw 1.0";
constexpr char kDllVersion[] = "fake_j2534 0.3";
constexpr char kApiVersion[] = "04.04";

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
/* The same for the FIRST PassThruReadMsgs: a read answered past the
 * client's deadline for it, with whatever the queue then holds. */
bool g_read_delay_fired = false;

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

/* The IDs PassThruOpen and PassThruConnect hand out: device 1 and
 * channel 0x55, or 0 for both under ECUIO_FAKE_ZERO_IDS=1, since J2534
 * doesn't reserve 0. The device-scoped calls (READ_VBATT,
 * PassThruReadVersion) answer only the device ID. */
constexpr unsigned long kDeviceId = 1;
constexpr unsigned long kChannelId = 0x55;
unsigned long g_device_id = kDeviceId;
unsigned long g_channel_id = kChannelId;

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

/* A knob's value: the control page's when it holds the name, else the
 * environment's. False when neither sets it, or it doesn't fit buf. */
bool read_knob(const char* name, char* buf, DWORD cap)
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
long refusal(const char* knob, const char* call)
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

bool knob_on(const char* name)
{
    char buf[8] = {};
    return read_knob(name, buf, sizeof(buf)) && buf[0] == '1';
}

unsigned long knob_ms(const char* name, unsigned long fallback)
{
    char buf[32] = {};
    if (!read_knob(name, buf, sizeof(buf))) {
        return fallback;
    }
    const unsigned long ms = strtoul(buf, nullptr, 10);
    return (ms <= 60000) ? ms : fallback;
}

long long now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* The VX's clock: microseconds, 32 bits, wrapping. */
unsigned long vendor_stamp(long long steady_us)
{
    return static_cast<unsigned long>(steady_us & 0xFFFFFFFFLL);
}

/* Enqueue into the shared RX FIFO (mutex held by caller), arriving at
 * arrives_us (0: now). The queue stays in arrival order, so a message
 * injected now goes ahead of a reply still on its way. Drops on full
 * exactly like InjectFrame: documented, keeps counts honest. */
void enqueue_rx_locked(const unsigned char* data, unsigned long size,
                       unsigned long timestamp_us, unsigned long rx_status,
                       long long arrives_us = 0)
{
    if (g_queue_len >= FAKE_QUEUE_DEPTH) {
        return;
    }
    if (arrives_us == 0) {
        arrives_us = now_us();
    }
    unsigned long i = g_queue_len;
    while (i > 0 && g_queue[i - 1].arrives_us > arrives_us) {
        g_queue[i] = g_queue[i - 1];
        --i;
    }
    FrameSlot& slot = g_queue[i];
    memcpy(slot.data, data, size);
    slot.size = size;
    slot.timestamp_us = timestamp_us;
    slot.rx_status = rx_status;
    slot.arrives_us = arrives_us;
    ++g_queue_len;
    g_arrived.notify_all();
}

/* How many queued messages have arrived by now: a prefix of the queue. */
unsigned long arrived_locked(long long now)
{
    unsigned long n = 0;
    while (n < g_queue_len && g_queue[n].arrives_us <= now) {
        ++n;
    }
    return n;
}

void pop_front_locked(unsigned long n)
{
    for (unsigned long i = n; i < g_queue_len; ++i) {
        g_queue[i - n] = g_queue[i];
    }
    g_queue_len -= n;
}

/* Moves the messages a test queued on the control page into the RX
 * FIFO (mutex held by caller), RxStatus verbatim. */
void take_posted_locked()
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

/* The ID an ECU answers a request on, into out (4 bytes, big-endian):
 * 11-bit physical requests 0x7E0 to 0x7E7 get theirs 8 above, the
 * functional 0x7DF is answered by the engine ECU (0x7E8), and a 29-bit
 * 18DA<ecu><tester> request by 18DA<tester><ecu> (ISO 15765-4). Any
 * other ID falls back to the last filter's pattern. */
void reply_header(const unsigned char* req, unsigned char* out)
{
    const uint32_t tx = (uint32_t{req[0]} << 24) | (uint32_t{req[1]} << 16) |
                        (uint32_t{req[2]} << 8) | uint32_t{req[3]};
    uint32_t rx = 0;
    if (tx == 0x7DFu) {
        rx = 0x7E8u;
    } else if (tx >= 0x7E0u && tx <= 0x7E7u) {
        rx = tx + 8;
    } else if ((tx & 0xFFFF0000u) == 0x18DA0000u) {
        rx = 0x18DA0000u | ((tx & 0xFFu) << 8) | ((tx >> 8) & 0xFFu);
    } else {
        memcpy(out, g_last_filter.pattern, 4);
        return;
    }
    out[0] = static_cast<unsigned char>(rx >> 24);
    out[1] = static_cast<unsigned char>(rx >> 16);
    out[2] = static_cast<unsigned char>(rx >> 8);
    out[3] = static_cast<unsigned char>(rx);
}

/* The Service 0x22 DIDs the fake's ECU knows: ADR-0015's example, and
 * three whose replies are long, filled with their byte index (& 0xFF).
 * A reply is [ID][62][DID][data], so 0xF1A1's 4092 bytes make the
 * longest ISO 15765-2 message on classic CAN, 4095, and 0xF1A2's fill
 * PASSTHRU_MSG.Data, past what a frame holds. */
constexpr uint16_t kFakeDid = 0x1E1C;
constexpr unsigned char kFakeDidValue[2] = {0x12, 0x34};
constexpr uint16_t kFakeLongDid = 0xF1A0;
constexpr unsigned long kFakeLongDidLen = 200;
constexpr uint16_t kFakeMaxDid = 0xF1A1;
constexpr unsigned long kFakeMaxDidLen = 4092;
constexpr uint16_t kFakeOverlongDid = 0xF1A2;
constexpr unsigned long kFakeOverlongDidLen = ECUIO_PT_MSG_DATA_SIZE - 7;

/* What the fake's ECU says it is (ADR-0021), every value marked FAKE so
 * none can pass for a vehicle's. The VIN has a VIN's 17 characters; the
 * CALID and the ECU name are NUL-padded to J1979's 16 and 20 bytes; the
 * CVN is four binary bytes. */
constexpr char kFakeVin[] = "FAKEJ2534VIN00001";
constexpr char kFakeCalid[16] = "FAKECAL-01";
constexpr unsigned char kFakeCvn[4] = {0xDE, 0xAD, 0xBE, 0xEF};
constexpr char kFakeEcuName[20] = "FAKE-ECM";

/* The identification DIDs the fake's ECU answers, as text; nullptr for
 * any other DID. 0xF194 and 0xF195 are left out, so identification
 * meets a refusal. */
const char* did_text(uint16_t did)
{
    switch (did) {
        case 0xF187: return "FAKE-PART-0001";
        case 0xF188: return "FAKE-SW-0001";
        case 0xF189: return "FAKE 1.0";
        case 0xF18A: return "FAKE SUPPLIER";
        case 0xF18C: return "FAKE-SERIAL-000001";
        case 0xF190: return kFakeVin;
        case 0xF191: return "FAKE-HW-01";
        default:     return nullptr;
    }
}

/* A DID's data length, or 0 for one the fake doesn't know. */
unsigned long did_length(uint16_t did)
{
    if (const char* text = did_text(did)) {
        return static_cast<unsigned long>(strlen(text));
    }
    switch (did) {
        case kFakeDid:         return sizeof(kFakeDidValue);
        case kFakeLongDid:     return kFakeLongDidLen;
        case kFakeMaxDid:      return kFakeMaxDidLen;
        case kFakeOverlongDid: return kFakeOverlongDidLen;
        default:               return 0;
    }
}

/* The Service 01 PIDs the fake's ECU supports: its canned ones, and the
 * bitmaps that lead to them. */
bool supports_pid(unsigned pid)
{
    switch (pid) {
        case 0x00: case 0x04: case 0x05: case 0x0C: case 0x0D: case 0x0F:
        case 0x10: case 0x11: case 0x20: case 0x2F: case 0x40: case 0x42:
        case 0x5C:
            return true;
        default:
            return false;
    }
}

/* The supported-PID bitmap at PID base (0x00, 0x20, 0x40), into out[4]:
 * PID base + k (k from 1 to 32) is bit 0x80 >> ((k - 1) % 8) of byte
 * (k - 1) / 8 (SAE J1979), the last bit the next bitmap. */
void pid_bitmap(unsigned base, unsigned char* out)
{
    for (unsigned k = 1; k <= 32; ++k) {
        const unsigned char bit = static_cast<unsigned char>(
            0x80u >> ((k - 1) % 8));
        if (supports_pid(base + k)) {
            out[(k - 1) / 8] = static_cast<unsigned char>(out[(k - 1) / 8] | bit);
        }
    }
}

/* A request's negative reply: [reply ID][7F][service][code]. */
void build_negative(const unsigned char* req, unsigned char code,
                    unsigned char* resp, unsigned long* resp_len)
{
    reply_header(req, resp);
    resp[4] = 0x7F;
    resp[5] = req[4];
    resp[6] = code;
    *resp_len = 7;
}

/* Build an OBD-II Service 01, 09 or 0x22 response into resp, which
 * holds ECUIO_PT_MSG_DATA_SIZE bytes.
 * Caller (WriteMsgs, mutex HELD) has verified: [tx_id BE][0x01][PID] or
 * [tx_id BE][0x09][item] (6 bytes) or [tx_id BE][0x22][DID] (7 bytes),
 * and an active filter.
 * Output: [reply ID BE][0x41][PID][canned data],
 * [reply ID BE][0x49][item][01][data] or
 * [reply ID BE][0x62][DID][value], or the negative reply for a PID, item
 * or DID the fake doesn't support — NO PCI BYTE in either direction. The
 * fake models the ADAPTER side of ISO-TP: this queue content is what a
 * conforming adapter delivers from ReadMsgs, and the transport's header
 * strip lands the bridge on [41][PID][A][B]... — the obd2 set's
 * mux-at-offset-0 grammar. Response length varies per PID.
 * unsupported_nrc is ECUIO_FAKE_UNSUPPORTED_NRC: -1 (unset) answers an
 * unsupported PID 0x12 and an unknown DID 0x31, 0 answers neither (an
 * ECU that stays silent), any other value is the code for both. False
 * when nothing answers. */
bool build_obd2_response(const unsigned char* req, unsigned long req_len,
                         long unsupported_nrc,
                         unsigned char* resp, unsigned long* resp_len)
{
    (void)req_len;   /* shape pre-validated by the caller */
    const auto refuse = [&](unsigned char fallback) {
        if (unsupported_nrc == 0) {
            return false;
        }
        build_negative(req, (unsupported_nrc < 0)
                                ? fallback
                                : static_cast<unsigned char>(unsupported_nrc),
                       resp, resp_len);
        return true;
    };
    if (req[4] == 0x22) {
        const uint16_t did = static_cast<uint16_t>((req[5] << 8) | req[6]);
        const unsigned long n = did_length(did);
        if (n == 0) {
            return refuse(0x31);   /* request out of range */
        }
        reply_header(req, resp);
        resp[4] = 0x62;
        resp[5] = req[5];
        resp[6] = req[6];
        const char* text = did_text(did);
        for (unsigned long i = 0; i < n; ++i) {
            resp[7 + i] = (text != nullptr) ? static_cast<unsigned char>(text[i])
                        : (did == kFakeDid) ? kFakeDidValue[i]
                        : static_cast<unsigned char>(i & 0xFF);
        }
        *resp_len = 7 + n;
        return true;
    }
    if (req[4] == 0x09) {
        /* Vehicle information: [ID][49][item][count 01][data]. */
        const unsigned char* data = nullptr;
        unsigned long n = 0;
        switch (req[5]) {
            case 0x02:
                data = reinterpret_cast<const unsigned char*>(kFakeVin);
                n = sizeof(kFakeVin) - 1;
                break;
            case 0x04:
                data = reinterpret_cast<const unsigned char*>(kFakeCalid);
                n = sizeof(kFakeCalid);
                break;
            case 0x06:
                data = kFakeCvn;
                n = sizeof(kFakeCvn);
                break;
            case 0x0A:
                data = reinterpret_cast<const unsigned char*>(kFakeEcuName);
                n = sizeof(kFakeEcuName);
                break;
            default:
                return refuse(0x12);
        }
        reply_header(req, resp);
        resp[4] = 0x49;
        resp[5] = req[5];
        resp[6] = 0x01;
        memcpy(resp + 7, data, n);
        *resp_len = 7 + n;
        return true;
    }
    const uint8_t pid = req[5];
    if (!supports_pid(pid)) {
        return refuse(0x12);   /* the first vehicle's answer for 0x5C */
    }

    reply_header(req, resp);
    resp[4] = 0x41;   /* Mode 01 + 0x40 response */
    resp[5] = pid;    /* PID echo */

    switch (pid) {
        case 0x00:
        case 0x20:
        case 0x40:
            resp[6] = resp[7] = resp[8] = resp[9] = 0;
            pid_bitmap(pid, resp + 6);
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
    }
    return true;
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
        if (fake_j2534::read_knob("ECUIO_FAKE_OPEN_DELAY_MS",
                                  buf, sizeof(buf))) {
            const DWORD delay = static_cast<DWORD>(strtoul(buf, nullptr, 10));
            if (delay > 0 && delay <= 60000) Sleep(delay);
        }
    }

    if (fake_control::Page* page = fake_control::attach()) {
        InterlockedExchange(&page->opener_pid,
                            static_cast<LONG>(GetCurrentProcessId()));
    }
    if (const long rc = fake_j2534::refusal("ECUIO_FAKE_OPEN_STATUS",
                                            "PassThruOpen")) {
        return rc;
    }

    const bool zero_ids = fake_j2534::knob_on("ECUIO_FAKE_ZERO_IDS");
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    fake_j2534::g_handle = 1;
    fake_j2534::g_open_thread_id = GetCurrentThreadId();
    fake_j2534::g_device_id = zero_ids ? 0 : fake_j2534::kDeviceId;
    fake_j2534::g_channel_id = zero_ids ? 0 : fake_j2534::kChannelId;
    *pDeviceID = fake_j2534::g_device_id;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruClose(unsigned long DeviceID)
{
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    fake_j2534::g_handle = 0;
    if (DeviceID == fake_j2534::g_device_id) {
        if (fake_control::Page* page = fake_control::attach()) {
            InterlockedIncrement(&page->closes);
        }
    }
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
    if (const long rc = fake_j2534::refusal("ECUIO_FAKE_CONNECT_STATUS",
                                            "PassThruConnect")) {
        return rc;
    }
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    if (fake_j2534::g_handle != 1) {
        return STATUS_FAILED;
    }
    fake_j2534::g_unacked = 0;   /* each VX connect accepted three again */
    /* Distinct from the device id (1) ON PURPOSE: if device and channel
     * were both 1, a caller passing the channel where J2534 wants the
     * device (READ_VBATT) would still pass. 0x55 is arbitrary, nonzero,
     * and unrelated to any protocol constant. */
    *pChannelID = fake_j2534::g_channel_id;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruDisconnect(unsigned long ChannelID)
{

    /* Test-only slow teardown: ECUIO_FAKE_DISCONNECT_DELAY_MS makes
     * every PassThruDisconnect sleep first, the slow-vendor teardown
     * an async disconnect must keep off the GUI thread. Re-read per
     * call and slept outside the lock, like the open delay. */
    {
        char buf[32] = {};
        if (fake_j2534::read_knob("ECUIO_FAKE_DISCONNECT_DELAY_MS",
                                  buf, sizeof(buf))) {
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
    if (ChannelID == fake_j2534::g_channel_id) {
        if (fake_control::Page* page = fake_control::attach()) {
            InterlockedIncrement(&page->disconnects);
        }
    }
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

    /* ECUIO_FAKE_READ_DELAY_MS: the first ReadMsgs after load sleeps
     * that long (max 60000) before reading, once, like the ioctl
     * stall. */
    {
        char buf[32] = {};
        bool stall = false;
        if (fake_j2534::read_knob("ECUIO_FAKE_READ_DELAY_MS",
                                  buf, sizeof(buf))) {
            std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
            if (!fake_j2534::g_read_delay_fired) {
                fake_j2534::g_read_delay_fired = true;
                stall = true;
            }
        }
        if (stall) {
            const DWORD delay = static_cast<DWORD>(strtoul(buf, nullptr, 10));
            if (delay > 0 && delay <= 60000) Sleep(delay);
        }
    }

    /* Link-health knob (HARNESS): ECUIO_FAKE_READ_STATUS forces this
     * call to return the given vendor status with zero messages —
     * 0x09 (ERR_TIMEOUT) for quiet-bus, a hard code (e.g. 0x07) for
     * the dead-adapter path. Persistent while set and re-read per
     * call, so in-proc tests toggle it with SetEnvironmentVariableA
     * between phases; the surrogate child sees only the value it
     * inherited at spawn, unless the control page sets it. Checked
     * before the queue, so a simulated dead adapter delivers nothing.
     * It always zeroes the count. Timeout WITH delivered messages is
     * spec mode's job, below. */
    {
        char buf[32] = {};
        if (fake_j2534::read_knob("ECUIO_FAKE_READ_STATUS",
                                  buf, sizeof(buf))) {
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
     * in-proc tests toggle live, the surrogate child sees the
     * spawn-time value unless the control page sets it. */
    {
        char uc[32] = {};
        if (fake_j2534::read_knob("ECUIO_FAKE_UNTOUCHED_COUNT_STATUS",
                                  uc, sizeof(uc))) {
            const long code = strtol(uc, nullptr, 0);
            if (code != 0) {
                return code;   /* *pNumMsgs and buffer left untouched */
            }
        }
    }

    std::unique_lock<std::mutex> lk(fake_j2534::g_lock);
    fake_j2534::take_posted_locked();

    /* BATCH READS, J2534-1 (ECUIO_FAKE_SPEC_READ_SEMANTICS=1) or the
     * VX (ECUIO_FAKE_VX=1): up to NumMsgs messages, and the call holds
     * until that many have arrived or Timeout passes. They differ only
     * in statuses. J2534-1 pairs a short batch with ERR_TIMEOUT when
     * Timeout is nonzero, and answers an empty timeout-0 read with
     * ERR_BUFFER_EMPTY. The VX returns a short batch with
     * STATUS_NOERROR and any empty read with ERR_BUFFER_EMPTY. Filter
     * gating isn't modelled here. Off by default: the exact-count
     * stream tests pin the single-message delivery below. */
    const bool vx = fake_j2534::knob_on("ECUIO_FAKE_VX");
    if (vx || fake_j2534::knob_on("ECUIO_FAKE_SPEC_READ_SEMANTICS")) {
        const unsigned long requested = *pNumMsgs;
        const long long deadline = fake_j2534::now_us() +
                                   1000LL * static_cast<long long>(Timeout);
        unsigned long avail = 0;
        for (;;) {
            const long long now = fake_j2534::now_us();
            avail = fake_j2534::arrived_locked(now);
            if (avail >= requested || now >= deadline) {
                break;
            }
            /* Wake for the next arrival or the deadline, and at least
             * every 2 ms for messages queued on the control page,
             * which signal nothing. */
            long long wake = deadline;
            if (avail < fake_j2534::g_queue_len &&
                fake_j2534::g_queue[avail].arrives_us < wake) {
                wake = fake_j2534::g_queue[avail].arrives_us;
            }
            if (wake > now + 2000) {
                wake = now + 2000;
            }
            fake_j2534::g_arrived.wait_for(
                lk, std::chrono::microseconds(wake - now));
            fake_j2534::take_posted_locked();
        }
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
        fake_j2534::pop_front_locked(avail);
        *pNumMsgs = avail;
        if (vx) {
            return (avail == 0) ? ECUIO_PT_STATUS_BUFFER_EMPTY
                                : STATUS_NOERROR;
        }
        if (avail < requested && Timeout > 0) {
            return ECUIO_PT_STATUS_TIMEOUT;
        }
        if (avail == 0 && Timeout == 0) {
            return ECUIO_PT_STATUS_BUFFER_EMPTY;
        }
        return STATUS_NOERROR;
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
     * receives nothing). Only messages that have arrived are
     * judged. */
    const unsigned long arrived =
        fake_j2534::arrived_locked(fake_j2534::now_us());
    if (fake_j2534::g_filter_gating_enabled) {
        bool have_match_at_head = false;
        unsigned long judged = 0;
        while (judged < arrived && fake_j2534::g_queue_len > 0) {
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
            fake_j2534::pop_front_locked(1);
            ++judged;
        }
        if (!have_match_at_head) {
            *pNumMsgs = 0;
            return STATUS_NOERROR;
        }
    } else if (arrived == 0) {
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
    fake_j2534::pop_front_locked(1);

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
     * call: in-proc tests toggle live; the child sees the spawn-time
     * value unless the control page sets it. */
    {
        char wb[32] = {};
        if (fake_j2534::read_knob("ECUIO_FAKE_WRITE_STATUS",
                                  wb, sizeof(wb))) {
            const long code = strtol(wb, nullptr, 0);
            if (code != 0) {
                return code;   /* not recorded, no echo enqueued */
            }
        }
    }

    /* ECUIO_FAKE_WRITE_ZERO_COUNT=1: STATUS_NOERROR with *pNumMsgs set
     * to 0, a vendor reporting success for a message it never sent.
     * Not recorded, nothing answered. Re-read per call, like the knob
     * above. */
    if (fake_j2534::knob_on("ECUIO_FAKE_WRITE_ZERO_COUNT")) {
        *pNumMsgs = 0;
        return STATUS_NOERROR;
    }

    if (*pNumMsgs != 1) {
        return STATUS_FAILED;   /* the relay sends one at a time — strict */
    }

    unsigned long n = pMsg->DataSize;
    if (n > ECUIO_PT_WRITE_MAX) {
        return STATUS_FAILED;   /* beyond the transport's write limit */
    }

    const bool vx = fake_j2534::knob_on("ECUIO_FAKE_VX");
    const bool no_ack = vx && fake_j2534::knob_on("ECUIO_FAKE_NO_ACK");
    const unsigned long reply_delay_ms = fake_j2534::knob_ms(
        "ECUIO_FAKE_REPLY_DELAY_MS", vx ? fake_j2534::kVxReplyDelayMs : 0);
    const bool pending_first = fake_j2534::knob_on("ECUIO_FAKE_PENDING");
    long unsupported_nrc = -1;
    {
        char nb[32] = {};
        if (fake_j2534::read_knob("ECUIO_FAKE_UNSUPPORTED_NRC", nb, sizeof(nb))) {
            unsupported_nrc = strtol(nb, nullptr, 0);
        }
    }

    std::unique_lock<std::mutex> lk(fake_j2534::g_lock);

    /* The VX with nothing on the bus to acknowledge: past the third
     * unacknowledged write, each one blocks, fails and is echoed back.
     * The count is left at 1, "written 1". Not recorded. */
    if (no_ack && fake_j2534::g_unacked >= fake_j2534::kVxUnackedDepth) {
        lk.unlock();
        Sleep(fake_j2534::kVxRefusedWriteMs);
        lk.lock();
        fake_j2534::enqueue_rx_locked(
            pMsg->Data, n, fake_j2534::vendor_stamp(fake_j2534::now_us()),
            fake_j2534::kVxRefusedEcho);
        return STATUS_FAILED;
    }

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
    if (fake_control::Page* page = fake_control::attach()) {
        InterlockedIncrement(&page->writes);
    }

    /* The VX's TX indication for an accepted write: the CAN ID alone,
     * stamped as the write returns. With no node on the bus, nothing
     * answers. */
    const long long written_us = fake_j2534::now_us();
    if (vx && n > 0) {
        fake_j2534::enqueue_rx_locked(pMsg->Data, (n < 4) ? n : 4,
                                      fake_j2534::vendor_stamp(written_us),
                                      fake_j2534::kVxIndication);
    }
    if (no_ack) {
        fake_j2534::g_unacked++;
        return STATUS_NOERROR;
    }

    /* RESPONSE GENERATION.
     * A request: Service 01 or 09, EXACTLY 6 bytes with req[4] 0x01 or
     * 0x09, or Service 0x22, 7 bytes — the adapter-framed logical
     * message. With
     * an active filter the ECU it's addressed to answers; without one an
     * ISO 15765 channel receives nothing, so the legacy echo applies.
     * Everything else keeps the deterministic echo semantics. */
    if (n > 0) {
        const unsigned char* req = pMsg->Data;
        unsigned char echo[ECUIO_PT_MSG_DATA_SIZE];
        unsigned long echo_len;

        const bool has_filter = (fake_j2534::g_last_filter.filter_id != 0)
                                 && (n >= 4);
        const bool request = has_filter &&
            ((n == 6 && (req[4] == 0x01 || req[4] == 0x09)) ||
             (n == 7 && req[4] == 0x22));

        if (request) {
            if (!fake_j2534::build_obd2_response(req, n, unsupported_nrc,
                                                 echo, &echo_len)) {
                return STATUS_NOERROR;   /* the ECU stays silent */
            }
            if (pending_first) {
                unsigned char pending[7];
                unsigned long pending_len = 0;
                fake_j2534::build_negative(req, 0x78, pending, &pending_len);
                fake_j2534::enqueue_rx_locked(
                    pending, pending_len,
                    fake_j2534::vendor_stamp(written_us), 0, written_us);
            }
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

        const long long arrives_us =
            written_us + 1000LL * static_cast<long long>(reply_delay_ms);
        static unsigned long resp_ts = 0x1000;
        resp_ts += 1;
        fake_j2534::enqueue_rx_locked(
            echo, echo_len,
            vx ? fake_j2534::vendor_stamp(arrives_us) : resp_ts, 0,
            arrives_us);
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
        if (fake_j2534::read_knob("ECUIO_FAKE_IOCTL_DELAY_MS",
                                  stall_buf, sizeof(stall_buf))) {
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
    if (IoControlID == static_cast<long>(ECUIO_PT_IOCTL_READ_VBATT)) {
        if (const long rc = fake_j2534::refusal("ECUIO_FAKE_VBATT_STATUS",
                                                "READ_VBATT")) {
            return rc;
        }
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
        if (ChannelID != fake_j2534::g_device_id) {
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
    if (const long rc = fake_j2534::refusal("ECUIO_FAKE_FILTER_STATUS",
                                            "PassThruStartMsgFilter")) {
        return rc;
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

/* v04.04's form (DeviceID first). Like READ_VBATT, it fails for any
 * device but the one PassThruOpen returned, so a caller passing the
 * channel can't pass by coincidence. Exported by fake_j2534.def alone,
 * like PassThruIoctl, so the legacy fixture has no such export: a
 * dllexport here would export it from both. */
extern "C"
long __stdcall PassThruReadVersion(unsigned long DeviceID,
                                   char* pFirmwareVersion,
                                   char* pDllVersion,
                                   char* pApiVersion)
{
    if (pFirmwareVersion == nullptr || pDllVersion == nullptr ||
        pApiVersion == nullptr) {
        return 0x04;   /* ERR_NULL_PARAMETER */
    }
    if (DeviceID != fake_j2534::g_device_id) {
        return 0x1A;   /* ERR_INVALID_DEVICE_ID */
    }
    if (const long rc = fake_j2534::refusal("ECUIO_FAKE_VERSION_STATUS",
                                            "PassThruReadVersion")) {
        return rc;
    }
    memcpy(pFirmwareVersion, fake_j2534::kFirmwareVersion,
           sizeof(fake_j2534::kFirmwareVersion));
    memcpy(pDllVersion, fake_j2534::kDllVersion,
           sizeof(fake_j2534::kDllVersion));
    memcpy(pApiVersion, fake_j2534::kApiVersion,
           sizeof(fake_j2534::kApiVersion));
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruGetLastError(char* pErrorDescription)
{
    if (pErrorDescription == nullptr) {
        return 0x04;   /* ERR_NULL_PARAMETER */
    }
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    memcpy(pErrorDescription, fake_j2534::g_last_error,
           sizeof(fake_j2534::g_last_error));
    return STATUS_NOERROR;
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
