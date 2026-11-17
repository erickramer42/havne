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

#include "ecuio/conditions.h"
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
typedef char abi_gauge_size_check[(sizeof(ecuio_signal_gauge_t) == 48) ? 1 : -1];
typedef char abi_gauge_warn_high_check[(offsetof(ecuio_signal_gauge_t, warn_high) == 24) ? 1 : -1];
typedef char abi_gauge_flags_check[(offsetof(ecuio_signal_gauge_t, flags) == 32) ? 1 : -1];
typedef char abi_gauge_reserved_check[(offsetof(ecuio_signal_gauge_t, reserved) == 36) ? 1 : -1];
typedef char
    abi_expr_check_size_check[(sizeof(ecuio_expr_check_t) == 248) ? 1 : -1];
typedef char
    abi_expr_check_at_check[(offsetof(ecuio_expr_check_t, at) == 8) ? 1 : -1];
typedef char abi_expr_check_format_check
    [(offsetof(ecuio_expr_check_t, format) == 12) ? 1 : -1];
typedef char abi_expr_check_depth_check
    [(offsetof(ecuio_expr_check_t, depth) == 16) ? 1 : -1];
typedef char abi_expr_check_reserved_check
    [(offsetof(ecuio_expr_check_t, reserved) == 20) ? 1 : -1];
typedef char abi_expr_check_program_check
    [(offsetof(ecuio_expr_check_t, program) == 40) ? 1 : -1];
typedef char
    abi_source_size_check[(sizeof(ecuio_signal_source_t) == 2632) ? 1 : -1];
typedef char abi_source_name_check[(offsetof(ecuio_signal_source_t, name) == 32)
                                       ? 1
                                       : -1];
typedef char abi_source_license_check
    [(offsetof(ecuio_signal_source_t, license) == 1592) ? 1 : -1];
typedef char abi_source_modified_check
    [(offsetof(ecuio_signal_source_t, modified) == 2112) ? 1 : -1];
typedef char
    abi_eval_input_size_check[(sizeof(ecuio_eval_input_t) == 24) ? 1 : -1];
typedef char
    abi_eval_input_age_check[(offsetof(ecuio_eval_input_t, age_us) == 8) ? 1
                                                                         : -1];
typedef char
    abi_eval_input_id_check[(offsetof(ecuio_eval_input_t, def_id) == 16) ? 1
                                                                         : -1];
typedef char abi_eval_input_flags_check
    [(offsetof(ecuio_eval_input_t, flags) == 20) ? 1 : -1];
typedef char
    abi_eval_result_size_check[(sizeof(ecuio_eval_result_t) == 48) ? 1 : -1];
typedef char abi_eval_result_value_check
    [(offsetof(ecuio_eval_result_t, value) == 8) ? 1 : -1];
typedef char
    abi_eval_result_why_check[(offsetof(ecuio_eval_result_t, why) == 16) ? 1
                                                                         : -1];
typedef char abi_eval_result_leaf_check
    [(offsetof(ecuio_eval_result_t, leaf) == 20) ? 1 : -1];
typedef char abi_eval_result_bound_check
    [(offsetof(ecuio_eval_result_t, bound_us) == 24) ? 1 : -1];
typedef char abi_eval_result_reserved_check
    [(offsetof(ecuio_eval_result_t, reserved) == 32) ? 1 : -1];
typedef char abi_validation_size_check[(sizeof(ecuio_validation_t) == 16) ? 1 : -1];
typedef char abi_validation_index_check[(offsetof(ecuio_validation_t, index) == 4) ? 1 : -1];
typedef char abi_validation_reserved_check[(offsetof(ecuio_validation_t, reserved) == 8) ? 1 : -1];
typedef char abi_info_size_check[(sizeof(ecuio_session_info_t) == 432) ? 1 : -1];
typedef char abi_info_rx_check[(offsetof(ecuio_session_info_t, rx_ids) == 32) ? 1 : -1];
typedef char abi_info_text_check[(offsetof(ecuio_session_info_t, firmware_version) == 64) ? 1 : -1];
typedef char abi_info_profile_check[(offsetof(ecuio_session_info_t, profile) == 304) ? 1 : -1];
typedef char abi_info_replay_check[(offsetof(ecuio_session_info_t, replay_frames) == 368) ? 1 : -1];
typedef char abi_info_torn_check[(offsetof(ecuio_session_info_t, replay_rows_torn) == 380) ? 1 : -1];
typedef char abi_info_modules_check[(offsetof(ecuio_session_info_t, module_count) == 384) ? 1 : -1];
typedef char abi_info_reserved_check[(offsetof(ecuio_session_info_t, reserved) == 388) ? 1 : -1];
typedef char abi_info_listed_check[(ECUIO_SESSION_INFO_MAX_CHANNELS == 4) ? 1 : -1];
typedef char abi_channels_check[(ECUIO_MAX_CHANNELS == 32) ? 1 : -1];
typedef char abi_decl_size_check[(sizeof(ecuio_channel_decl_t) == 16) ? 1 : -1];
typedef char abi_decl_tx_check[(offsetof(ecuio_channel_decl_t, tx_id) == 4) ? 1 : -1];
typedef char abi_decl_flags_check[(offsetof(ecuio_channel_decl_t, flags) == 8) ? 1 : -1];
typedef char abi_decl_reserved_check[(offsetof(ecuio_channel_decl_t, reserved) == 12) ? 1 : -1];
typedef char abi_layout_check[(ECUIO_SIGNAL_SET_LAYOUT == 5u) ? 1 : -1];
typedef char abi_dropped_size_check[(sizeof(ecuio_dropped_request_t) == 8) ? 1 : -1];
typedef char abi_support_size_check[(sizeof(ecuio_channel_support_t) == 152) ? 1 : -1];
typedef char abi_support_bitmap_check[(offsetof(ecuio_channel_support_t, pid_bitmap) == 24) ? 1 : -1];
typedef char abi_support_list_check[(offsetof(ecuio_channel_support_t, dropped_list) == 56) ? 1 : -1];
typedef char abi_support_scan_check[(offsetof(ecuio_channel_support_t, scan_total) == 120) ? 1 : -1];
typedef char abi_support_pids_check[(offsetof(ecuio_channel_support_t, pids_supported) == 132) ? 1 : -1];
typedef char abi_support_session_check[(offsetof(ecuio_channel_support_t, session_asked) == 136) ? 1 : -1];
typedef char abi_support_session_in_force_check[(offsetof(ecuio_channel_support_t, session) == 137) ? 1 : -1];
typedef char abi_support_session_state_check[(offsetof(ecuio_channel_support_t, session_state) == 138) ? 1 : -1];
typedef char abi_support_session_nrc_check[(offsetof(ecuio_channel_support_t, session_nrc) == 139) ? 1 : -1];
typedef char abi_support_session_entries_check[(offsetof(ecuio_channel_support_t, session_entries) == 140) ? 1 : -1];
typedef char abi_support_session_lost_check[(offsetof(ecuio_channel_support_t, session_lost) == 142) ? 1 : -1];
typedef char abi_support_reserved_check[(offsetof(ecuio_channel_support_t, reserved) == 144) ? 1 : -1];
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
typedef char abi_session_timing_untimed_check[(offsetof(ecuio_session_timing_t, untimed) == 36) ? 1 : -1];
typedef char abi_session_timing_reserved_check[(offsetof(ecuio_session_timing_t, reserved) == 40) ? 1 : -1];
typedef char abi_rotation_check[(ECUIO_PROFILE_MAX_REQUESTS == 1024) ? 1 : -1];
typedef char abi_module_requests_check[(ECUIO_MODULE_MAX_REQUESTS == 1024) ? 1 : -1];
typedef char abi_file_signals_check[(ECUIO_SIGNAL_FILE_MAX_SIGNALS == 1024u) ? 1 : -1];
typedef char abi_file_bytes_check[(ECUIO_SIGNAL_FILE_MAX_BYTES == 1048576u) ? 1 : -1];
typedef char abi_set_signals_check[(ECUIO_SIGNAL_SET_MAX_SIGNALS == 16384u) ? 1 : -1];
typedef char abi_set_bytes_check[(ECUIO_SIGNAL_SET_MAX_BYTES == 4194304u) ? 1 : -1];
typedef char abi_load_set_check[(ECUIO_SIGNAL_FILE_LOAD_SET == 0x1u) ? 1 : -1];
typedef char abi_pick_only_check[(ECUIO_SESSION_INFO_PICK_ONLY == 0x80u) ? 1 : -1];
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
typedef char abi_wrap_options_years_check
    [(offsetof(ecuio_plugin_wrap_options_t, years) == 8) ? 1 : -1];
