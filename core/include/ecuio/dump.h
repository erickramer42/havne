/* File/layer: core/include/ecuio/dump.h | public C ABI, strict C
 * Testing: UNIT: DumpFileTest, DumpExportTest (test_core), test_c_abi;
 *          FUZZ: fuzz_dump_file
 *
 * A copy of a module's memory as a file (ADR-0066 section 4): the bytes
 * read from the module, a record of what they are and how they were
 * read, and the checksums that seal both. Core writes a copy once and
 * never changes it; these calls read, check, list, export, bring back
 * and remove copies with no session, so a front end can show them
 * offline. A copy stays until ecuio_dump_remove is called for it: no
 * other call here deletes one, and core never prunes, ages or counts
 * them.
 *
 * The file (format 1). Every integer is little-endian.
 *
 *   offset  bytes  field
 *        0      8  the magic: 89 48 56 44 0D 0A 1A 0A
 *        8      4  the format, 1
 *       12      4  the header's bytes, 128: where the image starts
 *       16      8  the image's bytes N, 1 to ECUIO_DUMP_IMAGE_MAX
 *       24      4  the record's bytes M, 1 to ECUIO_DUMP_RECORD_MAX
 *       28      4  flags, zero
 *       32     32  the image's SHA-256
 *       64     32  the record's SHA-256
 *       96     32  reserved, zero
 *      128      N  the image: each range's bytes, in the record's order
 *  128 + N      M  the record: UTF-8 JSON, no byte order mark
 *
 * The file is exactly 128 + N + M bytes. A format above 1 is refused as
 * made by a later Havne, never as damaged, and before anything else of
 * the file is judged. A reader judges every length against the file's
 * real size before it allocates, and reads the image as a stream.
 *
 * The record. Its keys are exactly these; "obd", and inside "read" the
 * keys "volts_mv", "volts_min_mv", "rpm" and "kmh", may be absent (a
 * reading nothing answered is left out, never written as 0), and "obd"
 * holds any of its four keys:
 *
 *   {"havne_dump": 1,
 *    "kind": "read",
 *    "utc": 1795312345,
 *    "core": "0.9.1",
 *    "image": {"bytes": 65536, "sha256": "<64 hex>"},
 *    "ranges": [{"name": "calibration", "step": "read",
 *                "session": "extended", "address": "00010000",
 *                "bytes": 65536, "offset": 0}],
 *    "module": {"rx": "7E8", "tx": "7E0",
 *               "ident": [{"did": "D220", "role": "hardware",
 *                          "hex": "<bytes>"}],
 *               "obd": {"vin": "<17>", "calids": ["<id>"],
 *                       "cvns": ["<8 hex>"], "name": "<text>"}},
 *    "plugin": {"id": "example-car", "version": "1.2.0",
 *               "module_sha256": "<64 hex>", "fingerprint": "<64 hex>",
 *               "operation": "dump-calibration"},
 *    "transport": {"scheme": "j2534", "adapter": "<name>",
 *                  "firmware": "<version>"},
 *    "read": {"passes": 2, "seconds": 41, "volts_mv": 12450,
 *             "volts_min_mv": 12310, "rpm": 0, "kmh": 0,
 *             "parked_tick": true, "unlocks": 1, "writes_before": 0}}
 *
 *   - "havne_dump" is the header's format, judged before the keys.
 *   - "kind" is "read": bytes read by service 23 or by an upload.
 *   - "utc" is the second the copy was sealed, 1 to the year 9999's last.
 *   - "image" repeats the header's N and its SHA-256 (lower-case hex).
 *   - "ranges", 1 to ECUIO_DUMP_MAX_RANGES, tile the image: offsets
 *     ascend from 0 with no gap and the bytes add to N. "step" is "read"
 *     or "upload", "session" is "extended" or "programming", "address"
 *     is 8 upper-case hex digits. No two ranges overlap in address.
 *   - "rx" and "tx" are the module's reply and request IDs in upper-case
 *     hex: 3 digits for an 11-bit ID, 8 for a 29-bit one.
 *   - "ident", at most ECUIO_DUMP_MAX_IDENTS, holds the identification
 *     DIDs the plan declared, read before the first pass and equal after
 *     the second: "did" 4 upper-case hex digits, "role" one of
 *     "hardware", "software" and "base", "hex" its value, 1 to
 *     ECUIO_DUMP_IDENT_MAX bytes in upper-case hex.
 *   - "obd" is core's own identification read, where the module answers.
 *   - "transport" never holds a URI, a serial number or a path.
 *   - "read" holds how the copy was taken: "passes" is 2; "volts_mv" is
 *     the battery before the read and "volts_min_mv" its lowest through
 *     it; "rpm" and "kmh" the speeds read before it; "parked_tick"
 *     whether the person confirmed the vehicle parked; "unlocks" the
 *     unlock attempts the read made; "writes_before" the writes Havne
 *     had made to the module when the copy was taken.
 *
 * The name: <yyyymmdd>-<tx>-<64 hex>.havnedump. The date is "utc"'s in
 * UTC, <tx> is the record's, and the 64 lower-case hex digits are the
 * SHA-256 of the whole file, so sha256sum checks a copy with no Havne.
 * Core judges the hash part alone; the date and <tx> are labels, and
 * the record is their truth. The header's two checksums make a file
 * checkable under any name.
 */

