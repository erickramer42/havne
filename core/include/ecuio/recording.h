// File/layer: core/include/ecuio/recording.h | C ABI, public interface
// Testing: HARNESS: RecorderTest (test_core): the writer against the mock
//          and the replay fixture, the seams in internal.h; UNIT
//          (test_c_abi): the header as strict C and the structs' layout
#ifndef ECUIO_RECORDING_H
#define ECUIO_RECORDING_H
#include "ecuio/core.h" /* ecuio_status_t, ecuio_session_t */
#include "ecuio/signals.h" /* ecuio_enum_entry_t, ecuio_signal_file_t */
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
 * The signal file that will decode the session (ADR-0051 section 4),
 * given before ecuio_session_record: attach checks that its set is the
 * session's, and the recording holds a copy of the file's bytes as
 * signals.json, its SHA-256 on events.log's header (format 3's
 * "signal file" line), so a replay loading that copy decodes the same
 * values. file must outlive the call only. ECUIO_ERR_NULL_ARGUMENT for a
 * NULL recorder or file; ECUIO_ERR_INVALID_ARGUMENT once a session has
 * attached or on a second call; ECUIO_ERR_OUT_OF_MEMORY.
 */
ecuio_status_t ecuio_recorder_signal_file(ecuio_recorder_t* recorder,
                                          const ecuio_signal_file_t* file);
/*
 * Attaches recorder to session, before or after start (ADR-0056 section
 * 3), on the thread that starts and closes the session: copies the
 * session's defs (name, unit, kind, decimal places, plot rank), writes
 * events.log's header and sets the recording's zero, the moment of the
 * call. The recording holds the frames from then on; a started
 * session's header says what the link's state was ("link": "healthy"
 * for one that is OK with no episode) and which signals its focus held
 * ("focus", when one is set). Reply timing's counts in latency.csv are
 * the session's, cumulative since it started, so a recording begun
 * mid-session finds them already counted up. A session takes one
 * recorder at a time: once that one is closed, another can attach, and
 * a refused recorder is left as it was for a later try.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL session or recorder;
 * ECUIO_ERR_SESSION_CLOSED for a closed session;
 * ECUIO_ERR_INVALID_ARGUMENT for a recorder that has had a session, or a
 * session that has a recorder; ECUIO_ERR_OUT_OF_MEMORY when the defs'
 * copy can't be made.
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
#define ECUIO_RECORDING_INFO_FRAMES                                            \
    0x2u /* the values were decoded from
                                                frames.csv at the open
                                                (ecuio_recording_open_frames),
                                                not read from signals.csv */

/* ecuio_recording_info_t.format: events.log's header */
#define ECUIO_RECORDING_FORMAT_UNKNOWN 0u /* none, or a first line the
                                             reader doesn't know */
#define ECUIO_RECORDING_FORMAT_OLDER   1u /* ADR-0017's: kinds and decimal
                                             places inferred, no UTC */
#define ECUIO_RECORDING_FORMAT_CORE    2u /* ADR-0049's */
#define ECUIO_RECORDING_FORMAT_ENUMS   3u /* ADR-0049's with enum signals'
                                             labels (ADR-0051) */

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
#define ECUIO_RECORDING_SIGNAL_ENUM 0x2u /* a number shown as a word
                                            (format 3): read as a number,
                                            its labels from
                                            ecuio_recording_get_labels */

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

/* ecuio_recording_find_t.kind: an engine start is the engine speed
 * rising from a stop (ecuio_recording_find) */
#define ECUIO_RECORDING_FIND_THRESHOLD    1u
#define ECUIO_RECORDING_FIND_MARKER       2u
#define ECUIO_RECORDING_FIND_ENGINE_START 3u
/* An engine start's levels, in rpm: the engine-speed signal reaching
 * ECUIO_ENGINE_START_RPM after being below ECUIO_ENGINE_STOPPED_RPM. A
 * starter cranks an engine at 150-300 rpm, and an idle runs past 500. */
#define ECUIO_ENGINE_START_RPM   500
#define ECUIO_ENGINE_STOPPED_RPM 100
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

/* ecuio_recording_signals_t.flags: NAMED, events.log's header names
 * signals.json and its SHA-256 (format 3); PRESENT, signals.json is in
 * the folder; MATCH, named and present, and its SHA-256 the header's */
#define ECUIO_RECORDING_SIGNALS_NAMED   0x1u
#define ECUIO_RECORDING_SIGNALS_PRESENT 0x2u
#define ECUIO_RECORDING_SIGNALS_MATCH   0x4u

/* The signal file a recording was decoded with (ADR-0051 section 4):
 * copied into its folder as signals.json, its hash on events.log's
 * header, so a replay loading it decodes the same values */
typedef struct ecuio_recording_signals {
    uint32_t flags;       /* ECUIO_RECORDING_SIGNALS_* */
    uint32_t reserved0;   /* zero */
    uint64_t length;      /* signals.json's bytes; 0 when it's absent */
    char sha256[72];      /* the header's, 64 lowercase hex digits; ""
                              when it names none */
    uint64_t reserved[5]; /* zero; ABI growth space */
} ecuio_recording_signals_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_recording_signals_t) == 128,
              "ecuio_recording_signals_t size is ABI");
static_assert(offsetof(ecuio_recording_signals_t, sha256) == 16,
              "recording signals sha256 offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_recording_signals_t) == 128,
               "ecuio_recording_signals_t size is ABI");
