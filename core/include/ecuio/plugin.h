// File/layer: core/include/ecuio/plugin.h | C ABI, public interface
// Testing: UNIT: PluginTest, PluginWrapTest (test_signals); FUZZ:
//          fuzz_plugin_manifest, fuzz_plugin_wrap

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

#define ECUIO_PLUGIN_FORMAT            1 /* "havne_plugin" a wrap writes */
#define ECUIO_PLUGIN_FORMAT_MAX        2 /* the newest read; 1 is too */
#define ECUIO_PLUGIN_TIER_DATA    0x1u      /* signal files only */
#define ECUIO_PLUGIN_TIER_CONVERSATION 0x2u /* a module that asks (below) */
#define ECUIO_PLUGIN_MANIFEST_MAX 1048576u  /* a folder's manifest.json */
#define ECUIO_PLUGIN_PACKAGE_MAX  16777216u /* everything read, packed or not */
#define ECUIO_PLUGIN_MAX_FILES    64u
#define ECUIO_PLUGIN_MAX_VARIANTS 32u
#define ECUIO_PLUGIN_MAX_FIXTURES 16u
#define ECUIO_PLUGIN_PATH_MAX     127u /* bytes of a relative path */
#define ECUIO_PLUGIN_ID_MAX       31u
#define ECUIO_PLUGIN_VARIANT_ID_MAX 24u /* so the profile name fits */
#define ECUIO_PLUGIN_LINEAGE_LEN    32u /* a manifest's "lineage", hex digits */

typedef struct ecuio_plugin ecuio_plugin_t; /* opaque */

/* The host's reader for a folder package: the bytes of the file at a
 * manifest-relative path (forward slashes, no "..", no part a Windows
 * device name such as con.json or ending in a dot, checked before the
 * call), valid until the next call or the load's return. Returns 0 with
 * *bytes and *length set, nonzero when the file can't be read. A packed
 * file's paths are keys in its text, which nothing opens, and may take
 * such a name. */
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
    uint32_t page_count;    /* its Dashboard pages (ecuio_plugin_page) */
    uint32_t reserved[1];   /* zero; ABI growth space */
} ecuio_plugin_info_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_plugin_info_t) == 136,
              "ecuio_plugin_info_t size is ABI");
static_assert(offsetof(ecuio_plugin_info_t, variant_count) == 120,
              "plugin info variant_count offset is ABI");
static_assert(offsetof(ecuio_plugin_info_t, page_count) == 128,
              "plugin info page_count offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ecuio_plugin_info_t, page_count) == 128,
               "plugin info page_count offset is ABI");
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

/* The manifest's "lineage" into a buffer of cap bytes, "" when it has
 * none: ECUIO_PLUGIN_LINEAGE_LEN of 0-9 and a-f, the start of the
 * SHA-256 of the signal file the plugin was first made from
 * (ecuio_plugin_wrap). Two packages with one id and one lineage are
 * versions of one plugin; it names no person, device or vehicle.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL plugin or buffer;
 * ECUIO_ERR_INVALID_ARGUMENT for a buffer too small for the text and
 * its NUL. */
ecuio_status_t ecuio_plugin_lineage(const ecuio_plugin_t* plugin, char* lineage,
                                    size_t cap);

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

/*
 * Dashboard pages (ADR-0052's appendix on pages): the manifest's optional
 * "pages" lists 1 to ECUIO_PLUGIN_MAX_PAGES paths in the package, each an
 * ADR-0057 layout file of one page for one form: "format" 2, "form"
 * "phone" or "desktop", "pages" holding exactly one page, and no "files"
 * but an empty list. A packed plugin inlines each under "files" as it
 * does a data file; a file "pages" names is never a data file. Core reads
 * that much and nothing inside the page: its cells are the dashboard's
 * rules, which the app and havne-cli's layout check judge.
 */
#define ECUIO_PLUGIN_MAX_PAGES      16u
#define ECUIO_PLUGIN_PAGE_MAX_BYTES 82400u /* read and as written */
#define ECUIO_PLUGIN_PAGE_PHONE     1u
#define ECUIO_PLUGIN_PAGE_DESKTOP   2u

typedef struct ecuio_plugin_page {
    uint32_t struct_size; /* the caller's sizeof, at least this one's */
    uint32_t form;        /* ECUIO_PLUGIN_PAGE_PHONE or _DESKTOP */
    uint32_t text_bytes;  /* the text's length, its NUL left out */
    uint32_t reserved[3]; /* zero; ABI growth space */
    char path[ECUIO_PLUGIN_PATH_MAX + 1]; /* its path in the package */
} ecuio_plugin_page_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_plugin_page_t) == 152,
              "ecuio_plugin_page_t size is ABI");
