// File/layer: core/src/j2534_sur_reply.h | internal, not ABI-exported; the
//             surrogate client's reading of the child's replies, pure and
//             header-only: no clock, no I/O and no platform headers, so
//             fuzz_sur_reply runs the client's own checks on every platform
// Testing: HARNESS: J2534SurrogateTest (test_j2534_surrogate); FUZZ:
//          fuzz_sur_reply
//
// The pipe is a byte stream from another process, trusted only as far as
// these checks. A reply is one ecuio_sur_response_t, then, on a READ's
// alone, num_frames whole ecuio_pt_rx_t (surrogate_protocol.h). The
// caller hands in its read, which fills a buffer exactly or says why
// not, and decides what a verdict costs the handle: a timeout leaves a
// reply to drain, and a verdict out of step ends the handle.

#ifndef ECUIO_J2534_SUR_REPLY_H
#define ECUIO_J2534_SUR_REPLY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ecuio/core.h"
#include "ecuio/surrogate_protocol.h"

/* What one exact read did */
enum class sur_piece {
    whole,   /* every byte asked for */
    timeout, /* nothing arrived: the reply may still come whole */
    cut,     /* part of it, then nothing */
    failed,  /* the pipe failed */
};

/* One reply's verdict */
enum class sur_reply {
    whole,
    timeout,        /* nothing of it arrived */
    failed,         /* the pipe failed */
    cut,            /* the response cut off part way */
    frames_unasked, /* frames on a reply that takes none, or past the cap */
    frames_cut,     /* its frames cut off part way */
    wrong_id,       /* another request's reply */
};

/* A verdict after which every later reply would be misread */
inline bool sur_reply_out_of_step(sur_reply r)
{
    return r == sur_reply::cut || r == sur_reply::frames_unasked ||
           r == sur_reply::frames_cut || r == sur_reply::wrong_id;
}

inline sur_reply sur_reply_of(sur_piece p)
{
    switch (p) {
    case sur_piece::whole:
        return sur_reply::whole;
    case sur_piece::timeout:
        return sur_reply::timeout;
    case sur_piece::cut:
        return sur_reply::cut;
    case sur_piece::failed:
        break;
    }
    return sur_reply::failed;
}

/* One reply to want_id: the response, then its frames into frames (room
 * for ECUIO_SUR_MAX_FRAMES_PER_READ; nullptr for an opcode that takes
 * none). read(void* buf, size_t len) returns a sur_piece. A count is
 * judged before its frames are read, so a skewed one is never read past,
 * and the ID after them, so another request's reply is taken whole. */
template <class Read>
sur_reply sur_take_reply(Read&& read, uint32_t want_id,
                         ecuio_sur_response_t* resp, ecuio_pt_rx_t* frames)
{
    const sur_reply head = sur_reply_of(read(resp, sizeof(*resp)));
    if (head != sur_reply::whole) {
        return head;
    }
    if (resp->num_frames > 0) {
        if (frames == nullptr ||
            resp->num_frames > ECUIO_SUR_MAX_FRAMES_PER_READ) {
            return sur_reply::frames_unasked;
        }
        if (read(frames, resp->num_frames * sizeof(ecuio_pt_rx_t)) !=
            sur_piece::whole) {
            return sur_reply::frames_cut;
        }
    }
    return resp->request_id == want_id ? sur_reply::whole : sur_reply::wrong_id;
}

/* A late reply to want_id, read and dropped: any opcode's, so frames up
 * to the cap are taken, one at a time into scratch */
template <class Read>
sur_reply sur_drop_reply(Read&& read, uint32_t want_id,
                         ecuio_sur_response_t* stale, ecuio_pt_rx_t* scratch)
{
    const sur_reply head = sur_reply_of(read(stale, sizeof(*stale)));
    if (head != sur_reply::whole) {
        return head;
    }
    if (stale->num_frames > ECUIO_SUR_MAX_FRAMES_PER_READ) {
        return sur_reply::frames_unasked;
    }
    for (uint32_t i = 0; i < stale->num_frames; ++i) {
        if (read(scratch, sizeof(*scratch)) != sur_piece::whole) {
            return sur_reply::frames_cut;
        }
    }
    return stale->request_id == want_id ? sur_reply::whole
                                        : sur_reply::wrong_id;
}

