// File/layer: tests/fake_j2534.cpp | Fake J2534 vendor DLL, HARNESS tier; 
//             built in BOTH trees
// Testing: exercised via LoadLibraryEx by test_j2534_load/_stream and
//          via the surrogate child (either arch) - the existing VBATT test now only passes if the loader
//          really passes the device id.
// NOTE: InjectFrame/ReadMsgs share a mutex-guarded FIFO — injections
//      from the test thread race safely against the polling worker.
//      Connect params are recorded so tests can assert that URI
//      parsing reached PassThruConnect.
//
// PART 4 SEMANTICS (this revision):
//  * WriteMsgs: records the message (hook-observable) AND enqueues a
//    deterministic response — payload echoed with byte 0 + 0x40. This
//    is TEST-DEVICE SEMANTICS, not OBD correctness: it makes the
//    request→response loop observable through the RX path alone, from
//    EITHER architecture, without a script-arming pipe op (an x64 test
//    cannot reach into the x86 child's fake instance — the reason
//    OP_INJECT exists; arming scripts would need the same treatment).
//  * IoCtl: config table (SET stores, GET reads back), fixed VBATT,
//    CLEAR_* accepted. Unknown ids FAIL like a strict vendor.
//  * Filters: registration bookkeeping + last-invocation recording.
//    Filter-aware echo: once a filter is registered, WriteMsgs'
//    response prepends the filter pattern (4-byte BE CAN ID + pad).
//    SetFilterGating controls ONLY ReadMsgs dropping (frames whose
//    first mask_len bytes don't match the active filter are dropped);
//    the echo shape is unconditional once a filter lands.
//  * ExtraDataIndex now = DataSize (v4.04 no-extra-data convention).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "qttune/j2534_ptypes.h"

#include <cstring>
#include <mutex>
#include <cstdlib>

#define FAKE_QUEUE_DEPTH 16
#define FAKE_WRITE_DEPTH 16
#define FAKE_FILTER_DEPTH 8
#define FAKE_CONFIG_DEPTH 16
#define STATUS_NOERROR QTTUNE_PT_STATUS_NOERROR
#define STATUS_FAILED  QTTUNE_PT_STATUS_FAILED

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
/* One-shot IoCtl stall (review finding 1 repro): env-armed sleep on
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

/* Disconnect counter (issue #24): a successful client disconnect
 * must clear the child's channel so teardown never issues a SECOND
 * vendor disconnect. Monotonic, read via GetDisconnectCount —
 * in-proc instances only. The child's instance is a separate
 * process (see the OP_INJECT note above), so the automated
 * cross-process pin lands with the #23 tracing/query-op work;
 * this hook exists now so that work has its observable ready. */
unsigned long g_disconnect_count = 0;

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

} // namespace fake_j2534

