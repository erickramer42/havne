/* File/layer: tests/test_c_abi_merge.c | strict C compilation of signals.h
 * Testing: UNIT: c_abi_merge, and c_abi_merge_shared against the shared
 *          core: the structs and calls of a set made of several files
 *          (ADR-0065 section 9) parse as C, keep their sizes on both
 *          pointer widths and link unmangled.
 *
 * A translation unit of its own beside test_c_abi.c, which is held to
 * its length (scripts/filelength-baseline.txt). */

#include "ecuio/signals.h"
#include "ecuio/core.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Negative-array-size idiom, as test_c_abi.c's: valid in every C standard */
typedef char abi_module_size_check[(sizeof(ecuio_signal_module_t) == 136) ? 1 : -1];
typedef char abi_module_rx_check[(offsetof(ecuio_signal_module_t, rx_id) == 8) ? 1 : -1];
typedef char abi_module_part_check[(offsetof(ecuio_signal_module_t, part) == 16) ? 1 : -1];
typedef char abi_module_reserved_check[(offsetof(ecuio_signal_module_t, reserved) == 20) ? 1 : -1];
typedef char abi_module_id_check[(offsetof(ecuio_signal_module_t, id) == 40) ? 1 : -1];
typedef char abi_module_name_check[(offsetof(ecuio_signal_module_t, name) == 72) ? 1 : -1];
typedef char abi_part_size_check[(sizeof(ecuio_signal_part_t) == 248) ? 1 : -1];
typedef char abi_part_format_check[(offsetof(ecuio_signal_part_t, format) == 8)
                                       ? 1
                                       : -1];
typedef char abi_part_reserved_check
    [(offsetof(ecuio_signal_part_t, reserved) == 20) ? 1 : -1];
typedef char
    abi_part_profile_check[(offsetof(ecuio_signal_part_t, profile) == 40) ? 1
                                                                          : -1];
typedef char
    abi_part_name_check[(offsetof(ecuio_signal_part_t, name) == 104) ? 1 : -1];
typedef char abi_part_plugin_check
    [(offsetof(ecuio_signal_part_t, plugin_id) == 168) ? 1 : -1];
typedef char abi_part_version_check
    [(offsetof(ecuio_signal_part_t, plugin_version) == 200) ? 1 : -1];
typedef char abi_part_variant_check
    [(offsetof(ecuio_signal_part_t, plugin_variant) == 216) ? 1 : -1];
typedef char abi_note_size_check[(sizeof(ecuio_signal_note_t) == 424) ? 1 : -1];
typedef char abi_note_kind_check[(offsetof(ecuio_signal_note_t, kind) == 4) ? 1 : -1];
typedef char abi_note_def_check[(offsetof(ecuio_signal_note_t, def) == 16) ? 1 : -1];
typedef char abi_note_reserved_check[(offsetof(ecuio_signal_note_t, reserved) == 24) ? 1 : -1];
typedef char abi_note_text_check[(offsetof(ecuio_signal_note_t, text) == 40) ? 1 : -1];
typedef char abi_note_text_size_check[(sizeof(((ecuio_signal_note_t*)0)->text) == ECUIO_NOTE_TEXT_LEN) ? 1 : -1];

static const char kNamed[] =
    "{\"havne_signals\":3,\"name\":\"C\",\"source\":{\"name\":\"c\"},"
    "\"ecus\":[{\"id\":\"tcm\",\"rx\":\"7E9\",\"name\":\"Transmission\"}],"
    "\"signals\":[{\"key\":\"a\",\"name\":\"A\",\"read\":{\"service\":\"22\","
    "\"number\":\"D100\"},\"field\":{\"byte\":0}}]}";

/* One computed signal over a key another file provides */
static const char kNeeding[] =
    "{\"havne_signals\":3,\"name\":\"N\",\"source\":{\"name\":\"n\"},"
    "\"needs\":[\"oil_t\"],\"signals\":[{\"key\":\"margin\",\"name\":"
    "\"Margin\",\"kind\":\"computed\",\"expr\":\"130 - oil_t\"}]}";

/* A merged set of one plugin's part: b took the place of a */
static const char kMerged[] =
    "{\"havne_signals\":3,\"name\":\"M\",\"source\":{\"name\":\"m\"},"
    "\"parts\":[{\"profile\":\"plugin.p.base\",\"name\":\"P\",\"format\":2,"
    "\"plugin\":{\"id\":\"p\",\"version\":\"1.2.3\",\"variant\":\"base\"},"
    "\"source\":{\"name\":\"s\",\"license\":\"CC-BY-SA-4.0\"}}],"
    "\"ecus\":[{\"id\":\"tcm\",\"rx\":\"7E9\",\"part\":0}],"
    "\"signals\":[{\"key\":\"b\",\"part\":0,\"replaced\":[\"a\"],\"name\":"
    "\"B\","
    "\"read\":{\"service\":\"22\",\"number\":\"D100\"},\"field\":{\"byte\":0}}]"
    "}";