_Static_assert(offsetof(ecuio_recording_signals_t, sha256) == 16,
               "recording signals sha256 offset is ABI");
#endif

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
 * Opens the recording in folder_utf8 with its values decoded from its
 * frames.csv through signal_set, as a session under that set decodes
 * them (ADR-0049's appendix of 2026-11-06): so a raw recording, or one
 * made before a signal file was written, has values. signals.csv isn't
 * read; events.log, markers.csv and signals.json are, as by
 * ecuio_recording_open, and every other reader call answers alike on the
 * handle, with ECUIO_RECORDING_INFO_FRAMES set. The reader keeps its own
 * copy of the set, which the caller may free once this returns. The
 * signals are the set's defs that decoded at least once, in the set's
 * order, each with its def's name, unit, decimal places, plot rank,
 * group and kind (TEXT and HEX are text, an ENUM has its labels); a
 * number reads as signals.csv would hold it, to 12 significant digits.
 * A frame on ISO 15765-4's IDs that isn't REASSEMBLED and carries a
 * Single, First or Consecutive Frame's PCI is a frame as it crossed the
 * bus: the message it completes decodes, at the time of its last frame;
 * a First Frame while a message on its ID is under way (two senders)
 * ends both. Memory beside ecuio_recording_open's: 16 bytes for each
 * second with frames, 24 for each computed signal's input in each such
 * second, and 256 each time a packed reply's lengths change.
 * ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL folder, set or out;
 * ECUIO_ERR_INVALID_ARGUMENT for a set ecuio_validate_signal_set refuses;
 * ECUIO_ERR_LOAD_FAILED for a folder with no frames.csv or a file the
 * reader refuses ("frames.csv line 12: ...", never the path);
 * ECUIO_ERR_CANCELED and ECUIO_ERR_OUT_OF_MEMORY as for
 * ecuio_recording_open, progress following frames.csv.
 */
ecuio_status_t ecuio_recording_open_frames(const char* folder_utf8,
                                           const ecuio_signal_set_t* signal_set,
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
/* The signal file it was decoded with, as the open found it: signals.json
 * is hashed then, so MATCH says the bytes there now are the ones that
 * decoded it (a caller reads them from <folder>/signals.json, and a
 * caller that finds them changed since sees the hash differ) */
ecuio_status_t ecuio_recording_get_signals(const ecuio_recording_t* r,
                                           ecuio_recording_signals_t* out);
ecuio_status_t ecuio_recording_get_signal(const ecuio_recording_t* r,
                                          uint32_t index,
                                          ecuio_recording_signal_t* out);
ecuio_status_t ecuio_recording_get_marker(const ecuio_recording_t* r,
                                          uint32_t index,
                                          ecuio_recording_marker_t* out);

/*
 * An enum signal's labels (ECUIO_RECORDING_SIGNAL_ENUM), raw values
 * ascending, as ecuio_signal_set_t's enums hold them: *count gets them
 * all and the first capacity are copied to entries (capacity 0 with
 * entries NULL for the count). Any other signal has none, count 0.
 * ECUIO_ERR_NULL_ARGUMENT for NULL entries with a capacity.
 */
ecuio_status_t ecuio_recording_get_labels(const ecuio_recording_t* r,
                                          uint32_t signal,
                                          ecuio_enum_entry_t* entries,
                                          uint32_t capacity,
                                          uint32_t* count);

/*
 * An enum signal's label for a raw value, as ecuio_enum_label gives one:
 * NULL for a raw value with no label, a signal that isn't an enum or
 * past the count, or a NULL handle (a UI shows the number). Valid until
 * ecuio_recording_close.
 */
const char* ecuio_recording_enum_label(const ecuio_recording_t* r,
                                       uint32_t signal, uint32_t raw);

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
 * sample reaching its level, counting only samples from from_us on, a
 * marker with the label, or an engine start. An engine start is the
 * engine-speed signal's rising threshold at ECUIO_ENGINE_START_RPM with
 * a hysteresis of ECUIO_ENGINE_START_RPM - ECUIO_ENGINE_STOPPED_RPM: a
 * sample below ECUIO_ENGINE_STOPPED_RPM arms it, so a recording that
 * starts with the engine running finds nothing until a later start. The
 * engine-speed signal is the number signal whose unit is "rpm", ASCII
 * case ignored (obd2's and the mock's say "RPM"): of several, the one of
 * the lowest nonzero plot_rank, else the first in the recording's order;
 * with none, nothing is found. Its signal, direction, level and
 * hysteresis are 0. Nothing found is ECUIO_OK with *at_us
 * ECUIO_RECORDING_NONE. ECUIO_ERR_INVALID_ARGUMENT also for an unknown
 * kind or direction, a negative or NaN hysteresis, nonzero flags or
 * reserved, a threshold on a text signal, or an engine start with a
 * nonzero signal, direction, level or hysteresis;
 * ECUIO_ERR_NULL_ARGUMENT for a marker's NULL label.
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

/* --- Exports and imports (ADR-0054) --------------------------------- */

/* ecuio_export_options_t.format; append-only */
#define ECUIO_EXPORT_FOLDER 1u /* a scrubbed copy of the folder */
#define ECUIO_EXPORT_CSV    2u /* the values over a range, a row each */
#define ECUIO_EXPORT_PCAPNG 3u /* the frames for Wireshark (section 3) */

/* ecuio_export_options_t.flags */
#define ECUIO_EXPORT_RAW 0x1u /* the recording as it is, not scrubbed: an
                                 explicit choice, never the default */

/* What a scrubbed export leaves out, by class (section 1): the indexes of
 * ecuio_export_report_t.left_out. A failed check's class is its index
 * plus one (ecuio_export_report_t.failed_class, 0 for none). */
#define ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION 0u /* identification replies'
                                                   frames, every segment of
                                                   a segmented one: Service
                                                   09, 62 F180-F1FF */
#define ECUIO_EXPORT_LEFT_OUT_UNATTRIBUTABLE 1u /* segments on a diagnostic
                                                   reply ID whose message
                                                   can't be reassembled */
#define ECUIO_EXPORT_LEFT_OUT_TEXT_VALUES    2u /* text signals' values */
#define ECUIO_EXPORT_LEFT_OUT_ACTIVITY       3u /* activity lines of a kind
                                                   the export doesn't know */
#define ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED     4u /* the check alone: a token
                                                   of 17 VIN characters; an
                                                   export holding one fails,
                                                   so its count stays 0 */
#define ECUIO_EXPORT_LEFT_OUT_COUNT          5u

/* ecuio_export_options_t.layout, the CSV's (ADR-0056 section 5) */
#define ECUIO_CSV_WIDE 0u /* a column a signal, a row a sweep or a grid */
#define ECUIO_CSV_LONG 1u /* a row a value: time_s,signal,unit,value */

typedef struct ecuio_export_options {
    uint32_t format;           /* ECUIO_EXPORT_* */
    uint32_t flags;            /* ECUIO_EXPORT_RAW, or 0 */
    uint64_t from_us;          /* host_us, from_us to to_us inclusive;
                                  to_us 0 is the end, so 0 and 0 is all
                                  of it */
    uint64_t to_us;
    const char* folder_utf8;   /* the recording's folder */
    const char* out_path_utf8; /* the file to write, refused when it
                                  exists: written under a name of its
                                  own beside it, renamed into place
                                  once checked */
    /* The CSV's shape (ADR-0056 section 5); 0 or NULL throughout for
     * the other formats and for ECUIO_CSV_LONG */
    uint32_t layout;                 /* ECUIO_CSV_WIDE (0, the default) or
                                  ECUIO_CSV_LONG */
    uint32_t row_ms;                 /* wide: 0 a row a sweep, else a grid this
                                  many ms apart from the recording's zero */
    uint32_t units;                  /* wide: ECUIO_UNITS_* (signals.h) */
    uint32_t column_count;           /* wide: the columns named below; 0 takes
                                  the header's focus line, else every
                                  number signal in the recording's order */
    const char* const* columns_utf8; /* the columns by name, in order */
    uint64_t reserved[1];            /* zero; ABI growth space */
} ecuio_export_options_t;

#if defined(__cplusplus)
static_assert(offsetof(ecuio_export_options_t, layout) ==
                  24 + 2 * sizeof(void*),
              "export options layout offset is ABI");
static_assert(offsetof(ecuio_export_options_t, columns_utf8) ==
                  40 + 2 * sizeof(void*),
              "export options columns offset is ABI");
static_assert(sizeof(ecuio_export_options_t) == 56 + 2 * sizeof(void*),
              "ecuio_export_options_t size is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ecuio_export_options_t, layout) ==
                   24 + 2 * sizeof(void*),
               "export options layout offset is ABI");
_Static_assert(offsetof(ecuio_export_options_t, columns_utf8) ==
                   40 + 2 * sizeof(void*),
               "export options columns offset is ABI");
_Static_assert(sizeof(ecuio_export_options_t) == 56 + 2 * sizeof(void*),
               "ecuio_export_options_t size is ABI");
#endif

typedef struct ecuio_export_report {
    uint64_t frames;   /* written, made-up ones among them; 0 for CSV */
    uint64_t made_up;  /* re-segmented from a reassembled message, each
                          marked so (section 3) */
    uint64_t left_out[8];  /* by ECUIO_EXPORT_LEFT_OUT_*, the frames
                              (pcapng) or values (CSV) each class left
                              out */
    uint32_t failed_class; /* the check's: a class plus one, 0 when it
                              passed or didn't run */
    uint32_t flags;        /* zero */
    uint64_t values;       /* CSV, folder: the values written; 0 for pcapng */
    uint64_t signal_count; /* CSV, folder: the signals they're of */
    uint32_t files;          /* folder: the files written */
    uint32_t files_left_out; /* folder: the files it doesn't know */
    uint64_t rows;           /* CSV wide: the rows written (values counts
                                the filled cells, signal_count the
                                columns); 0 otherwise */
} ecuio_export_report_t;

#if defined(__cplusplus)
static_assert(ECUIO_EXPORT_LEFT_OUT_COUNT <= 8,
              "the report holds 8 classes");
static_assert(sizeof(ecuio_export_report_t) == 120,
              "ecuio_export_report_t size is ABI");
static_assert(offsetof(ecuio_export_report_t, failed_class) == 80,
              "export report failed_class offset is ABI");
static_assert(offsetof(ecuio_export_report_t, values) == 88,
              "export report values offset is ABI");
static_assert(offsetof(ecuio_export_report_t, files) == 104,
              "export report files offset is ABI");
static_assert(offsetof(ecuio_export_report_t, files_left_out) == 108,
              "export report files_left_out offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(ECUIO_EXPORT_LEFT_OUT_COUNT <= 8, "the report holds 8 classes");
_Static_assert(sizeof(ecuio_export_report_t) == 120,
               "ecuio_export_report_t size is ABI");
_Static_assert(offsetof(ecuio_export_report_t, failed_class) == 80,
               "export report failed_class offset is ABI");
_Static_assert(offsetof(ecuio_export_report_t, values) == 88,
               "export report values offset is ABI");
_Static_assert(offsetof(ecuio_export_report_t, files) == 104,
               "export report files offset is ABI");
_Static_assert(offsetof(ecuio_export_report_t, files_left_out) == 108,
               "export report files_left_out offset is ABI");
#endif

/*
 * Writes the recording in options->folder_utf8 to options->out_path_utf8
 * in options->format, its frames between from_us and to_us, scrubbed by
 * construction unless ECUIO_EXPORT_RAW (ADR-0054 section 1): each
 * class's frames or values left out and counted in *report, then the
 * written bytes, and every message as it was before any segmenting,
 * searched for the recording's own identity values and VIN-shaped
 * tokens; one match fails the export, leaving no file, with the class
 * in report->failed_class. ECUIO_EXPORT_CSV writes signals.csv's number
 * values. ECUIO_CSV_WIDE, the default (ADR-0056 sections 4 and 5): a
 * column a signal, Time (ms) first; a row each time every column in
 * the rule has a sample newer than the last row, at the completing
 * sample's time in whole ms from the recording's zero, each cell the
 * column's newest value, held, never interpolated; a column with no
 * sample for 1 s, or 3x its usual interval when that is longer, leaves
 * the rule and its cells stay empty until its next sample; with
 * row_ms, a grid of that many ms from the zero instead, each row the
 * newest values at its time, a row with no new cell skipped. The
 * columns are columns_utf8's names in order (one the recording lacks
 * is an empty column), else the header's focus line's, else every
 * number signal in the recording's order that has a sample in range;
 * more than 16384 columns, named or found, fail the export (the
 * survey refuses at the first one past, before any row is costed).
 * Each cell carries its signal's decimal places (the header's; an
 * older recording's the most its values show) in units
 * (ecuio_display_convert), the head cell "Name (unit)" in the unit
 * shown; CRLF, no comment line, RFC 4180 quotes only where needed.
 * ECUIO_CSV_LONG writes a row a value, a comment line of counts first
 * (pandas: skiprows=1), then time_s,signal,unit,value. In both, a text
 * or hex signal's values and a signal keyed as an identification reply
 * are left out unless RAW (never columns); a name, unit or raw text
 * value that starts as a spreadsheet formula does (=, +, -, @, a tab,
 * a CR) fails it, while a number written as recorded, such as -12.5,
 * is exempt. Markers aren't written.
 * ECUIO_EXPORT_FOLDER makes out_path_utf8 a scrubbed copy of the
 * folder (the caller names it; the source's name, which may hold the
 * vehicle's label, is never carried): frames.csv's kept rows as
 * recorded, signals.csv by the CSV's classes, events.log's header and
 * its link, drop, asleep and end lines (the URI's authority as
 * <path>, <address> or <label>, the vehicle's label as <vehicle>, the
 * caller's notes left out), markers.csv with its labels as typed,
 * latency.csv and signals.json, each file through the check; a file it
 * doesn't know is left out and counted (files_left_out).
 * Deterministic: the same recording and options give the same bytes.
 * report may be NULL. ECUIO_OK;
 * ECUIO_ERR_NULL_ARGUMENT for a NULL options or path, a column_count
 * with NULL columns_utf8 or a NULL name among them;
 * ECUIO_ERR_INVALID_ARGUMENT for an unknown format, layout or units, a
 * flag or reserved field this core doesn't know, a to_us other than 0
 * before from_us, a layout field set for another format or for
 * ECUIO_CSV_LONG, an empty column name, a column named twice, or a
 * column that names a text or identification signal;
 * ECUIO_ERR_NOT_IMPLEMENTED for a format not built;
 * ECUIO_ERR_LOAD_FAILED for a recording it can't read ("frames.csv line
 * 12: ...", never a path; for CSV, a folder with no signals.csv), a
 * file that exists or can't be written, a formula's start, more than
 * 16384 wide columns ("the wide CSV is limited to 16384 columns: pick
 * the columns to keep"), and a failed check (the class's words, never
 * the value).
 */
ecuio_status_t ecuio_export(const ecuio_export_options_t* options,
                            ecuio_export_report_t* report);

/* What a wide CSV export would write, per row interval */
typedef struct ecuio_export_estimate {
    uint64_t rows;  /* the rows the interval gives */
    uint64_t bytes; /* the file's size, exact */
    uint32_t columns;
    uint32_t flags;       /* zero */
    uint64_t reserved[2]; /* zero; ABI growth space */
} ecuio_export_estimate_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_export_estimate_t) == 40,
              "ecuio_export_estimate_t size is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_export_estimate_t) == 40,
               "ecuio_export_estimate_t size is ABI");
#endif

/*
 * Sizes the wide CSV export options describe (ECUIO_EXPORT_CSV,
 * ECUIO_CSV_WIDE) for each of the count row intervals in row_ms (0 a
 * row a sweep), in one pass over the recording: out[i] gets the rows,
 * the file's exact size (every cell formatted as the export would
 * write it) and the columns. options->row_ms is ignored and
 * out_path_utf8 may be NULL; nothing is written and the file check
 * doesn't run, so an export can still fail it. ECUIO_OK;
 * ECUIO_ERR_NULL_ARGUMENT for a NULL options, folder, row_ms or out;
 * ECUIO_ERR_INVALID_ARGUMENT as ecuio_export's, for a count of 0 or
 * above 64 (refused before anything is allocated), and for a format or
 * layout other than the wide CSV;
 * ECUIO_ERR_LOAD_FAILED as ecuio_export's.
 */
ecuio_status_t ecuio_export_estimate(const ecuio_export_options_t* options,
                                     const uint32_t* row_ms, uint32_t count,
                                     ecuio_export_estimate_t* out);

/* Puts frame index of the caller's frames in *out (zeroed first); nonzero
 * cancels. Called on the caller's thread before ecuio_scrub_frames
 * returns, never later, so a binding can pass a plain foreign function. */
typedef int (*ecuio_scrub_frame_callback_t)(void* user_data, size_t index,
                                            ecuio_frame_t* out);

/*
 * Frames in memory scrubbed as ecuio_export scrubs a recording's (ADR-0054
 * section 1 and its appendix "frames in memory"): get is called for each
 * index from 0 to count - 1 in the order the frames arrived, twice, and
 * must give the same frame both times. The frames of ISO-TP messages as
 * they crossed the bus are reassembled first, on their timestamp_us. An
 * identification reply's frames (every segment of a segmented one), a
 * torn message's segments on a diagnostic reply ID and the messages a
 * text or hex def of set applies to (set may be NULL) are left out:
 * keep[i] is 0 for those and 1 for every other frame. Then each kept
 * frame, and each whole message a kept frame carries, is searched for
 * the identity values the left-out replies carry and for VIN-shaped
 * tokens; one match fails it, the same as ecuio_export, with keep all
 * zero and the class in report->failed_class. report: frames kept,
 * left_out by class, made_up 0 (nothing is re-segmented). report may be
 * NULL. ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL get, or a NULL keep
 * with a count; ECUIO_ERR_INVALID_ARGUMENT for a flag (none is defined)
 * or a frame whose data_length is past ECUIO_FRAME_DATA_MAX;
 * ECUIO_ERR_CANCELED when get returned nonzero; ECUIO_ERR_LOAD_FAILED
 * for a failed check (the class's words, never the value);
 * ECUIO_ERR_OUT_OF_MEMORY. On any failure keep is all zero.
 */
ecuio_status_t ecuio_scrub_frames(size_t count,
                                  ecuio_scrub_frame_callback_t get,
                                  void* user_data,
                                  const ecuio_signal_set_t* set, uint32_t flags,
                                  uint8_t* keep, ecuio_export_report_t* report);

/* One frames.csv row as ecuio_recording_read_frames hands it on: the
 * frame (timestamp_us the row's session time, bytes past data_length
 * zero) and the row's host_us. Nonzero stops the read. Called on the
 * caller's thread before ecuio_recording_read_frames returns, never
 * later, so a binding can pass a plain foreign function. */
typedef int (*ecuio_frame_row_callback_t)(void* user_data,
                                          const ecuio_frame_t* frame,
                                          uint64_t host_us);

typedef struct ecuio_frames_read {
    uint64_t rows;      /* handed to the callback */
    uint32_t torn_rows; /* 0 or 1: a last row with no line end, reached
                           and left out (ADR-0049 section 7) */
    uint32_t reserved0; /* zero; ABI growth space */
    uint64_t reserved[2];
} ecuio_frames_read_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_frames_read_t) == 32,
              "ecuio_frames_read_t size is ABI");
