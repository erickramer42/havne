/* File/layer: tests/test_c_abi.c | strict C compilation of public headers
 * Testing: UNIT: compile-only guarantee that the C ABI headers are C,
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
 *
 * c_abi_shared builds this file again against the shared core
 * (HAVNE_SHARED_CORE, ADR-0053), which exports the public headers'
 * functions and nothing else: there each call resolves through the
 * library's export table, and the calls it never exports (internal.h's
 * test hooks, the surrogate client's log path) are left out
 * (ECUIO_C_ABI_SHARED, INTERNAL()).
 */

#include "ecuio/queue.h"
#include "ecuio/obdb.h"
#include "ecuio/recording.h"
#include "ecuio/internal.h"
#include "ecuio/surrogate_protocol.h"
#include "ecuio/j2534_surrogate.h"
#include "ecuio/j2534.h"
#include "ecuio/stream.h"
#include "ecuio/serial.h"
#include "ecuio/plugin.h"
#include "ecuio/vin.h"
#include "ecuio/sim.h"
#include "ecuio/socketcan.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "ecuio/core.h"
#include "ecuio/version.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* An internal hook's call, or the value it gives in the archive's build
 * where the shared core has no such export */
#ifdef ECUIO_C_ABI_SHARED
#define INTERNAL(call, value) (value)
#else
#define INTERNAL(call, value) (call)
#endif

/* Negative-array-size idiom: valid in ALL C standards including
 * pedantic C89, complementing the header's _Static_assert (C11+). */
