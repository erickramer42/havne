// File/layer: core/include/ecuio/recording.h | C ABI, public interface
// Testing: HARNESS: RecorderTest (test_core): the writer against the mock
//          and the replay fixture, the seams in internal.h; UNIT
//          (test_c_abi): the header as strict C and the structs' layout
#ifndef ECUIO_RECORDING_H
#define ECUIO_RECORDING_H
#include "ecuio/core.h" /* ecuio_status_t, ecuio_session_t */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Recordings (ADR-0049): a recorder is a handle of its own, opened by the
 * caller once ecuio_session_create has succeeded and attached to the
 * session before ecuio_session_start. The session's worker copies each
 * frame and its decoded values into a ring; the recorder's writer thread
 * drains it into one folder: frames.csv, signals.csv, events.log,
 * latency.csv when the session times requests, markers.csv once marked.
 * A full ring drops records and counts them; it never blocks the worker.
 *
 * The recorder outlives its session: ecuio_session_destroy detaches it,
 * and notes and marks are taken until ecuio_recorder_close, so a
 * recording brackets the caller's whole teardown.
 */

typedef struct ecuio_recorder ecuio_recorder_t; /* opaque */

/* Options for ecuio_recorder_open. Pointers last (as ecuio_value_t has
 * them), so every other offset is the same on every pointer width. */
typedef struct ecuio_record_options {
    uint32_t ring_bytes; /* 0 for the default, 4 MiB */
    uint32_t note_lines; /* 0 for the default, 256 */
    uint32_t header_line_count;
    uint32_t flags;                  /* zero */
    uint64_t reserved[4];            /* zero; ABI growth space */
    const char* root_utf8;           /* the folder recordings go in */
    const char* label_utf8;          /* NULL or "" for none */
    const char* const* header_lines; /* the caller's header lines */
} ecuio_record_options_t;

#if defined(__cplusplus)
static_assert(offsetof(ecuio_record_options_t, reserved) == 16,
              "record options reserved offset is ABI");
static_assert(offsetof(ecuio_record_options_t, root_utf8) == 48,
              "record options root offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ecuio_record_options_t, reserved) == 16,
               "record options reserved offset is ABI");
_Static_assert(offsetof(ecuio_record_options_t, root_utf8) == 48,
               "record options root offset is ABI");
#endif

/* ecuio_recorder_state_t.state; append-only */
#define ECUIO_RECORDING_ACTIVE   1u /* writing */
#define ECUIO_RECORDING_DETACHED 2u /* its session destroyed; notes go on */
#define ECUIO_RECORDING_FAILED   3u /* a write failed; reason says why */

#define ECUIO_RECORDING_REASON_LEN 128

typedef struct ecuio_recorder_state {
    uint32_t state;                          /* ECUIO_RECORDING_* */
    uint32_t flags;                          /* zero */
    uint64_t frame_rows;                     /* rows written to frames.csv */
    uint64_t value_rows;                     /* rows written to signals.csv */
    uint64_t records_dropped;                /* the ring was full */
    uint64_t notes_dropped;                  /* the note queue was full */
    char reason[ECUIO_RECORDING_REASON_LEN]; /* the OS's reason for
                                                      FAILED, no path */
    uint64_t reserved[4];                    /* zero; ABI growth space */
} ecuio_recorder_state_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_recorder_state_t) == 200,
              "ecuio_recorder_state_t size is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_recorder_state_t) == 200,
               "ecuio_recorder_state_t size is ABI");
#endif

/*
 * Creates <root>/<yyyyMMdd-HHmmss>[-n][ <label>]/ (the label reduced to
 * ASCII letters and digits, space, '-', '_', '.' and any character past
 * ASCII, runs of spaces made one, at most 40 characters, with no dot or
 * space at its end, which Windows refuses; the next -n when the name is
 * taken, never checked first), creates frames.csv, signals.csv and
 * events.log in it and starts the writer thread. Nothing is written
 * into them until a session attaches. *out is
 * NULL on every failure. ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL
 * options, root or out; ECUIO_ERR_INVALID_ARGUMENT for nonzero flags or
 * reserved; ECUIO_ERR_RECORDING_FAILED when the folder can't be created,
 * with ecuio_last_error_detail naming the cause, never the path.
 */
ecuio_status_t ecuio_recorder_open(const ecuio_record_options_t* options,
                                   ecuio_recorder_t** out);

/*
 * Attaches recorder to session, after create and before start: copies
 * the session's defs (name, unit, kind, decimal places, plot rank),
 * writes events.log's header and sets the recording's zero.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL session or recorder;
 * ECUIO_ERR_SESSION_CLOSED for a closed session;
 * ECUIO_ERR_INVALID_ARGUMENT for a started session, a recorder that has
 * had a session, or a session that has a recorder;
 * ECUIO_ERR_OUT_OF_MEMORY when the defs' copy can't be made.
 */
ecuio_status_t ecuio_session_record(ecuio_session_t* session,
                                    ecuio_recorder_t* recorder);

/*
 * A note in events.log, stamped when called. Safe from any thread until
 * close; every caller's last call returns before ecuio_recorder_close.
 * ECUIO_OK once queued; ECUIO_ERR_INVALID_ARGUMENT before a session first
 * attaches; ECUIO_ERR_RECORDING_FAILED after a write failed;
 * ECUIO_ERR_NULL_ARGUMENT for a NULL recorder or line. A newline in the
 * line is written as a space; past 1 KB it is cut at a character
 * boundary. A full queue drops it and counts it.
 */
ecuio_status_t ecuio_recorder_note(ecuio_recorder_t* recorder,
                                   const char* utf8_line);

/* A marker in markers.csv; statuses as ecuio_recorder_note's */
ecuio_status_t ecuio_recorder_mark(ecuio_recorder_t* recorder,
                                   const char* utf8_label);

/* Safe from any thread until close */
ecuio_status_t ecuio_recorder_get_state(const ecuio_recorder_t* recorder,
                                        ecuio_recorder_state_t* out);

/* The folder, UTF-8 and absolute, copied terminated and cut to capacity;
 * *out_len is its full length, so a caller spots a cut by comparing.
 * Safe from any thread until close. */
ecuio_status_t ecuio_recorder_get_folder(const ecuio_recorder_t* recorder,
                                         char* utf8, uint32_t capacity,
                                         uint32_t* out_len);

/*
 * Detaches a session still attached, writes the summary (utf8_why first,
 * a line as a note's) and the files' last rows, closes them, stops the
 * writer and frees the handle, whatever it returns: ECUIO_OK when every
 * file ended whole, ECUIO_ERR_RECORDING_FAILED when a write had failed.
 * NULL is a safe no-op returning ECUIO_OK.
 */
ecuio_status_t ecuio_recorder_close(ecuio_recorder_t* recorder,
                                    const char* utf8_why);

/* The reader (steps 3 and 4) and the export (v0.6.0) join this header
 * later; ADR-0049 sections 5, 6 and 8. */

#ifdef __cplusplus
}
#endif
#endif /* ECUIO_RECORDING_H */
