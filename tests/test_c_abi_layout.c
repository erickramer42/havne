/* File/layer: tests/test_c_abi_layout.c | strict C part of test_c_abi
 * Testing: UNIT: the public structs' sizes and offsets, compile and link, run as c_abi and c_abi_shared
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

#include "test_c_abi_parts.h"


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
typedef char abi_conversation_size_check[(sizeof(ecuio_channel_conversation_t) == 64) ? 1 : -1];
typedef char abi_conversation_state_check[(offsetof(ecuio_channel_conversation_t, state) == 12) ? 1 : -1];
typedef char abi_conversation_level_check[(offsetof(ecuio_channel_conversation_t, level) == 20) ? 1 : -1];
typedef char abi_conversation_delay_check[(offsetof(ecuio_channel_conversation_t, delay_left_ms) == 24) ? 1 : -1];
typedef char abi_conversation_failed_check[(offsetof(ecuio_channel_conversation_t, failed) == 36) ? 1 : -1];
typedef char abi_conversation_reserved_check[(offsetof(ecuio_channel_conversation_t, reserved) == 40) ? 1 : -1];
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
typedef char
    abi_sim_reply_fc_check[(offsetof(ecuio_sim_reply_t, fc_us) == 20) ? 1 : -1];
typedef char
    abi_sim_reply_cf_check[(offsetof(ecuio_sim_reply_t, cf_us) == 24) ? 1 : -1];
typedef char abi_sim_reply_reserved_check
    [(offsetof(ecuio_sim_reply_t, reserved) == 28) ? 1 : -1];
typedef char abi_sim_reply_data_check[(offsetof(ecuio_sim_reply_t, data) == 40)
                                          ? 1
                                          : -1];
typedef char
    abi_sim_gateway_size_check[(sizeof(ecuio_sim_gateway_t) == 144) ? 1 : -1];
typedef char abi_sim_gateway_route_check
    [(offsetof(ecuio_sim_gateway_t, route) == 14) ? 1 : -1];
typedef char abi_sim_gateway_now_check
    [(offsetof(ecuio_sim_gateway_t, now) == 30) ? 1 : -1];
typedef char abi_sim_gateway_delay_check
    [(offsetof(ecuio_sim_gateway_t, delayed_until_us) == 40) ? 1 : -1];
typedef char abi_sim_gateway_forwarded_check
    [(offsetof(ecuio_sim_gateway_t, forwarded) == 48) ? 1 : -1];
typedef char abi_sim_gateway_too_early_check
    [(offsetof(ecuio_sim_gateway_t, too_early) == 120) ? 1 : -1];
typedef char abi_sim_gateway_overrides_check
    [(offsetof(ecuio_sim_gateway_t, overrides) == 128) ? 1 : -1];
typedef char abi_sim_gateway_override_size_check
    [(sizeof(ecuio_sim_gateway_override_t) == 24) ? 1 : -1];
typedef char abi_sim_gateway_override_now_check
    [(offsetof(ecuio_sim_gateway_override_t, now) == 8) ? 1 : -1];
typedef char abi_vin_size_check[(sizeof(ecuio_vin_info_t) == 32) ? 1 : -1];
typedef char abi_vin_wmi_check[(offsetof(ecuio_vin_info_t, wmi) == 8) ? 1 : -1];
typedef char abi_plugin_info_size_check[(sizeof(ecuio_plugin_info_t) == 136) ? 1 : -1];
typedef char abi_plugin_info_count_check[(offsetof(ecuio_plugin_info_t, variant_count) == 120) ? 1 : -1];
typedef char abi_plugin_info_pages_check
    [(offsetof(ecuio_plugin_info_t, page_count) == 128) ? 1 : -1];
typedef char
    abi_plugin_page_size_check[(sizeof(ecuio_plugin_page_t) == 152) ? 1 : -1];
typedef char abi_plugin_page_path_check
    [(offsetof(ecuio_plugin_page_t, path) == 24) ? 1 : -1];
typedef char abi_plugin_conversation_size_check
    [(sizeof(ecuio_plugin_conversation_t) == 64) ? 1 : -1];
typedef char abi_plugin_conversation_budget_check
    [(offsetof(ecuio_plugin_conversation_t, budget) == 40) ? 1 : -1];
typedef char abi_plugin_conversation_ecu_size_check
    [(sizeof(ecuio_plugin_conversation_ecu_t) == 64) ? 1 : -1];
typedef char abi_plugin_conversation_ecu_id_check
    [(offsetof(ecuio_plugin_conversation_ecu_t, id) == 12) ? 1 : -1];
typedef char abi_plugin_conversation_ecu_flags_check
    [(offsetof(ecuio_plugin_conversation_ecu_t, flags) == 44) ? 1 : -1];
typedef char
    abi_plugin_service_size_check[(sizeof(ecuio_plugin_service_t) == 32) ? 1
                                                                         : -1];
typedef char abi_plugin_service_subs_check
    [(offsetof(ecuio_plugin_service_t, subs) == 6) ? 1 : -1];
typedef char abi_plugin_service_flags_check
    [(offsetof(ecuio_plugin_service_t, flags) == 20) ? 1 : -1];
typedef char abi_plugin_level_size_check
    [(sizeof(ecuio_plugin_security_level_t) == 40) ? 1 : -1];
typedef char abi_plugin_level_level_check
    [(offsetof(ecuio_plugin_security_level_t, level) == 8) ? 1 : -1];
typedef char abi_plugin_level_delay_check
    [(offsetof(ecuio_plugin_security_level_t, delay_ms) == 12) ? 1 : -1];
typedef char abi_plugin_level_flags_check
    [(offsetof(ecuio_plugin_security_level_t, flags) == 20) ? 1 : -1];
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

