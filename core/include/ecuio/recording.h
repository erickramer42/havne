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

/*
 * The reader (ADR-0049 section 5): a recording opened offline from its
 * folder. One pass at open reads events.log's header and signals.csv and
 * builds, per signal, its range and a summary for each second it has
 * samples in, beside each second's byte offset in the file; queries
 * answer from those and, zoomed in closer than a second a column, from
 * the raw rows of just the seconds they need. Once open, every call but
 * close is safe from several threads at once on one handle.
 */

typedef struct ecuio_recording ecuio_recording_t; /* opaque */

/* "Nothing found": ecuio_recording_find and _text_at's *at_us, a
 * signal's first_us and last_us with no samples */
#define ECUIO_RECORDING_NONE UINT64_MAX

/* ecuio_recording_info_t.flags */
#define ECUIO_RECORDING_INFO_NO_SUMMARY 0x1u /* events.log has no end line:
                                                the process crashed or the
                                                power went; drops counted
                                                from its drop lines */

/* ecuio_recording_info_t.format: events.log's header */
#define ECUIO_RECORDING_FORMAT_UNKNOWN 0u /* none, or a first line the
                                             reader doesn't know */
#define ECUIO_RECORDING_FORMAT_OLDER   1u /* ADR-0017's: kinds and decimal
                                             places inferred, no UTC */
#define ECUIO_RECORDING_FORMAT_CORE    2u /* ADR-0049's */

typedef struct ecuio_recording_info {
    uint32_t flags;           /* ECUIO_RECORDING_INFO_* */
    uint32_t format;          /* ECUIO_RECORDING_FORMAT_* */
    uint32_t signal_count;
    uint32_t marker_count;
    uint64_t duration_us;     /* the latest host_us of a sample, a marker or
                                 an events.log line */
    uint64_t utc_start_us;    /* microseconds since 1970-01-01 UTC; 0 when
                                 the header has none */
    uint64_t rows_torn;       /* last rows dropped for having no newline */
    uint64_t records_dropped; /* the writer's, from the summary */
    uint64_t notes_dropped;
    uint64_t reserved[5];     /* zero; ABI growth space */
} ecuio_recording_info_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_recording_info_t) == 96,
              "ecuio_recording_info_t size is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_recording_info_t) == 96,
               "ecuio_recording_info_t size is ABI");
#endif

/* ecuio_recording_signal_t.flags */
#define ECUIO_RECORDING_SIGNAL_TEXT 0x1u /* a text kind: counts, no range */

typedef struct ecuio_recording_signal {
    char name[32];          /* as ecuio_signal_def_t holds them */
    char unit[16];
    uint32_t flags;         /* ECUIO_RECORDING_SIGNAL_* */
    uint8_t decimal_places; /* an older recording's: the most its values
                               show, at most 3 */
    uint8_t plot_rank;      /* 0 in an older recording */
    uint8_t group;          /* ECUIO_SIGNAL_GROUP_* (ADR-0050); 0 in an
                               older recording */
    uint8_t reserved0[1];   /* zero */
    uint64_t samples;
    double lowest;          /* a number's; 0 for text or no samples */
    double highest;
    uint64_t first_us;      /* host_us; ECUIO_RECORDING_NONE with none */
    uint64_t last_us;
    uint64_t reserved[4];   /* zero; ABI growth space */
} ecuio_recording_signal_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_recording_signal_t) == 128,
              "ecuio_recording_signal_t size is ABI");
static_assert(offsetof(ecuio_recording_signal_t, samples) == 56,
              "recording signal samples offset is ABI");
static_assert(offsetof(ecuio_recording_signal_t, group) == 54,
              "recording signal group offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_recording_signal_t) == 128,
               "ecuio_recording_signal_t size is ABI");
_Static_assert(offsetof(ecuio_recording_signal_t, samples) == 56,
               "recording signal samples offset is ABI");
_Static_assert(offsetof(ecuio_recording_signal_t, group) == 54,
               "recording signal group offset is ABI");
#endif

/* A marker. Pointer last, as ecuio_value_t's: the label lives in the
 * reader and stays valid until ecuio_recording_close. */
typedef struct ecuio_recording_marker {
    uint64_t host_us;
    uint32_t flags;           /* zero */
    uint32_t label_length;    /* bytes, the terminator left out */
    uint64_t reserved[2];     /* zero; ABI growth space */
    const char* label_utf8;
} ecuio_recording_marker_t;

