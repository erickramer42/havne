/* File/layer: tests/test_c_abi_dump.c | strict C part of test_c_abi
 * Testing: UNIT: ecuio/dump.h's layouts and refusals, compile and link, run as c_abi and c_abi_shared
 */

#include "ecuio/dump.h"
#include "ecuio/core.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "test_c_abi_parts.h"

int c_abi_dump(void)
{
    int ok = 1;
    ecuio_dump_t* dump = (ecuio_dump_t*)&ok;
    ecuio_dump_info_t info;
    ecuio_dump_range_t range;
    ecuio_dump_ident_t ident;
    ecuio_dump_calibration_t calibration;
    ecuio_dump_entry_t entry;
    uint8_t byte = 0;
    size_t count = 7;
    char name[ECUIO_DUMP_NAME_MAX];

    ok = ok && sizeof(ecuio_dump_info_t) == 528u &&
         offsetof(ecuio_dump_info_t, image_bytes) == 16u &&
         offsetof(ecuio_dump_info_t, image_sha256) == 32u &&
         offsetof(ecuio_dump_info_t, rx_id) == 96u &&
         offsetof(ecuio_dump_info_t, core) == 152u &&
         offsetof(ecuio_dump_info_t, plugin_id) == 168u &&
         offsetof(ecuio_dump_info_t, module_sha256) == 248u &&
         offsetof(ecuio_dump_info_t, scheme) == 312u &&
         offsetof(ecuio_dump_info_t, vin) == 456u &&
         offsetof(ecuio_dump_info_t, reserved) == 500u;
    ok = ok && sizeof(ecuio_dump_range_t) == 88u &&
         offsetof(ecuio_dump_range_t, address) == 16u &&
         offsetof(ecuio_dump_range_t, name) == 40u;
    ok = ok && sizeof(ecuio_dump_ident_t) == 96u &&
         offsetof(ecuio_dump_ident_t, value) == 24u;
    ok = ok && sizeof(ecuio_dump_calibration_t) == 48u &&
         offsetof(ecuio_dump_calibration_t, cvn) == 28u;
    ok = ok && sizeof(ecuio_dump_entry_t) == 456u &&
         offsetof(ecuio_dump_entry_t, utc) == 16u &&
         offsetof(ecuio_dump_entry_t, name) == 48u &&
         offsetof(ecuio_dump_entry_t, words) == 304u;
    ok = ok && ECUIO_DUMP_FORMAT == 1u && ECUIO_DUMP_NAME_MAX == 96u &&
         ECUIO_DUMP_KIND_READ == 1u && ECUIO_DUMP_STATE_INVALID == 3u;
    if (!ok) {
        fprintf(stderr, "c_abi: a dump struct's layout moved\n");
        return 1;
    }

    memset(&info, 0, sizeof info);
    memset(&range, 0, sizeof range);
    memset(&ident, 0, sizeof ident);
    memset(&calibration, 0, sizeof calibration);
    info.struct_size = sizeof info;
    ok = ok && ecuio_dump_open(NULL, &dump) == ECUIO_ERR_NULL_ARGUMENT &&
         dump == NULL;
    ok = ok && ecuio_dump_open("", NULL) == ECUIO_ERR_NULL_ARGUMENT;
    ok = ok &&
         ecuio_dump_open("no-such-folder/none.havnedump", &dump) ==
             ECUIO_ERR_LOAD_FAILED &&
         dump == NULL && ecuio_last_error_detail()[0] != '\0';
    ecuio_dump_close(NULL);
    ok = ok && ecuio_dump_get_info(NULL, &info) == ECUIO_ERR_NULL_ARGUMENT;
    ok = ok && ecuio_dump_get_range(NULL, 0, &range) == ECUIO_ERR_NULL_ARGUMENT;
    ok = ok && ecuio_dump_get_ident(NULL, 0, &ident) == ECUIO_ERR_NULL_ARGUMENT;
    ok = ok && ecuio_dump_get_calibration(NULL, 0, &calibration) ==
                   ECUIO_ERR_NULL_ARGUMENT;
    ok = ok &&
         ecuio_dump_read(NULL, 0, &byte, 1, &count) == ECUIO_ERR_NULL_ARGUMENT;
    ok = ok && ecuio_dump_verify(NULL, NULL, NULL, name, sizeof name) ==
                   ECUIO_ERR_NULL_ARGUMENT;
    ok = ok &&
         ecuio_dump_list(NULL, &entry, 1, &count) == ECUIO_ERR_NULL_ARGUMENT;
    ok = ok && ecuio_dump_list("no-such-folder", NULL, 1, &count) ==
                   ECUIO_ERR_NULL_ARGUMENT;
    ok = ok &&
         ecuio_dump_list("no-such-folder", &entry, 1, &count) ==
             ECUIO_ERR_LOAD_FAILED &&
         count == 0u;
    if (!ok) {
        fprintf(stderr, "c_abi: a dump call's refusal moved\n");
        return 1;
    }
    return 0;
}
