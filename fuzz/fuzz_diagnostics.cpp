// File/layer: fuzz/fuzz_diagnostics.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: the request engine reading an ECU's diagnostics
//          (ADR-0037) from arbitrary replies: each request the list sends
//          answered by a timeout, raw bytes, a positive reply with an
//          arbitrary tail or a negative one, with Read again and rescans
//          between; the report stays within its caps, every code and
//          monitor it holds has a name, its values are finite, it reads
//          complete only with nothing left to ask, two runs agree

#include <ecuio/core.h>
#include <ecuio/signals.h>

#include "request_engine.h" /* core/src: the engine, with the time passed in */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace {

/* Large (every channel's tables), so off the stack, as the transports
 * keep it */
ecuio_request_engine g_engine;

/* The bytes left of the input, taken from the front */
struct script {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    bool done() const { return pos >= size; }
    uint8_t next() { return pos < size ? data[pos++] : 0; }
};

void check(const ecuio_request_engine& e)
{
    const ecuio_engine_channel& ch = e.chans[0];
    if (ch.diag_count > ECUIO_DIAG_MAX_ITEMS ||
        ch.diag_pending > ch.diag_count) {
        abort();
    }
    ecuio_channel_diagnostics_t d;
    ecuio_engine_diagnostics(&e, 0, &d);
    if ((d.flags & ECUIO_DIAG_COMPLETE) != 0 &&
        (ch.diag_pending != 0 || ch.diagnosing)) {
        abort();
    }
    if (d.readiness > ECUIO_DIAG_UNANSWERED ||
        d.freeze > ECUIO_DIAG_UNANSWERED) {
        abort();
    }
    for (uint32_t k = 0; k < ECUIO_DIAG_LISTS; ++k) {
        const ecuio_dtc_list_t& l = d.lists[k];
        if (l.outcome > ECUIO_DIAG_UNANSWERED || l.held > ECUIO_DIAG_MAX_DTCS ||
            l.held > l.reported) {
            abort();
        }
        for (uint32_t i = 0; i < l.held; ++i) {
            char code[6];
            if (ecuio_dtc_format(l.codes[i], code, sizeof code) != ECUIO_OK ||
                strlen(code) != 5) {
                abort();
            }
        }
    }
    if (d.monitor_count > ECUIO_DIAG_MAX_MONITORS) {
        abort();
    }
    for (uint32_t i = 0; i < d.monitor_count; ++i) {
        const char* name = ecuio_monitor_name(d.monitors[i].id, d.flags);
        if (name == nullptr || name[0] == '\0') {
            abort();
        }
    }
    if (d.freeze_count > ECUIO_DIAG_MAX_FREEZE) {
        abort();
    }
    for (uint32_t i = 0; i < d.freeze_count; ++i) {
        if (!isfinite(d.freeze_values[i].value)) {
            abort();
        }
    }
}

/* One run of the script; the report it ends with */
void run(const uint8_t* data, size_t size, ecuio_channel_diagnostics_t* out)
{
    ecuio_request_engine& e = g_engine;
    ecuio_engine_init(&e, 1);
    ecuio_engine_diagnose(&e, ecuio_obd2_signal_set());
    script s{data, size};
    int64_t now = 0;
    for (int step = 0; step < 512 && !s.done(); ++step) {
        const uint8_t op = s.next();
        if ((op & 7u) == 0) {
            ecuio_engine_diagnose_again(&e);
        } else if ((op & 7u) == 1) {
            ecuio_engine_rescan(&e);
        } else {
            const int32_t r = ecuio_engine_due(&e, 0, now);
            if (r == ECUIO_ENGINE_NONE) {
                now += ECUIO_ENGINE_WAIT_US;
                (void)ecuio_engine_expire(&e, 0, now);
                check(e);
                continue;
            }
            const ecuio_request q = ecuio_engine_request_of(&e, 0, r);
            ecuio_engine_sent(&e, 0, r, true, now);
            uint8_t reply[128]; /* past the 32 codes a list holds */
            uint16_t len = 0;
            const uint8_t tail = static_cast<uint8_t>(s.next() % 120u);
            switch ((op >> 3) & 3u) {
            case 0: /* no answer */
                now += ECUIO_ENGINE_WAIT_US;
                (void)ecuio_engine_expire(&e, 0, now);
                break;
            case 1: /* whatever bytes came */
                while (len < tail && !s.done()) {
                    reply[len++] = s.next();
                }
                break;
            case 2: /* a positive reply to the request, any tail */
                reply[len++] = static_cast<uint8_t>(q.bytes[0] + 0x40u);
                for (uint8_t i = 1; i < q.len && len < sizeof reply; ++i) {
                    reply[len++] = q.bytes[i];
                }
                while (len < sizeof reply && tail > 0 && len < q.len + tail &&
                       !s.done()) {
                    reply[len++] = s.next();
                }
                break;
            default: /* a negative one, a pending code among them */
                reply[len++] = 0x7F;
                reply[len++] = q.bytes[0];
                reply[len++] = s.next();
                break;
            }
            if (len > 0) {
                (void)ecuio_engine_on_frame(&e, 0, reply, len, now + 4000,
                                            nullptr);
            }
        }
        now += ECUIO_ENGINE_GAP_US;
        check(e);
    }
    ecuio_engine_diagnostics(&e, 0, out);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    ecuio_channel_diagnostics_t a;
    ecuio_channel_diagnostics_t b;
    run(data, size, &a);
    run(data, size, &b);
    if (memcmp(&a, &b, sizeof a) != 0) {
        abort();
    }
    return 0;
}
