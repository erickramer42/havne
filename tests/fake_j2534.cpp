// File/layer: tests/fake_j2534.cpp | Fake J2534 vendor DLL, HARNESS tier;
//             built in BOTH trees
// Testing: loaded by the J2534 suites' sessions in the surrogate
//          child (either arch); tests reach it only through its
//          control page (fake_control.h)
// NOTE: ReadMsgs, WriteMsgs' replies and InjectFrame (OP_INJECT's and
//      the loader's inject hook) share a mutex-guarded FIFO, so
//      injections race safely against the polling worker. Connect
//      parameters are published so tests can assert that URI parsing
//      reached PassThruConnect.
//
// BEHAVIOR:
//  * WriteMsgs: publishes the message on the page. Response
//    generation is two-mode:
//      - A request, with a filter registered: Service 01 or 09
//        [4-byte ID BE][01|09][PID] (exactly 6 bytes, the adapter-framed
//        logical message) or Service 0x22 [ID][22][DID] (7 bytes). The
//        ECU the request is addressed to answers: [reply ID BE][41][PID]
//        [canned] from tests/fake_ecu.h or its supported-PID bitmap;
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
//        direction: the fake models the adapter side of ISO-TP, and
//        the transport's 4-byte header strip lands the bridge on the
//        obd2 set's mux-at-offset-0 grammar.
//      - Everything else: the legacy deterministic echo (byte 0
//        +0x40), preserving existing framing tests.
//    ECUIO_FAKE_WRITE_STATUS fails every write with the given status,
//    recording nothing and echoing nothing; ECUIO_FAKE_WRITE_ZERO_COUNT=1
//    answers every write STATUS_NOERROR with a count of 0, the same
//    way. ECUIO_FAKE_REPLY_DELAY_MS
//    holds each response back that long after the write (default 0);
//    ECUIO_FAKE_LONG_REPLY_DELAY_MS adds its delay to a response longer
//    than one Single Frame (over 7 bytes), as a long reply takes;
//    ECUIO_FAKE_LOST_UNDER_SEGMENTS=1 drops a Single Frame reply from
//    one ID arriving while another ID's long reply is on the bus (its
//    First Frame at the reply delay to its whole arrival), whichever
//    was written first: Known Issue #42 on the VX, counted on the page;
//    ECUIO_FAKE_PENDING=1 answers each request with "response pending"
//    (7F <service> 78) at once, ahead of that reply.
//    ECUIO_FAKE_HELD_SERVICE=<service, hex> answers each request of that
//    service with 7F <service> 78 alone: its answer never comes, so each
//    stays out for the engine's pending wait.
//    ECUIO_FAKE_ABSENT_ECU=<reply ID, hex> leaves every request to that
//    ECU with no answer: a module this car lacks. FFFFFFFF leaves every
//    ECU's: a car with its ignition off.
//    ECUIO_FAKE_MEM=<fault>:<n>[,...] breaks the engine ECU's memory
//    reads as the simulated ECU's mem= does (core/src/sim_memory.h).
//    ECUIO_FAKE_SECURITY_STRICT=1 has the engine ECU's security access
//    want its key straight after its seed (fake_ecus.h).
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
//  * Filters: registration bookkeeping; the last started and stopped,
//    and the count held now and at most, are published.
//    ECUIO_FAKE_FILTER_MAX=N refuses a start with N held
//    (ERR_EXCEEDED_LIMIT), as an adapter out of filters does. Filter-aware echo prepends the filter pattern
//    (4-byte BE CAN ID). ECUIO_FAKE_FILTER_GATING=1 controls ONLY
//    ReadMsgs dropping; the echo shape is unconditional once a filter
//    lands.
//  * Knobs (ECUIO_FAKE_*) are read on every call: from the control
//    page (fake_control.h) when it holds the name, else from the
//    environment. The page reaches the surrogate child's instance
//    mid-session, which the child's spawn-time environment can't. It
//    also carries messages a test queues for ReadMsgs, and the fake
//    publishes its successful writes (the first 16 whole), the last
//    connect's parameters, the last filter started and stopped, the
//    process that opened it, and its disconnects and closes by the IDs
//    it handed out.
//  * ReadMsgs, by default, returns at most one message per call,
//    whatever NumMsgs and Timeout ask for, and STATUS_NOERROR with zero
//    messages on an empty queue. ECUIO_FAKE_READ_STATUS forces a
//    status with no messages; ECUIO_FAKE_UNTOUCHED_COUNT_STATUS
//    forces a status with the caller's count and buffer left as
//    passed (a sloppy vendor; see ecuio_trusted_count);
//    ECUIO_FAKE_SPEC_READ_SEMANTICS=1 switches to J2534-1 batches;
//    there ECUIO_FAKE_TIMED_READ_TIMEOUT=1 answers every read with a
//    timeout ERR_TIMEOUT, even one that filled its count (a vendor that
//    reports nearly every response that way). ECUIO_FAKE_OVER_REPORT=1,
//    in a batch mode, hands a call one message more than it asked when
//    one more has arrived (J2534-1 forbids it; only into the surrogate's
//    read buffer, room for 16, so the asked count is under 16).
//    ECUIO_FAKE_READ_OVERFLOW=1 answers each delivered message with
//    ERR_BUFFER_OVERFLOW (v04.04's form of frames lost).
//    ECUIO_FAKE_READ_DELAY_MS stalls the first read after load, once.
//    ECUIO_FAKE_EXIT_ON_READ=1 ends a surrogate's process at its next
//    read (a vendor DLL that crashes); ECUIO_FAKE_EXIT_ON_CLOSE=1 at
//    PassThruClose, with exit code 11.
//  * ECUIO_FAKE_VX=1 models the OBDX Pro VX as measured
//    (docs/bench-sessions/2026-10-22.md and its vehicle sheet): batch
//    reads held for their timeout, a short batch with STATUS_NOERROR
//    and an empty read with ERR_BUFFER_EMPTY; after each accepted
//    write a TX indication (RxStatus 0x09, the 4-byte CAN ID), and the
//    reply 4 ms later; a read with a timeout of 0 held 50 ms, as a
//    50 ms timeout would (docs/bench-sessions/2026-11-04-two-ecu-car.md),
//    and one asked to wait less than 10 ms held 10 ms
//    (docs/bench-sessions/2026-11-05-two-ecu-car-rerun.md). ECUIO_FAKE_NO_ACK=1
//    adds the bus with no other node: three writes accepted per connect, then each blocks about
//    500 ms and fails ERR_FAILED, echoed with RxStatus 0x201.
//  * A queued message has an arrival time; reads see only those that
//    have arrived, in arrival order.
//  * ExtraDataIndex = DataSize (v4.04 no-extra-data convention).