static_assert(offsetof(ecuio_plugin_page_t, path) == 24,
              "plugin page path offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_plugin_page_t) == 152,
               "ecuio_plugin_page_t size is ABI");
_Static_assert(offsetof(ecuio_plugin_page_t, path) == 24,
               "plugin page path offset is ABI");
#endif

/* Page index's form, path and text length, in the manifest's order.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL plugin or out;
 * ECUIO_ERR_INVALID_ARGUMENT for a struct_size smaller than the struct or
 * an index at or past info's page_count, out untouched. */
ecuio_status_t ecuio_plugin_page(const ecuio_plugin_t* plugin, uint32_t index,
                                 ecuio_plugin_page_t* out);

/* Page index's layout file as core writes it (two spaces a level, UTF-8
 * kept), the same for a folder and its packed form: NUL-terminated,
 * text_bytes long, valid until the plugin is freed. NULL for a NULL
 * plugin or an index at or past the count. */
const char* ecuio_plugin_page_text(const ecuio_plugin_t* plugin,
                                   uint32_t index);

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

/*
 * The conversation tier (ADR-0066; "havne_plugin" 2): a plugin that
 * also carries a WebAssembly module and declares, in its manifest, which
 * of its ECUs the module talks to, which services it may cause core to
 * send and which security-access levels it computes a key for. Loading
 * checks all of it: every declared service against the tier's ceiling,
 * the module's SHA-256 against the manifest's, and the module's shape
 * (its exports by name and type, one bounded memory, no import, no start
 * function). Nothing in this header runs the module.
 */

#define ECUIO_PLUGIN_BUDGET_MAX   1000000u /* instructions in one call */
#define ECUIO_PLUGIN_MAX_SERVICES 16u
#define ECUIO_PLUGIN_MAX_SUBS     14u /* sub-functions of one service */
#define ECUIO_PLUGIN_MAX_LEVELS   32u
#define ECUIO_PLUGIN_KEY_MAX 58u /* a key's bytes: one request's, less 27 xx */

typedef struct ecuio_plugin_conversation {
    uint32_t struct_size; /* the caller's sizeof, at least this one's */
    uint32_t code_bytes;  /* the module's size */
    uint8_t code_sha256[32];
    uint32_t budget; /* instructions one call into the module may run */
    uint32_t ecu_count;
    uint32_t service_count;
    uint32_t level_count;
    uint32_t reserved[2]; /* zero; ABI growth space */
} ecuio_plugin_conversation_t;

/* An ECU the module talks to */
typedef struct ecuio_plugin_conversation_ecu {
    uint32_t struct_size; /* the caller's sizeof, at least this one's */
    uint32_t rx_id;       /* its response ID */
    uint32_t tx_id;       /* its request ID */
    char id[32];          /* the manifest's id for it */
    uint32_t flags;       /* zero */
    uint32_t reserved[4]; /* zero; ABI growth space */
} ecuio_plugin_conversation_ecu_t;

/* A service the module may cause core to send */
typedef struct ecuio_plugin_service {
    uint32_t struct_size; /* the caller's sizeof, at least this one's */
    uint8_t sid;
    uint8_t sub_count; /* 0: the service isn't declared by sub-function */
    /* the sub-functions declared, in the manifest's order */
    uint8_t subs[ECUIO_PLUGIN_MAX_SUBS];
    uint32_t flags;       /* zero */
    uint32_t reserved[2]; /* zero; ABI growth space */
} ecuio_plugin_service_t;

/* A security-access level the module computes a key for: level is the
 * seed request's sub-function and the key's is the next */
typedef struct ecuio_plugin_security_level {
    uint32_t struct_size;   /* the caller's sizeof, at least this one's */
    uint32_t rx_id;         /* the ECU's response ID */
    uint8_t level;          /* the seed request's sub-function, odd */
    uint8_t seed_bytes;     /* the seed's length */
    uint8_t key_bytes;      /* the key's, 1 to ECUIO_PLUGIN_KEY_MAX */
    uint8_t reserved0;      /* zero */
    uint32_t delay_ms;      /* the least wait after a refused key */
    uint32_t key_window_ms; /* how long the module has to give the key */
    uint32_t flags;         /* zero */
    uint32_t reserved[4];   /* zero; ABI growth space */
} ecuio_plugin_security_level_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_plugin_conversation_t) == 64,
              "ecuio_plugin_conversation_t size is ABI");