extern "C" __declspec(dllexport)
long __stdcall PassThruOpen(void* pName, unsigned long* pDeviceID)
{
    (void)pName;
    if (pDeviceID == nullptr) {
        return STATUS_FAILED;
    }

    /* Test-only open blocking (Issue #22 test): QTTUNE_FAKE_OPEN_DELAY_MS
     * makes PassThruOpen sleep before doing anything — the blocked-
     * vendor-open mode. Armed via ENVIRONMENT, not a hook: the child
     * inherits the parent's environment, so an x64 test arms the x86
     * child's fake with zero protocol surface. */
    {
        char buf[32] = {};
        const DWORD n = GetEnvironmentVariableA("QTTUNE_FAKE_OPEN_DELAY_MS",
                                                 buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            const DWORD delay = static_cast<DWORD>(strtoul(buf, nullptr, 10));
            if (delay > 0 && delay <= 60000) Sleep(delay);
        }
    }

    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    fake_j2534::g_handle = 1;
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
    /* Distinct from the device id (1) ON PURPOSE: the fake's device and
     * channel used to collide at 1, which hid the READ_VBATT
     * channel-vs-device bug (review 2026-10-17, finding 9). 0x55 is
     * arbitrary, nonzero, and unrelated to any protocol constant. */
    *pChannelID = 0x55;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruDisconnect(unsigned long ChannelID)
{
    (void)ChannelID;
    /* Tolerant by design: a real vendor's second-disconnect behavior
     * is vendor swamp we don't model — the COUNT is the contract
     * observable (issue #24), not the return code. */
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    fake_j2534::g_disconnect_count++;
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

    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);

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
                if (filter_matches(fake_j2534::g_queue[0].data,
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
    pMsg->ProtocolID    = QTTUNE_PT_PROTOCOL_ISO15765;
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

    /* Deterministic response — DEVICE MODEL, filter-aware.
     *
     * With no filter registered: raw echo of the payload with byte 0
     * + 0x40 (Part 4 relay-test semantics — unchanged, keeps the
     * surrogate/routing write tests byte-compatible).
     *
     * With a filter registered: a real ISO 15765 device CONSUMES the
     * request's 4-byte address header and responds [its own ID BE]
     * [payload, byte 0 + 0x40]. Modeling the device (not the filter
     * message shape) is what makes the transport's 4-byte strip leave
     * a clean payload — and it works from EITHER architecture with no
     * arming hook (the cross-arch observable).
     *
     * Request-side precondition: a filter-registered write is expected
     * to carry a 4-byte address prefix (the transport always sends
     * one). Writes shorter than 4 bytes fall back to the raw echo —
     * defensive, not a contract.
     */
    if (n > 0) {
        unsigned char echo[64];
        unsigned long echo_len;

        const bool has_filter = (fake_j2534::g_last_filter.filter_id != 0)
                                 && (n >= 4);

        if (has_filter) {
            /* device model: [pattern ID (4)][payload with +0x40] */
            memcpy(echo, fake_j2534::g_last_filter.pattern, 4);
            memcpy(echo + 4, pMsg->Data + 4, n - 4);
            echo[4] = static_cast<unsigned char>(pMsg->Data[4] + 0x40);
            echo_len = 4 + (n - 4);
        } else {
            /* legacy Part 4 semantics: raw echo, byte 0 + 0x40 */
            memcpy(echo, pMsg->Data, n);
            echo[0] = static_cast<unsigned char>(pMsg->Data[0] + 0x40);
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

   /* Review finding 1 repro: one-shot env-armed stall. Flag taken
     * under the lock, SLEEP taken outside it — the read pump calls
     * PassThruReadMsgs on the same mutex and must keep flowing. */
    bool do_stall = false;
    char stall_buf[32] = {};
    {
        const DWORD n = GetEnvironmentVariableA("QTTUNE_FAKE_IOCTL_DELAY_MS",
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
    case QTTUNE_PT_IOCTL_GET_CONFIG: {
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
    case QTTUNE_PT_IOCTL_SET_CONFIG: {
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
    case QTTUNE_PT_IOCTL_READ_VBATT:
        /* Contract pin (review finding 9): v04.04 passes the DeviceID
         * to READ_VBATT. This fake's PassThruOpen returns device 1 and
         * PassThruConnect returns channel 0x55 — a caller passing the
         * channel now FAILS here instead of colliding at 1 == 1. */
        if (ChannelID != 1) {
            return STATUS_FAILED;
        }
        if (pOutput != nullptr) {
            *static_cast<unsigned long*>(pOutput) = 13800;  /* 13.8 V in mV */
        }
        return STATUS_NOERROR;
    case QTTUNE_PT_IOCTL_CLEAR_TX_BUFFER:
    case QTTUNE_PT_IOCTL_CLEAR_RX_BUFFER:
    case QTTUNE_PT_IOCTL_CLEAR_MSG_FILTERS:
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
    if (FilterType != QTTUNE_PT_PASS_FILTER &&
        FilterType != QTTUNE_PT_BLOCK_FILTER &&
        FilterType != QTTUNE_PT_FLOW_CONTROL_FILTER) {
        return STATUS_FAILED;
    }
    if (FilterType == QTTUNE_PT_FLOW_CONTROL_FILTER &&
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
                                  is_extended ? QTTUNE_PT_CAN_29BIT_ID : 0ul);
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

/* ---- Part 4 hooks ---- */

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

/* Monotonic PassThruDisconnect count (issue #24): explicit
 * client-disconnect-then-close must total exactly 1. In-proc
 * instances only — cross-process pin pending the #23 query op. */
extern "C" __declspec(dllexport)
unsigned long GetDisconnectCount(void)
{
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    return fake_j2534::g_disconnect_count;
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

#endif // TEST_HOOKS_ENABLED || _DEBUG