static_assert(offsetof(ecuio_frames_read_t, torn_rows) == 8,
              "ecuio_frames_read_t layout is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_frames_read_t) == 32,
               "ecuio_frames_read_t size is ABI");
_Static_assert(offsetof(ecuio_frames_read_t, torn_rows) == 8,
               "ecuio_frames_read_t layout is ABI");
#endif

/*
 * A recording's frames as it recorded them (ADR-0049's appendix "frames
 * read back"): each frames.csv row whose host_us is from_us to to_us,
 * both included (to_us 0: to the end), handed to callback in the file's
 * order, a whole message's row (ECUIO_FRAME_FLAG_REASSEMBLED) as one
 * frame. Nothing is decoded or scrubbed: identification replies come as
 * they were recorded. The range's first row is found by a search on the
 * file's offsets (host_us never goes back in a frames.csv), so a
 * stretch costs its own rows, not the file's. folder_utf8 is the
 * recording's folder, or its frames.csv. summary may be NULL; it counts
 * the rows handed on, whatever the outcome.
 * ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL folder or callback;
 * ECUIO_ERR_INVALID_ARGUMENT for a to_us other than 0 before from_us;
 * ECUIO_ERR_CANCELED when the callback returned nonzero;
 * ECUIO_ERR_LOAD_FAILED for a folder with no frames.csv ("the folder
 * has no frames.csv"), one that isn't a Havne frames.csv, or a row it
 * refuses, an earlier host_us than the row before among them
 * ("frames.csv, the row at byte N: ...", never a path);
 * ECUIO_ERR_OUT_OF_MEMORY.
 */
