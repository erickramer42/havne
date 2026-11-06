// File/layer: core/include/ecuio/obdb.h | C ABI, public interface
// Testing: UNIT: ObdbImportTest (test_core): each rule and its note over
//          files written by hand and the synthetic fixture in
//          tests/fixtures/obdb/, the output loaded by ecuio_signal_file_load;
//          fuzz_obdb_import; test_c_abi: the header as strict C and the
//          structs' layout
#ifndef ECUIO_OBDB_H
#define ECUIO_OBDB_H
#include "ecuio/core.h" /* ecuio_status_t */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/*
 * OBDb import (ADR-0051 section 7): an OBDb signal set (github.com/OBDb, a
 * vehicle repository's JSON under signalsets/v3) converted to a signal file
 * (docs/signal-files.md), which the caller loads with
 * ecuio_signal_file_load as any other. Core reads the set's bytes, never a
 * path, and fetches nothing. OBDb's data is CC BY-SA 4.0, so the converted
 * file's "source" block names OBDb's editors and contributors, the
 * license, the caller's URL and commit, and that the file was converted;
 * every copy of the file keeps it.
 *
 * Each signal is reported field by field through the caller's note
 * callback: converted as it is, converted with a field that differs
 * (OBDb holds a value within min and max, Havne shows it as read), or
 * left out and why.
 */

/* ecuio_obdb_options_t.flags */
#define ECUIO_OBDB_ALONE 0x1u /* no "extends": "obd2": the file's signals alone */

/* Options for ecuio_obdb_import; a NULL options is all defaults. Pointers
 * last, so every other offset is the same on every pointer width. */
typedef struct ecuio_obdb_options {
    uint32_t model_year;  /* the vehicle's; 0 for none, which leaves out
                             every command OBDb gives for some years only */
    uint32_t flags;       /* ECUIO_OBDB_*; any other bit zero */
    uint32_t reserved[6]; /* zero; ABI growth space */
    const char* name;     /* the file's "name" (1 to 63 bytes, UTF-8);
                             NULL for "OBDb import" */
    const char* url;      /* the source's "url" (the repository); NULL for
                             none */
    const char* commit;   /* the source's "commit"; NULL for none */
} ecuio_obdb_options_t;

#if defined(__cplusplus)
static_assert(offsetof(ecuio_obdb_options_t, reserved) == 8,
              "obdb options reserved offset is ABI");
static_assert(offsetof(ecuio_obdb_options_t, name) == 32,
              "obdb options name offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ecuio_obdb_options_t, reserved) == 8,
               "obdb options reserved offset is ABI");
_Static_assert(offsetof(ecuio_obdb_options_t, name) == 32,
               "obdb options name offset is ABI");
#endif

/* ecuio_obdb_note_t.what; append-only */
#define ECUIO_OBDB_NOTE_CONVERTED 0u /* a signal in the file, key given */
#define ECUIO_OBDB_NOTE_CHANGED   1u /* a field of it that differs */
#define ECUIO_OBDB_NOTE_SKIPPED   2u /* a signal, or the set's own key,
                                        left out */
#define ECUIO_OBDB_NOTE_FILE      3u /* the set as a whole: a rule that
                                        holds for every signal */

/* One note. The strings are UTF-8, NUL-terminated and valid only during
 * the call; pointers last, as in ecuio_obdb_options_t. */
typedef struct ecuio_obdb_note {
    uint32_t what;      /* ECUIO_OBDB_NOTE_* */
    uint32_t line;      /* the signal's or key's line in the OBDb file,
                           from 1 */
    const char* signal; /* OBDb's id for the signal; "" for the set's own */
    const char* key;    /* the key it has in the converted file; "" when
                           left out */
    const char* field;  /* what differs: OBDb's key ("min", "nullmax",
                           "unit", "name", "map", ...); "" for the signal
                           as a whole */
    const char* text;   /* why, in words, never the file's path */
} ecuio_obdb_note_t;

#if defined(__cplusplus)
static_assert(offsetof(ecuio_obdb_note_t, signal) == 8,
              "obdb note signal offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ecuio_obdb_note_t, signal) == 8,
               "obdb note signal offset is ABI");
#endif

/* Called once a note, on the importing thread, in the file's order; a
 * nonzero return cancels the import (ECUIO_ERR_CANCELED). */
typedef int (*ecuio_obdb_note_fn)(const ecuio_obdb_note_t* note, void* user);

/* Converts an OBDb signal set (UTF-8 JSON, length bytes, at most
 * ECUIO_OBDB_MAX_BYTES) to a signal file. ECUIO_OK with *out_json a
 * NUL-terminated file of *out_length bytes, which the caller frees with
 * ecuio_obdb_free, and which ecuio_signal_file_load takes;
 * ECUIO_ERR_NULL_ARGUMENT for a NULL bytes (with a length), out_json or
 * out_length; ECUIO_ERR_INVALID_ARGUMENT for input that isn't an OBDb
 * signal set, or one with no signal Havne can read, with
 * ecuio_last_error_detail() saying where and why; ECUIO_ERR_CANCELED;
 * ECUIO_ERR_OUT_OF_MEMORY. note may be NULL. *out_json is NULL on any
 * failure. Thread-safe: each call is its own. */
#define ECUIO_OBDB_MAX_BYTES 1048576u /* 1 MiB */
ecuio_status_t ecuio_obdb_import(const char* bytes, size_t length,
                                 const ecuio_obdb_options_t* options,
                                 ecuio_obdb_note_fn note, void* user,
                                 char** out_json, size_t* out_length);

/* Frees a converted file; NULL is a no-op */
void ecuio_obdb_free(char* json);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_OBDB_H */
