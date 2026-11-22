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

static const char kNamed[] =
    "{\"havne_signals\":3,\"name\":\"C\",\"source\":{\"name\":\"c\"},"
    "\"ecus\":[{\"id\":\"tcm\",\"rx\":\"7E9\",\"name\":\"Transmission\"}],"
    "\"signals\":[{\"key\":\"a\",\"name\":\"A\",\"read\":{\"service\":\"22\","
    "\"number\":\"D100\"},\"field\":{\"byte\":0}}]}";

int main(void)
{
    ecuio_signal_file_t* file = NULL;
    ecuio_signal_module_t module;

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
    printf("c_abi_merge: ok\n");
    return 0;
}