ecuio_status_t ecuio_recording_read_frames(const char* folder_utf8,
                                           uint64_t from_us, uint64_t to_us,
                                           ecuio_frame_row_callback_t callback,
                                           void* user_data,
                                           ecuio_frames_read_t* summary);

/* ecuio_import_options_t.format; append-only */
#define ECUIO_IMPORT_CANDUMP 1u /* can-utils' log (section 4) */
#define ECUIO_IMPORT_CSV     2u /* another logger's signals (section 5),
                                   through its column map */

typedef struct ecuio_import_options {
    uint32_t format;               /* ECUIO_IMPORT_* */
    uint32_t flags;                /* zero */
    const char* in_path_utf8;
    const char* out_folder_utf8;   /* the recording's folder, made here and
                                      refused when it exists */
    const char* interface_utf8;    /* candump: the bus to take; NULL for
                                      the log's only one. NULL for CSV */
    const char* column_map_utf8;   /* CSV: its column map, JSON text
                                      (ADR-0054's appendix "the CSV import
                                      as built"); NULL for candump */
    uint64_t reserved[4];          /* zero; ABI growth space */
} ecuio_import_options_t;

typedef struct ecuio_import_report {
    uint64_t lines;           /* read */
    uint64_t frames;          /* written, error frames among them; 0 for
                                 CSV */
    uint64_t error_frames;
    uint64_t interfaces;      /* the log's, the one taken among them */
    uint64_t utc_start_us;    /* the first frame's or row's time,
                                 microseconds since 1970-01-01 UTC; 0 for
                                 a CSV whose map and times give none */
    uint64_t duration_us;     /* the last frame's or row's time since the
                                 first */
    uint64_t values;          /* CSV: the samples written */
    uint64_t columns_ignored; /* CSV: the header's columns the map
                                 doesn't name */
    uint64_t reserved[2];     /* zero; ABI growth space */
} ecuio_import_report_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_import_report_t) == 80,
              "ecuio_import_report_t size is ABI");
