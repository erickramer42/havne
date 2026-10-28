/* File/layer: tests/test_c_abi.c | strict C compilation of public headers
 * Testing: UNIT — compile-only guarantee that the C ABI headers are C,
 *          not C++-in-disguise. Without this, `static_assert`
 *          spellings, name mangling surprises, or stray C++ keywords
 *          would only surface at a third-party FFI consumer's build.
 *
 * Built as a real executable linking ecuio-core from a C translation
 * unit: proves both that the headers parse under C and that exported
 * symbols resolve unmangled. Every public header is included, the ones
 * that include others first, so a header missing an include of its own
 * can't hide behind an earlier one. internal.h, j2534_ptypes.h and
 * surrogate_protocol.h aren't public, but they declare C (core's
 * transport seam, the vendor types, the surrogate's wire), so they're
 * compiled here too and can't take a C++-only construct unnoticed;
 * j2534_ptypes.h comes in through j2534_surrogate.h.
 */

#include "ecuio/internal.h"
#include "ecuio/surrogate_protocol.h"
#include "ecuio/j2534_surrogate.h"
#include "ecuio/j2534.h"
#include "ecuio/stream.h"
#include "ecuio/serial.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "ecuio/core.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Negative-array-size idiom — valid in ALL C standards including
 * pedantic C89, complementing the header's _Static_assert (C11+). */
typedef char abi_frame_size_check[(sizeof(ecuio_frame_t) == 4120) ? 1 : -1];
typedef char abi_data_offset_check[(offsetof(ecuio_frame_t, data) == 16) ? 1 : -1];
typedef char abi_data_size_check[(sizeof(((ecuio_frame_t*)0)->data) == ECUIO_FRAME_DATA_MAX) ? 1 : -1];
typedef char abi_flags_offset_check[(offsetof(ecuio_frame_t, flags) == 4112) ? 1 : -1];
typedef char abi_def_size_check[(sizeof(ecuio_signal_def_t) == 120) ? 1 : -1];
typedef char abi_def_kind_check[(offsetof(ecuio_signal_def_t, kind) == 16) ? 1 : -1];
typedef char abi_def_start_byte_check[(offsetof(ecuio_signal_def_t, start_byte) == 18) ? 1 : -1];
typedef char abi_def_byte_length_check[(offsetof(ecuio_signal_def_t, byte_length) == 22) ? 1 : -1];
typedef char abi_def_scale_check[(offsetof(ecuio_signal_def_t, scale) == 24) ? 1 : -1];
typedef char abi_def_name_check[(offsetof(ecuio_signal_def_t, name) == 40) ? 1 : -1];
typedef char abi_def_flags_check[(offsetof(ecuio_signal_def_t, flags) == 92) ? 1 : -1];
typedef char abi_def_reserved_check[(offsetof(ecuio_signal_def_t, reserved) == 96) ? 1 : -1];
typedef char abi_set_layout_check[(offsetof(ecuio_signal_set_t, layout_version) == 0) ? 1 : -1];
typedef char abi_validation_size_check[(sizeof(ecuio_validation_t) == 16) ? 1 : -1];
typedef char abi_validation_index_check[(offsetof(ecuio_validation_t, index) == 4) ? 1 : -1];
typedef char abi_validation_reserved_check[(offsetof(ecuio_validation_t, reserved) == 8) ? 1 : -1];
typedef char abi_info_size_check[(sizeof(ecuio_session_info_t) == 432) ? 1 : -1];
typedef char abi_info_rx_check[(offsetof(ecuio_session_info_t, rx_ids) == 32) ? 1 : -1];
typedef char abi_info_text_check[(offsetof(ecuio_session_info_t, firmware_version) == 64) ? 1 : -1];
typedef char abi_info_profile_check[(offsetof(ecuio_session_info_t, profile) == 304) ? 1 : -1];
typedef char abi_info_replay_check[(offsetof(ecuio_session_info_t, replay_frames) == 368) ? 1 : -1];
typedef char abi_info_reserved_check[(offsetof(ecuio_session_info_t, reserved) == 380) ? 1 : -1];
typedef char abi_dropped_size_check[(sizeof(ecuio_dropped_request_t) == 8) ? 1 : -1];
typedef char abi_support_size_check[(sizeof(ecuio_channel_support_t) == 152) ? 1 : -1];
typedef char abi_support_bitmap_check[(offsetof(ecuio_channel_support_t, pid_bitmap) == 24) ? 1 : -1];
typedef char abi_support_list_check[(offsetof(ecuio_channel_support_t, dropped_list) == 56) ? 1 : -1];
typedef char abi_support_scan_check[(offsetof(ecuio_channel_support_t, scan_total) == 120) ? 1 : -1];
typedef char abi_support_reserved_check[(offsetof(ecuio_channel_support_t, reserved) == 132) ? 1 : -1];
typedef char abi_identity_item_check[(sizeof(ecuio_identity_item_t) == 72) ? 1 : -1];
typedef char abi_identity_size_check[(sizeof(ecuio_channel_identity_t) == 1184) ? 1 : -1];
typedef char abi_identity_list_check[(offsetof(ecuio_channel_identity_t, list) == 32) ? 1 : -1];
typedef char abi_display_size_check[(sizeof(ecuio_display_value_t) == 32) ? 1 : -1];
typedef char abi_display_unit_check[(offsetof(ecuio_display_value_t, unit) == 8) ? 1 : -1];
typedef char abi_display_decimals_check[(offsetof(ecuio_display_value_t, decimal_places) == 24) ? 1 : -1];
typedef char abi_value_value_check[(offsetof(ecuio_value_t, value) == 8) ? 1 : -1];
typedef char abi_value_text_check[(offsetof(ecuio_value_t, text) == 16) ? 1 : -1];
typedef char abi_value_size_check[(sizeof(void*) != 8 || sizeof(ecuio_value_t) == 24) ? 1 : -1];
typedef char abi_batch_values_check[(offsetof(ecuio_value_batch_t, values) == 24) ? 1 : -1];
typedef char abi_batch_size_check[(sizeof(void*) != 8 || sizeof(ecuio_value_batch_t) == 32) ? 1 : -1];
typedef char abi_serial_size_check[(sizeof(ecuio_serial_port_t) == 288) ? 1 : -1];
typedef char abi_serial_device_check[(offsetof(ecuio_serial_port_t, device) == 128) ? 1 : -1];
typedef char abi_serial_flags_check[(offsetof(ecuio_serial_port_t, flags) == 256) ? 1 : -1];
typedef char abi_stream_flags_check[(offsetof(ecuio_stream_callbacks_t, flags) == 4 * sizeof(void*)) ? 1 : -1];
typedef char abi_stream_size_check[(sizeof(void*) != 8 || sizeof(ecuio_stream_callbacks_t) == 56) ? 1 : -1];