#ifndef ECUIO_DUMP_H
#define ECUIO_DUMP_H

#include "ecuio/core.h"
#include "ecuio/recording.h" /* ecuio_cancel_t */

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define ECUIO_DUMP_FORMAT           1u
#define ECUIO_DUMP_HEADER_BYTES     128u
#define ECUIO_DUMP_IMAGE_MAX        67108864u /* 64 MiB */
#define ECUIO_DUMP_RECORD_MAX       65536u
#define ECUIO_DUMP_MAX_RANGES       64u
#define ECUIO_DUMP_MAX_IDENTS       8u
#define ECUIO_DUMP_IDENT_MAX        64u /* an identification value's bytes */
#define ECUIO_DUMP_MAX_CALIBRATIONS 8u
/* A copy's name with its NUL: the longest is a 29-bit <tx>'s */
#define ECUIO_DUMP_NAME_MAX 96u
/* A listed file's name with its NUL */
#define ECUIO_DUMP_ENTRY_NAME_MAX 256u

/* kind: closed, append-only */
#define ECUIO_DUMP_KIND_READ 1u

/* A range's step and session */
#define ECUIO_DUMP_STEP_READ           1u /* service 23 */
#define ECUIO_DUMP_STEP_UPLOAD         2u /* services 35, 36 and 37 */
#define ECUIO_DUMP_SESSION_PROGRAMMING 2u /* the session's own byte */
#define ECUIO_DUMP_SESSION_EXTENDED    3u

/* An identification DID's role */
#define ECUIO_DUMP_ROLE_HARDWARE 1u
#define ECUIO_DUMP_ROLE_SOFTWARE 2u
#define ECUIO_DUMP_ROLE_BASE     3u

/* ecuio_dump_info_t.flags */
#define ECUIO_DUMP_HAS_VOLTS     0x01u /* volts_mv was read */
#define ECUIO_DUMP_HAS_VOLTS_MIN 0x02u
#define ECUIO_DUMP_HAS_RPM       0x04u
#define ECUIO_DUMP_HAS_KMH       0x08u
#define ECUIO_DUMP_PARKED_TICK   0x10u /* the person confirmed "parked" */
#define ECUIO_DUMP_HAS_OBD       0x20u /* the record holds "obd" */

typedef struct ecuio_dump ecuio_dump_t; /* opaque */

/* What a copy's header and record say. POD, 528 bytes. Text is
 * NUL-terminated; a text the record lacks is "". */