typedef char abi_wrap_options_reserved_check
    [(offsetof(ecuio_plugin_wrap_options_t, reserved) == 16) ? 1 : -1];
typedef char abi_wrap_options_id_check
    [(offsetof(ecuio_plugin_wrap_options_t, id) == 40) ? 1 : -1];
typedef char
    abi_wrap_options_variant_check[(offsetof(ecuio_plugin_wrap_options_t,
                                             variant) == 40 + sizeof(char*))
                                       ? 1
                                       : -1];
typedef char
    abi_wrap_options_wmi_check[(offsetof(ecuio_plugin_wrap_options_t, wmi) ==
                                40 + 2 * sizeof(char*))
                                   ? 1
                                   : -1];
typedef char abi_wrap_options_size_check
    [(sizeof(ecuio_plugin_wrap_options_t) == 40 + 3 * sizeof(char*)) ? 1 : -1];
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

typedef char abi_condition_info_size_check[(sizeof(ecuio_condition_info_t) == 48 + 2 * sizeof(void*)) ? 1 : -1];
typedef char abi_condition_info_uses_check[(offsetof(ecuio_condition_info_t, uses) == 4) ? 1 : -1];
typedef char abi_condition_info_terms_check[(offsetof(ecuio_condition_info_t, term_count) == 12) ? 1 : -1];
typedef char abi_condition_info_for_check[(offsetof(ecuio_condition_info_t, for_us) == 16) ? 1 : -1];
typedef char abi_condition_info_before_check[(offsetof(ecuio_condition_info_t, before_us) == 24) ? 1 : -1];
typedef char abi_condition_info_reserved_check[(offsetof(ecuio_condition_info_t, reserved) == 32) ? 1 : -1];
typedef char abi_condition_info_key_check[(offsetof(ecuio_condition_info_t, key) == 48) ? 1 : -1];
typedef char abi_condition_info_name_check[(offsetof(ecuio_condition_info_t, name) == 48 + sizeof(void*)) ? 1 : -1];
typedef char abi_condition_reference_size_check[(sizeof(ecuio_condition_reference_t) == 48) ? 1 : -1];
typedef char abi_condition_reference_id_check[(offsetof(ecuio_condition_reference_t, id) == 4) ? 1 : -1];
typedef char abi_condition_reference_text_check[(offsetof(ecuio_condition_reference_t, text) == 8) ? 1 : -1];
typedef char abi_condition_term_size_check[(sizeof(ecuio_condition_term_t) == 456) ? 1 : -1];
typedef char abi_condition_term_test_check[(offsetof(ecuio_condition_term_t, test) == 4) ? 1 : -1];
typedef char abi_condition_term_level_check[(offsetof(ecuio_condition_term_t, level) == 8) ? 1 : -1];
typedef char abi_condition_term_clear_check[(offsetof(ecuio_condition_term_t, clear) == 16) ? 1 : -1];
typedef char abi_condition_term_age_check[(offsetof(ecuio_condition_term_t, max_age_us) == 24) ? 1 : -1];
typedef char abi_condition_term_count_check[(offsetof(ecuio_condition_term_t, reference_count) == 32) ? 1 : -1];
typedef char abi_condition_term_unit_check[(offsetof(ecuio_condition_term_t, unit) == 40) ? 1 : -1];
typedef char abi_condition_term_reserved_check[(offsetof(ecuio_condition_term_t, reserved) == 56) ? 1 : -1];
typedef char abi_condition_term_references_check[(offsetof(ecuio_condition_term_t, references) == 72) ? 1 : -1];
typedef char abi_condition_binding_size_check[(sizeof(ecuio_condition_binding_t) == 64) ? 1 : -1];
typedef char abi_condition_binding_term_check[(offsetof(ecuio_condition_binding_t, why_term) == 4) ? 1 : -1];
typedef char abi_condition_binding_reference_check[(offsetof(ecuio_condition_binding_t, reference) == 16) ? 1 : -1];
typedef char abi_condition_binding_signal_check[(offsetof(ecuio_condition_binding_t, signal_id) == 32) ? 1 : -1];
typedef char abi_condition_binding_reserved_check[(offsetof(ecuio_condition_binding_t, reserved) == 48) ? 1 : -1];
typedef char abi_condition_event_size_check[(sizeof(ecuio_condition_event_t) == 56) ? 1 : -1];
typedef char abi_condition_event_condition_check[(offsetof(ecuio_condition_event_t, condition) == 8) ? 1 : -1];
typedef char abi_condition_event_flags_check[(offsetof(ecuio_condition_event_t, flags) == 16) ? 1 : -1];
typedef char abi_condition_event_value_check[(offsetof(ecuio_condition_event_t, value) == 24) ? 1 : -1];
typedef char abi_condition_event_id_check[(offsetof(ecuio_condition_event_t, arbitration_id) == 32) ? 1 : -1];
typedef char abi_condition_event_generation_check[(offsetof(ecuio_condition_event_t, generation) == 40) ? 1 : -1];
typedef char abi_condition_event_reserved_check[(offsetof(ecuio_condition_event_t, reserved) == 48) ? 1 : -1];
typedef char abi_condition_state_size_check[(sizeof(ecuio_condition_state_t) == 56) ? 1 : -1];
typedef char abi_condition_state_since_check[(offsetof(ecuio_condition_state_t, since_us) == 8) ? 1 : -1];
typedef char abi_condition_state_fired_check[(offsetof(ecuio_condition_state_t, fired) == 16) ? 1 : -1];
typedef char abi_condition_state_value_check[(offsetof(ecuio_condition_state_t, value) == 24) ? 1 : -1];
typedef char abi_condition_state_why_check[(offsetof(ecuio_condition_state_t, why) == 32) ? 1 : -1];
typedef char abi_condition_state_generation_check[(offsetof(ecuio_condition_state_t, generation) == 36) ? 1 : -1];
typedef char abi_condition_state_reserved_check[(offsetof(ecuio_condition_state_t, reserved) == 40) ? 1 : -1];