#if defined(__cplusplus)
static_assert(offsetof(ecuio_recording_marker_t, label_utf8) == 32,
              "recording marker label offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ecuio_recording_marker_t, label_utf8) == 32,
               "recording marker label offset is ABI");
#endif

/* One column of a range query: a number's lowest and highest in it, and
 * its sample count (a text signal's count alone; 0 with none) */
typedef struct ecuio_recording_column {
    double lowest;
    double highest;
    uint32_t samples;
    uint32_t flags; /* zero */
} ecuio_recording_column_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_recording_column_t) == 24,
              "ecuio_recording_column_t size is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_recording_column_t) == 24,
               "ecuio_recording_column_t size is ABI");
#endif

/* ecuio_recording_find_t.kind */
#define ECUIO_RECORDING_FIND_THRESHOLD 1u
#define ECUIO_RECORDING_FIND_MARKER    2u
/* ecuio_recording_find_t.direction, for a threshold */
#define ECUIO_RECORDING_FIND_RISING  1u /* reaches level after being below
                                           level - hysteresis */
#define ECUIO_RECORDING_FIND_FALLING 2u /* reaches level after being above
                                           level + hysteresis */

/* What ecuio_recording_find looks for. Pointer last. */
typedef struct ecuio_recording_find {
    uint32_t kind;          /* ECUIO_RECORDING_FIND_* */
    uint32_t signal;        /* a threshold's signal index */
    uint32_t direction;     /* a threshold's ECUIO_RECORDING_FIND_RISING
                               or _FALLING */
    uint32_t flags;         /* zero */
    double level;
    double hysteresis;      /* 0 or more */
    uint64_t reserved[4];   /* zero; ABI growth space */
    const char* label_utf8; /* a marker's label, matched whole */
} ecuio_recording_find_t;

#if defined(__cplusplus)
static_assert(offsetof(ecuio_recording_find_t, label_utf8) == 64,
              "recording find label offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ecuio_recording_find_t, label_utf8) == 64,
               "recording find label offset is ABI");
#endif

/* Called on the opening thread every few megabytes; a nonzero return
 * cancels the open */
typedef int (*ecuio_open_progress_t)(uint64_t bytes_read,
                                     uint64_t bytes_total, void* user_data);

/*
 * Opens the recording in folder_utf8 (the folder holding its
 * signals.csv), reading it once. *out is NULL on every failure, which
 * leaves no file open and nothing allocated. ECUIO_OK;
 * ECUIO_ERR_NULL_ARGUMENT for a NULL folder or out; ECUIO_ERR_LOAD_FAILED
 * for a folder with no signals.csv or a file the reader refuses, with
 * ecuio_last_error_detail naming the file and line ("signals.csv line 12:
 * ..."), never the path; ECUIO_ERR_CANCELED when progress returned
 * nonzero; ECUIO_ERR_OUT_OF_MEMORY. A last row with no newline is
 * dropped and counted (rows_torn); a row earlier than the one before it
 * is refused. Rows added after the open aren't seen.
 */
ecuio_status_t ecuio_recording_open(const char* folder_utf8,
                                    ecuio_open_progress_t progress,
                                    void* user_data,
                                    ecuio_recording_t** out);

/*
 * The getters. Every reader call returns ECUIO_ERR_NULL_ARGUMENT for a
 * NULL handle or out pointer and ECUIO_ERR_INVALID_ARGUMENT for a signal
 * or marker index past the count. Signals are in the header's order,
 * then any signals.csv names that the header lacks, in the order they
 * first appear; markers in time order.
 */
ecuio_status_t ecuio_recording_get_info(const ecuio_recording_t* r,
                                        ecuio_recording_info_t* out);
ecuio_status_t ecuio_recording_get_signal(const ecuio_recording_t* r,
                                          uint32_t index,
                                          ecuio_recording_signal_t* out);
ecuio_status_t ecuio_recording_get_marker(const ecuio_recording_t* r,
                                          uint32_t index,
                                          ecuio_recording_marker_t* out);