typedef struct ecuio_dump_info {
    uint32_t struct_size; /* the caller's sizeof, at least this one's */
    uint32_t flags;       /* ECUIO_DUMP_HAS_* and ECUIO_DUMP_PARKED_TICK */
    uint32_t format;      /* ECUIO_DUMP_FORMAT */
    uint32_t kind;        /* ECUIO_DUMP_KIND_* */
    uint64_t image_bytes;
    uint64_t utc;
    uint8_t image_sha256[32];
    uint8_t record_sha256[32];
    uint32_t rx_id; /* the module's reply ID */
    uint32_t tx_id; /* its request ID */
    uint32_t range_count;
    uint32_t ident_count;
    uint32_t calibration_count;
    uint32_t passes;
    uint32_t seconds;
    uint32_t volts_mv;     /* with ECUIO_DUMP_HAS_VOLTS */
    uint32_t volts_min_mv; /* with ECUIO_DUMP_HAS_VOLTS_MIN */
    uint32_t rpm;          /* with ECUIO_DUMP_HAS_RPM */
    uint32_t kmh;          /* with ECUIO_DUMP_HAS_KMH */
    uint32_t unlocks;
    uint32_t writes_before;
    uint32_t reserved0; /* zero */
    char core[16];      /* the core that wrote it, X.Y.Z */
    char plugin_id[32];
    char plugin_version[16];
    char operation[32];        /* the plan's id */
    uint8_t module_sha256[32]; /* the plugin's conversation module */
    uint8_t fingerprint[32];   /* ecuio_plugin_fingerprint's */
    char scheme[16];
    char adapter[64];
    char firmware[64];
    char vin[20];         /* "obd"'s, 17 characters */
    char ecu_name[24];    /* "obd"'s, 20 characters at most */
    uint32_t reserved[7]; /* zero; ABI growth space */
} ecuio_dump_info_t;

/* One range of the image. POD, 88 bytes. */
typedef struct ecuio_dump_range {
    uint32_t struct_size;
    uint32_t flags;   /* zero */
    uint32_t step;    /* ECUIO_DUMP_STEP_* */
    uint32_t session; /* ECUIO_DUMP_SESSION_* */
    uint64_t address; /* in the module */
    uint64_t bytes;
    uint64_t offset; /* in the image */
    char name[32];
    uint32_t reserved[4]; /* zero; ABI growth space */
} ecuio_dump_range_t;

/* One identification DID's value. POD, 96 bytes. */
typedef struct ecuio_dump_ident {
    uint32_t struct_size;
    uint32_t flags; /* zero */
    uint32_t did;
    uint32_t role;   /* ECUIO_DUMP_ROLE_* */
    uint32_t length; /* bytes of value in use */
    uint32_t reserved0;
    uint8_t value[ECUIO_DUMP_IDENT_MAX];
    uint32_t reserved[2]; /* zero; ABI growth space */
} ecuio_dump_ident_t;

/* ecuio_dump_calibration_t.flags */
#define ECUIO_DUMP_HAS_CALID 0x1u
#define ECUIO_DUMP_HAS_CVN   0x2u

/* The record's "calids" and "cvns" by place: the index-th of each, where
 * the list is that long. POD, 48 bytes. */
typedef struct ecuio_dump_calibration {
    uint32_t struct_size;
    uint32_t flags; /* ECUIO_DUMP_HAS_CALID, ECUIO_DUMP_HAS_CVN */
    char calid[20]; /* 16 characters at most */
    uint8_t cvn[4];
    uint32_t reserved[4]; /* zero; ABI growth space */
} ecuio_dump_calibration_t;

/* ecuio_dump_entry_t.state */
#define ECUIO_DUMP_STATE_COPY       1u /* reads as a copy under a copy's name */
#define ECUIO_DUMP_STATE_UNFINISHED 2u /* a partial a stopped write left */
#define ECUIO_DUMP_STATE_INVALID    3u /* neither; words says why */

/* ecuio_dump_entry_t.flags */
#define ECUIO_DUMP_ENTRY_NAME_CUT 0x1u /* the name is longer than name holds */

/* One file of a folder of copies. POD, 456 bytes. The record's fields
 * are set only for a file that reads as a copy, whatever its name. */
