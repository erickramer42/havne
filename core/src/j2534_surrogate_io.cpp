// File/layer: core/src/j2534_surrogate_io.cpp | C++17, Qt-free, Win32-guarded:
//             the pipe I/O, transact, the client log lines
// Testing: UNIT: CoreTest (test_core); HARNESS: every test_j2534_* binary;
//          Cross-arch

#include "ecuio/core.h"
#include "ecuio/j2534.h"
#include "ecuio/j2534_ptypes.h"
#include "ecuio/surrogate_protocol.h"
#include "ecuio/sur_fingerprint.h"

#include <cstring>
#include <cstdint>
#include <new>
#include <cstdlib>

#ifdef _WIN32

#include <windows.h>
#include <bcrypt.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

#include "win_utf8.h"
#include "error_detail.h"
#include "j2534_sur_reply.h"
#include "j2534_surrogate_client.h"
#include "j2534_surrogate_log.h"

namespace {

bool ov_write(HANDLE pipe, HANDLE event, const void* buf, DWORD len,
              DWORD timeout_ms)
{
    OVERLAPPED ov = {};
    ov.hEvent = event;
    DWORD written = 0;
    const BOOL ok = WriteFile(pipe, buf, len, &written, &ov);
    bool res = false;
    if (ok) {
        res = (written == len);
    } else if (GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeout_ms) == WAIT_OBJECT_0) {
            DWORD done = 0;
            if (GetOverlappedResult(pipe, &ov, &done, FALSE)) {
                res = (done == len);
            }
        } else {
            CancelIoEx(pipe, &ov);
            DWORD done = 0;
            GetOverlappedResult(pipe, &ov, &done, TRUE);
        }
    }
    return res;
}

enum read_result { RD_OK, RD_SHORT_READ, RD_TIMEOUT, RD_OS_ERROR };

/* One overlapped ReadFile of up to len bytes; *out_got includes a canceled
 * read's. */
read_result ov_read(HANDLE pipe, HANDLE event, void* buf, DWORD len,
                    DWORD timeout_ms, DWORD* out_got, DWORD* out_err)
{
    *out_got = 0;
    *out_err = 0;
    OVERLAPPED ov = {};
    ov.hEvent = event;
    DWORD nread = 0;
    const BOOL ok = ReadFile(pipe, buf, len, &nread, &ov);
    read_result res = RD_OK;
    if (ok) {
        *out_got = nread;
        res = (nread == len) ? RD_OK : RD_SHORT_READ;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        const DWORD wr = WaitForSingleObject(ov.hEvent, timeout_ms);
        if (wr == WAIT_OBJECT_0) {
            DWORD done = 0;
            if (GetOverlappedResult(pipe, &ov, &done, FALSE)) {
                *out_got = done;
                res = (done == len) ? RD_OK : RD_SHORT_READ;
            } else {
                *out_err = GetLastError();
                res = RD_OS_ERROR;
            }
        } else if (wr == WAIT_TIMEOUT) {
            CancelIoEx(pipe, &ov);
            DWORD done = 0;
            GetOverlappedResult(pipe, &ov, &done, TRUE);
            *out_got = done;
            res = RD_TIMEOUT;
        } else {
            *out_err = GetLastError();
            res = RD_OS_ERROR;
        }
    } else {
        *out_err = GetLastError();
        res = RD_OS_ERROR;
    }
    return res;
}

/* Read exactly len bytes within timeout_ms (a byte-mode pipe may deliver a
 * reply in pieces). RD_TIMEOUT: nothing arrived, the reply may still come
 * whole later; RD_SHORT_READ: cut off part way, the pipe is out of step. */