/* The frames of the last READ, handed out one a call. A loss reported
 * with no frame after it marks the next one handed out. */
struct sur_batch {
    ecuio_pt_rx_t frames[ECUIO_SUR_MAX_FRAMES_PER_READ];
    uint32_t count = 0;
    uint32_t next = 0;
    bool loss_pending = false;
};

inline bool sur_batch_take(sur_batch* b, ecuio_pt_rx_t* out)
{
    if (b->next >= b->count) {
        return false;
    }
    *out = b->frames[b->next++];
    if (b->loss_pending) {
        out->rx_status |= ECUIO_PT_BUFFER_OVERFLOW;
        b->loss_pending = false;
    }
    return true;
}

/* A READ that brought no frame, judged by its vendor status (the
 * child's count-trust rule already applied): ERR_TIMEOUT a quiet
 * vehicle, ERR_BUFFER_EMPTY or NOERROR nothing yet, ERR_BUFFER_OVERFLOW
 * frames lost (the next frame marked, the link unchanged), anything
 * else a hard failure */
inline ecuio_status_t sur_empty_read(sur_batch* b, int32_t status)
{
    if (status == static_cast<int32_t>(ECUIO_PT_STATUS_BUFFER_OVERFLOW)) {
        b->loss_pending = true;
        return ECUIO_OK;
    }
    if (status == static_cast<int32_t>(ECUIO_PT_STATUS_TIMEOUT)) {
        return ECUIO_ERR_VEHICLE_TIMEOUT;
    }
    if (status == static_cast<int32_t>(ECUIO_PT_STATUS_BUFFER_EMPTY) ||
        status == static_cast<int32_t>(ECUIO_PT_STATUS_NOERROR)) {
        return ECUIO_OK;
    }
    return ECUIO_ERR_TRANSPORT_HARD_FAILURE;
}

/* One message for the caller: the batch's next, else one READ through
 * transact(ecuio_sur_response_t*, ecuio_pt_rx_t* frames), false when the
 * transaction failed. Frames are delivered before the status is judged:
 * a conforming vendor pairs ERR_TIMEOUT with the messages that came. */
template <class Transact>
ecuio_status_t sur_read_msg(sur_batch* b, Transact&& transact,
                            ecuio_pt_rx_t* out, bool* got)
{
    *got = sur_batch_take(b, out);
    if (*got) {
        return ECUIO_OK;
    }
    ecuio_sur_response_t resp = {};
    if (!transact(&resp, b->frames)) {
        return ECUIO_ERR_TRANSPORT_HARD_FAILURE;
    }
    if (resp.num_frames > 0) {
        b->count = resp.num_frames;
        b->next = 0;
        *got = sur_batch_take(b, out);
        return ECUIO_OK;
    }
    return sur_empty_read(b, resp.status);
}

/* An IOCTL reply's payload: at most output_cap bytes into output, the
 * whole length in *out_len, so the caller sees a cut. False, nothing
 * copied, for a length past what the response holds. */
inline bool sur_ioctl_payload(const ecuio_sur_response_t& r, uint8_t* output,
                              uint32_t output_cap, uint32_t* out_len)
{
    if (r.data_len > ECUIO_SUR_IOCTL_DATA_MAX) {
        return false;
    }
    if (output != nullptr && r.data_len > 0) {
        memcpy(output, r.data,
               (r.data_len < output_cap) ? r.data_len : output_cap);
    }
    if (out_len != nullptr) {
        *out_len = r.data_len;
    }
    return true;
}

/* A reply's text field into out (ECUIO_SUR_TEXT_MAX bytes), terminated
 * whatever the child sent */
inline void sur_reply_text(const ecuio_sur_response_t& r, int field, char* out)
{
    memcpy(out, r.text[field], ECUIO_SUR_TEXT_MAX);
    out[ECUIO_SUR_TEXT_MAX - 1] = '\0';
}

#endif /* ECUIO_J2534_SUR_REPLY_H */