typedef char abi_frame_size_check[(sizeof(ecuio_frame_t) == 4120) ? 1 : -1];
typedef char abi_data_offset_check[(offsetof(ecuio_frame_t, data) == 16) ? 1 : -1];
typedef char abi_replay_faults_size_check[(sizeof(ecuio_replay_faults_t) == 64) ? 1 : -1];
typedef char abi_replay_faults_stalled_check[(offsetof(ecuio_replay_faults_t, stalled) == 40) ? 1 : -1];
typedef char abi_data_size_check[(sizeof(((ecuio_frame_t*)0)->data) == ECUIO_FRAME_DATA_MAX) ? 1 : -1];
typedef char abi_flags_offset_check[(offsetof(ecuio_frame_t, flags) == 4112) ? 1 : -1];
typedef char abi_def_size_check[(sizeof(ecuio_signal_def_t) == 120) ? 1 : -1];
typedef char abi_def_kind_check[(offsetof(ecuio_signal_def_t, kind) == 16) ? 1 : -1];
typedef char abi_def_start_byte_check[(offsetof(ecuio_signal_def_t, start_byte) == 18) ? 1 : -1];
typedef char abi_def_byte_length_check[(offsetof(ecuio_signal_def_t, byte_length) == 22) ? 1 : -1];
typedef char abi_def_scale_check[(offsetof(ecuio_signal_def_t, scale) == 24) ? 1 : -1];
typedef char abi_def_name_check[(offsetof(ecuio_signal_def_t, name) == 40) ? 1 : -1];
typedef char abi_def_plot_rank_check[(offsetof(ecuio_signal_def_t, plot_rank) == 89) ? 1 : -1];
typedef char abi_def_group_check[(offsetof(ecuio_signal_def_t, group) == 90) ? 1 : -1];
typedef char abi_def_flags_check[(offsetof(ecuio_signal_def_t, flags) == 92) ? 1 : -1];
typedef char abi_def_quantity_check[(offsetof(ecuio_signal_def_t, quantity_reserved) == 96) ? 1 : -1];
typedef char abi_def_table_first_check[(offsetof(ecuio_signal_def_t, table_first) == 100) ? 1 : -1];
typedef char abi_def_table_count_check[(offsetof(ecuio_signal_def_t, table_count) == 102) ? 1 : -1];
typedef char abi_def_reserved_check[(offsetof(ecuio_signal_def_t, reserved) == 104) ? 1 : -1];
typedef char abi_set_layout_check[(offsetof(ecuio_signal_set_t, layout_version) == 0) ? 1 : -1];
/* The set's members sit at natural alignment on both widths (layout 4) */
typedef char abi_set_enum_count_check[(offsetof(ecuio_signal_set_t, enum_count) == 2 * sizeof(void*) + 16) ? 1 : -1];
typedef char abi_set_enums_check[(offsetof(ecuio_signal_set_t, enums) == 2 * sizeof(void*) + 24) ? 1 : -1];
typedef char abi_set_reserved_check[(offsetof(ecuio_signal_set_t, reserved) == 4 * sizeof(void*) + 24) ? 1 : -1];
typedef char abi_set_size_check[(sizeof(ecuio_signal_set_t) == 4 * sizeof(void*) + 40) ? 1 : -1];
typedef char abi_obdb_options_reserved_check[(offsetof(ecuio_obdb_options_t, reserved) == 8) ? 1 : -1];
typedef char abi_obdb_options_name_check[(offsetof(ecuio_obdb_options_t, name) == 32) ? 1 : -1];
typedef char abi_obdb_options_commit_check[(offsetof(ecuio_obdb_options_t, commit) == 32 + 2 * sizeof(void*)) ? 1 : -1];
typedef char abi_obdb_note_signal_check[(offsetof(ecuio_obdb_note_t, signal) == 8) ? 1 : -1];
typedef char abi_obdb_note_text_check[(offsetof(ecuio_obdb_note_t, text) == 8 + 3 * sizeof(void*)) ? 1 : -1];
typedef char abi_enum_entry_size_check[(sizeof(ecuio_enum_entry_t) == 32) ? 1 : -1];
typedef char abi_enum_label_check[(offsetof(ecuio_enum_entry_t, label) == 4) ? 1 : -1];
typedef char abi_computed_size_check[(sizeof(ecuio_computed_t) == 208) ? 1 : -1];
typedef char abi_computed_ages_check[(offsetof(ecuio_computed_t, max_age_us) == 8) ? 1 : -1];
typedef char abi_computed_steps_check[(offsetof(ecuio_computed_t, steps) == 28) ? 1 : -1];
typedef char abi_computed_constants_check[(offsetof(ecuio_computed_t, constants) == 64) ? 1 : -1];
typedef char abi_computed_reserved_check[(offsetof(ecuio_computed_t, reserved) == 192) ? 1 : -1];
typedef char abi_validation_size_check[(sizeof(ecuio_validation_t) == 16) ? 1 : -1];
typedef char abi_validation_index_check[(offsetof(ecuio_validation_t, index) == 4) ? 1 : -1];
typedef char abi_validation_reserved_check[(offsetof(ecuio_validation_t, reserved) == 8) ? 1 : -1];
typedef char abi_info_size_check[(sizeof(ecuio_session_info_t) == 432) ? 1 : -1];
typedef char abi_info_rx_check[(offsetof(ecuio_session_info_t, rx_ids) == 32) ? 1 : -1];
typedef char abi_info_text_check[(offsetof(ecuio_session_info_t, firmware_version) == 64) ? 1 : -1];
typedef char abi_info_profile_check[(offsetof(ecuio_session_info_t, profile) == 304) ? 1 : -1];
typedef char abi_info_replay_check[(offsetof(ecuio_session_info_t, replay_frames) == 368) ? 1 : -1];
typedef char abi_info_torn_check[(offsetof(ecuio_session_info_t, replay_rows_torn) == 380) ? 1 : -1];
typedef char abi_info_reserved_check[(offsetof(ecuio_session_info_t, reserved) == 384) ? 1 : -1];
typedef char abi_dropped_size_check[(sizeof(ecuio_dropped_request_t) == 8) ? 1 : -1];
typedef char abi_support_size_check[(sizeof(ecuio_channel_support_t) == 152) ? 1 : -1];
typedef char abi_support_bitmap_check[(offsetof(ecuio_channel_support_t, pid_bitmap) == 24) ? 1 : -1];
typedef char abi_support_list_check[(offsetof(ecuio_channel_support_t, dropped_list) == 56) ? 1 : -1];
typedef char abi_support_scan_check[(offsetof(ecuio_channel_support_t, scan_total) == 120) ? 1 : -1];
typedef char abi_support_pids_check[(offsetof(ecuio_channel_support_t, pids_supported) == 132) ? 1 : -1];
typedef char abi_support_reserved_check[(offsetof(ecuio_channel_support_t, reserved) == 136) ? 1 : -1];
typedef char abi_identity_item_check[(sizeof(ecuio_identity_item_t) == 72) ? 1 : -1];
typedef char abi_identity_size_check[(sizeof(ecuio_channel_identity_t) == 1184) ? 1 : -1];
typedef char abi_identity_list_check[(offsetof(ecuio_channel_identity_t, list) == 32) ? 1 : -1];
typedef char abi_dtc_list_check[(sizeof(ecuio_dtc_list_t) == 68) ? 1 : -1];
typedef char abi_monitor_check[(sizeof(ecuio_monitor_t) == 4) ? 1 : -1];
typedef char abi_freeze_value_check[(sizeof(ecuio_freeze_value_t) == 16) ? 1 : -1];
typedef char abi_diag_size_check[(sizeof(ecuio_channel_diagnostics_t) == 552) ? 1 : -1];
typedef char abi_diag_clear_check[(offsetof(ecuio_channel_diagnostics_t, clear) == 21) ? 1 : -1];
typedef char abi_diag_cycle_check[(offsetof(ecuio_channel_diagnostics_t, cycle) == 23) ? 1 : -1];
typedef char abi_diag_clear_rpm_check[(offsetof(ecuio_channel_diagnostics_t, clear_rpm) == 24) ? 1 : -1];
typedef char abi_diag_clear_kmh_check[(offsetof(ecuio_channel_diagnostics_t, clear_kmh) == 28) ? 1 : -1];
typedef char abi_diag_lists_check[(offsetof(ecuio_channel_diagnostics_t, lists) == 40) ? 1 : -1];
typedef char abi_diag_monitors_check[(offsetof(ecuio_channel_diagnostics_t, monitors) == 244) ? 1 : -1];
typedef char abi_diag_freeze_check[(offsetof(ecuio_channel_diagnostics_t, freeze_values) == 296) ? 1 : -1];
typedef char abi_display_size_check[(sizeof(ecuio_display_value_t) == 32) ? 1 : -1];
typedef char abi_display_unit_check[(offsetof(ecuio_display_value_t, unit) == 8) ? 1 : -1];
typedef char abi_display_decimals_check[(offsetof(ecuio_display_value_t, decimal_places) == 24) ? 1 : -1];
typedef char abi_value_value_check[(offsetof(ecuio_value_t, value) == 8) ? 1 : -1];
typedef char abi_value_text_check[(offsetof(ecuio_value_t, text) == 16) ? 1 : -1];
typedef char abi_value_size_check[(sizeof(void*) != 8 || sizeof(ecuio_value_t) == 24) ? 1 : -1];
typedef char abi_batch_values_check[(offsetof(ecuio_value_batch_t, values) == 24) ? 1 : -1];
typedef char abi_batch_size_check[(sizeof(void*) != 8 || sizeof(ecuio_value_batch_t) == 32) ? 1 : -1];
typedef char abi_session_timing_size_check[(sizeof(ecuio_session_timing_t) == 64) ? 1 : -1];
typedef char abi_request_timing_size_check[(sizeof(ecuio_request_timing_t) == 80) ? 1 : -1];
typedef char abi_request_timing_intervals_check[(offsetof(ecuio_request_timing_t, intervals) == 44) ? 1 : -1];
typedef char abi_serial_size_check[(sizeof(ecuio_serial_port_t) == 288) ? 1 : -1];
typedef char abi_serial_device_check[(offsetof(ecuio_serial_port_t, device) == 128) ? 1 : -1];
typedef char abi_serial_flags_check[(offsetof(ecuio_serial_port_t, flags) == 256) ? 1 : -1];
typedef char abi_inspection_size_check[(sizeof(ecuio_inspection_t) == 24) ? 1 : -1];
typedef char abi_inspection_flags_check[(offsetof(ecuio_inspection_t, diag_flags) == 8) ? 1 : -1];
typedef char abi_sim_stats_size_check[(sizeof(ecuio_sim_stats_t) == 96) ? 1 : -1];
typedef char abi_sim_stats_flags_check[(offsetof(ecuio_sim_stats_t, flags) == 72) ? 1 : -1];
typedef char abi_sim_stats_pending_check[(offsetof(ecuio_sim_stats_t, pending) == 80) ? 1 : -1];
typedef char
    abi_sim_reply_size_check[(sizeof(ecuio_sim_reply_t) == 4136) ? 1 : -1];
