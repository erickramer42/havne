/* File/layer: tests/test_c_abi_recording.c | strict C part of test_c_abi
 * Testing: UNIT: the recorder and the recording reader, compile and link, run as c_abi and c_abi_shared
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

#include "test_c_abi_parts.h"

int c_abi_recording(void)
{
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
        offsetof(ecuio_recording_signal_t, key) != 96 ||
        sizeof reading_signal.key != ECUIO_SIGNAL_KEY_LEN ||
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
        ECUIO_RECORDING_INFO_CONVERSATION != 0x8u ||
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
        ecuio_plugin_page(NULL, 0, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_plugin_page_text(NULL, 0) != NULL ||
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
        ecuio_recording_find_signal(NULL, "x", &reading_pick) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_open_frames_with(NULL, NULL, NULL, NULL, NULL, NULL) !=
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
    return 0;
}