static_assert(offsetof(ecuio_plugin_conversation_t, budget) == 40,
              "plugin conversation budget offset is ABI");
static_assert(sizeof(ecuio_plugin_conversation_ecu_t) == 64,
              "ecuio_plugin_conversation_ecu_t size is ABI");
static_assert(offsetof(ecuio_plugin_conversation_ecu_t, id) == 12,
              "plugin conversation ecu id offset is ABI");
static_assert(offsetof(ecuio_plugin_conversation_ecu_t, flags) == 44,
              "plugin conversation ecu flags offset is ABI");
static_assert(sizeof(ecuio_plugin_service_t) == 32,
              "ecuio_plugin_service_t size is ABI");
static_assert(offsetof(ecuio_plugin_service_t, subs) == 6,
              "plugin service subs offset is ABI");
static_assert(offsetof(ecuio_plugin_service_t, flags) == 20,
              "plugin service flags offset is ABI");
static_assert(sizeof(ecuio_plugin_security_level_t) == 40,
              "ecuio_plugin_security_level_t size is ABI");
static_assert(offsetof(ecuio_plugin_security_level_t, level) == 8,
              "plugin security level level offset is ABI");
static_assert(offsetof(ecuio_plugin_security_level_t, delay_ms) == 12,
              "plugin security level delay_ms offset is ABI");
static_assert(offsetof(ecuio_plugin_security_level_t, flags) == 20,
              "plugin security level flags offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_plugin_conversation_t) == 64,
               "ecuio_plugin_conversation_t size is ABI");
_Static_assert(offsetof(ecuio_plugin_conversation_t, budget) == 40,
               "plugin conversation budget offset is ABI");
_Static_assert(sizeof(ecuio_plugin_conversation_ecu_t) == 64,
               "ecuio_plugin_conversation_ecu_t size is ABI");
_Static_assert(offsetof(ecuio_plugin_conversation_ecu_t, id) == 12,
               "plugin conversation ecu id offset is ABI");
_Static_assert(offsetof(ecuio_plugin_conversation_ecu_t, flags) == 44,
               "plugin conversation ecu flags offset is ABI");
_Static_assert(sizeof(ecuio_plugin_service_t) == 32,
               "ecuio_plugin_service_t size is ABI");
_Static_assert(offsetof(ecuio_plugin_service_t, subs) == 6,
               "plugin service subs offset is ABI");
_Static_assert(offsetof(ecuio_plugin_service_t, flags) == 20,
               "plugin service flags offset is ABI");
_Static_assert(sizeof(ecuio_plugin_security_level_t) == 40,
               "ecuio_plugin_security_level_t size is ABI");
_Static_assert(offsetof(ecuio_plugin_security_level_t, level) == 8,
               "plugin security level level offset is ABI");
_Static_assert(offsetof(ecuio_plugin_security_level_t, delay_ms) == 12,
               "plugin security level delay_ms offset is ABI");
_Static_assert(offsetof(ecuio_plugin_security_level_t, flags) == 20,
               "plugin security level flags offset is ABI");
#endif

/* The conversation tier's part of plugin. ECUIO_ERR_NULL_ARGUMENT for a
 * NULL; ECUIO_ERR_INVALID_ARGUMENT, out untouched, for a plugin without
 * the tier (ecuio_plugin_info's tiers says) or a struct_size smaller
 * than the struct. */
ecuio_status_t ecuio_plugin_conversation(const ecuio_plugin_t* plugin,
                                         ecuio_plugin_conversation_t* out);

/* The index-th ECU the module talks to, in the group's order: a getter
 * by index, ECUIO_ERR_INVALID_ARGUMENT at or past ecu_count, for a
 * plugin without the tier and for a struct_size smaller than the
 * struct, out untouched. */
ecuio_status_t
ecuio_plugin_conversation_ecu(const ecuio_plugin_t* plugin, uint32_t index,
                              ecuio_plugin_conversation_ecu_t* out);

/* The index-th declared service, and the index-th security level: getters
 * by index against service_count and level_count, with the ECU getter's
 * refusals, a struct_size smaller than the struct among them. */
ecuio_status_t ecuio_plugin_service(const ecuio_plugin_t* plugin,
                                    uint32_t index,
                                    ecuio_plugin_service_t* out);
ecuio_status_t ecuio_plugin_security_level(const ecuio_plugin_t* plugin,
                                           uint32_t index,
                                           ecuio_plugin_security_level_t* out);