typedef struct ecuio_dump_entry {
    uint32_t state; /* ECUIO_DUMP_STATE_* */
    uint32_t flags; /* ECUIO_DUMP_ENTRY_* */
    uint32_t kind;  /* ECUIO_DUMP_KIND_*; 0 when it doesn't read */
    uint32_t writes_before;
    uint64_t utc;
    uint64_t image_bytes;
    uint64_t file_bytes; /* the file's size on the disk */
    uint32_t rx_id;
    uint32_t tx_id;
    char name[ECUIO_DUMP_ENTRY_NAME_MAX]; /* the file's, in its folder */
    char words[128];                      /* why it is no copy; "" for a copy */
    uint32_t reserved[6];                 /* zero; ABI growth space */
} ecuio_dump_entry_t;

/* ecuio_dump_export's forms */
#define ECUIO_DUMP_EXPORT_RESTORE 1u /* the file itself, still a copy */
#define ECUIO_DUMP_EXPORT_IMAGE   2u /* the raw image, a .bin for each range */

/* What an export wrote. POD, 32 bytes. */
typedef struct ecuio_dump_export_result {
    uint32_t struct_size; /* the caller's sizeof, at least this one's */
    uint32_t flags;       /* zero */
    uint32_t files;       /* the files written */
    uint32_t vin_runs;    /* ECUIO_DUMP_EXPORT_IMAGE: the runs of the image
                             that read as a VIN */
    uint32_t reserved[4]; /* zero; ABI growth space */
} ecuio_dump_export_result_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_dump_export_result_t) == 32,
              "ecuio_dump_export_result_t size is ABI");
static_assert(sizeof(ecuio_dump_info_t) == 528,
              "ecuio_dump_info_t size is ABI");
static_assert(offsetof(ecuio_dump_info_t, image_sha256) == 32,
              "dump info image_sha256 offset is ABI");
static_assert(offsetof(ecuio_dump_info_t, rx_id) == 96,
              "dump info rx_id offset is ABI");
static_assert(offsetof(ecuio_dump_info_t, core) == 152,
              "dump info core offset is ABI");
static_assert(offsetof(ecuio_dump_info_t, module_sha256) == 248,
              "dump info module_sha256 offset is ABI");
static_assert(offsetof(ecuio_dump_info_t, scheme) == 312,
              "dump info scheme offset is ABI");
static_assert(offsetof(ecuio_dump_info_t, vin) == 456,
              "dump info vin offset is ABI");
static_assert(sizeof(ecuio_dump_range_t) == 88,
              "ecuio_dump_range_t size is ABI");
static_assert(offsetof(ecuio_dump_range_t, name) == 40,
              "dump range name offset is ABI");
static_assert(sizeof(ecuio_dump_ident_t) == 96,
              "ecuio_dump_ident_t size is ABI");
static_assert(offsetof(ecuio_dump_ident_t, value) == 24,
              "dump ident value offset is ABI");
static_assert(sizeof(ecuio_dump_calibration_t) == 48,
              "ecuio_dump_calibration_t size is ABI");
static_assert(sizeof(ecuio_dump_entry_t) == 456,
              "ecuio_dump_entry_t size is ABI");
static_assert(offsetof(ecuio_dump_entry_t, name) == 48,
              "dump entry name offset is ABI");
static_assert(offsetof(ecuio_dump_entry_t, words) == 304,
              "dump entry words offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_dump_info_t) == 528,
               "ecuio_dump_info_t size is ABI");
_Static_assert(sizeof(ecuio_dump_range_t) == 88,
               "ecuio_dump_range_t size is ABI");
_Static_assert(sizeof(ecuio_dump_ident_t) == 96,
               "ecuio_dump_ident_t size is ABI");
_Static_assert(sizeof(ecuio_dump_calibration_t) == 48,
               "ecuio_dump_calibration_t size is ABI");
_Static_assert(sizeof(ecuio_dump_entry_t) == 456,
               "ecuio_dump_entry_t size is ABI");
_Static_assert(sizeof(ecuio_dump_export_result_t) == 32,
               "ecuio_dump_export_result_t size is ABI");
#endif

