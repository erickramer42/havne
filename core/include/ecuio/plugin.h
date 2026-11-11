// File/layer: core/include/ecuio/plugin.h | C ABI, public interface
// Testing: UNIT: PluginTest (test_signals); FUZZ: fuzz_plugin_manifest

#ifndef ECUIO_PLUGIN_H
#define ECUIO_PLUGIN_H

#include "ecuio/core.h"    /* ecuio_status_t */
#include "ecuio/signals.h" /* ecuio_signal_set_t, ecuio_signal_def_t */
#include "ecuio/vin.h"     /* ecuio_vin_info_t */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Plugins, the data tier (ADR-0052): a package of ADR-0051 signal files
 * with one manifest that names the plugin, the cars it's for, the ECUs
 * its files read (once, by id) and its variants, each an ordered list
 * of the package's files merged by signal key. Loading checks the whole
 * package and registers nothing; registering a variant makes it the
 * profile "plugin.<id>.<variant>". No code runs: a data file is decoded
 * as any signal file is.
 *
 * A package comes as a folder (manifest.json and the files it names,
 * read through the host's callback) or packed into one file, the
 * manifest with each data file inlined under "files" by its relative
 * name. Core reads bytes, never a path.
 *
 * Refusals come through ecuio_last_error_detail(): for a folder, the
 * file (its name in the package) and the line and column, for a packed
 * package the line and column, then why. Never the package's own path.
 */

#define ECUIO_PLUGIN_FORMAT       1         /* "havne_plugin" read */
#define ECUIO_PLUGIN_TIER_DATA    0x1u      /* signal files only */
#define ECUIO_PLUGIN_MANIFEST_MAX 1048576u  /* a folder's manifest.json */
#define ECUIO_PLUGIN_PACKAGE_MAX  16777216u /* everything read, packed or not */
#define ECUIO_PLUGIN_MAX_FILES    64u
#define ECUIO_PLUGIN_MAX_VARIANTS 32u
#define ECUIO_PLUGIN_MAX_FIXTURES 16u
#define ECUIO_PLUGIN_PATH_MAX     127u /* bytes of a relative path */
#define ECUIO_PLUGIN_ID_MAX       31u
#define ECUIO_PLUGIN_VARIANT_ID_MAX 24u /* so the profile name fits */

typedef struct ecuio_plugin ecuio_plugin_t; /* opaque */

/* The host's reader for a folder package: the bytes of the file at a
 * manifest-relative path (forward slashes, no "..", checked before the
 * call), valid until the next call or the load's return. Returns 0 with
 * *bytes and *length set, nonzero when the file can't be read. */
typedef int (*ecuio_plugin_read_t)(void* user, const char* path,
                                   const char** bytes, size_t* length);

/* Reads a package: read NULL for a packed one (manifest is the whole
 * file), else manifest is manifest.json's bytes and read fetches the
 * files it names. *out is NULL on any failure. ECUIO_OK;
 * ECUIO_ERR_NULL_ARGUMENT for a NULL out, or NULL manifest bytes with a
 * length; ECUIO_ERR_INVALID_ARGUMENT for a package refused, with the
 * detail; ECUIO_ERR_OUT_OF_MEMORY. */
ecuio_status_t ecuio_plugin_load(const char* manifest, size_t length,
                                 ecuio_plugin_read_t read, void* user,
                                 ecuio_plugin_t** out);

typedef struct ecuio_plugin_info {
    uint32_t struct_size; /* the caller's sizeof, at least this one's */
    uint32_t tiers;       /* ECUIO_PLUGIN_TIER_* */
    char id[32];
    char version[16]; /* the plugin's own X.Y.Z */
    char name[64];
    uint32_t variant_count;
    uint32_t fixture_count; /* a folder's fixture files; 0 when packed */
    uint32_t reserved[2];   /* zero; ABI growth space */
} ecuio_plugin_info_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_plugin_info_t) == 136,
              "ecuio_plugin_info_t size is ABI");
static_assert(offsetof(ecuio_plugin_info_t, variant_count) == 120,
              "plugin info variant_count offset is ABI");
#endif

/* ECUIO_ERR_INVALID_ARGUMENT for a struct_size smaller than the struct */
ecuio_status_t ecuio_plugin_info(const ecuio_plugin_t* plugin,
                                 ecuio_plugin_info_t* out);

/* The manifest's requires.core, the oldest core that reads the plugin,
 * as X, Y and Z (each 0 to 65535); ECUIO_ERR_NULL_ARGUMENT for a NULL
 * plugin or part, nothing written. Any thread may call it while the
 * plugin lives: it only reads. */
ecuio_status_t ecuio_plugin_requires_core(const ecuio_plugin_t* plugin,
                                          uint16_t* major, uint16_t* minor,
                                          uint16_t* patch);

/* Variant index's id, or fixture index's path, into a buffer of cap
 * bytes; ECUIO_ERR_INVALID_ARGUMENT for an index past the count or a
 * buffer too small for the whole text and its NUL */
ecuio_status_t ecuio_plugin_variant(const ecuio_plugin_t* plugin,
                                    uint32_t index, char* id, size_t cap);
ecuio_status_t ecuio_plugin_fixture(const ecuio_plugin_t* plugin,
                                    uint32_t index, char* path, size_t cap);