typedef char abi_sim_reply_delay_check
    [(offsetof(ecuio_sim_reply_t, delay_us) == 8) ? 1 : -1];
typedef char abi_sim_reply_length_check
    [(offsetof(ecuio_sim_reply_t, length) == 16) ? 1 : -1];
typedef char abi_sim_reply_data_check[(offsetof(ecuio_sim_reply_t, data) == 40)
                                          ? 1
                                          : -1];
typedef char abi_vin_size_check[(sizeof(ecuio_vin_info_t) == 32) ? 1 : -1];
typedef char abi_vin_wmi_check[(offsetof(ecuio_vin_info_t, wmi) == 8) ? 1 : -1];
typedef char abi_plugin_info_size_check[(sizeof(ecuio_plugin_info_t) == 136) ? 1 : -1];
typedef char abi_plugin_info_count_check[(offsetof(ecuio_plugin_info_t, variant_count) == 120) ? 1 : -1];
typedef char abi_plugin_value_line_check[(offsetof(ecuio_plugin_value_t, line) == 12) ? 1 : -1];
typedef char abi_plugin_value_signal_check[(offsetof(ecuio_plugin_value_t, signal) == 16) ? 1 : -1];
typedef char abi_plugin_value_def_check[(offsetof(ecuio_plugin_value_t, def) == 64) ? 1 : -1];
typedef char abi_can_size_check[(sizeof(ecuio_can_interface_t) == 64) ? 1 : -1];
typedef char abi_can_flags_check[(offsetof(ecuio_can_interface_t, flags) == 32) ? 1 : -1];
typedef char abi_can_state_check[(offsetof(ecuio_can_interface_t, state) == 40) ? 1 : -1];
typedef char abi_can_modes_check[(offsetof(ecuio_can_interface_t, modes_supported) == 48) ? 1 : -1];
typedef char abi_stream_flags_check[(offsetof(ecuio_stream_callbacks_t, flags) == 4 * sizeof(void*)) ? 1 : -1];
typedef char abi_stream_size_check[(sizeof(void*) != 8 || sizeof(ecuio_stream_callbacks_t) == 56) ? 1 : -1];
typedef char abi_queue_value_size_check[(sizeof(ecuio_queue_value_t) == 160) ? 1 : -1];
typedef char abi_queue_value_value_check[(offsetof(ecuio_queue_value_t, value) == 16) ? 1 : -1];
typedef char abi_queue_value_len_check[(offsetof(ecuio_queue_value_t, text_len) == 24) ? 1 : -1];
typedef char abi_queue_value_text_check[(offsetof(ecuio_queue_value_t, text) == 32) ? 1 : -1];