read_result read_exact(ecuio_j2534_sur_handle_t* h, void* buf, DWORD len,
                       DWORD timeout_ms, DWORD* out_got, DWORD* out_err)
{
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    DWORD total = 0;
    *out_got = 0;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        const DWORD left = (now < deadline)
                           ? static_cast<DWORD>(deadline - now) : 0;
        DWORD got = 0;
        const read_result rr = ov_read(h->pipe, h->io_event,
                                       static_cast<char*>(buf) + total,
                                       len - total, left, &got, out_err);
        total += got;
        *out_got = total;
        if (rr == RD_OK || rr == RD_OS_ERROR) {
            return rr;
        }
        if (total == 0 && rr == RD_TIMEOUT) {
            return RD_TIMEOUT;
        }
        if (got == 0 || left == 0) {
            return RD_SHORT_READ;
        }
    }
}

/* read_exact as j2534_sur_reply.h's read: got, want and err keep the
 * last read's for the log */
struct pipe_read {
    ecuio_j2534_sur_handle_t* h;
    DWORD timeout_ms;
    DWORD got = 0;
    DWORD want = 0;
    DWORD err = 0;
    sur_piece operator()(void* buf, size_t len)
    {
        want = static_cast<DWORD>(len);
        switch (read_exact(h, buf, want, timeout_ms, &got, &err)) {
        case RD_OK:
            return sur_piece::whole;
        case RD_TIMEOUT:
            return sur_piece::timeout;
        case RD_SHORT_READ:
            return sur_piece::cut;
        case RD_OS_ERROR:
            break;
        }
        return sur_piece::failed;
    }
};

/* Read and discard the late reply to a timed-out request, or every later
 * call fails the ID check. If it never arrives the child is hung: mark the
 * handle unresponsive and send nothing new.
 *
 * An OP_READ's late frames are dropped with it (rare: the child answered
 * over 500 ms past its own timeout); the drop is logged. */
bool drain_pending(ecuio_j2534_sur_handle_t* h, DWORD timeout_ms)
{
    ecuio_sur_response_t stale = {};
    ecuio_pt_rx_t scratch;
    pipe_read read{h, timeout_ms};
    switch (sur_drop_reply(read, h->pending_response_id, &stale, &scratch)) {
    case sur_reply::whole:
        if (stale.num_frames > 0) {
            SUR_LOG(h,
                    "drain_pending: dropped %u frame(s) from the late "
                    "reply to req_id=%u\n",
                    stale.num_frames, stale.request_id);
        }
        return true;
    case sur_reply::timeout:
    case sur_reply::failed:
    case sur_reply::cut:
        SUR_LOG(h, "drain_pending: stale response id=%u never arrived; "
                "child unresponsive, later requests fail at once\n",
                h->pending_response_id);
        break;
    case sur_reply::frames_unasked:
    case sur_reply::frames_cut:
        SUR_LOG(h, "drain_pending: the late reply's %u frame(s) didn't "
                "arrive whole; child unresponsive\n", stale.num_frames);
        break;
    case sur_reply::wrong_id:
        SUR_LOG(h, "drain_pending: unexpected id=%u want=%u; the pipe is "
                "out of step, child unresponsive\n",
                stale.request_id, h->pending_response_id);
        break;
    }
    h->unresponsive = true;
    return false;
}

/* A reply transact refused, in the session log */
void log_refused_reply(const ecuio_j2534_sur_handle_t* h,
                       const ecuio_sur_request_t& req,
                       const ecuio_sur_response_t& resp, sur_reply r,
                       const pipe_read& read)
{
    switch (r) {
    case sur_reply::whole:
        break;
    case sur_reply::timeout:
    case sur_reply::failed:
    case sur_reply::cut:
        SUR_LOG(h,
                "transact: ov_read failed (req_id=%u op=%u %s "
                "got=%lu/%zu err=%lu)\n",
                req.request_id, req.opcode,
                r == sur_reply::cut       ? "SHORT-READ"
                : r == sur_reply::timeout ? "TIMEOUT"
                                          : "OS-ERROR",
                read.got, sizeof(resp), read.err);
        break;
    case sur_reply::frames_unasked:
        SUR_LOG(h,
                "transact: %u frame(s) on the reply to op=%u "
                "(skew?); child unresponsive\n",
                resp.num_frames, req.opcode);
        break;
    case sur_reply::frames_cut:
        SUR_LOG(h,
                "transact: the reply's frames didn't arrive whole "
                "(req_id=%u got=%lu/%lu err=%lu); child unresponsive\n",
                req.request_id, read.got, read.want, read.err);
        break;
    case sur_reply::wrong_id:
        /* This request's reply is still in the pipe, so every later read
         * would be one reply behind */
        SUR_LOG(h,
                "transact: DESYNC (got req_id=%u want=%u); child "
                "unresponsive\n",
                resp.request_id, req.request_id);
        break;
    }
}

} // namespace