/* Variant index's merged set (NULL past the count), valid until the
 * plugin is freed, and its def by signal key (NULL with none) */
const ecuio_signal_set_t* ecuio_plugin_variant_set(const ecuio_plugin_t* plugin,
                                                   uint32_t index);
const ecuio_signal_def_t*
ecuio_plugin_variant_find(const ecuio_plugin_t* plugin, uint32_t index,
                          const char* key);

/* Variant index's merged set as a signal file (NULL past the count),
 * valid until the plugin is freed and never passed to
 * ecuio_signal_file_free: what ecuio_recorder_signal_file copies into a
 * recording (the variant's files merged and written as one signal file,
 * with the plugin's id, version and variant on its own header line), and
 * what ecuio_signal_file_name, _profile ("plugin.<id>.<variant>"), _find
 * and _extends read */
const ecuio_signal_file_t*
ecuio_plugin_variant_file(const ecuio_plugin_t* plugin, uint32_t index);
/* A value fixture (ADR-0052 section 8): a variant's numeric signal and
 * the value its field must decode to when the simulated ECU answers it.
 * The pointer goes last, so the offsets are the same on every width. */
typedef struct ecuio_plugin_value {
    double value;
    uint32_t variant;              /* its index */
    uint32_t line;                 /* the entry's line in its file */
    char signal[32];               /* the signal's key, NUL-terminated */
    uint32_t reserved[4];          /* zero; ABI growth space */
    const ecuio_signal_def_t* def; /* in the variant's set, valid until
                                      the plugin is freed */
} ecuio_plugin_value_t;

#if defined(__cplusplus)
static_assert(offsetof(ecuio_plugin_value_t, line) == 12,
              "plugin value line offset is ABI");
static_assert(offsetof(ecuio_plugin_value_t, signal) == 16,
              "plugin value signal offset is ABI");
static_assert(offsetof(ecuio_plugin_value_t, def) == 64,
              "plugin value def offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ecuio_plugin_value_t, line) == 12,
               "plugin value line offset is ABI");
_Static_assert(offsetof(ecuio_plugin_value_t, signal) == 16,
               "plugin value signal offset is ABI");
_Static_assert(offsetof(ecuio_plugin_value_t, def) == 64,
               "plugin value def offset is ABI");
#endif

#define ECUIO_PLUGIN_FIXTURES_FORMAT 1     /* "havne_fixtures" read */
#define ECUIO_PLUGIN_MAX_VALUES      4096u /* entries in one file */

/* Reads one of the plugin's fixture files (ecuio_plugin_fixture names
 * them; the host reads its bytes, at most ECUIO_SIGNAL_FILE_MAX_BYTES):
 * {"havne_fixtures": 1, "values": [{"variant", "signal", "value"}, ...]},
 * 1 to ECUIO_PLUGIN_MAX_VALUES entries, each a variant of this plugin, a
 * numeric signal of that variant's by key (a number or an enum; not
 * text, hex or computed, since the simulated ECU answers a field), the
 * pair given once in the file, and a number. *count gets every entry;
 * the first cap are copied, in the file's order. cap 0 (values NULL)
 * asks the count. ECUIO_ERR_NULL_ARGUMENT for a NULL plugin or count,
 * NULL bytes with a length, or NULL values with a cap;
 * ECUIO_ERR_INVALID_ARGUMENT for a file refused, *count untouched, with
 * "line L, column C: why" in the detail; ECUIO_ERR_OUT_OF_MEMORY. */
ecuio_status_t ecuio_plugin_values(const ecuio_plugin_t* plugin,
                                   const char* bytes, size_t length,
                                   ecuio_plugin_value_t* values, uint32_t cap,
                                   uint32_t* count);

/* The variants the VIN's manufacturer code and model year fall in: one
 * of the manifest's "match" entries covers it and the variant's own
 * years do (a year range never covers model year 0). A proposal only:
 * the user picks (ADR-0052 section 5). *count gets every one; the
 * first cap indices are copied, in the manifest's order. cap 0 (indices
 * NULL) asks the count. ECUIO_ERR_NULL_ARGUMENT for a NULL plugin, vin
 * or count, or NULL indices with a cap. */
ecuio_status_t ecuio_plugin_match(const ecuio_plugin_t* plugin,
                                  const ecuio_vin_info_t* vin,
                                  uint32_t* indices, uint32_t cap,
                                  uint32_t* count);

/* Registers variant index as profile "plugin.<id>.<variant>", which a
 * URI's ?profile= then names, and writes that name to name (NULL with a
 * cap of 0 skips it). ecuio_profile_register's results; also
 * ECUIO_ERR_INVALID_ARGUMENT for an index past the count or a name
 * buffer too small. */
ecuio_status_t ecuio_plugin_register(ecuio_plugin_t* plugin, uint32_t index,
                                     char* name, size_t cap);

/* Unregisters the plugin's profiles (each name still registered to its
 * variant's set) and frees it. ECUIO_ERR_IN_USE, with nothing changed,
 * while a session or a simulated ECU decodes with one of its sets:
 * destroy those first. NULL is ECUIO_OK. */
ecuio_status_t ecuio_plugin_free(ecuio_plugin_t* plugin);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_PLUGIN_H */