/*
 * Opens the file at path (UTF-8) and judges its structure and its
 * record: the header, the sizes against the file's own, the record's
 * JSON and keys, and the ranges' tiling. It reads the header and the
 * record, never the image, so it is cheap enough for a list; the
 * checksums are ecuio_dump_verify's. ECUIO_ERR_NULL_ARGUMENT for a NULL
 * path or out; ECUIO_ERR_LOAD_FAILED, the words in
 * ecuio_last_error_detail() naming what is wrong and never the path,
 * for a file that can't be read or is no copy this core reads;
 * ECUIO_ERR_OUT_OF_MEMORY. *out is NULL on any failure, else released by
 * ecuio_dump_close. A handle's calls may come from any thread, one at a
 * time. The file is never opened for writing and isn't locked.
 */
ecuio_status_t ecuio_dump_open(const char* path_utf8, ecuio_dump_t** out);

/* Releases a handle; NULL is fine */
void ecuio_dump_close(ecuio_dump_t* dump);

/* The getters: ECUIO_ERR_NULL_ARGUMENT for a NULL dump or out;
 * ECUIO_ERR_INVALID_ARGUMENT for a struct_size smaller than the struct,
 * or an index at or past its count, out untouched. This core's
 * struct_size is written back. */
ecuio_status_t ecuio_dump_get_info(const ecuio_dump_t* dump,
                                   ecuio_dump_info_t* out);
ecuio_status_t ecuio_dump_get_range(const ecuio_dump_t* dump, uint32_t index,
                                    ecuio_dump_range_t* out);
ecuio_status_t ecuio_dump_get_ident(const ecuio_dump_t* dump, uint32_t index,
                                    ecuio_dump_ident_t* out);
ecuio_status_t ecuio_dump_get_calibration(const ecuio_dump_t* dump,
                                          uint32_t index,
                                          ecuio_dump_calibration_t* out);

/*
 * Reads the image from offset: up to capacity bytes into out, *count
 * those read (fewer only at the image's end, 0 at or past it). The
 * bytes are the file's as it is now, unchecked: ecuio_dump_verify says
 * whether they are the copy's. ECUIO_ERR_NULL_ARGUMENT for a NULL dump,
 * out or count; ECUIO_ERR_LOAD_FAILED, with words, when the file no
 * longer reads.
 */
ecuio_status_t ecuio_dump_read(const ecuio_dump_t* dump, uint64_t offset,
                               uint8_t* out, size_t capacity, size_t* count);

/*
 * Reads the whole file again and checks it three ways: the image's
 * SHA-256 against the header's, the record's against the header's, and
 * the whole file's against the hash part of the name it was opened
 * under. ECUIO_OK only when all three hold. name_out (name_capacity at
 * least ECUIO_DUMP_NAME_MAX, or NULL with 0) takes the name core gives
 * the file, from its record and its checksum, whenever the first two
 * hold: a sound file under another name is ECUIO_ERR_LOAD_FAILED with
 * its own name there, and a damaged one leaves "". cancel, when not
 * NULL, is asked between pieces and a non-zero answer ends the call
 * with ECUIO_ERR_CANCELED. ECUIO_ERR_NULL_ARGUMENT for a NULL dump or a
 * NULL name_out with a capacity; ECUIO_ERR_INVALID_ARGUMENT for a
 * capacity below ECUIO_DUMP_NAME_MAX that isn't 0.
 */
ecuio_status_t ecuio_dump_verify(const ecuio_dump_t* dump,
                                 ecuio_cancel_t cancel, void* user_data,
                                 char* name_out, size_t name_capacity);

/*
 * The copies in a folder, by name in byte order: every file named
 * *.havnedump, and every partial a stopped write left beside them
 * (*.havnedump.partial-*). Each is opened as ecuio_dump_open opens it:
 * one that reads under a copy's name is ECUIO_DUMP_STATE_COPY, a partial
 * ECUIO_DUMP_STATE_UNFINISHED, any other ECUIO_DUMP_STATE_INVALID with
 * words. A damaged file is listed, never hidden: a person relies on this
 * folder for a restore. The checksums aren't recomputed here. It follows
 * the serial ports' contract: *count is all found and the first capacity
 * are copied; capacity 0 with a NULL array asks the count; a NULL folder
 * or count, or a NULL array with a capacity, is ECUIO_ERR_NULL_ARGUMENT.
 * ECUIO_ERR_LOAD_FAILED, with words, for a folder that can't be read;
 * ECUIO_ERR_OUT_OF_MEMORY.
 */