namespace ecuio_sur_client {

/* Client trace line to stderr and the session log (a GUI launch has no
 * stderr), local-time stamped like the child's. One WriteFile per line so
 * the two sides never splice mid-line. h may be null. */
void sur_log(const ecuio_j2534_sur_handle_t* h, const char* fmt, ...)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[512];
    const int n = snprintf(line, sizeof(line) - 2, "[sur %02u:%02u:%02u.%03u] ",
                           static_cast<unsigned>(t.wHour),
                           static_cast<unsigned>(t.wMinute),
                           static_cast<unsigned>(t.wSecond),
                           static_cast<unsigned>(t.wMilliseconds));
    if (n < 0) return;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line + n, sizeof(line) - 2 - static_cast<size_t>(n),
                    fmt, ap);
    va_end(ap);
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) --len;
    line[len] = '\0';
    fprintf(stderr, "%s\n", line);
    if (h != nullptr && h->log != INVALID_HANDLE_VALUE) {
        line[len++] = '\r';
        line[len++] = '\n';
        DWORD written = 0;
        (void)WriteFile(h->log, line, static_cast<DWORD>(len), &written, nullptr);
    }
}

/* One request and its reply. frames receives a READ's messages (room for
 * ECUIO_SUR_MAX_FRAMES_PER_READ); other opcodes pass none, so frames on
 * their reply mean a skewed child. A reply out of step
 * (sur_reply_out_of_step) turns the handle unresponsive; one that timed
 * out is drained by the next transaction. */
bool transact(ecuio_j2534_sur_handle_t* h, const ecuio_sur_request_t& req,
              ecuio_sur_response_t* out_resp, DWORD timeout_ms,
              ecuio_pt_rx_t* frames)
{
    if (h->unresponsive) {
        return false;
    }
    if (h->pending_response_id != 0) {
        if (!drain_pending(h, timeout_ms)) {
            return false;
        }
        h->pending_response_id = 0;
    }
    if (!ov_write(h->pipe, h->io_event, &req, sizeof(req), timeout_ms)) {
        SUR_LOG(h, "transact: ov_write failed (req_id=%u op=%u err=%lu)\n",
                req.request_id, req.opcode, GetLastError());
        return false;
    }
    pipe_read read{h, timeout_ms};
    const sur_reply r = sur_take_reply(read, req.request_id, out_resp, frames);
    if (r == sur_reply::timeout) {
        h->pending_response_id = req.request_id;
    }
    if (sur_reply_out_of_step(r)) {
        h->unresponsive = true;
    }
    log_refused_reply(h, req, *out_resp, r, read);
    return r == sur_reply::whole;
}

/* The vendor's description of its last failed call, through
 * OP_GET_LAST_ERROR, into out (ECUIO_PT_TEXT_LEN + 1 bytes); "" when
 * the DLL exports no PassThruGetLastError or the child can't answer. */
void fetch_vendor_text(ecuio_j2534_sur_handle_t* h, char* out)
{
    out[0] = '\0';
    ecuio_sur_request_t req = {};
    req.request_id = h->next_request_id++;
    req.opcode = ECUIO_SUR_OP_GET_LAST_ERROR;
    ecuio_sur_response_t resp = {};
    if (!transact(h, req, &resp, h->transact_ms) ||
        resp.status != static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        return;
    }
    sur_reply_text(resp, 0, out);
    out[ECUIO_PT_TEXT_LEN] = '\0';
}

} // namespace ecuio_sur_client

#endif /* _WIN32 */
