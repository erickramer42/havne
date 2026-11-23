// File/layer: core/src/socketcan_transport.h | internal, C++17, Qt-free;
//             Linux only (ECUIO_HAVE_SOCKETCAN)
// Testing: UNIT: SocketCanTest (test_socketcan); HARNESS: SocketCanBusTest,
//          SocketCanRequestTest, SocketCanFaultTest (test_vcan)
//
// socketcan://'s transport (ADR-0032, ADR-0040) as its two sources share
// it: socketcan_transport.cpp (attach, the link, the listening worker)
// and socketcan_request.cpp (the requesting worker).

#ifndef ECUIO_SOCKETCAN_TRANSPORT_H
#define ECUIO_SOCKETCAN_TRANSPORT_H

#include "ecuio/core.h"
#include "ecuio/internal.h"

#if ECUIO_HAVE_SOCKETCAN

#include "host_clock.h"
#include "isotp.h"
#include "request_engine.h"
#include "session.h"
#include "socketcan_wire.h"

#include <unistd.h>

#include <atomic>
#include <memory>
#include <thread>

namespace ecuio_socketcan {

/* Frames read per wake before the stop flag is looked at again */
constexpr int kReadBurst = 256;
/* The worker's wait: one quiet window of link health */
constexpr int kWaitMs = 100;

} // namespace ecuio_socketcan

/* Everything the worker uses is made in attach (the frame off its stack),
 * so the worker allocates nothing */
struct ecuio_socketcan_transport {
    ecuio_session_t* session = nullptr;
    char name[SOCKETCAN_IFNAME_MAX + 1] = {};
    uint32_t want_baud = 0;
    socketcan_filter filters[SOCKETCAN_MAX_FILTERS];
    size_t filter_count = 0;
    /* A session that requests (ADR-0040): its channels and their
     * addressing, the engine, ISO-TP's receiver (timed) and sender, the
     * message frame, and the rate's proof */
    bool requesting = false;
    /* The modules whose IDs the socket's filter holds, a bit each: every
     * one until the worker's first take, then the worker's alone */
    uint32_t hearing = UINT32_MAX;
    uint32_t chan_count = 0;
    uint32_t rx_ids[ECUIO_MAX_CHANNELS] = {};
    uint32_t tx_ids[ECUIO_MAX_CHANNELS] = {};
    bool ext29[ECUIO_MAX_CHANNELS] = {};
    ecuio_request_engine engine;
    std::unique_ptr<ecuio_timing_table> timing; /* the engine's, ADR-0042 */
    bool probe = false;
    bool identify = false;
    std::unique_ptr<isotp_rx> isotp;
    std::unique_ptr<isotp_tx> sender;
    std::unique_ptr<ecuio_frame_t> reply;
    socketcan_proof proof;
    int fd = -1;
    int wake_fd = -1;
    host_clock::time_point epoch;
    std::unique_ptr<ecuio_frame_t> frame;
    std::thread worker;
    std::atomic<bool> stop{false};

    ~ecuio_socketcan_transport()
    {
        if (fd >= 0) {
            close(fd);
        }
        if (wake_fd >= 0) {
            close(wake_fd);
        }
    }
};

namespace ecuio_socketcan {

/* The link again, judged as attach judged it; its socket, or -1 */
int reopen(const ecuio_socketcan_transport* t);
/* Waits up to ms for the stop; true when it came */
bool wait_for_stop(ecuio_socketcan_transport* t, uint32_t ms);
/* A requesting session's filter on fd: the hearing modules' IDs, none
 * passing anything else (an empty list passes nothing); false when the
 * kernel refuses it */
bool set_channel_filters(const ecuio_socketcan_transport* t, int fd,
                         uint32_t hearing);
/* A requesting session's worker, until the stop (socketcan_request.cpp) */
void request_worker(ecuio_socketcan_transport* t);

} // namespace ecuio_socketcan

#endif /* ECUIO_HAVE_SOCKETCAN */

#endif /* ECUIO_SOCKETCAN_TRANSPORT_H */