static_assert(offsetof(ecuio_import_report_t, values) == 48,
              "ecuio_import_report_t layout is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_import_report_t) == 80,
               "ecuio_import_report_t size is ABI");
_Static_assert(offsetof(ecuio_import_report_t, values) == 48,
               "ecuio_import_report_t layout is ABI");
#endif

/*
 * Reads options->in_path_utf8 in options->format and writes a recording
 * folder at options->out_folder_utf8 (ADR-0054 section 4): frames.csv
 * with each frame as it arrived (never REASSEMBLED), and an events.log
 * header naming the import as its source, the first frame's time as its
 * UTC start, profile raw. It has no signals.csv: its values come from
 * ecuio_recording_open_frames. One recording is one bus: a log with
 * several interfaces takes the one interface_utf8 names, and refuses
 * none named, its detail listing them. A frame stamped up to 100 ms
 * before the one before it takes that one's time (candump stamps on the
 * CPU that received it); further back is refused. The folder is written
 * under a name of its own beside it and renamed once whole, so a failed
 * import leaves none.
 *
 * A CSV (section 5) through options->column_map_utf8: RFC 4180, strict
 * and bounded, checked whole before anything is written; signals.csv
 * with the mapped columns' samples (an empty cell, or one of the map's
 * "missing" texts, no sample) and a format 3 events.log whose signal
 * lines are the map's, its utc line the map's start or the file's first
 * time (none for a relative time with no start); no frames.csv. A row up
 * to 100 ms before the one before it takes that one's time; further back
 * is refused. report may be NULL.
 *
 * ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL options or path;
 * ECUIO_ERR_INVALID_ARGUMENT for an unknown format, a flag or reserved
 * field this core doesn't know, a column map given to candump or missing
 * for CSV, or an interface given to CSV; ECUIO_ERR_LOAD_FAILED for a map
 * or line it refuses ("the column map: ...", "line 12: ...", never a
 * path), a log with no frames of the interface, a CSV with no rows, a
 * folder that exists or can't be written.
 */