/*
 * Fills column_count columns over [t0_us, t1_us) for each of the
 * signal_count signal indexes in signal_ids (a name Qt's "signals"
 * macro can't take): columns holds signal_count x column_count entries,
 * signal by signal. Columns of a second or more combine the
 * per-second summaries, each second in the column it starts in (the
 * first, for one that starts before t0_us); narrower ones read the raw
 * rows of the seconds they cover. ECUIO_ERR_INVALID_ARGUMENT also for
 * t0_us >= t1_us or a count of 0; ECUIO_ERR_RECORDING_CHANGED when a
 * row read again no longer parses, or the file went away.
 */
ecuio_status_t ecuio_recording_query(const ecuio_recording_t* r,
                                     const uint32_t* signal_ids,
                                     uint32_t signal_count, uint64_t t0_us,
                                     uint64_t t1_us,
                                     ecuio_recording_column_t* columns,
                                     uint32_t column_count);

/*
 * The value in force at t_us, as signals.csv holds it (the last sample
 * at or before t_us), copied terminated and cut to capacity (at least
 * 1), and its time. Before the signal's first sample: an empty string and
 * *at_us ECUIO_RECORDING_NONE, with ECUIO_OK. ECUIO_ERR_RECORDING_CHANGED
 * as for a query.
 */
ecuio_status_t ecuio_recording_text_at(const ecuio_recording_t* r,
                                       uint32_t signal, uint64_t t_us,
                                       char* utf8, uint32_t capacity,
                                       uint64_t* at_us);

/*
 * The first time at or after from_us that what describes: a threshold's
 * sample reaching its level, counting only samples from from_us on, or
 * a marker with the label. Nothing found is ECUIO_OK with *at_us
 * ECUIO_RECORDING_NONE. ECUIO_ERR_INVALID_ARGUMENT also for an unknown
 * kind or direction, a negative or NaN hysteresis, nonzero flags or
 * reserved, or a threshold on a text signal; ECUIO_ERR_NULL_ARGUMENT for
 * a marker's NULL label.
 */
ecuio_status_t ecuio_recording_find(const ecuio_recording_t* r,
                                    const ecuio_recording_find_t* what,
                                    uint64_t from_us, uint64_t* at_us);

/* Frees the reader. NULL is a safe no-op. No other call on the handle
 * may overlap it. */
void ecuio_recording_close(ecuio_recording_t* r);

/* --- The frames check (ADR-0049 section 6) ---------------------------- */

/* The defaults a zero field of ecuio_check_options_t takes */
#define ECUIO_CHECK_DEFAULT_GAP_US         1000000u /* 1 s */
#define ECUIO_CHECK_DEFAULT_RATE_DROP_X100 1000u    /* 10x the median */
#define ECUIO_CHECK_DEFAULT_SETTLE_US      2000000u /* 2 s */

/* What the check looks for; zero fields take the defaults */
typedef struct ecuio_check_options {
    uint64_t gap_us;         /* 0 for the default */
    uint32_t rate_drop_x100; /* 0 for the default; else more than 100 */
    uint32_t flags;          /* zero */
    uint64_t settle_us;      /* 0 for the default */
    uint64_t reserved[4];    /* zero; ABI growth space */
} ecuio_check_options_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_check_options_t) == 56,
              "ecuio_check_options_t size is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_check_options_t) == 56,
               "ecuio_check_options_t size is ABI");
#endif

/* ecuio_check_finding_t.kind; append-only, and the summary's counts index */
#define ECUIO_CHECK_GAP       1u /* no frame at all for longer than gap_us,
                                    outside a sleep: the whole bus's */
#define ECUIO_CHECK_RATE_DROP 2u /* an ID's intervals past rate_drop_x100 /
                                    100 of its median; a run of them one */
#define ECUIO_CHECK_LATE_ID   3u /* an ID first seen more than settle_us
                                    after the first frame */
#define ECUIO_CHECK_OVERFLOW  4u /* frames flagged OVERFLOW, a run of them
                                    one: the whole bus's */
#define ECUIO_CHECK_ISOTP     5u /* on ISO 15765-4's IDs, a consecutive frame
                                    out of order, or a first frame never
                                    completed */
#define ECUIO_CHECK_LINK      6u /* an events.log link line: code its state,
                                    count its episodes */
#define ECUIO_CHECK_ASLEEP    7u /* an events.log asleep line: the sleep */

