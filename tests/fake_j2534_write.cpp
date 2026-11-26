// File/layer: tests/fake_j2534_write.cpp | Fake J2534 vendor DLL, HARNESS tier;
//             built in BOTH trees
// Testing: PassThruWriteMsgs of the fake (see fake_j2534.cpp)

#include "fake_j2534_state.h"

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
     * recording the message or generating a response: the VX quiet-
     * bus shape: writes fail (no ACK on the bus) while reads stay
     * clean. Like ECUIO_FAKE_UNTOUCHED_COUNT_STATUS on the read side,
     * *pNumMsgs is left as passed (1), the VX's "status 7, written 1",
     * so a write path that read the count before the status would be
     * caught. Unrecorded writes don't move the page's count. Re-read per
     * call: the child sees the spawn-time value unless the control
     * page sets it. */
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
        return STATUS_FAILED;   /* the relay sends one at a time: strict */
    }

    unsigned long n = pMsg->DataSize;
    if (n > ECUIO_PT_WRITE_MAX) {
        return STATUS_FAILED;   /* beyond the transport's write limit */
    }

    const bool vx = fake_j2534::knob_on("ECUIO_FAKE_VX");
    const bool no_ack = vx && fake_j2534::knob_on("ECUIO_FAKE_NO_ACK");
    const unsigned long reply_delay_ms = fake_j2534::knob_ms(
        "ECUIO_FAKE_REPLY_DELAY_MS", vx ? fake_j2534::kVxReplyDelayMs : 0);
    const unsigned long long_delay_ms =
        fake_j2534::knob_ms("ECUIO_FAKE_LONG_REPLY_DELAY_MS", 0);
    const bool pending_first = fake_j2534::knob_on("ECUIO_FAKE_PENDING");
    const bool lose = fake_j2534::knob_on("ECUIO_FAKE_LOST_UNDER_SEGMENTS");
    long held_service = -1;
    {
        char hb[32] = {};
        if (fake_j2534::read_knob("ECUIO_FAKE_HELD_SERVICE", hb, sizeof(hb))) {
            held_service = strtol(hb, nullptr, 16);
        }
    }
    long unsupported_nrc = -1;
    {
        char nb[32] = {};
        if (fake_j2534::read_knob("ECUIO_FAKE_UNSUPPORTED_NRC", nb, sizeof(nb))) {
            unsupported_nrc = strtol(nb, nullptr, 0);
        }
    }
    {
        /* Service 04 refused with this code (fake_ecu::Diagnostics) */
        char nb[32] = {};
        fake_ecu::diagnostics().clear_nrc =
            fake_j2534::read_knob("ECUIO_FAKE_CLEAR_NRC", nb, sizeof(nb))
                ? static_cast<int>(strtol(nb, nullptr, 0))
                : 0;
        /* PID 0C at 0 rpm: the engine stopped, as a clear needs */
        fake_ecu::diagnostics().engine_off =
            fake_j2534::read_knob("ECUIO_FAKE_ENGINE_OFF", nb, sizeof(nb)) &&
            strtol(nb, nullptr, 0) != 0;
        /* PID 0D at this speed: the vehicle moving */
        fake_ecu::diagnostics().speed_kmh =
            fake_j2534::read_knob("ECUIO_FAKE_SPEED_KMH", nb, sizeof(nb))
                ? static_cast<int>(strtol(nb, nullptr, 0))
                : -1;
        /* A packed Service 01 request answered for its first PID alone */
        fake_ecu::pack_first_only() =
            fake_j2534::read_knob("ECUIO_FAKE_PACK_FIRST_ONLY", nb,
                                  sizeof(nb)) &&
            strtol(nb, nullptr, 0) != 0;
    }
    {
        /* PIDs left out of the bitmaps: hex, separated by anything
         * else ("0A,5C") */
        char list[128] = {};
        uint32_t words[8] = {};
        if (fake_j2534::read_knob("ECUIO_FAKE_WITHHELD_PIDS", list,
                                  sizeof(list))) {
            for (const char* s = list; *s != '\0';) {
                char* end = nullptr;
                const unsigned long pid = strtoul(s, &end, 16);
                if (end == s) {
                    ++s;
                    continue;
                }
                if (pid <= 0xFF) {
                    words[pid / 32] |= 1u << (pid % 32);
                }
                s = end;
            }
        }
        for (unsigned i = 0; i < 8; ++i) {
            fake_ecu::withheld_pids()[i] = words[i];
        }
    }
    {
        /* Modules of the fake's own: "7E9:D801=00 12;..." (fake_ecus.h's
         * set_module_dids); unset or malformed, none */
        char spec[2048] = {};
        if (!fake_j2534::read_knob("ECUIO_FAKE_MODULE_DIDS", spec,
                                   sizeof(spec)) ||
            !fake_ecu::set_module_dids(spec)) {
            fake_ecu::set_module_dids(nullptr);
        }
        /* An ECU that isn't there: its reply ID in hex (fake_ecus.h's
         * absent_ecu); unset, none */
        char absent[32] = {};
        fake_ecu::absent_ecu().store(
            fake_j2534::read_knob("ECUIO_FAKE_ABSENT_ECU", absent,
                                  sizeof(absent))
                ? static_cast<uint32_t>(strtoul(absent, nullptr, 16))
                : 0u,
            std::memory_order_release);
        /* The code a module's 7F 10 carries (0, the default, is 0x12) */
        char code[32] = {};
        fake_ecu::session_nrc() =
            fake_j2534::read_knob("ECUIO_FAKE_SESSION_NRC", code, sizeof(code))
                ? static_cast<unsigned>(strtol(code, nullptr, 0))
                : 0u;
        /* Security access that wants its key straight after its seed */
        fake_ecu::security_strict() =
            fake_j2534::knob_on("ECUIO_FAKE_SECURITY_STRICT");
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

    /* Counted on the page, the first kWriteDepth kept whole. */
    if (fake_control::Page* page = fake_control::attach()) {
        fake_control::record_write(
            page, pMsg->Data, n, pMsg->TxFlags,
            static_cast<uint32_t>(fake_j2534::now_us() & 0xFFFFFFFFLL));
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
     * 0x09 (or 0x10 and 0x3E, the session's, ADR-0060); Service 0x22 or
     * 02, 7 bytes; Service 03, 04, 07 or 0x0A, 5
     * bytes; Service 0x27 of any length (the engine ECU's security
     * access) and 0x23 (its memory): the adapter-framed logical message. With
     * an active filter the ECU it's addressed to answers; without one an
     * ISO 15765 channel receives nothing, so the legacy echo applies.
     * Everything else keeps the deterministic echo semantics. */
    if (n > 0) {
        const unsigned char* req = pMsg->Data;
        unsigned char echo[ECUIO_PT_MSG_DATA_SIZE];
        unsigned long echo_len;

        const bool has_filter = (fake_j2534::g_last_filter.filter_id != 0)
                                 && (n >= 4);
        const bool request =
            has_filter && ((n == 6 && (req[4] == 0x01 || req[4] == 0x09 ||
                                       req[4] == 0x10 || req[4] == 0x3E)) ||
                           (n >= 7 && n <= 11 && req[4] == 0x01) ||
                           (n == 7 && (req[4] == 0x22 || req[4] == 0x02)) ||
                           (n >= 5 && (req[4] == 0x27 || req[4] == 0x23)) ||
                           (n == 5 && (req[4] == 0x03 || req[4] == 0x04 ||
                                       req[4] == 0x07 || req[4] == 0x0A)));

        if (request && req[4] == held_service) {
            unsigned char pending[7];
            unsigned long pending_len = 0;
            fake_j2534::build_negative(req, 0x78, pending, &pending_len);
            fake_j2534::enqueue_rx_locked(pending, pending_len,
                                          fake_j2534::vendor_stamp(written_us),
                                          0, written_us);
            return STATUS_NOERROR; /* the answer never comes */
        }
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

        const unsigned long delay_ms =
            reply_delay_ms + (echo_len > 4 + 7 ? long_delay_ms : 0);
        const long long arrives_us =
            written_us + 1000LL * static_cast<long long>(delay_ms);
        static unsigned long resp_ts = 0x1000;
        resp_ts += 1;
        const uint32_t lost = fake_j2534::enqueue_reply_locked(
            echo, echo_len, vx ? fake_j2534::vendor_stamp(arrives_us) : resp_ts,
            written_us + 1000LL * static_cast<long long>(reply_delay_ms),
            arrives_us, lose);
        fake_control::Page* page = fake_control::attach();
        if (lost > 0 && page != nullptr) {
            fake_control::count_lost(page, lost);
        }
    }

    return STATUS_NOERROR;   /* *pNumMsgs stays 1: fully written */
}