ecuio_status_t ecuio_import(const ecuio_import_options_t* options,
                            ecuio_import_report_t* report);

/* ecuio_csv_guess_t.flags: TIME_UNIT_KNOWN the map's time has a unit;
 * DECIMAL_COMMA its decimal mark is a comma; PREAMBLE lines above the
 * header are left out (the map's header_row is above 1); TIME_NUMBERS
 * the time column holds plain numbers, which time_unit can give a unit */
#define ECUIO_CSV_GUESS_TIME_UNIT_KNOWN 0x00000001u
#define ECUIO_CSV_GUESS_DECIMAL_COMMA   0x00000002u
#define ECUIO_CSV_GUESS_PREAMBLE        0x00000004u
#define ECUIO_CSV_GUESS_TIME_NUMBERS    0x00000008u

typedef struct ecuio_csv_guess {
    uint32_t flags;      /* ECUIO_CSV_GUESS_* */
    uint32_t columns;    /* signals in the map */
    uint32_t left_out;   /* header columns left out ("columns" says why) */
    uint32_t rows_read;  /* the rows below the header the guess read */
    uint32_t map_offset; /* the map's value in the text written: bytes */
    uint32_t map_length; /* from map_offset, its braces included */
    uint64_t reserved[3];
} ecuio_csv_guess_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_csv_guess_t) == 48, "ecuio_csv_guess_t size is ABI");
static_assert(offsetof(ecuio_csv_guess_t, map_offset) == 16,
              "ecuio_csv_guess_t layout is ABI");
