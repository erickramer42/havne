// File/layer: core/src/j2534_transport.h | internal, C++17, Qt-free,
//             Windows only
// Testing: HARNESS: test_j2534_connect, test_j2534_stream,
//          test_j2534_health (through the transport they build)
//
// j2534:// transport as its sources share it: the transport struct, its
// channel, and the constants the worker and attach both use. The parts:
// j2534_transport.cpp (attach, start, detach, the URI) and
// j2534_worker.cpp (the worker).

#ifndef ECUIO_J2534_TRANSPORT_H
#define ECUIO_J2534_TRANSPORT_H

#ifdef _WIN32

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/j2534.h"
#include "session.h"
#include "request_engine.h"
#include "j2534_clock.h"
#include "host_clock.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace ecuio_j2534 {

/* Passive mode reads with this timeout. An empty read waits it out even
 * from a vendor that returns at once, so a quiet run takes the same
 * time on every adapter. */
constexpr uint32_t J2534_IDLE_READ_TIMEOUT_MS = 100;

/* With requests to serve, no read holds longer than this. The child asks
 * the vendor for one message a call, so a reply ends its read; a read that
 * brings nothing the VX holds to its timeout, and this keeps it short
 * enough for another channel's request
 * (docs/bench-sessions/2026-11-05-two-ecu-car-rerun.md). */
constexpr uint32_t J2534_READ_SLICE_MS = 10;

/* The floor between requests on a channel without ?gap=: through the VX
 * a Single Frame reply took 4.6 ms and a six-PID pack 16.4 ms
 * (docs/bench-sessions/2026-11-05-two-ecu-car-rerun.md), so the replies
 * set the pace and the floor only keeps an instant answer from spinning. */
constexpr int64_t J2534_GAP_US = 5000;

/* The request engine's timing is in request_engine.h; the link-health
 * rules, their quiet run and the failure backoff in link_health.h. */

/* On ISO 15765 channels J2534 carries the CAN ID in-band as 4
 * big-endian bytes, and the filter mask, pattern and flow-control
 * messages are bare 4-byte IDs: the adapter's ISO-TP stack does the
 * framing, so no PCI bytes. J2534-1 requires the three filter
 * messages to share DataSize and TxFlags. Verified: Opus IVS
 * "Constructing an OBDII Message", Quantex PassThruStartMsgFilter
 * docs. */
/* The filters a channel is taken to hold before any is refused (ADR-0065
 * step 5): no call reports a count, so live data keeps one less, the rest
 * for a visit, raised only on a measured adapter's count */
constexpr uint32_t J2534_FILTERS_GUARANTEED = 10;

constexpr uint32_t J2534_ADDR_LEN       = 4;
constexpr uint32_t J2534_FILTER_MSG_LEN = 4;

inline void pack_be32(uint8_t* out, uint32_t v)
{
    out[0] = static_cast<uint8_t>(v >> 24);
    out[1] = static_cast<uint8_t>(v >> 16);
    out[2] = static_cast<uint8_t>(v >> 8);
    out[3] = static_cast<uint8_t>(v);
}

} // namespace ecuio_j2534

/* One resolved receive/transmit channel. rx_id doubles as the
 * strip/stamp identity in pump_one: a received frame's 4-byte
 * address header matching chans[i].rx_id is stripped and stamped
 * with that rx_id before dispatch. */
struct j2534_channel {
    uint32_t rx_id = 0;
    uint32_t tx_id = 0;
    unsigned long filter_id = 0;
    bool filter_active = false;
    uint32_t addr_flags = 0;   /* CAN_29BIT_ID when either id > 11 bits */
};

struct ecuio_j2534_transport {
    ecuio_session_t* session = nullptr;
    ecuio_j2534_handle_t* handle = nullptr;
    unsigned long channel = 0;
    /*   chans        resolved channel table. Derived from the
     *                profile's declared channels at attach, with
     *                ?filter=/?txid= as overrides. ?filter= wins
     *                outright (single-channel shorthand); ?txid=
     *                alone rewrites every derived tx. Bounded by
     *                ECUIO_MAX_CHANNELS; each entry owns its filter
     *                id and CAN_29BIT flags. Frozen pre-thread.
     *   engine       the requests and their pacing (request_engine.h):
     *                the set's rotation, uds-generic's DID scan and the
     *                identification items, or the one ?poll= request,
     *                which outranks them all; empty for passive receive.
     *                Built at attach, the worker's alone after.
     *   probe        ADR-0015 section 4 for the set's rotation: on unless
     *                ?probe=0 or ?poll=. Frozen pre-thread.
     *   identify     ADR-0021: each channel asks its ECU's identity
     *                once; on with the set's requests unless ?identify=0
     *                or ?poll=. Frozen pre-thread.
     *   hold         Known Issue #42's hold (j2534_hold.h): on unless
     *                ?hold=0; it acts with two or more channels. Frozen
     *                pre-thread. */
    j2534_channel chans[ECUIO_MAX_CHANNELS] = {};
    uint32_t chan_count = 0;
    ecuio_request_engine engine;
    std::unique_ptr<ecuio_timing_table> timing; /* the engine's, ADR-0042 */
    bool probe = false;
    bool identify = false;
    bool hold = true;
    /* Frame time (j2534_clock.h): the host's at the first frame, counted
     * from epoch, the end of attach; the vendor's deltas after that.
     * Worker-only, through pump_one. */
    host_clock::time_point epoch{};
    j2534_clock clock;
    /* The frame pump_one builds from each read, dispatched from here;
     * worker-only, and 4 KB, so it lives with the transport rather than
     * on the worker's stack. read_outcome::payload points into it. */
    ecuio_frame_t rx_frame{};
    /* A message carried BUFFER_OVERFLOW and no frame has said so yet
     * (mark_overflow); worker-only */
    bool overflow_pending = false;
    std::atomic<bool> stop_requested{false};
    std::thread worker;
};

namespace ecuio_j2534 {

/* The worker thread's body (j2534_worker.cpp): runs until stop_requested. */
void j2534_worker(ecuio_j2534_transport* t);

} // namespace ecuio_j2534

#endif /* _WIN32 */

#endif // ECUIO_J2534_TRANSPORT_H