/* A conditions callback as C writes one */
static void no_conditions(const ecuio_condition_event_t* events, uint32_t count,
                          void* user_data)
{
    (void)events;
    (void)count;
    (void)user_data;
}

/* A condition file read from C: a condition, its term and how it binds
 * to obd2, and each call's answer to a NULL */
static int conditions_from_c(void)
{
    static const char kConditions[] =
        "{\"havne_conditions\":1,\"name\":\"C conditions\","
        "\"checks\":{\"gap\":2},\"conditions\":["
        "{\"key\":\"revs\",\"name\":\"Revs\","
        "\"when\":{\"signal\":[\"id:1001\",\"obd2.rpm\"],"
        "\"unit\":\"RPM\",\"above\":6500,\"clear\":6000},"
        "\"for\":0.5,\"alert\":{\"level\":\"warning\",\"banner\":true}}]}";
    ecuio_condition_set_t* set = NULL;
    ecuio_condition_info_t info;
    ecuio_condition_term_t term;
    ecuio_condition_binding_t binding;
    ecuio_check_options_t checks;
    int ok = 0;

    if (ecuio_condition_set_load(NULL, 1, &set) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_condition_set_count(NULL) != 0u ||
        strcmp(ecuio_condition_set_name(NULL), "") != 0 ||
        ecuio_condition_set_checks(NULL, &checks) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_condition_set_get(NULL, 0, &info) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_condition_set_get_term(NULL, 0, 0, &term) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_condition_set_resolve(NULL, 0, NULL, NULL, &binding) !=
            ECUIO_ERR_NULL_ARGUMENT) {
        return 1;
    }
    ecuio_condition_set_free(NULL);
    if (ecuio_condition_set_load(kConditions, sizeof kConditions - 1, &set) !=
        ECUIO_OK) {
        return 1;
    }
    ok = ecuio_condition_set_count(set) == 1u &&
         strcmp(ecuio_condition_set_name(set), "C conditions") == 0 &&
         ecuio_condition_set_checks(set, &checks) == ECUIO_OK &&
         checks.gap_us == 2000000u &&
         ecuio_condition_set_get(set, 0, &info) == ECUIO_OK &&
         strcmp(info.key, "revs") == 0 && strcmp(info.name, "Revs") == 0 &&
         info.flags == 0u &&
         info.uses ==
             (ECUIO_CONDITION_USE_ALERT | ECUIO_CONDITION_ALERT_BANNER) &&
         info.alert_level == ECUIO_ALERT_WARNING && info.term_count == 1u &&
         info.for_us == 500000u &&
         ecuio_condition_set_get_term(set, 0, 0, &term) == ECUIO_OK &&
         term.kind == ECUIO_TERM_SIGNAL && term.test == ECUIO_TERM_ABOVE &&
         term.level == 6500.0 && term.clear == 6000.0 &&
         term.flags == (ECUIO_TERM_HAS_CLEAR | ECUIO_TERM_HAS_UNIT) &&
         strcmp(term.unit, "RPM") == 0 && term.reference_count == 2u &&
         term.references[0].kind == ECUIO_REFERENCE_ID &&
         term.references[1].id == ECUIO_OBD2_SIG_RPM_ID &&
         strcmp(term.references[1].text, "obd2.rpm") == 0 &&
         ecuio_condition_set_resolve(set, 0, ecuio_obd2_signal_set(), NULL,
                                     &binding) == ECUIO_OK &&
         binding.why == ECUIO_CONDITION_WHY_NONE &&
         binding.why_term == UINT32_MAX && binding.term_count == 1u &&
         binding.reference[0] == 1u &&
         binding.signal_id[0] == ECUIO_OBD2_SIG_RPM_ID &&
         ecuio_condition_set_get(set, 1, &info) == ECUIO_ERR_INVALID_ARGUMENT;
    /* Attached to a session from C: its states before a frame, a queue of
     * its events, and each call's answer to a NULL */
    if (ok) {
        ecuio_session_t* session = NULL;
        ecuio_queue_t* queue = NULL;
        ecuio_condition_state_t state;
        ecuio_condition_event_t event;
        uint32_t count = 99u;
        uint64_t dropped = 99u;
        ok = ecuio_session_create("mock://c?profile=obd2", &session) == ECUIO_OK &&
             ecuio_queue_open(session, ECUIO_QUEUE_CONDITIONS, 4u, &queue) ==
                 ECUIO_OK &&
             ecuio_register_conditions_callback(session, no_conditions, NULL) ==
                 ECUIO_OK &&
             ecuio_session_set_conditions(session, set, NULL) == ECUIO_OK &&
             ecuio_session_get_condition_states(session, &state, 1u, &count) ==
                 ECUIO_OK &&
             count == 1u && state.state == ECUIO_CONDITION_OFF &&
             state.flags == ECUIO_CONDITION_STALE && state.generation == 1u &&
             state.why == ECUIO_CONDITION_WHY_NONE &&
             ecuio_queue_read_conditions(queue, &event, 1u, &count, &dropped,
                                         0u) == ECUIO_OK &&
             count == 0u && dropped == 0u &&
             ecuio_unregister_conditions_callback(session, no_conditions,
                                                  NULL) == ECUIO_OK &&
             ecuio_session_set_conditions(NULL, set, NULL) ==
                 ECUIO_ERR_NULL_ARGUMENT &&
             ecuio_register_conditions_callback(NULL, no_conditions, NULL) ==
                 ECUIO_ERR_NULL_ARGUMENT &&
             ecuio_unregister_conditions_callback(NULL, no_conditions, NULL) ==
                 ECUIO_ERR_NULL_ARGUMENT &&
             ecuio_session_get_condition_states(NULL, &state, 1u, &count) ==
                 ECUIO_ERR_NULL_ARGUMENT &&
             ecuio_queue_read_conditions(NULL, &event, 1u, &count, &dropped,
                                         0u) == ECUIO_ERR_NULL_ARGUMENT;
        ecuio_queue_close(queue);
        ecuio_session_destroy(session);
    }
    ecuio_condition_set_free(set);
    return ok ? 0 : 1;
}