static_assert(offsetof(ecuio_csv_guess_t, reserved) == 24,
              "ecuio_csv_guess_t layout is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_csv_guess_t) == 48,
               "ecuio_csv_guess_t size is ABI");
_Static_assert(offsetof(ecuio_csv_guess_t, map_offset) == 16,
               "ecuio_csv_guess_t layout is ABI");
_Static_assert(offsetof(ecuio_csv_guess_t, reserved) == 24,
               "ecuio_csv_guess_t layout is ABI");
#endif

/*
 * Guesses a CSV file's column map from its header (ADR-0054's appendix
 * "a column map guessed from the header") and writes one JSON text:
 *
 *   {"map": {...}, "columns": [{"column": "<header text>", "used": true},
 *    {"column": ..., "used": false, "why": "<words>"}, ...],
 *    "notes": ["<a line>", ...]}
 *
 * "map" is a column map (havne_column_map 1) that ecuio_import takes as
 * it is; core checks it before handing it out. Its byte range in the text
 * this call wrote is report->map_offset and map_length: that slice alone
 * is complete JSON, to print, save or pass as column_map_utf8. Imports
 * nothing and writes no file; the same file always gives the same text.
 * It reads the first 1000 rows within 1 MiB for the header, then up to
 * 200 rows or 64 KiB below it. Notes and words name columns as the header
 * has them, never a cell's value or the file's path.
 *
 * A time column of numbers takes time_unit ("s", "ms" or "us") when it
 * is given, else the unit its header names ("Time (s)", "[ms]"); with
 * neither the map's time has no unit and the flag is clear, and the
 * import refuses the map until one is given. ISO 8601 times with their
 * offset are "iso8601" whatever time_unit says. Any other time
 * (ISO 8601 with no offset, times of day, cells not all times) needs a
 * column map with the zone (or the date and offset): no time_unit gives
 * it a unit, and TIME_NUMBERS is clear. The start is never guessed.
 *
 * map_out and cap follow ecuio_serial_ports' contract: on ECUIO_OK
 * *needed is the text's length plus its NUL; the text is written only
 * when cap holds it all (otherwise map_out, when cap > 0, is ""), and cap
 * 0 with a NULL map_out asks the size. On any failure, the arguments'
 * refusals too, *needed (when not NULL) is 0, the report zero and
 * map_out, when not NULL with cap > 0, "". report may be NULL.
 *
 * ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL path or needed, or a NULL
 * map_out with a cap; ECUIO_ERR_INVALID_ARGUMENT for a time_unit not "s",
 * "ms" or "us"; ECUIO_ERR_LOAD_FAILED, the detail saying why, for a file
 * that can't be opened or read, one whose first 1152 KiB hold no line
 * end, or one with no header row, no time column or no column of values
 * ("line 12: ..." where a line is at fault).
 */
ecuio_status_t ecuio_import_csv_guess(const char* path_utf8,
                                      const char* time_unit, char* map_out,
                                      size_t cap, size_t* needed,
                                      ecuio_csv_guess_t* report);

/* --- The health report (ADR-0054's appendix "the health report as
 * built") ------------------------------------------------------------ */

/* ecuio_health_options_t.format; append-only */
#define ECUIO_HEALTH_JSON 1u /* one JSON object, "havne_health": 1 */
#define ECUIO_HEALTH_TEXT 2u /* the same facts as plain text */

/* ecuio_health_report_t.flags */
#define ECUIO_HEALTH_VIN_DECODED 0x00000001u /* a VIN was read: the
                                                vehicle's model year and
                                                make are its decode's */
#define ECUIO_HEALTH_BASELINE_READ 0x00000002u /* the baseline was read */

/* POD, 32 bytes: a session's newest value of one def, as the caller
 * holds it (core keeps none), its age by the caller's clock */
typedef struct ecuio_health_value {
    uint32_t def_id;   /* the session's set's def */
    uint32_t flags;    /* zero */
    double value;      /* in the def's unit, as decoded */
    uint64_t age_us;   /* from its decode to the report's time */
    uint64_t reserved; /* zero */
} ecuio_health_value_t;

/* The scalars first and the pointers last, so every offset but the
 * pointers' is the same on either width; the baseline's pointer sits in
 * a union of eight bytes, which keeps it at 48 on either width too */
typedef struct ecuio_health_options {
    uint32_t format;            /* ECUIO_HEALTH_* */
    uint32_t flags;             /* zero */
    uint32_t units;             /* ECUIO_UNITS_*: the values' system */
    uint32_t latest_model_year; /* for the VIN's decode: this year + 1,
                                   as the front ends pass it; 0 none */
    uint64_t from_us;           /* a recording's range, host_us as the
                                   export's; 0 and 0 is all of it, and
                                   a session's must be */
    uint64_t to_us;
    uint64_t utc_us;            /* the report's time, microseconds since
                                   1970 UTC, the caller's; 0 writes none */
    uint32_t value_count;       /* entries of values; 0 for a folder */
    uint32_t reserved0;         /* zero */
    union {
        const char* baseline_folder_utf8; /* the vehicle's baseline
                                             recording's folder, NULL or
                                             "" for none: the report
                                             names it, never by its
                                             folder */
        uint64_t baseline_room;           /* its place on either width */
    };
    uint64_t reserved[3];       /* zero; ABI growth space */
    ecuio_session_t* session;   /* the source: a live session, or */
    const char* folder_utf8;    /* a recording's folder */
    const ecuio_health_value_t* values; /* a session's newest values, or
                                           NULL with value_count 0 */
    const char* out_path_utf8;  /* the file to write, refused when it
                                   exists: written under a name of its
                                   own beside it, renamed into place
                                   once checked */
} ecuio_health_options_t;