/* ecuio_check_finding_t.flags */
#define ECUIO_CHECK_FINDING_WHOLE_BUS 0x1u /* no ID's: a gap, an overflow,
                                              a link line, a sleep */
#define ECUIO_CHECK_FINDING_EXTENDED  0x2u /* a 29-bit ID */

/* One finding. Passed to the callback as the check settles it, in the
 * order it reads: at the frame or events.log line that ends it, so a run
 * comes after what began within it; at_us and length_us place it. */
typedef struct ecuio_check_finding {
    uint32_t kind;           /* ECUIO_CHECK_* */
    uint32_t arbitration_id; /* the ID's; 0 with WHOLE_BUS (0x000 is an ID) */
    uint64_t at_us;          /* host_us where it starts */
    uint64_t length_us;      /* how long it lasted; 0 for an instant */
    uint64_t count;          /* the frames it covers; a link's episodes */
    uint32_t flags;          /* ECUIO_CHECK_FINDING_* */
    int32_t code;            /* a link's state (an ecuio_status_t); else 0 */
    uint64_t reserved[2];    /* zero; ABI growth space */
} ecuio_check_finding_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_check_finding_t) == 56,
              "ecuio_check_finding_t size is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_check_finding_t) == 56,
               "ecuio_check_finding_t size is ABI");
#endif

/* ecuio_check_summary_t.flags: why ISO-TP sequences weren't checked */
#define ECUIO_CHECK_SKIPPED_ISOTP       0x1u /* the kind was skipped */
#define ECUIO_CHECK_ISOTP_FLAGS_UNKNOWN 0x2u /* events.log doesn't say its
                                                frame flags are complete (an
                                                older recording, a replay of
                                                one, or no events.log) */
#define ECUIO_CHECK_ISOTP_NO_WIRE       0x4u /* no frame on ISO 15765-4's IDs
                                                came as it crossed the bus:
                                                each a reassembled payload,
                                                or none */

typedef struct ecuio_check_summary {
    uint64_t duration_us;  /* the latest host_us of a frame or an
                              events.log line */
    uint64_t frames;
    uint32_t ids;          /* IDs seen, error frames left out */
    uint32_t flags;        /* ECUIO_CHECK_SKIPPED_ISOTP and why */
    uint64_t counts[8];    /* findings by kind: counts[ECUIO_CHECK_GAP] and
                              on; a kind past the eighth only through the
                              callback */
    uint64_t torn_rows;    /* frames.csv's last row dropped (section 7) */
    uint64_t reserved[4];  /* zero; ABI growth space */
} ecuio_check_summary_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_check_summary_t) == 128,
              "ecuio_check_summary_t size is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_check_summary_t) == 128,
               "ecuio_check_summary_t size is ABI");
#endif

/* Called on the checking thread, once per finding; a nonzero return stops
 * the check (ECUIO_ERR_CANCELED) */
typedef int (*ecuio_check_finding_callback_t)(
    const ecuio_check_finding_t* finding, void* user_data);

/*
 * One pass over the recording in folder_utf8: frames.csv, and events.log's
 * header, link and asleep lines when it has one. Memory follows the IDs
 * seen, not the file. options NULL for every default; finding NULL for the
 * summary alone; progress as ecuio_recording_open's, over frames.csv.
 * *out is filled on success and on a stop. ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT
 * for a NULL folder or out; ECUIO_ERR_INVALID_ARGUMENT for nonzero flags or
 * reserved, or a rate_drop_x100 of 1 to 100; ECUIO_ERR_LOAD_FAILED for a
 * folder with no frames.csv or a row the check refuses ("frames.csv line
 * N: ...", never the path; a row earlier than the one before it among
 * them); ECUIO_ERR_CANCELED when progress or finding returned nonzero;
 * ECUIO_ERR_OUT_OF_MEMORY. A last row with no newline is dropped and
 * counted (torn_rows).
 */
ecuio_status_t ecuio_recording_check(const char* folder_utf8,
                                     const ecuio_check_options_t* options,
                                     ecuio_open_progress_t progress,
                                     ecuio_check_finding_callback_t finding,
                                     void* user_data,
                                     ecuio_check_summary_t* out);

/* The export (v0.6.0) joins this header later; ADR-0049 section 8. */

#ifdef __cplusplus
}
#endif
#endif /* ECUIO_RECORDING_H */