/* A long call's cancel as C writes one (ecuio_cancel_t) */
static int never_cancel(void* user_data)
{
    (void)user_data;
    return 0;
}

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
    ecuio_recording_conditions_t reading_conditions;
    ecuio_recording_condition_event_t reading_event;
    ecuio_recording_signal_t reading_signal;
    ecuio_recording_marker_t reading_marker;
    ecuio_recording_column_t reading_column;
    ecuio_recording_find_t reading_find;
    ecuio_recording_stats_t reading_stats;
    ecuio_stretch_rules_t reading_rules;
    ecuio_recording_stretch_t reading_stretch;
    uint32_t reading_count = 0;
    uint64_t reading_at = 0;
    uint32_t reading_pick = 0;
    char reading_text[8];
    uint16_t core_major = 0;
    uint16_t core_minor = 0;
    uint16_t core_patch = 0;
    ecuio_check_options_t check_options;
    ecuio_check_summary_t check_summary;
    ecuio_export_options_t export_options;
    ecuio_export_report_t export_report;
    ecuio_export_estimate_t estimate;
    uint32_t estimate_ms = 0;
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
    if (conditions_from_c() != 0) {
        return 1;
    }
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
        ecuio_session_get_unasked_signals(NULL, NULL, 0, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_identity_vin(NULL, NULL, ECUIO_VIN_TEXT_LEN) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_get_unsplit_replies(NULL, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_get_replay_faults(NULL, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_get_timing(NULL, NULL, NULL, 0, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_get_module(NULL, 0, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ECUIO_VALIDATE_ERR_CHANNEL_RESERVED != -26 ||
        sizeof(ECUIO_TIMING_COLUMNS) < 2u ||
        (ECUIO_SESSION_INFO_REPLAY_FAULTS & ECUIO_SESSION_INFO_LISTEN_ONLY) != 0u ||
        (ECUIO_SESSION_INFO_REQUESTS & ECUIO_SESSION_INFO_REPLAY_FAULTS) != 0u ||
        (ECUIO_SESSION_INFO_PICK_ONLY &
         (ECUIO_SESSION_INFO_VBATT | ECUIO_SESSION_INFO_VERSIONS |
          ECUIO_SESSION_INFO_REPLAY_LOOP | ECUIO_SESSION_INFO_PENDING_OFF |
          ECUIO_SESSION_INFO_LISTEN_ONLY | ECUIO_SESSION_INFO_REPLAY_FAULTS |
          ECUIO_SESSION_INFO_REQUESTS)) != 0u ||
        ECUIO_ABSENT_AFTER_FRAMES != 3u) {
        return 1;
    }
    if (ecuio_decode_text(NULL, NULL, 0, NULL, 0) != ECUIO_DECODE_ERR_NULL) {
        return 1;
    }
    {
        /* ADR-0057's two: a field's span, and a def's key */
        ecuio_signal_def_t byte_def;
        double lo = 0.0;
        double hi = 0.0;
        memset(&byte_def, 0, sizeof byte_def);
        byte_def.kind = ECUIO_VALUE_UINT;
        byte_def.bit_length = 8;
        byte_def.scale = 1.0;
        byte_def.offset = -40.0;
        if (ecuio_def_range(&byte_def, &lo, &hi) != ECUIO_DECODE_OK ||
            lo != -40.0 || hi != 215.0 ||
            ecuio_def_range(NULL, &lo, &hi) != ECUIO_DECODE_ERR_NULL ||
            strcmp(ecuio_signal_file_key(NULL, &byte_def), "") != 0 ||
            ecuio_signal_file_gauge(NULL, &byte_def) != NULL ||
            ecuio_signal_file_format(NULL) != 0u ||
            strcmp(ecuio_signal_file_expr(NULL, &byte_def), "") != 0) {
            return 1;
        }
    }
    {
        /* A gauge read from C: a def with one, a def without, a NULL def,
         * and the file's format, its signals' expressions and its source;
         * then a packed plugin's requires.core, its patch past a byte, and
         * its variant's source, the data file's own */
        static const char kGauged[] =
            "{\"havne_signals\":1,\"name\":\"C gauges\","
            "\"source\":{\"name\":\"Havne's tests\"},"
            "\"ecus\":[{\"id\":\"pcm\",\"rx\":\"7E8\"}],\"signals\":["
            "{\"key\":\"volts\",\"name\":\"Module Volts\","
            "\"read\":{\"service\":\"01\",\"number\":\"42\"},"
            "\"field\":{\"byte\":0,\"bits\":16},\"scale\":0.001,\"unit\":\"V\","
            "\"gauge\":{\"min\":10,\"max\":16,\"warn_low\":11.5,"
            "\"warn_high\":15.25}},"
            "{\"key\":\"load\",\"name\":\"Load Share\","
            "\"read\":{\"service\":\"01\",\"number\":\"04\"},"
            "\"field\":{\"byte\":0,\"bits\":8},\"unit\":\"%\"},"
            "{\"key\":\"half\",\"name\":\"Half Volts\",\"kind\":\"computed\","
            "\"expr\":\"volts / 2\"}]}";
        static const char kPacked[] =
            "{\"havne_plugin\":1,\"id\":\"c-abi\",\"name\":\"C ABI\","
            "\"version\":\"1.0.0\",\"requires\":{\"core\":\"0.5.258\"},"
            "\"source\":{\"name\":\"Havne's tests\"},\"tiers\":[\"data\"],"
            "\"ecus\":[{\"id\":\"pcm\",\"rx\":\"7E8\"}],"
            "\"variants\":[{\"id\":\"base\",\"files\":[\"a.json\"]}],"
            "\"files\":{\"a.json\":{\"havne_signals\":1,"
            "\"source\":{\"name\":\"OBDb\",\"license\":\"CC-BY-SA-4.0\"},"
            "\"signals\":["
            "{\"key\":\"rpm\",\"name\":\"Engine Speed\",\"ecu\":\"pcm\","
            "\"read\":{\"service\":\"22\",\"number\":\"F40C\"},"
            "\"field\":{\"byte\":0,\"bits\":16},\"unit\":\"rpm\"}]}}}";
        ecuio_signal_file_t* file = NULL;
        ecuio_plugin_t* plugin = NULL;
        const ecuio_signal_gauge_t* gauge = NULL;
        const ecuio_signal_def_t* volts = NULL;
        const ecuio_signal_def_t* load = NULL;
        /* A caller's struct grown past core's: core fills its own size */
        struct {
            ecuio_signal_source_t source;
            uint32_t after;
        } grown;
        ecuio_signal_source_t* const source = &grown.source;
        uint16_t major = 1;
        uint16_t minor = 1;
        uint16_t patch = 1;
        /* A flag the load doesn't know is refused; a set's written copy
         * is read as any file is */
        if (ecuio_signal_file_load_with(kGauged, sizeof kGauged - 1, 0x2u,
                                        &file) != ECUIO_ERR_INVALID_ARGUMENT ||
            file != NULL ||
            ecuio_signal_file_load_with(kGauged, sizeof kGauged - 1,
                                        ECUIO_SIGNAL_FILE_LOAD_SET,
                                        &file) != ECUIO_OK) {
            return 1;
        }
        ecuio_signal_file_free(file);
        file = NULL;
        if (ecuio_signal_file_load(kGauged, sizeof kGauged - 1, &file) !=
            ECUIO_OK) {
            return 1;
        }
        volts = ecuio_signal_file_find(file, "volts");
        load = ecuio_signal_file_find(file, "load");
        gauge = ecuio_signal_file_gauge(file, volts);
        if (volts == NULL || load == NULL || gauge == NULL ||
            gauge->min != 10.0 || gauge->max != 16.0 ||
            gauge->warn_low != 11.5 || gauge->warn_high != 15.25 ||
            gauge->flags != (ECUIO_GAUGE_WARN_LOW | ECUIO_GAUGE_WARN_HIGH) ||
            gauge->reserved[0] != 0u || gauge->reserved[1] != 0u ||
            gauge->reserved[2] != 0u ||
            ecuio_signal_file_gauge(file, load) != NULL ||
            ecuio_signal_file_gauge(file, NULL) != NULL ||
            ecuio_signal_file_format(file) != ECUIO_SIGNAL_FILE_FORMAT_OLDEST ||
            ECUIO_SIGNAL_FILE_FORMAT < ECUIO_SIGNAL_FILE_FORMAT_OLDEST ||
            strcmp(ecuio_signal_file_expr(file, volts), "") != 0 ||
            strcmp(ecuio_signal_file_expr(file,
                                          ecuio_signal_file_find(file, "half")),
                   "volts / 2") != 0) {
            ecuio_signal_file_free(file);
            return 1;
        }
        {
            /* An expression checked from C: one that compiles, one refused
             * at its character, and the name a def is read by */
            ecuio_expr_check_t checked;
            char name[ECUIO_EXPR_REF_LEN];
            memset(&checked, 0, sizeof checked);
            checked.struct_size = (uint32_t)sizeof checked;
            if (ecuio_signal_file_check_expr(file, NULL, "volts * 2", 9,
                                             &checked) != ECUIO_OK ||
                checked.at != 0u || checked.format != 1u ||
                checked.depth != 1u || checked.program.input_count != 1u ||
                checked.program.input_ids[0] != volts->id ||
                ecuio_signal_file_check_expr(file, "half", "volts +", 7,
                                             &checked) !=
                    ECUIO_ERR_INVALID_ARGUMENT ||
                checked.at != 8u || checked.program.input_count != 0u ||
                ecuio_signal_file_expr_ref(file, volts, name, sizeof name) !=
                    ECUIO_OK ||
                strcmp(name, "volts") != 0 ||
                ecuio_signal_file_expr_ref(file, volts, name, 5) !=
                    ECUIO_ERR_INVALID_ARGUMENT) {
                ecuio_signal_file_free(file);
                return 1;
            }
            {
                /* And worked out from C: the checked program's one leaf,
                 * its value from a fresh input and none from a stale one */
                ecuio_eval_input_t input;
                ecuio_eval_result_t result;
                uint16_t leaf = 0;
                uint32_t leaves = 0;
                memset(&checked, 0, sizeof checked);
                checked.struct_size = (uint32_t)sizeof checked;
                memset(&input, 0, sizeof input);
                input.def_id = volts->id;
                input.value = 12.5;
                memset(&result, 0, sizeof result);
                result.struct_size = (uint32_t)sizeof result;
                if (ecuio_signal_file_check_expr(file, NULL, "volts * 2", 9,
                                                 &checked) != ECUIO_OK ||
                    ecuio_signal_file_expr_leaves(file, &checked.program, &leaf,
                                                  1, &leaves) != ECUIO_OK ||
                    leaves != 1u || leaf != volts->id ||
                    ecuio_signal_file_evaluate(file, &checked.program, &input,
                                               1, &result) != ECUIO_OK ||
                    result.flags != ECUIO_EVAL_VALUE || result.value != 25.0 ||
                    result.why != ECUIO_EVAL_WHY_NONE) {
                    ecuio_signal_file_free(file);
                    return 1;
                }
                input.age_us = (uint64_t)ECUIO_VALUE_STALE_US + 1u;
                if (ecuio_signal_file_evaluate(file, &checked.program, &input,
                                               1, &result) != ECUIO_OK ||
                    result.flags != 0u || result.why != ECUIO_EVAL_WHY_STALE ||
                    result.leaf != volts->id ||
                    result.bound_us != ECUIO_VALUE_STALE_US) {
                    ecuio_signal_file_free(file);
                    return 1;
                }
            }
        }
        {
            size_t length = 0;
            const char* text = ecuio_signal_file_text(file, &length);
            if (length != sizeof kGauged - 1 || strcmp(text, kGauged) != 0) {
                ecuio_signal_file_free(file);
                return 1;
            }
        }
        memset(&grown, 0xAA, sizeof grown);
        source->struct_size = (uint32_t)sizeof grown;
        if (ecuio_signal_file_source(file, source) != ECUIO_OK ||
            source->struct_size != sizeof(ecuio_signal_source_t) ||
            grown.after != 0xAAAAAAAAu || source->flags != 0u ||
            source->reserved[5] != 0u ||
            strcmp(source->name, "Havne's tests") != 0 ||
            source->license[0] != 0 ||
            source->modified[ECUIO_SOURCE_TEXT_LEN - 1] != 0) {
            ecuio_signal_file_free(file);
            return 1;
        }
        {
            /* ADR-0065's keys from C: a def's address, the set's keys by
             * the serial ports' contract, and a def named by key or name */
            const ecuio_signal_set_t* keyed = ecuio_signal_file_set(file);
            ecuio_signal_key_t key;
            ecuio_signal_key_t keys[3];
            uint32_t count = 0;
            uint32_t index = 99;
            memset(&key, 0xAA, sizeof key);
            memset(keys, 0xAA, sizeof keys);
            if (sizeof(ecuio_signal_key_t) != 32u ||
                ECUIO_SIGNAL_KEY_LEN != 32u || ECUIO_SIGNAL_KEY_MAX != 31u ||
                ecuio_signal_address_key(NULL, &key) !=
                    ECUIO_ERR_NULL_ARGUMENT ||
                ecuio_signal_address_key(volts, NULL) !=
                    ECUIO_ERR_NULL_ARGUMENT ||
                ecuio_signal_address_key(volts, &key) != ECUIO_OK ||
                strcmp(key.text, "7E8:4142:2.0:16") != 0 ||
                key.text[ECUIO_SIGNAL_KEY_LEN - 1] != 0 ||
                ecuio_signal_set_keys(keyed, file, NULL, 0, &count) !=
                    ECUIO_OK ||
                count != 3u ||
                ecuio_signal_set_keys(keyed, file, NULL, 1, &count) !=
                    ECUIO_ERR_NULL_ARGUMENT ||
                count != 0u ||
                ecuio_signal_set_keys(NULL, NULL, keys, 3, &count) !=
                    ECUIO_ERR_NULL_ARGUMENT ||
                ecuio_signal_set_keys(keyed, file, keys, 3, NULL) !=
                    ECUIO_ERR_NULL_ARGUMENT ||
                ecuio_signal_set_keys(keyed, file, keys, 3, &count) !=
                    ECUIO_OK ||
                count != 3u || strcmp(keys[0].text, key.text) != 0 ||
                strcmp(keys[1].text, "7E8:4104:2.0:8") != 0 ||
                strcmp(keys[2].text, "half") != 0 ||
                ecuio_signal_set_find(keyed, keys, count, "half", &index) !=
                    ECUIO_OK ||
                index != 2u ||
                ecuio_signal_set_find(keyed, keys, count, "Load Share",
                                      &index) != ECUIO_OK ||
                index != 1u ||
                ecuio_signal_set_find(keyed, keys, count, "none", &index) !=
                    ECUIO_ERR_INVALID_ARGUMENT ||
                index != 1u ||
                ecuio_signal_set_find(keyed, keys, count, NULL, &index) !=
                    ECUIO_ERR_NULL_ARGUMENT) {
                ecuio_signal_file_free(file);
                return 1;
            }
        }
        ecuio_signal_file_free(file);
        if (ecuio_plugin_load(kPacked, sizeof kPacked - 1, NULL, NULL,
                              &plugin) != ECUIO_OK) {
            return 1;
        }
        if (ecuio_plugin_requires_core(plugin, &major, &minor, &patch) !=
                ECUIO_OK ||
            major != 0u || minor != 5u || patch != 258u ||
            ecuio_signal_file_source(ecuio_plugin_variant_file(plugin, 0),
                                     source) != ECUIO_OK ||
            strcmp(source->name, "OBDb") != 0 ||
            strcmp(source->license, "CC-BY-SA-4.0") != 0 ||
            source->flags != ECUIO_SOURCE_SHARE_ALIKE) {
            ecuio_plugin_free(plugin);
            return 1;
        }
        if (ecuio_plugin_free(plugin) != ECUIO_OK) {
            return 1;
        }
    }
    {
        /* A signal file wrapped, a variant put in and dropped again,
         * from C: each text freed, the lineage read back */
        static const char kWrapped[] =
            "{\"havne_signals\":1,\"name\":\"C wrap\","
            "\"source\":{\"name\":\"Havne's tests\"},"
            "\"ecus\":[{\"id\":\"pcm\",\"rx\":\"7E8\"}],\"signals\":["
            "{\"key\":\"rpm\",\"name\":\"Engine Speed\",\"ecu\":\"pcm\","
            "\"read\":{\"service\":\"22\",\"number\":\"F40C\"},"
            "\"field\":{\"byte\":0,\"bits\":16},\"unit\":\"rpm\"}]}";
        ecuio_plugin_wrap_options_t options;
        ecuio_plugin_t* plugin = NULL;
        ecuio_plugin_info_t made;
        char lineage[ECUIO_PLUGIN_LINEAGE_LEN + 1];
        char* one = NULL;
        char* two = NULL;
        char* back = NULL;
        size_t one_length = 0;
        size_t two_length = 0;
        size_t back_length = 0;
        memset(&options, 0, sizeof options);
        options.struct_size = (uint32_t)sizeof options;
        options.id = "c-wrap";
        options.wmi = "ZZZ";
        options.years[0] = 2015u;
        options.years[1] = 2030u;
        if (ecuio_plugin_wrap(kWrapped, sizeof kWrapped - 1, &options, &one,
                              &one_length) != ECUIO_OK ||
            one == NULL || strlen(one) != one_length) {
            return 1;
        }
        if (ecuio_plugin_put_variant(one, one_length, "second", kWrapped,
                                     sizeof kWrapped - 1, &two,
                                     &two_length) != ECUIO_OK ||
            ecuio_plugin_drop_variant(two, two_length, "base", &back,
                                      &back_length) != ECUIO_OK ||
            ecuio_plugin_load(back, back_length, NULL, NULL, &plugin) !=
                ECUIO_OK) {
            ecuio_plugin_text_free(one);
            ecuio_plugin_text_free(two);
            ecuio_plugin_text_free(back);
            return 1;
        }
        ecuio_plugin_text_free(one);
        ecuio_plugin_text_free(two);
        ecuio_plugin_text_free(back);
        ecuio_plugin_text_free(NULL);
        memset(&made, 0, sizeof made);
        made.struct_size = (uint32_t)sizeof made;
        if (ecuio_plugin_info(plugin, &made) != ECUIO_OK ||
            strcmp(made.id, "c-wrap") != 0 ||
            strcmp(made.version, "1.2.0") != 0 || made.variant_count != 1u ||
            ecuio_plugin_lineage(plugin, lineage, sizeof lineage) != ECUIO_OK ||
            strlen(lineage) != ECUIO_PLUGIN_LINEAGE_LEN) {
            ecuio_plugin_free(plugin);
            return 1;
        }
        if (ecuio_plugin_free(plugin) != ECUIO_OK) {
            return 1;
        }
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
        ecuio_session_set_focus(NULL, NULL, 0) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_set_focus_with(NULL, NULL, 0, 0, 0) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ECUIO_FOCUS_MIN_SWEEP_MAX_MS != 60000u ||
        ecuio_session_clear_diagnostics(NULL, 0, 0) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_clear_diagnostics_with(
            NULL, 0, 0, ECUIO_CLEAR_ALLOW_RUNNING | ECUIO_CLEAR_ALLOW_MOVING) !=
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
        sim != NULL || ECUIO_SIM_MAX_REPLIES != 66u ||
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
        offsetof(ecuio_recorder_state_t, conditions_lost) != 168 ||
        offsetof(ecuio_recorder_state_t, windows) != 176 ||
        offsetof(ecuio_recorder_state_t, preroll_short) != 180 ||
        offsetof(ecuio_recorder_state_t, frames_passed) != 184 ||
        offsetof(ecuio_recorder_state_t, reserved) != 192 ||
        ECUIO_RECORD_ARMED != 1 || ECUIO_RECORDING_ARMED != 4 ||
        ECUIO_RECORDER_DRAINED != 1 || ECUIO_CONDITION_HELD != 8 ||
        ECUIO_ERR_RECORDING_FAILED != 18 ||
        ECUIO_CONDITION_WHY_NOT_RECEIVED != 8 ||
        ECUIO_VALIDATE_ERR_FORMULA != 18 ||
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
    memset(&reading_rules, 0, sizeof reading_rules);
    if (sizeof(ecuio_recording_info_t) != 96 ||
        sizeof(ecuio_recording_signal_t) != 128 ||
        offsetof(ecuio_recording_signal_t, samples) != 56 ||
        offsetof(ecuio_recording_signal_t, group) != 54 ||
        offsetof(ecuio_recording_marker_t, label_utf8) != 32 ||
        sizeof(ecuio_recording_column_t) != 24 ||
        offsetof(ecuio_recording_find_t, label_utf8) != 64 ||
        sizeof(ecuio_recording_stats_t) != 96 ||
        offsetof(ecuio_recording_stats_t, lowest_us) != 40 ||
        offsetof(ecuio_recording_stats_t, mean) != 56 ||
        offsetof(ecuio_recording_stats_t, flags) != 64 ||
        ECUIO_RECORDING_STEP_FORWARD != 1u ||
        ECUIO_RECORDING_STEP_BACKWARD != 2u ||
        sizeof(ecuio_stretch_rules_t) != 120 ||
        offsetof(ecuio_stretch_rules_t, cruise_floor) != 64 ||
        offsetof(ecuio_stretch_rules_t, throttle) != 72 ||
        offsetof(ecuio_stretch_rules_t, pull_min_rise) != 88 ||
        offsetof(ecuio_stretch_rules_t, pull_min_gain) != 96 ||
        offsetof(ecuio_stretch_rules_t, reserved) != 104 ||
        sizeof(ecuio_recording_stretch_t) != 64 ||
        offsetof(ecuio_recording_stretch_t, start) != 24 ||
        offsetof(ecuio_recording_stretch_t, flags) != 40 ||
        offsetof(ecuio_recording_stretch_t, reserved) != 48 ||
        ECUIO_STRETCH_PULLS != 0x1u || ECUIO_STRETCH_CRUISES != 0x2u ||
        ECUIO_RECORDING_STRETCH_PULL != 1u ||
        ECUIO_RECORDING_STRETCH_CRUISE != 2u ||
        ECUIO_RECORDING_ROLE_ENGINE_SPEED != 1u ||
        ECUIO_RECORDING_ROLE_VEHICLE_SPEED != 2u ||
        ECUIO_RECORDING_ROLE_THROTTLE != 3u ||
        ECUIO_RECORDING_ROLE_ENGINE_LOAD != 4u ||
        ECUIO_RECORDING_STRETCH_BY_LOAD != 0x1u ||
        ECUIO_RECORDING_STRETCH_BY_ENGINE_SPEED != 0x2u ||
        ECUIO_RECORDING_STRETCH_NO_THROTTLE != 0x4u ||
        ECUIO_RECORDING_STRETCH_CUT_START != 0x8u ||
        ECUIO_RECORDING_STRETCH_CUT_END != 0x10u ||
        ECUIO_VALIDATE_ERR_COMPUTED_INPUT != -22 ||
        ECUIO_VALIDATE_ERR_COMPUTED_CHAIN != -23 ||
        ECUIO_VALIDATE_ERR_OUT_OF_MEMORY != -24 ||
        ECUIO_VALIDATE_ERR_DUPLICATE_NAME != -25 ||
        ECUIO_FOCUS_DEFAULT_PICK != 0x1u ||
        ECUIO_QUEUE_MAX_CONDITIONS != 65536u ||
        ECUIO_CONDITION_UNAVAILABLE != 0x4u ||
        ECUIO_OBD2_SIG_BOOST_ID != 7011 ||
        ECUIO_STRETCH_DEFAULT_CRUISE_FLOOR != 56.32704 ||
        sizeof(ecuio_recording_signals_t) != 128 ||
        offsetof(ecuio_recording_signals_t, length) != 8 ||
        offsetof(ecuio_recording_signals_t, sha256) != 16 ||
        offsetof(ecuio_recording_info_t, condition_events) != 56 ||
        offsetof(ecuio_recording_info_t, reserved) != 64 ||
        ECUIO_RECORDING_INFO_CONDITIONS_CUT != 0x4u ||
        ECUIO_RECORDING_MAX_CONDITION_EVENTS != 1048576u ||
        sizeof(ecuio_recording_conditions_t) != 128 ||
        offsetof(ecuio_recording_conditions_t, lines_skipped) != 4 ||
        offsetof(ecuio_recording_conditions_t, length) != 8 ||
        offsetof(ecuio_recording_conditions_t, sha256) != 16 ||
        offsetof(ecuio_recording_conditions_t, events_lost) != 88 ||
        offsetof(ecuio_recording_conditions_t, reserved) != 96 ||
        ECUIO_RECORDING_CONDITIONS_NAMED != 0x1u ||
        ECUIO_RECORDING_CONDITIONS_PRESENT != 0x2u ||
        ECUIO_RECORDING_CONDITIONS_MATCH != 0x4u ||
        sizeof(ecuio_recording_condition_event_t) != 88 ||
        offsetof(ecuio_recording_condition_event_t, host_us) != 0 ||
        offsetof(ecuio_recording_condition_event_t, timestamp_us) != 8 ||
        offsetof(ecuio_recording_condition_event_t, reserved0) != 36 ||
        offsetof(ecuio_recording_condition_event_t, value) != 16 ||
        offsetof(ecuio_recording_condition_event_t, state) != 24 ||
        offsetof(ecuio_recording_condition_event_t, flags) != 28 ||
        offsetof(ecuio_recording_condition_event_t, term) != 32 ||
        offsetof(ecuio_recording_condition_event_t, key) != 40 ||
        offsetof(ecuio_recording_condition_event_t, reserved) != 72 ||
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
        ecuio_recording_get_conditions(NULL, &reading_conditions) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_get_condition_event(NULL, 0, &reading_event) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recorder_signal_file(NULL, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_plugin_variant_file(NULL, 0) != NULL ||
        ecuio_plugin_lineage(NULL, NULL, 0) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_plugin_wrap(NULL, 0, NULL, NULL, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_plugin_put_variant(NULL, 0, NULL, NULL, 0, NULL, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_plugin_drop_variant(NULL, 0, NULL, NULL, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_plugin_requires_core(NULL, &core_major, &core_minor,
                                   &core_patch) != ECUIO_ERR_NULL_ARGUMENT ||
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
        ecuio_recording_find_with(NULL, &reading_find, 0, NULL, NULL,
                                  &reading_at) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_stretches_with(NULL, &reading_rules, 0, 1, NULL, NULL,
                                       &reading_stretch, 1, &reading_count) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_stats(NULL, &reading_pick, 1, 0, 1, &reading_stats) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_step(NULL, 0, 0, ECUIO_RECORDING_STEP_FORWARD,
                             &reading_at) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_role_signal(NULL, ECUIO_RECORDING_ROLE_THROTTLE,
                                    &reading_pick) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_stretches(NULL, &reading_rules, 0, 1, &reading_stretch,
                                  1,
                                  &reading_count) != ECUIO_ERR_NULL_ARGUMENT ||
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
    export_options.folder_utf8 = "."; /* the estimate judges the format */
    import_options.format = ECUIO_IMPORT_CANDUMP;
    if (sizeof(ecuio_export_report_t) != 120 ||
        offsetof(ecuio_export_report_t, failed_class) != 80 ||
        offsetof(ecuio_export_report_t, values) != 88 ||
        offsetof(ecuio_export_report_t, files) != 104 ||
        offsetof(ecuio_export_report_t, files_left_out) != 108 ||
        offsetof(ecuio_export_options_t, folder_utf8) != 24 ||
        offsetof(ecuio_export_options_t, layout) != 24 + 2 * sizeof(void*) ||
        offsetof(ecuio_export_options_t, columns_utf8) !=
            40 + 2 * sizeof(void*) ||
        sizeof(ecuio_export_options_t) != 56 + 2 * sizeof(void*) ||
        offsetof(ecuio_export_report_t, rows) != 112 ||
        sizeof(ecuio_export_estimate_t) != 40 || ECUIO_CSV_WIDE != 0u ||
        ECUIO_CSV_LONG != 1u || sizeof(ecuio_import_report_t) != 80 ||
        offsetof(ecuio_import_report_t, duration_us) != 40 ||
        offsetof(ecuio_import_report_t, values) != 48 ||
        offsetof(ecuio_import_report_t, columns_ignored) != 56 ||
        offsetof(ecuio_import_report_t, rows_no_time) != 64 ||
        offsetof(ecuio_import_report_t, reserved) != 72 ||
        offsetof(ecuio_import_options_t, in_path_utf8) != 8 ||
        ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED != 4u ||
        ECUIO_EXPORT_LEFT_OUT_COUNT != 5u ||
        ECUIO_FRAME_FLAG_FD_ESI != 0x200u ||
        ecuio_export(NULL, &export_report) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_export(&export_options, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_export_with(NULL, never_cancel, NULL, &export_report) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_export_estimate_with(NULL, &estimate_ms, 1, never_cancel, NULL,
                                   &estimate) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_export_estimate(NULL, &estimate_ms, 1, &estimate) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_export_estimate(&export_options, &estimate_ms, 1, &estimate) !=
            ECUIO_ERR_INVALID_ARGUMENT ||
        ecuio_scrub_frames(0, NULL, NULL, NULL, 0, NULL, &export_report) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_import(NULL, &import_report) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_import(&import_options, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_import_with(NULL, never_cancel, NULL, &import_report) !=
            ECUIO_ERR_NULL_ARGUMENT) {
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
        ecuio_health_export(&health_options, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_health_export_with(NULL, never_cancel, NULL, &health_report) !=
            ECUIO_ERR_NULL_ARGUMENT) {
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