typedef struct ecuio_health_report {
    uint32_t ecus;          /* ECUs written */
    uint32_t codes;         /* trouble codes written, every list */
    uint32_t values;        /* key values written */
    uint32_t since_cleared; /* the since-the-codes-were-cleared values */
    uint64_t left_out[8];   /* by ECUIO_EXPORT_LEFT_OUT_*: the
                               identification items whose text is left
                               out (IDENTIFICATION) */
    uint32_t failed_class;  /* the check's, as the export report's */
    uint32_t flags;         /* ECUIO_HEALTH_VIN_DECODED */
    uint64_t reserved[4];   /* zero; ABI growth space */
} ecuio_health_report_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_health_value_t) == 32,
              "ecuio_health_value_t size is ABI");
static_assert(offsetof(ecuio_health_value_t, age_us) == 16,
              "health value age_us offset is ABI");
static_assert(offsetof(ecuio_health_options_t, utc_us) == 32,
              "health options utc_us offset is ABI");
static_assert(offsetof(ecuio_health_options_t, baseline_folder_utf8) == 48,
              "health options baseline_folder_utf8 offset is ABI");
static_assert(offsetof(ecuio_health_options_t, session) == 80,
              "health options session offset is ABI");
static_assert(sizeof(ecuio_health_report_t) == 120,
              "ecuio_health_report_t size is ABI");
static_assert(offsetof(ecuio_health_report_t, failed_class) == 80,
              "health report failed_class offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_health_value_t) == 32,
               "ecuio_health_value_t size is ABI");
_Static_assert(offsetof(ecuio_health_value_t, age_us) == 16,
               "health value age_us offset is ABI");
_Static_assert(offsetof(ecuio_health_options_t, utc_us) == 32,
               "health options utc_us offset is ABI");
_Static_assert(offsetof(ecuio_health_options_t, baseline_folder_utf8) == 48,
               "health options baseline_folder_utf8 offset is ABI");
_Static_assert(offsetof(ecuio_health_options_t, session) == 80,
               "health options session offset is ABI");
_Static_assert(sizeof(ecuio_health_report_t) == 120,
               "ecuio_health_report_t size is ABI");
_Static_assert(offsetof(ecuio_health_report_t, failed_class) == 80,
               "health report failed_class offset is ABI");
#endif

/*
 * A car's health as one scrubbed file (ADR-0054's appendix "the health
 * report as built"), in ECUIO_HEALTH_JSON or ECUIO_HEALTH_TEXT, which
 * hold the same facts: the source (the session's URI as the folder
 * export scrubs it, its backend and profile; no vehicle label, no
 * adapter versions), the vehicle (its VIN's decode: model year, make,
 * whether the check digit matched; never the VIN), the inspection
 * (ecuio_diag_inspect over every ECU for that model year, the rule for
 * none when no VIN was read), each ECU's readiness, codes with their
 * titles, freeze frame, the since-the-codes-were-cleared values
 * (Service 01 PIDs 21, 30, 31, 4D and 4E, whenever given) and its
 * identification items by name and outcome (their text left out), then
 * the key values: the set's ranked signals (plot_rank above 0, numbers
 * and enumerations) among the values given, in rank order. With a
 * baseline_folder_utf8, the report names the vehicle's baseline
 * recording from its events.log: its start (UTC), length, profile and
 * backend, and whether the report's own recording is it (the same
 * start); one that can't be read says so and the report is still
 * written. From a
 * recording's folder, each ECU's reports are rebuilt from its replies in
 * frames.csv that end in the range (an ECU: a reply ID with a positive
 * diagnostic or identification reply; the last of each kind; one with
 * none "not recorded"), and the values are the set's (its signals.json,
 * else its uds-generic DIDs, else its profile) in force at the range's
 * end, from signals.csv or, with none, decoded from frames.csv, each aged
 * from that end; a recording with no frames.csv has no ECU and says so.
 * The file is checked for the identification texts and the VIN's shape
 * before it's renamed into place; a match fails it with its class.
 * Deterministic: the time and each value's age are the caller's, or the
 * recording's. report may be NULL; a report not written counts nothing
 * but a failed check's class.
 * ECUIO_OK; ECUIO_ERR_NULL_ARGUMENT for a NULL options or out path;
 * ECUIO_ERR_INVALID_ARGUMENT for an unknown format or units, a flag or
 * reserved field this core doesn't know, a session and a folder both or
 * neither, NULL values with a count, values with a folder, a value's
 * flags or reserved nonzero, a nonzero to_us before from_us, a range
 * (from_us or to_us nonzero) with a session, whose values are its
 * newest; ECUIO_ERR_LOAD_FAILED for a folder with
 * neither frames.csv nor signals.csv, a row the reader refuses
 * ("frames.csv line N: ..."), a range that starts after the recording
 * ends, a file that exists or can't be written, or
 * a failed check (its words in the detail, never a value or a path);
 * the reader's own codes for a recording that changes while it's read.
 */
ecuio_status_t ecuio_health_export(const ecuio_health_options_t* options,
                                   ecuio_health_report_t* report);

#ifdef __cplusplus
}
#endif
#endif /* ECUIO_RECORDING_H */