int main(void)
{
    /* Zeroed frame must be a plain classic CAN data frame (flag value 0). */
    ecuio_frame_t frame;
    const ecuio_signal_set_t* set = NULL;
    ecuio_validation_t validation;
    ecuio_session_info_t info;
    ecuio_channel_support_t support;
    ecuio_channel_identity_t identity;
    ecuio_channel_diagnostics_t diagnostics;
    ecuio_display_value_t shown;
    ecuio_sur_request_t sur_request;
    ecuio_transport_ops_t ops;
    ecuio_stream_callbacks_t stream_cb;
    ecuio_stream_t* stream = NULL;
    uint32_t serial_count = 0;
    uint32_t can_count = 0;
    ecuio_vin_info_t vin_info;
    ecuio_sim_t* sim = NULL;
    ecuio_queue_t* queue = NULL;
    ecuio_queue_value_t queue_value;
    uint32_t queue_count = 0;
    uint64_t queue_dropped = 0;
    ecuio_sim_stats_t sim_stats;
    ecuio_inspection_t inspection;
    ecuio_record_options_t record_options;
    ecuio_recorder_t* recorder = NULL;
    ecuio_recorder_state_t recorder_state;
    uint32_t folder_len = 0;
    ecuio_recording_t* reading = NULL;
    ecuio_recording_info_t reading_info;
    ecuio_recording_signals_t reading_signals;
    ecuio_recording_signal_t reading_signal;
    ecuio_recording_marker_t reading_marker;
    ecuio_recording_column_t reading_column;
    ecuio_recording_find_t reading_find;
    uint64_t reading_at = 0;
    uint32_t reading_pick = 0;
    char reading_text[8];
    ecuio_check_options_t check_options;
    ecuio_check_summary_t check_summary;
    ecuio_export_options_t export_options;
    ecuio_export_report_t export_report;
    ecuio_import_options_t import_options;
    ecuio_import_report_t import_report;
    ecuio_csv_guess_t csv_guess;
    size_t csv_guess_needed = 1;
    ecuio_health_options_t health_options;
    ecuio_health_report_t health_report;
    ecuio_frames_read_t frames_read;
    const uint8_t vin_request[2] = {0x09, 0x02};
    char dtc[6];
    uint32_t version_major = 0, version_minor = 0, version_patch = 0;
    frame.arbitration_id = 0x7FF;
    frame.channel_id = 0;
    frame.data_length = 8;
    frame.flags = ECUIO_FRAME_FLAG_EXTENDED;
    frame.reserved = 0;

    /* Exercise exported symbols from each header: catches mangling
     * regressions at link time. */
    printf("%s %s\n", ecuio_version_string(),
           ecuio_status_string(ECUIO_ERR_SESSION_CLOSED));
    if (ecuio_version(&version_major, &version_minor, &version_patch) !=
            ECUIO_OK ||
        version_major != ECUIO_VERSION_MAJOR ||
        version_minor != ECUIO_VERSION_MINOR ||
        version_patch != ECUIO_VERSION_PATCH ||
        ecuio_version(NULL, &version_minor, &version_patch) !=
            ECUIO_ERR_NULL_ARGUMENT) {
        return 1;
    }
    printf("detail '%s'\n", ecuio_last_error_detail());
    printf("clock %llu slept %llu\n", (unsigned long long)ecuio_clock_us(),
           (unsigned long long)ecuio_clock_slept_us());
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
    if (ecuio_register_values_callback(NULL, NULL, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_unregister_values_callback(NULL, NULL, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_get_absent_signals(NULL, NULL, 0, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_get_unsupported_signals(NULL, NULL, 0, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_identity_vin(NULL, NULL, ECUIO_VIN_TEXT_LEN) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_get_unsplit_replies(NULL, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_get_replay_faults(NULL, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_get_timing(NULL, NULL, NULL, 0, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        sizeof(ECUIO_TIMING_COLUMNS) < 2u ||
        (ECUIO_SESSION_INFO_REPLAY_FAULTS & ECUIO_SESSION_INFO_LISTEN_ONLY) != 0u ||
        ECUIO_ABSENT_AFTER_FRAMES != 3u) {
        return 1;
    }
    if (ecuio_decode_text(NULL, NULL, 0, NULL, 0) != ECUIO_DECODE_ERR_NULL) {
        return 1;
    }
    if (strcmp(ecuio_signal_group_name(ECUIO_SIGNAL_GROUP_DIAGNOSTICS),
               "Diagnostics") != 0) {
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
    if (ecuio_dtc_format(0x0301, dtc, sizeof dtc) != ECUIO_OK ||
        strcmp(dtc, "P0301") != 0 ||
        strcmp(ecuio_monitor_name(8, 0), "Catalyst") != 0 ||
        strcmp(ecuio_monitor_short_name(8, ECUIO_DIAG_COMPRESSION),
               "NMHC catalyst") != 0) {
        return 1;
    }
    if (ecuio_session_get_channel_diagnostics(NULL, 0, &diagnostics) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_read_diagnostics(NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_clear_diagnostics(NULL, 0, 0) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_clear_diagnostics_with(NULL, 0, 0, ECUIO_CLEAR_ALLOW_RUNNING |
                                                 ECUIO_CLEAR_ALLOW_MOVING) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        (ECUIO_DIAG_CLEAR_RPM & ECUIO_DIAG_CLEARING) != 0 ||
        (ECUIO_DIAG_CLEAR_KMH & ECUIO_DIAG_CLEAR_RPM) != 0 ||
        ECUIO_CLEAR_MOVING != 7u || ECUIO_CLEAR_NO_VEHICLE_SPEED != 8u ||
        ECUIO_ERR_REPORT_CHANGED != 17 ||
        (ECUIO_DIAG_ASKED & ECUIO_DIAG_FREEZE_FRAME) != 0 ||
        (ECUIO_DIAG_CLEARING & ECUIO_DIAG_ASKED) != 0 ||
        ECUIO_CLEAR_REPORT_CHANGED != 6u) {
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
    if (ecuio_can_interfaces(NULL, 1, &can_count) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_can_state_name(ECUIO_CAN_STATE_BUS_OFF)[0] == 0 ||
        ecuio_can_mode_name(ECUIO_CAN_MODE_ONE_SHOT)[0] == 0) {
        return 1;
    }
    if (ecuio_dtc_parse(NULL, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_dtc_title(0x0301)[0] == 0 || ecuio_dtc_title_source()[0] == 0 ||
        ecuio_dtc_title_notice()[0] == 0) {
        return 1;
    }
    if (ecuio_diag_inspect(NULL, 0, 2001, &inspection) != ECUIO_OK ||
        inspection.verdict != ECUIO_INSPECTION_UNKNOWN ||
        ecuio_diag_inspect(NULL, 1, 2001, &inspection) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ECUIO_INSPECTION_READING != 6u) {
        return 1;
    }
    if (ecuio_sim_create(NULL, "vcan0", &sim) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_sim_create_with(NULL, "vcan0", ECUIO_SIM_ALLOW_DESK_BUS, &sim) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_sim_get_stats(NULL, &sim_stats) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_sim_set_value(NULL, 1, 0.0) != ECUIO_ERR_NULL_ARGUMENT ||
        sim != NULL || ECUIO_SIM_FAILED != 1u || ECUIO_SIM_YIELDED != 2u ||
        ECUIO_SIM_ALLOW_DESK_BUS != 1u) {
        return 1;
    }
    /* The unbound sim (ADR-0055) */
    if (ecuio_sim_create_unbound(NULL, 0, &sim) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_sim_ask(NULL, 0x7E0u, NULL, 0, ECUIO_SIM_NOW, NULL, 0, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        sim != NULL || ECUIO_SIM_MAX_REPLIES != 10u ||
        ECUIO_SIM_REPLY_PENDING != 1u || ECUIO_SIM_REPLY_LATE != 2u ||
        ECUIO_SIM_REPLY_BAD_SEQUENCE != 4u || ECUIO_SIM_REPLY_STALLED != 8u) {
        return 1;
    }
    ecuio_sim_destroy(NULL);
    /* The recorder (ADR-0049): its layout and its NULL refusals */
    memset(&record_options, 0, sizeof record_options);
    if (offsetof(ecuio_record_options_t, root_utf8) != 48 ||
        sizeof(ecuio_recorder_state_t) != 200 ||
        offsetof(ecuio_recorder_state_t, reason) != 40 ||
        ECUIO_ERR_RECORDING_FAILED != 18 || ECUIO_VALIDATE_ERR_FORMULA != 18 ||
        ecuio_recorder_open(&record_options, &recorder) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        recorder != NULL ||
        ecuio_session_record(NULL, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recorder_note(NULL, "x") != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recorder_mark(NULL, "x") != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recorder_get_state(NULL, &recorder_state) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recorder_get_folder(NULL, NULL, 0, &folder_len) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recorder_close(NULL, NULL) != ECUIO_OK ||
        INTERNAL(ecuio_recorder_test_syncs(NULL), 0) != 0) {
        return 1;
    }
    /* The reader (ADR-0049 section 5): its layout and its NULL refusals */
    memset(&reading_find, 0, sizeof reading_find);
    if (sizeof(ecuio_recording_info_t) != 96 ||
        sizeof(ecuio_recording_signal_t) != 128 ||
        offsetof(ecuio_recording_signal_t, samples) != 56 ||
        offsetof(ecuio_recording_signal_t, group) != 54 ||
        offsetof(ecuio_recording_marker_t, label_utf8) != 32 ||
        sizeof(ecuio_recording_column_t) != 24 ||
        offsetof(ecuio_recording_find_t, label_utf8) != 64 ||
        sizeof(ecuio_recording_signals_t) != 128 ||
        offsetof(ecuio_recording_signals_t, length) != 8 ||
        offsetof(ecuio_recording_signals_t, sha256) != 16 ||
        ECUIO_RECORDING_SIGNALS_MATCH != 0x4u || ECUIO_ERR_CANCELED != 19 ||
        ECUIO_RECORDING_NONE != UINT64_MAX || ECUIO_ERR_IN_USE != 20 ||
        ECUIO_RECORDING_FIND_ENGINE_START != 3u ||
        ECUIO_ENGINE_START_RPM - ECUIO_ENGINE_STOPPED_RPM != 400.0 ||
        ecuio_plugin_free(NULL) != ECUIO_OK ||
        ecuio_plugin_values(NULL, NULL, 0, NULL, 0, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_open(NULL, NULL, NULL, &reading) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        reading != NULL ||
        ecuio_recording_get_info(NULL, &reading_info) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_get_signals(NULL, &reading_signals) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recorder_signal_file(NULL, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_plugin_variant_file(NULL, 0) != NULL ||
        ecuio_recording_get_signal(NULL, 0, &reading_signal) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_get_marker(NULL, 0, &reading_marker) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_query(NULL, &reading_pick, 1, 0, 1, &reading_column,
                              1) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_text_at(NULL, 0, 0, reading_text, sizeof reading_text,
                                &reading_at) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_find(NULL, &reading_find, 0, &reading_at) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        INTERNAL(ecuio_recording_test_table_bytes(NULL), 0) != 0) {
        return 1;
    }
    ecuio_recording_close(NULL);
    /* The frames check (ADR-0049 section 6): its layout and refusals */
    memset(&check_options, 0, sizeof check_options);
    check_options.flags = 1;
    if (sizeof(ecuio_check_options_t) != 56 ||
        sizeof(ecuio_check_finding_t) != 56 ||
        offsetof(ecuio_check_finding_t, code) != 36 ||
        sizeof(ecuio_check_summary_t) != 128 ||
        offsetof(ecuio_check_summary_t, counts) != 24 ||
        offsetof(ecuio_check_summary_t, torn_rows) != 88 ||
        ECUIO_CHECK_GAP != 1 || ECUIO_CHECK_ASLEEP != 7 ||
        ecuio_recording_check(NULL, NULL, NULL, NULL, NULL, &check_summary) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_check(".", NULL, NULL, NULL, NULL, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_check(".", &check_options, NULL, NULL, NULL,
                              &check_summary) != ECUIO_ERR_INVALID_ARGUMENT) {
        return 1;
    }
    /* Exports and imports (ADR-0054): their layouts and refusals */
    memset(&export_options, 0, sizeof export_options);
    memset(&import_options, 0, sizeof import_options);
    export_options.format = ECUIO_EXPORT_PCAPNG;
    import_options.format = ECUIO_IMPORT_CANDUMP;
    if (sizeof(ecuio_export_report_t) != 120 ||
        offsetof(ecuio_export_report_t, failed_class) != 80 ||
        offsetof(ecuio_export_report_t, values) != 88 ||
        offsetof(ecuio_export_report_t, files) != 104 ||
        offsetof(ecuio_export_report_t, files_left_out) != 108 ||
        offsetof(ecuio_export_options_t, folder_utf8) != 24 ||
        sizeof(ecuio_import_report_t) != 80 ||
        offsetof(ecuio_import_report_t, duration_us) != 40 ||
        offsetof(ecuio_import_report_t, values) != 48 ||
        offsetof(ecuio_import_report_t, columns_ignored) != 56 ||
        offsetof(ecuio_import_report_t, reserved) != 64 ||
        offsetof(ecuio_import_options_t, in_path_utf8) != 8 ||
        ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED != 4u ||
        ECUIO_EXPORT_LEFT_OUT_COUNT != 5u ||
        ECUIO_FRAME_FLAG_FD_ESI != 0x200u ||
        ecuio_export(NULL, &export_report) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_export(&export_options, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_scrub_frames(0, NULL, NULL, NULL, 0, NULL, &export_report) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_import(NULL, &import_report) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_import(&import_options, NULL) != ECUIO_ERR_NULL_ARGUMENT) {
        return 1;
    }
    /* Frames read back (ADR-0049's appendix): the summary's layout, zeroed
     * by a refused call */
    memset(&frames_read, 0xFF, sizeof frames_read);
    if (sizeof(ecuio_frames_read_t) != 32 ||
        offsetof(ecuio_frames_read_t, torn_rows) != 8 ||
        offsetof(ecuio_frames_read_t, reserved0) != 12 ||
        offsetof(ecuio_frames_read_t, reserved) != 16 ||
        ecuio_recording_read_frames(NULL, 0, 0, NULL, NULL, &frames_read) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        frames_read.rows != 0 || frames_read.torn_rows != 0 ||
        frames_read.reserved[1] != 0) {
        return 1;
    }
    /* The CSV map guess (ADR-0054's appendix): its layout and refusals */
    if (sizeof(ecuio_csv_guess_t) != 48 ||
        offsetof(ecuio_csv_guess_t, rows_read) != 12 ||
        offsetof(ecuio_csv_guess_t, map_offset) != 16 ||
        offsetof(ecuio_csv_guess_t, map_length) != 20 ||
        offsetof(ecuio_csv_guess_t, reserved) != 24 ||
        ECUIO_CSV_GUESS_TIME_UNIT_KNOWN != 0x1u ||
        ECUIO_CSV_GUESS_DECIMAL_COMMA != 0x2u ||
        ECUIO_CSV_GUESS_PREAMBLE != 0x4u ||
        ECUIO_CSV_GUESS_TIME_NUMBERS != 0x8u ||
        ecuio_import_csv_guess(NULL, NULL, NULL, 0, &csv_guess_needed,
                               &csv_guess) != ECUIO_ERR_NULL_ARGUMENT ||
        csv_guess_needed != 0 ||
        ecuio_import_csv_guess("log.csv", "h", NULL, 0, &csv_guess_needed,
                               NULL) != ECUIO_ERR_INVALID_ARGUMENT) {
        return 1;
    }
    /* The health report (ADR-0054's appendix): its layouts and refusals */
    memset(&health_options, 0, sizeof health_options);
    health_options.format = ECUIO_HEALTH_JSON;
    if (sizeof(ecuio_health_value_t) != 32 ||
        offsetof(ecuio_health_value_t, age_us) != 16 ||
        offsetof(ecuio_health_options_t, utc_us) != 32 ||
        offsetof(ecuio_health_options_t, baseline_folder_utf8) != 48 ||
        offsetof(ecuio_health_options_t, baseline_room) != 48 ||
        offsetof(ecuio_health_options_t, reserved) != 56 ||
        offsetof(ecuio_health_options_t, session) != 80 ||
        sizeof(ecuio_health_report_t) != 120 ||
        offsetof(ecuio_health_report_t, failed_class) != 80 ||
        ECUIO_HEALTH_TEXT != 2u || ECUIO_HEALTH_VIN_DECODED != 1u ||
        ECUIO_HEALTH_BASELINE_READ != 2u ||
        ecuio_health_export(NULL, &health_report) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_health_export(&health_options, NULL) != ECUIO_ERR_NULL_ARGUMENT) {
        return 1;
    }
    if (ecuio_obdb_import("{}", 2, NULL, NULL, NULL, NULL, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ECUIO_OBDB_NOTE_SKIPPED != 2u || ECUIO_OBDB_ALONE != 1u) {
        return 1;
    }
    ecuio_obdb_free(NULL);
    if (ecuio_vin_decode(NULL, 0, &vin_info) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_vin_make(NULL)[0] != 0 || ecuio_vin_source()[0] == 0) {
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
    /* The pull queue (ADR-0053 section 4): its NULL refusals */
    if (ecuio_queue_open(NULL, ECUIO_QUEUE_FRAMES, 1, &queue) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        queue != NULL ||
        ecuio_queue_read_frames(NULL, &frame, 1, &queue_count, &queue_dropped,
                                0) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_queue_read_values(NULL, &queue_value, 1, &queue_count,
                                &queue_dropped, 0) != ECUIO_ERR_NULL_ARGUMENT) {
        return 1;
    }
    ecuio_queue_close(NULL);
    printf("log '%s'\n", INTERNAL(ecuio_j2534_sur_log_path(NULL), ""));
    (void)frame;
    return 0;
}