int main(void)
{
    /* Zeroed frame must be a plain classic CAN data frame (flag value 0). */
    ecuio_frame_t frame;
    const ecuio_signal_set_t* set = NULL;
    ecuio_validation_t validation;
    ecuio_session_info_t info;
    ecuio_channel_support_t support;
    ecuio_channel_identity_t identity;
    ecuio_display_value_t shown;
    ecuio_sur_request_t sur_request;
    ecuio_transport_ops_t ops;
    ecuio_stream_callbacks_t stream_cb;
    ecuio_stream_t* stream = NULL;
    uint32_t serial_count = 0;
    const uint8_t vin_request[2] = {0x09, 0x02};
    frame.arbitration_id = 0x7FF;
    frame.channel_id = 0;
    frame.data_length = 8;
    frame.flags = ECUIO_FRAME_FLAG_EXTENDED;
    frame.reserved = 0;

    /* Exercise exported symbols from each header — catches mangling
     * regressions at link time. */
    printf("%s %s\n", ecuio_version_string(),
           ecuio_status_string(ECUIO_ERR_SESSION_CLOSED));
    printf("detail '%s'\n", ecuio_last_error_detail());
    if (ecuio_validate_signal_set(ecuio_mock_signal_set(), &validation) != ECUIO_VALIDATE_OK ||
        validation.code != ECUIO_VALIDATE_OK || validation.index != UINT32_MAX ||
        ecuio_mock_signal_set()->layout_version != ECUIO_SIGNAL_SET_LAYOUT) {
        return 1;
    }
    if (ecuio_profile_lookup("raw", &set) != ECUIO_OK || set != NULL) {
        return 1;
    }
    if (ecuio_profile_register(NULL, NULL, &validation) != ECUIO_ERR_NULL_ARGUMENT ||
        validation.code != 0 || validation.index != UINT32_MAX) {
        return 1;
    }
    if (ecuio_session_get_info(NULL, &info) != ECUIO_ERR_NULL_ARGUMENT) {
        return 1;
    }
    if (ecuio_register_values_callback(NULL, NULL, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_unregister_values_callback(NULL, NULL, NULL) != ECUIO_ERR_NULL_ARGUMENT) {
        return 1;
    }
    if (ecuio_decode_text(NULL, NULL, 0, NULL, 0) != ECUIO_DECODE_ERR_NULL) {
        return 1;
    }
    if (ecuio_display_convert("km/h", 0, 100.0, ECUIO_UNITS_IMPERIAL, &shown) !=
            ECUIO_DISPLAY_OK ||
        strcmp(shown.unit, "mph") != 0 || shown.converted != 1) {
        return 1;
    }
    if (ecuio_session_get_channel_support(NULL, 0, &support) !=
        ECUIO_ERR_NULL_ARGUMENT) {
        return 1;
    }
    if (ecuio_session_get_channel_identity(NULL, 0, &identity) !=
        ECUIO_ERR_NULL_ARGUMENT ||
        strcmp(ecuio_identity_request_name(vin_request, 2), "VIN") != 0 ||
        ecuio_identity_request_name(vin_request, 1)[0] != '\0') {
        return 1;
    }
    memset(&sur_request, 0, sizeof sur_request);
    memset(&ops, 0, sizeof ops);
    if (sur_request.opcode != 0 || ops.start != NULL) {
        return 1;
    }
    if (ecuio_serial_ports(NULL, 0, &serial_count) != ECUIO_OK ||
        ecuio_serial_ports(NULL, 1, &serial_count) != ECUIO_ERR_NULL_ARGUMENT) {
        return 1;
    }
    memset(&stream_cb, 0, sizeof stream_cb);
    if (ecuio_stream_register("bt:x", &stream_cb, &stream) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_stream_unregister(NULL) != ECUIO_ERR_NULL_ARGUMENT || stream != NULL) {
        return 1;
    }
    ecuio_stream_opened(NULL);
    ecuio_stream_received(NULL, NULL, 0);
    ecuio_stream_failed(NULL, NULL);
    ecuio_j2534_free_devices(NULL);
    printf("log '%s'\n", ecuio_j2534_sur_log_path(NULL));
    (void)frame;
    return 0;
}