static int merged_text_from_c(void)
{
    ecuio_signal_file_t* file = NULL;
    ecuio_signal_part_t part;
    ecuio_signal_source_t source;
    ecuio_signal_key_t key;
    const ecuio_signal_def_t* def;
    uint32_t count = 0;

    if (ECUIO_MERGE_MAX_PARTS != 16u || ECUIO_PART_PLUGIN != 0x1u ||
        ECUIO_SOURCE_MERGED != 0x2u) {
        printf("FAIL: a constant of a merged set's parts moved\n");
        return 1;
    }
    if (ecuio_signal_file_load(kMerged, sizeof kMerged - 1, &file) !=
            ECUIO_OK ||
        ecuio_signal_file_part_count(file) != 1u) {
        printf("FAIL: a merged text from C: %s\n", ecuio_last_error_detail());
        return 1;
    }
    memset(&part, 0, sizeof part);
    part.struct_size = sizeof part;
    memset(&source, 0, sizeof source);
    source.struct_size = sizeof source;
    if (ecuio_signal_file_part(file, 0, &part) != ECUIO_OK ||
        part.flags != ECUIO_PART_PLUGIN || part.format != 2u ||
        part.signal_count != 1u || part.module_count != 1u ||
        strcmp(part.profile, "plugin.p.base") != 0 ||
        strcmp(part.plugin_version, "1.2.3") != 0 ||
        strcmp(part.plugin_variant, "base") != 0 ||
        ecuio_signal_file_part_source(file, 0, &source) != ECUIO_OK ||
        source.flags != ECUIO_SOURCE_SHARE_ALIKE ||
        ecuio_signal_file_part(file, 1, &part) != ECUIO_ERR_INVALID_ARGUMENT ||
        ecuio_signal_file_part(NULL, 0, &part) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_signal_file_part_source(file, 1, &source) !=
            ECUIO_ERR_INVALID_ARGUMENT) {
        printf("FAIL: a part from C: %s\n", ecuio_last_error_detail());
        return 1;
    }
    def = ecuio_signal_file_find(file, "a");
    if (def == NULL || def != ecuio_signal_file_find(file, "b") ||
        strcmp(ecuio_signal_file_key(file, def), "b") != 0 ||
        ecuio_signal_file_def_part(file, def) != 0u ||
        ecuio_signal_file_def_part(file, NULL) != ECUIO_SIGNAL_PART_NONE ||
        ecuio_signal_file_replaced_keys(file, def, &key, 1, &count) !=
            ECUIO_OK ||
        count != 1u || strcmp(key.text, "a") != 0 ||
        ecuio_signal_file_replaced_keys(file, def, NULL, 1, &count) !=
            ECUIO_ERR_NULL_ARGUMENT) {
        printf("FAIL: a replaced key from C\n");
        return 1;
    }
    ecuio_signal_file_free(file);
    return 0;
}

typedef char
    abi_options_size_check[(sizeof(ecuio_merge_options_t) == 32) ? 1 : -1];
typedef char abi_options_reserved_check
    [(offsetof(ecuio_merge_options_t, reserved) == 8) ? 1 : -1];

/* Another file's signal at kNamed's address */
static const char kLater[] =
    "{\"havne_signals\":3,\"name\":\"L\",\"source\":{\"name\":\"l\"},"
    "\"ecus\":[{\"id\":\"trans\",\"rx\":\"7E9\"}],"
    "\"signals\":[{\"key\":\"b\",\"name\":\"B\",\"read\":{\"service\":\"22\","
    "\"number\":\"D100\"},\"field\":{\"byte\":0}}]}";