/*
 * Making and changing a packed plugin (ADR-0064): a signal file wrapped
 * into a plugin of one variant, and a packed plugin's text with a
 * variant put in or dropped. Each call reads bytes and gives a new
 * text; none changes its input, registers a profile or keeps anything.
 *
 * What the three share:
 * - *out is the packed plugin's text, NUL-terminated, the caller's to
 *   release with ecuio_plugin_text_free; *out_length (NULL skips it) is
 *   its length without the NUL. On any failure *out is NULL and
 *   *out_length 0.
 * - The text is one ecuio_plugin_load has taken (read NULL) before the
 *   call returns, so it is never a plugin this core refuses.
 * - The same input gives the same bytes on every run and platform:
 *   members in a fixed order, two spaces a level, "\n" line ends, no
 *   time and nothing random. An input's own line ends and spacing leave
 *   no trace, since the text is written from what was read.
 * - A refusal is ECUIO_ERR_INVALID_ARGUMENT with the words in
 *   ecuio_last_error_detail(): for an input a loader refuses, that
 *   loader's "line L, column C: why" in the input's own lines (a put's
 *   packed plugin after "the plugin, "); for an option or a rule of
 *   the call, why alone; for a text ecuio_plugin_load refuses, its why
 *   without the place, since the caller never saw that text.
 * - ECUIO_ERR_NULL_ARGUMENT for a NULL out, or NULL bytes with a
 *   length; ECUIO_ERR_OUT_OF_MEMORY. The inputs' bounds are the
 *   loaders': ECUIO_SIGNAL_FILE_MAX_BYTES a signal file,
 *   ECUIO_PLUGIN_PACKAGE_MAX a packed plugin.
 * - A wrap writes format ECUIO_PLUGIN_FORMAT. A put or a drop refuses a
 *   plugin with the conversation tier: its module and what the manifest
 *   declares for it are not these calls' to carry over.
 */

/* The pointers go last, so the numbers' offsets are the same on every
 * width. A struct_size past this struct's is taken and what lies past
 * the struct is never read: a field a later header adds there always
 * comes with a bit in flags, so a core that doesn't know the field
 * refuses the call by its nonzero flags and never drops what was
 * asked. */
typedef struct ecuio_plugin_wrap_options {
    uint32_t struct_size; /* the caller's sizeof, at least this one's */
    uint32_t flags;       /* none yet; zero */
    uint32_t years[2];    /* the match's first and last model year,
                             1980 to 2039; 0 0 for none, and only with
                             wmi */
    uint32_t reserved[6]; /* zero; ABI growth space */
    const char* id;       /* the plugin's id; NULL: from the file's name */
    const char* variant;  /* the variant's id; NULL: "base" */
    const char* wmi;      /* the match's manufacturer codes, separated by
                             commas ("AAA,BBB"); NULL: no match */
} ecuio_plugin_wrap_options_t;

#if defined(__cplusplus)
static_assert(offsetof(ecuio_plugin_wrap_options_t, years) == 8,
              "wrap options years offset is ABI");
static_assert(offsetof(ecuio_plugin_wrap_options_t, reserved) == 16,
              "wrap options reserved offset is ABI");
static_assert(offsetof(ecuio_plugin_wrap_options_t, id) == 40,
              "wrap options id offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ecuio_plugin_wrap_options_t, years) == 8,
               "wrap options years offset is ABI");
_Static_assert(offsetof(ecuio_plugin_wrap_options_t, reserved) == 16,
               "wrap options reserved offset is ABI");
_Static_assert(offsetof(ecuio_plugin_wrap_options_t, id) == 40,
               "wrap options id offset is ABI");
#endif

