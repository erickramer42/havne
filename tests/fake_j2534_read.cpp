// File/layer: tests/fake_j2534_read.cpp | Fake J2534 vendor DLL, HARNESS tier;
//             built in BOTH trees
// Testing: PassThruReadMsgs of the fake (see fake_j2534.cpp)

#include "fake_j2534_state.h"

extern "C" __declspec(dllexport)
long __stdcall PassThruReadMsgs(unsigned long ChannelID,
                                PASSTHRU_MSG* pMsg,
                                unsigned long* pNumMsgs,
                                unsigned long Timeout)
{
    (void)ChannelID;
    if (pMsg == nullptr || pNumMsgs == nullptr) {
        return STATUS_FAILED;
    }
    /* Every call and the wait it asked, for a test to read how a read
     * called the vendor (the VX holds a 0 ms call 50 ms) */
    if (fake_control::Page* page = fake_control::attach()) {
        fake_control::record_read(page, static_cast<uint32_t>(Timeout));
    }

    /* ECUIO_FAKE_EXIT_ON_READ=1: the vendor DLL crashes mid-session.
     * It ends the process at once (exit code 9, no surrogate stage's),
     * but only a surrogate's, so a test that loads the fake itself
     * never takes its own runner down. */
    if (fake_j2534::knob_on("ECUIO_FAKE_EXIT_ON_READ") &&
        fake_j2534::in_a_surrogate()) {
        TerminateProcess(GetCurrentProcess(), 9);
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
     * call to return the given vendor status with zero messages:
     * 0x09 (ERR_TIMEOUT) for quiet-bus, a hard code (e.g. 0x07) for
     * the dead-adapter path. Persistent while set and re-read per
     * call: the surrogate child sees the value it inherited at spawn,
     * unless the control page sets it. Checked
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
     * message array: the caller's request count stays DIRTY and its
     * buffer keeps whatever the caller put there (zeros, or the
     * surrogate child's previous batch contents). Mirrors the VX,
     * which reports "status 7, written 1" on a failed write
     * (docs/bench-sessions/2026-10-20.md). Re-read per call: the
     * surrogate child sees the spawn-time value unless the control
     * page sets it. */
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
     * STATUS_NOERROR and any empty read with ERR_BUFFER_EMPTY, holds
     * a timeout-0 read kVxZeroTimeoutHoldMs, and one asked to wait
     * less than kVxShortReadHoldMs that long. Filter
     * gating isn't modeled here. Off by default: the exact-count
     * stream tests pin the single-message delivery below. */
    const bool vx = fake_j2534::knob_on("ECUIO_FAKE_VX");
    if (vx || fake_j2534::knob_on("ECUIO_FAKE_SPEC_READ_SEMANTICS")) {
        const unsigned long requested = *pNumMsgs;
        unsigned long hold_ms = Timeout;
        if (vx && Timeout == 0) {
            hold_ms = fake_j2534::kVxZeroTimeoutHoldMs;
        } else if (vx && Timeout < fake_j2534::kVxShortReadHoldMs) {
            hold_ms = fake_j2534::kVxShortReadHoldMs;
        }
        const long long deadline =
            fake_j2534::now_us() + 1000LL * static_cast<long long>(hold_ms);
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
        /* ECUIO_FAKE_OVER_REPORT=1: one message past the count, when one
         * more has arrived. The caller must have room for it: only the
         * surrogate's read buffer, room for 16, is read this way, and its
         * calls ask for one */
        const unsigned long cap =
            (fake_j2534::knob_on("ECUIO_FAKE_OVER_REPORT") && requested < 16)
                ? requested + 1
                : requested;
        if (avail > cap) {
            avail = cap;
        }
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
        if (Timeout > 0 &&
            (avail < requested ||
             fake_j2534::knob_on("ECUIO_FAKE_TIMED_READ_TIMEOUT"))) {
            return ECUIO_PT_STATUS_TIMEOUT;
        }
        if (avail == 0 && Timeout == 0) {
            return ECUIO_PT_STATUS_BUFFER_EMPTY;
        }
        return STATUS_NOERROR;
    }

    /* FILTER GATING (ECUIO_FAKE_FILTER_GATING=1): model the device's
     * filter hardware: non-matching frames are DISCARDED, never
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
    if (fake_j2534::knob_on("ECUIO_FAKE_FILTER_GATING")) {
        bool have_match_at_head = false;
        unsigned long judged = 0;
        while (judged < arrived && fake_j2534::g_queue_len > 0) {
            bool match = false;
            for (unsigned long i = 0; i < FAKE_FILTER_DEPTH; ++i) {
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
        *pNumMsgs = 0;   /* quiet bus: idle, not an error */
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
    /* ECUIO_FAKE_READ_OVERFLOW=1: a v04.04 vendor whose buffer
     * overflowed, answering ERR_BUFFER_OVERFLOW with the message it read
     * and no RxStatus bit */
    if (fake_j2534::knob_on("ECUIO_FAKE_READ_OVERFLOW")) {
        return ECUIO_PT_STATUS_BUFFER_OVERFLOW;
    }
    return STATUS_NOERROR;
}