static int merge_from_c(void)
{
    ecuio_signal_file_t* files[2] = {NULL, NULL};
    const ecuio_signal_file_t* parts[2];
    ecuio_signal_file_t* merged = NULL;
    ecuio_merge_options_t options;
    ecuio_signal_note_t note;
    int failed = 0;

    if (ECUIO_MERGE_BASE_OBD2 != 0x1u || ECUIO_NOTE_REPLACED_BY_ADDRESS != 1u ||
        ECUIO_NOTE_REPLACED_BY_KEY != 2u || ECUIO_NOTE_KEY_TAKEN != 3u ||
        ECUIO_NOTE_BESIDE_BASE != 4u || ECUIO_NOTE_MODULE_RENAMED != 5u ||
        ECUIO_NOTE_MODULE_LEFT_OUT != 6u || ECUIO_NOTE_PART_LEFT_OUT != 7u ||
        ECUIO_NOTE_SIGNAL_LEFT_OUT != 8u) {
        printf("FAIL: a constant of the merge moved\n");
        return 1;
    }
    if (ecuio_signal_file_load(kNamed, sizeof kNamed - 1, &files[0]) !=
            ECUIO_OK ||
        ecuio_signal_file_load(kLater, sizeof kLater - 1, &files[1]) !=
            ECUIO_OK) {
        printf("FAIL: the parts from C: %s\n", ecuio_last_error_detail());
        return 1;
    }
    parts[0] = files[0];
    parts[1] = files[1];
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    memset(&note, 0, sizeof note);
    note.struct_size = sizeof note;
    if (ecuio_signal_file_merge(parts, 2, &options, &merged) != ECUIO_OK ||
        ecuio_signal_file_part_count(merged) != 2u ||
        ecuio_signal_file_find(merged, "a") == NULL ||
        ecuio_signal_file_find(merged, "a") !=
            ecuio_signal_file_find(merged, "b") ||
        ecuio_signal_file_note_count(merged) != 1u ||
        ecuio_signal_file_note(merged, 0, &note) != ECUIO_OK ||
        note.kind != ECUIO_NOTE_REPLACED_BY_ADDRESS || note.part != 1u ||
        note.other_part != 0u || note.def != 0u) {
        printf("FAIL: a merge from C: %s\n", ecuio_last_error_detail());
        failed = 1;
    }
    ecuio_signal_file_free(merged);
    merged = NULL;
    options.flags = 0x80u;
    if (!failed && (ecuio_signal_file_merge(parts, 2, &options, &merged) !=
                        ECUIO_ERR_INVALID_ARGUMENT ||
                    merged != NULL ||
                    ecuio_signal_file_merge(parts, 2, NULL, NULL) !=
                        ECUIO_ERR_NULL_ARGUMENT)) {
        printf("FAIL: the merge's refusals from C\n");
        failed = 1;
    }
    ecuio_signal_file_free(files[0]);
    ecuio_signal_file_free(files[1]);
    return failed;
}

int main(void)
{
    ecuio_signal_file_t* file = NULL;
    ecuio_signal_module_t module;
    ecuio_signal_note_t note;
    ecuio_signal_key_t need;
    uint32_t count = 0;

    if (ECUIO_SIGNAL_FILE_FORMAT != 3u ||
        ECUIO_SIGNAL_PART_NONE != 0xFFFFFFFFu || ECUIO_MODULE_BASE != 0x1u) {
        printf("FAIL: a constant of the merged set moved\n");
        return 1;
    }
    if (ecuio_signal_file_load(kNamed, sizeof kNamed - 1, &file) != ECUIO_OK ||
        ecuio_signal_file_format(file) != 3u) {
        printf("FAIL: a format 3 file from C: %s\n", ecuio_last_error_detail());
        return 1;
    }
    memset(&module, 0, sizeof module);
    module.struct_size = sizeof module;
    if (ecuio_signal_file_module(file, 0, &module) != ECUIO_OK ||
        module.rx_id != 0x7E9u || module.tx_id != 0x7E1u ||
        module.flags != 0u || module.part != ECUIO_SIGNAL_PART_NONE ||
        strcmp(module.id, "tcm") != 0 ||
        strcmp(module.name, "Transmission") != 0) {
        printf("FAIL: a module from C: %s\n", ecuio_last_error_detail());
        return 1;
    }
    if (ecuio_signal_file_module(file, 1, &module) != ECUIO_ERR_INVALID_ARGUMENT ||
        ecuio_signal_file_module(NULL, 0, &module) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_signal_file_module(file, 0, NULL) != ECUIO_ERR_NULL_ARGUMENT) {
        printf("FAIL: the module getter's refusals from C\n");
        return 1;
    }
    ecuio_signal_file_free(file);

    file = NULL;
    if (ecuio_signal_file_load(kNeeding, sizeof kNeeding - 1, &file) != ECUIO_OK ||
        ecuio_signal_file_needs(file, &need, 1, &count) != ECUIO_OK ||
        count != 1u || strcmp(need.text, "oil_t") != 0 ||
        ecuio_signal_file_note_count(file) != 1u) {
        printf("FAIL: a file's needs from C: %s\n", ecuio_last_error_detail());
        return 1;
    }
    memset(&note, 0, sizeof note);
    note.struct_size = sizeof note;
    if (ecuio_signal_file_note(file, 0, &note) != ECUIO_OK ||
        note.kind != ECUIO_NOTE_SIGNAL_LEFT_OUT ||
        note.part != ECUIO_SIGNAL_PART_NONE || note.def != 0xFFFFFFFFu ||
        strstr(note.text, "is left out") == NULL ||
        ecuio_signal_file_note(file, 1, &note) != ECUIO_ERR_INVALID_ARGUMENT ||
        ecuio_signal_file_note(NULL, 0, &note) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_signal_file_needs(NULL, &need, 1, &count) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_signal_file_note_count(NULL) != 0u) {
        printf("FAIL: a load's notes from C: %s\n", ecuio_last_error_detail());
        return 1;
    }
    ecuio_signal_file_free(file);
    if (merged_text_from_c() != 0 || merge_from_c() != 0) {
        return 1;
    }
    printf("c_abi_merge: ok\n");
    return 0;
}