ecuio_status_t ecuio_dump_list(const char* folder_utf8, ecuio_dump_entry_t* out,
                               size_t capacity, size_t* count);

/*
 * Exports an opened copy into folder (UTF-8, an existing one), never
 * automatically and never over a file: a name that is taken refuses the
 * export. The copy is read whole and its two checksums checked first; a
 * damaged one is not exported.
 *
 *   ECUIO_DUMP_EXPORT_RESTORE  the file itself, byte for byte, under the
 *     name core gives it (ecuio_dump_verify's), read back from the disk
 *     and checked again before it is put in place. The one export that
 *     is still a copy: ecuio_dump_import takes it back.
 *   ECUIO_DUMP_EXPORT_IMAGE  the raw image as one file for each range,
 *     named <yyyymmdd>-<tx>-<first 12 hex of the copy's checksum>-<the
 *     range's start address, 8 hex>.bin. The bytes are the copy's,
 *     unchanged, with no record and no identifier; all the files are
 *     written or none. result->vin_runs counts the runs in them that
 *     read as a VIN, by the finder a scrubbed export uses; core masks
 *     nothing here (a changed image is no longer the module's memory),
 *     and none found proves nothing.
 *
 * Each file is written under a partial name and renamed into place, and
 * any failure or cancel leaves nothing behind. cancel, when not NULL, is
 * asked between pieces (never call this handle's functions from it).
 * result may be NULL; on a failure it is zeroed but for its size.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL dump or folder;
 * ECUIO_ERR_INVALID_ARGUMENT for an unknown form or a result smaller
 * than the struct; ECUIO_ERR_LOAD_FAILED with words (never a path);
 * ECUIO_ERR_CANCELED; ECUIO_ERR_OUT_OF_MEMORY.
 */
ecuio_status_t ecuio_dump_export(const ecuio_dump_t* dump, uint32_t form,
                                 const char* folder_utf8, ecuio_cancel_t cancel,
                                 void* user_data,
                                 ecuio_dump_export_result_t* result);

/*
 * Brings a copy back: the file at path, under whatever name, is opened
 * and checked as ecuio_dump_verify checks one (its two checksums; its
 * name isn't judged), then copied into folder under the name core gives
 * it and set read-only, as a copy is kept. A file of that name there
 * refuses it. name_out (name_capacity at least ECUIO_DUMP_NAME_MAX, or
 * NULL with 0) takes that name once the file is known sound, whether or
 * not the copy then lands. ECUIO_ERR_NULL_ARGUMENT for a NULL path or
 * folder, or a NULL name_out with a capacity;
 * ECUIO_ERR_INVALID_ARGUMENT for a capacity below ECUIO_DUMP_NAME_MAX
 * that isn't 0; ECUIO_ERR_LOAD_FAILED with words;
 * ECUIO_ERR_OUT_OF_MEMORY.
 */
ecuio_status_t ecuio_dump_import(const char* path_utf8, const char* folder_utf8,
                                 char* name_out, size_t name_capacity);

/*
 * Removes one file of a folder of copies: name is the file's whole name
 * there, as ecuio_dump_list gives it (a copy's, read-only or not, an
 * unfinished partial's or an invalid file's). The one call that deletes
 * a copy, and it asks nothing: the front end confirms with the person
 * first. ECUIO_ERR_NULL_ARGUMENT for a NULL folder or name;
 * ECUIO_ERR_INVALID_ARGUMENT, with words, for a name that isn't one the
 * list shows (it holds a path separator, or has neither a copy's suffix
 * nor a partial's); ECUIO_ERR_LOAD_FAILED, with words, when there is no
 * such file or it can't be removed, the file left as it was.
 */
ecuio_status_t ecuio_dump_remove(const char* folder_utf8, const char* name);

#if defined(__cplusplus)
}
#endif

#endif /* ECUIO_DUMP_H */
