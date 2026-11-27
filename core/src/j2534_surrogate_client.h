// File/layer: core/src/j2534_surrogate_client.h | C++17, Qt-free, core-internal,
//             Win32 only
// Testing: HARNESS: every test_j2534_* binary; Cross-arch
//
// The surrogate client's handle and its transaction helpers, shared by
// j2534_surrogate_client.cpp (open and its failures), j2534_surrogate_io.cpp
// (the pipe and the session log lines) and j2534_surrogate_ops.cpp (the
// calls after open and close).

#ifndef ECUIO_J2534_SURROGATE_CLIENT_H
#define ECUIO_J2534_SURROGATE_CLIENT_H

#ifdef _WIN32

#include <windows.h>

#include "ecuio/j2534_ptypes.h"
#include "ecuio/j2534_surrogate.h"
#include "ecuio/surrogate_protocol.h"
#include "j2534_sur_reply.h"

namespace {
constexpr DWORD kTransactTimeoutMs = 3000;
}

struct ecuio_j2534_sur_handle {
    /* A transaction's deadline: kTransactTimeoutMs but under
     * ecuio_j2534_sur_set_transact_ms */
    DWORD transact_ms = kTransactTimeoutMs;
    bool    pipe_connected = false;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    /* Manual-reset event for every overlapped pipe operation; one is enough
     * because one thread uses a handle at a time and each operation ends
     * (or is canceled and drained) before the next. */
    HANDLE io_event = nullptr;
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    /* The session log: the child's stdout/stderr, and the client's own
     * lines for this session. log_path is UTF-8 (up to three bytes per
     * path character), "" when there is none. */
    HANDLE log = INVALID_HANDLE_VALUE;
    char log_path[3 * (MAX_PATH + 64)] = {};
    uint32_t next_request_id = 1;
    uint32_t pending_response_id = 0;   /* response in flight from a
                                           timed-out transaction */
    /* The child missed a transaction's deadline, then the wait for its late
     * reply, which a slow vendor answers inside: wedged. Every transaction
     * now fails at once (a teardown would otherwise pay a timeout each) and
     * close kills the child without the polite shutdown. */
    bool unresponsive = false;
    /* An OP_READ's frames, dispensed one per read_msg. Worker-thread-only. */
    sur_batch batch;
};

namespace ecuio_sur_client {

void sur_log(const ecuio_j2534_sur_handle_t* h, const char* fmt, ...);

/* One request and its reply (the definition says how) */
bool transact(ecuio_j2534_sur_handle_t* h, const ecuio_sur_request_t& req,
              ecuio_sur_response_t* out_resp, DWORD timeout_ms,
              ecuio_pt_rx_t* frames = nullptr, const uint8_t* tail = nullptr,
              uint32_t tail_len = 0);

/* The vendor's text for its last failed call, "" when there is none */
void fetch_vendor_text(ecuio_j2534_sur_handle_t* h, char* out);

} // namespace ecuio_sur_client

#define SUR_LOG(h, ...) ecuio_sur_client::sur_log((h), __VA_ARGS__)

#endif /* _WIN32 */

#endif /* ECUIO_J2534_SURROGATE_CLIENT_H */