/* A signal file (ecuio_signal_file_load's rules) as a packed plugin of
 * one variant. options NULL is every default.
 * - "id": options->id, or the file's name as user.<slug>'s slug (lower
 *   case letters and digits, every other run one '-'), cut to
 *   ECUIO_PLUGIN_ID_MAX bytes with no '-' left at its end. A caller
 *   whose store holds that id already asks again with an id of its own.
 * - "name": the file's. "version": "1.0.0". "requires": the least core
 *   X.Y.Z that reads the plugin's format and what its file uses (0.8.0
 *   at least, which reads "lineage"), so the same file wraps to the
 *   same bytes under any core and an older one refuses it in words;
 *   never past this core's own version, which a build ahead of a
 *   release writes instead.
 * - "lineage": the first ECUIO_PLUGIN_LINEAGE_LEN hex digits of the
 *   SHA-256 of the length bytes at file.
 * - "source" and "ecus": the file's, moved to the manifest. "match":
 *   one entry, options->wmi's codes with options->years, only with wmi.
 * - One variant (options->variant, or "base") of one data file,
 *   "data/<variant>.json" ("data/<variant>-2.json" and on when that
 *   names a Windows device, as aux does): the file without its
 *   "source" and "ecus", every other member as it was read.
 * The variant's set is the file's own, table by table, and its keys
 * the file's. A file at the edge of the loader's bounds may load and
 * not wrap: the plugin holds it two levels deeper, and the variant's
 * signal file, which a recording copies and a replay reads, is written
 * two spaces a level and must itself be within
 * ECUIO_SIGNAL_FILE_MAX_BYTES (a file of 600 KB on one line can pass
 * it written out). The refusal names the bound.
 * ECUIO_ERR_INVALID_ARGUMENT also for a struct_size smaller
 * than the struct, nonzero flags or reserved words, an id or variant
 * the manifest's rules refuse, years with no wmi or out of order, and
 * a code that isn't 3 or 6 of a VIN's letters and digits. */
ecuio_status_t ecuio_plugin_wrap(const char* file, size_t length,
                                 const ecuio_plugin_wrap_options_t* options,
                                 char** out, size_t* out_length);

/* The packed plugin with variant `variant` made from a signal file:
 * added after the others, or, when the plugin has that id, put in its
 * place (its "years" kept, its data files left out unless another
 * variant lists them). variant NULL is the file's name as a slug, cut
 * to ECUIO_PLUGIN_VARIANT_ID_MAX bytes.
 * - "version": the minor one higher and the patch 0 ("1.2.3" gives
 *   "1.3.0"); refused when the minor is 65535. "requires": the higher
 *   of the plugin's own and the least core that reads it with the file
 *   put in, that least never past this core's own version. "id",
 *   "name", "lineage", "source", "match" and every other
 *   variant: as they were.
 * - The data file is "data/<variant>.json" ("data/<variant>-2.json"
 *   and on while a file that stays has the path or it names a Windows
 *   device): the signal file without its "ecus", its own "source"
 *   kept, which then names where that variant comes from in every
 *   recording of it.
 * - The file's ECUs join the manifest's, which all variants share. One
 *   at the addresses (rx and tx) of an ECU the manifest has is that
 *   ECU, under the manifest's id; any other is added, under its own id
 *   or, when the manifest has that id at other addresses, as
 *   "<id>_2" ("_3" and on). The file's signals name the id the
 *   manifest has. An ECU that only a replaced variant's files read
 *   leaves the manifest first.
 * - A signal that names no ECU keeps the ECU it read: when the first
 *   ECU it would fall to is another, it is given its ECU by id, the
 *   one the profile obd2 asks added to the manifest as "obd2" when
 *   none there has its addresses.
 * On ECUIO_OK, ecuio_last_error_detail() says which of the file's ECUs
 * took another id, in words for the user, or is "": one note each, joined
 * with "; " while they fit the detail, then "and N more". A text the loader
 * refuses (an ECU past what one session reads, two ID widths, more
 * variants than a plugin holds) ends "; the file can be a plugin of
 * its own" when ecuio_plugin_wrap takes the file. */
ecuio_status_t ecuio_plugin_put_variant(const char* packed, size_t length,
                                        const char* variant, const char* file,
                                        size_t file_length, char** out,
                                        size_t* out_length);

/* The packed plugin without variant `variant`: its data files left out
 * unless another variant lists them, the ECUs only they read with
 * them, and the version raised as a put raises it. "requires": the
 * higher of the plugin's own and the least core that reads the result,
 * as a put writes it. The rest as it was. ECUIO_ERR_NULL_ARGUMENT for
 * a NULL variant; ECUIO_ERR_INVALID_ARGUMENT for a variant the plugin
 * lacks, or its only one. */
ecuio_status_t ecuio_plugin_drop_variant(const char* packed, size_t length,
                                         const char* variant, char** out,
                                         size_t* out_length);

/* Releases a text one of the three calls above gave; NULL is nothing */
void ecuio_plugin_text_free(char* text);

/* Unregisters the plugin's profiles (each name still registered to its
 * variant's set) and frees it. ECUIO_ERR_IN_USE, with nothing changed,
 * while a session or a simulated ECU decodes with one of its sets:
 * destroy those first. NULL is ECUIO_OK. */
ecuio_status_t ecuio_plugin_free(ecuio_plugin_t* plugin);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_PLUGIN_H */
