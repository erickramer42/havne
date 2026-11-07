// File/layer: core/include/ecuio/queue.h | C ABI, public interface
// Testing: UNIT: QueueTest (test_core); c_abi, c_abi_shared

#ifndef ECUIO_QUEUE_H
#define ECUIO_QUEUE_H

#include "ecuio/core.h"    /* ecuio_status_t, ecuio_session_t, ecuio_frame_t */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The pull queue (ADR-0053 section 4 and its appendix "the pull queue as
 * built"): a session's frames or decoded values, waiting for a caller to
 * read them on a thread of its own, so a program that can't take a
 * callback on the worker (Python's ctypes, which would hold the
 * interpreter's lock there; Rust, Go and C# the same) never runs on it.
 * Core's own handler copies each frame or value into the queue on the
 * worker and returns.
 *
 * One kind per queue: ECUIO_QUEUE_FRAMES, read with
 * ecuio_queue_read_frames, or ECUIO_QUEUE_VALUES, read with
 * ecuio_queue_read_values; a program wanting both opens two.
 *
 * The bound: a queue holds `capacity` entries, set at open. A full queue
 * drops its oldest entry for the new one and counts it; the count is the
 * running total since open, which each read reports. Entries are single
 * values, not frames: a frame's values share timestamp_us and
 * arbitration_id, so a full values queue can drop some of one frame's
 * values and keep the rest, and `dropped` counts values. A program that
 * needs every frame records the session (recording.h) and reads the
 * recording.
 *
 * Lifetime (ADR-0002's ordering): open before ecuio_session_start. The
 * worker copies into the queue until ecuio_session_close, which stops
 * the worker first and then detaches every queue still open, waking its
 * readers: what's waiting is still read, and then a read returns
 * ECUIO_ERR_SESSION_CLOSED. ecuio_queue_close frees the queue, before or
 * after its session is closed or destroyed; a read under way on another
 * thread returns first. A closed queue keeps its place until the session
 * is freed: it still counts toward ECUIO_QUEUE_MAX_PER_SESSION, and a
 * closed values queue still has the worker decode each frame.
 *
 * The stream's end: a replay that doesn't loop ends its queues once its
 * last pass is over (or it stops early, its file changed): what's waiting
 * is still read, and then a read returns ECUIO_ERR_STREAM_ENDED. Only a
 * replay ends a stream; a looping replay never does, and a live
 * transport's queues end only at the session's close. Reading or closing a queue after
 * ecuio_queue_close is undefined, as for every handle (core.h's
 * lifecycle). Two threads may read one queue; each entry goes to one of
 * them.
 */

typedef struct ecuio_queue ecuio_queue_t;

/* The kinds (append-only) */
#define ECUIO_QUEUE_FRAMES 1u
#define ECUIO_QUEUE_VALUES 2u

/* Capacity: 1 to these, in entries. A frame entry holds a whole
 * ecuio_frame_t (4120 bytes), so a frames queue at its ceiling holds
 * about 17 MB. */
#define ECUIO_QUEUE_MAX_FRAMES 4096u
#define ECUIO_QUEUE_MAX_VALUES 65536u

/* Queues one session takes */
#define ECUIO_QUEUE_MAX_PER_SESSION 4u

/* A value entry's text: up to 127 bytes and a NUL */
#define ECUIO_QUEUE_TEXT_MAX 128u

/* ecuio_queue_value_t flags */
#define ECUIO_QUEUE_VALUE_TEXT_CUT 0x1u   /* the value's text was longer than
                                             text holds: cut at 127 bytes */

/* One decoded value (profile.h's ecuio_value_t), with its frame's time
 * and ID and its text copied in, so it outlives the worker's call. No
 * pointer: the same layout on 32- and 64-bit builds. */
typedef struct ecuio_queue_value {
    uint64_t timestamp_us;      /* the frame's session time */
    uint32_t arbitration_id;    /* the frame's */
    uint16_t signal_id;         /* the def's id */
    uint8_t  kind;              /* the def's ECUIO_VALUE_* (signals.h) */
    uint8_t  flags;             /* ECUIO_QUEUE_VALUE_* */
    double   value;             /* as ecuio_value_t's: an ENUM's raw
                                   number, 0 for text */
    uint32_t text_len;          /* the bytes in text, the NUL not
                                   counted; 0 where the value has none */
    uint32_t reserved;          /* 0 */
    char     text[ECUIO_QUEUE_TEXT_MAX];  /* TEXT and HEX: the text; a
                                   labeled ENUM: its label; else "".
                                   NUL-terminated */
} ecuio_queue_value_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_queue_value_t) == 160,
              "ecuio_queue_value_t size is ABI");
static_assert(offsetof(ecuio_queue_value_t, value) == 16,
              "queue value offset is ABI");
static_assert(offsetof(ecuio_queue_value_t, text) == 32,
              "queue value text offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_queue_value_t) == 160,
               "ecuio_queue_value_t size is ABI");
_Static_assert(offsetof(ecuio_queue_value_t, value) == 16,
               "queue value offset is ABI");
_Static_assert(offsetof(ecuio_queue_value_t, text) == 32,
               "queue value text offset is ABI");
#endif

/* Attaches a queue of `kind` holding up to `capacity` entries to a
 * session not yet started; *out NULL first. ECUIO_OK;
 * ECUIO_ERR_NULL_ARGUMENT for a NULL session or out;
 * ECUIO_ERR_SESSION_CLOSED on a closed session; ECUIO_ERR_INVALID_ARGUMENT
 * for an unknown kind, a capacity of 0 or past its kind's ceiling, a
 * session already started, or one holding ECUIO_QUEUE_MAX_PER_SESSION
 * queues (a closed one among them); ECUIO_ERR_OUT_OF_MEMORY when the entries can't be allocated. */
ecuio_status_t ecuio_queue_open(ecuio_session_t* session, uint32_t kind,
                                uint32_t capacity, ecuio_queue_t** out);

/* Copies up to `capacity` waiting entries into out, oldest first, and
 * sets *count to how many; *dropped gets the entries dropped since open.
 * With none waiting it waits up to timeout_ms (0: not at all) for the
 * first; ECUIO_OK with *count 0 when none came. Once the stream has
 * ended and nothing is left: ECUIO_ERR_STREAM_ENDED; once the session has
 * closed (and the stream hadn't ended): ECUIO_ERR_SESSION_CLOSED; *count
 * 0 either way.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL queue, out, count or dropped;
 * ECUIO_ERR_INVALID_ARGUMENT for a capacity of 0, or the other kind's
 * queue. A frame's bytes past its data_length are zero. */
ecuio_status_t ecuio_queue_read_frames(ecuio_queue_t* queue, ecuio_frame_t* out,
                                       uint32_t capacity, uint32_t* count,
                                       uint64_t* dropped, uint32_t timeout_ms);
ecuio_status_t ecuio_queue_read_values(ecuio_queue_t* queue,
                                       ecuio_queue_value_t* out,
                                       uint32_t capacity, uint32_t* count,
                                       uint64_t* dropped, uint32_t timeout_ms);

/* Detaches the queue from its session if it's still attached, ends any
 * read under way (it returns what it took, or ECUIO_ERR_SESSION_CLOSED),
 * and frees it. NULL is a no-op. */
void ecuio_queue_close(ecuio_queue_t* queue);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_QUEUE_H */