#define WIN32_LEAN_AND_MEAN
#include "fake_j2534_state.h"

extern "C" __declspec(dllexport)
long __stdcall PassThruOpen(void* pName, unsigned long* pDeviceID)
{
    (void)pName;
    if (pDeviceID == nullptr) {
        return STATUS_FAILED;
    }

    /* Test-only open blocking: ECUIO_FAKE_OPEN_DELAY_MS
     * makes PassThruOpen sleep before doing anything: the blocked-
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
    if (fake_j2534::knob_on("ECUIO_FAKE_EXIT_ON_CLOSE") &&
        fake_j2534::in_a_surrogate()) {
        TerminateProcess(GetCurrentProcess(), 11);
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
    /* Published before the checks: a refused connect still shows what
     * was asked, which the query-param tests read. */
    if (fake_control::Page* page = fake_control::attach()) {
        fake_control::record_connect(page, ProtocolID, Flags, BaudRate);
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
     * is vendor swamp we don't model: the COUNT is the contract
     * observable, not the return code. */
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    if (ChannelID == fake_j2534::g_channel_id) {
        if (fake_control::Page* page = fake_control::attach()) {
            InterlockedIncrement(&page->disconnects);
        }
    }
    return STATUS_NOERROR;
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
    /* the filters the fake holds at once (a number, read as knob_ms does) */
    const unsigned long most =
        fake_j2534::knob_ms("ECUIO_FAKE_FILTER_MAX", FAKE_FILTER_DEPTH);

    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    unsigned long active = 0;
    fake_j2534::FilterRecord* slot = nullptr;
    for (fake_j2534::FilterRecord& f : fake_j2534::g_filters) {
        if (f.filter_id != 0) {
            ++active;
        } else if (slot == nullptr) {
            slot = &f;
        }
    }
    if (active >= most || slot == nullptr) {
        snprintf(fake_j2534::g_last_error, sizeof(fake_j2534::g_last_error),
                 "fake_j2534: PassThruStartMsgFilter refused "
                 "(ECUIO_FAKE_FILTER_MAX)");
        if (fake_control::Page* page = fake_control::attach()) {
            InterlockedIncrement(&page->filters_refused);
        }
        return ERR_EXCEEDED_LIMIT;
    }

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
    *slot = rec;
    fake_j2534::g_last_filter = rec;
    if (fake_control::Page* page = fake_control::attach()) {
        fake_control::record_filter(page, rec.filter_id, rec.filter_type,
                                    rec.mask, rec.pattern, rec.mask_len,
                                    rec.flow_control);
        fake_control::record_filters_active(page,
                                            static_cast<uint32_t>(active + 1));
    }

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
    /* Published for the detach-ordering tests; the page's lock is taken
     * under the fake's, as the writes' record takes it */
    if (fake_control::Page* page = fake_control::attach()) {
        fake_control::record_filter_stop(
            page, FilterID, static_cast<uint32_t>(fake_j2534::g_queue_len));
    }
    unsigned long active = 0;
    bool found = false;
    for (fake_j2534::FilterRecord& f : fake_j2534::g_filters) {
        if (f.filter_id == FilterID) {
            f.filter_id = 0; /* the slot is free */
            found = true;
        } else if (f.filter_id != 0) {
            ++active;
        }
    }
    if (!found) {
        return STATUS_FAILED; /* unknown/already-stopped: strict vendor */
    }
    if (fake_control::Page* page = fake_control::attach()) {
        fake_control::record_filters_active(page,
                                            static_cast<uint32_t>(active));
    }
    return STATUS_NOERROR;
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

#endif // TEST_HOOKS_ENABLED || _DEBUG
