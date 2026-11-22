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
 *
 * The calls themselves live in tests/test_c_abi_*.c, one strict-C part
 * each (test_c_abi_parts.h lists them); the include list below is the
 * standalone-header check, so a new public header goes in it, and its
 * calls in the part that matches.
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
    if (c_abi_conditions() != 0) {
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
        /* A format 2 plugin's conversation tier read from C (ADR-0066):
         * the module's size, the ECU, each service and the level, and
         * each getter's refusal past its count. The module is
         * tests/fixtures/plugins/conversation.json's. */
        static const char kTalker[] =
            "{\"havne_plugin\":2,\"id\":\"c-abi-talker\",\"name\":\"C ABI t"
            "alker\",\"version\":\"1.0.0\",\"requires\":{\"core\":\"0.1.0\""
            "},\"source\":{\"name\":\"Havne's tests\"},\"tiers\":[\"data\","
            "\"conversation\"],\"ecus\":[{\"id\":\"pcm\",\"rx\":\"7E8\"}],"
            "\"variants\":[{\"id\":\"base\",\"files\":[\"a.json\"]}],\"conv"
            "ersation\":{\"code\":{\"file\":\"guest.wasm\",\"sha256\":\"3d3"
            "854650ca1a655ba8aac88c18eb14befa0e552fc65033127dbd2a2e3a8428a"
            "\"},\"budget\":1000,\"ecus\":[\"pcm\"],\"services\":[{\"sid\":"
            "\"22\"},{\"sid\":\"27\",\"sub\":[\"03\",\"04\"]}],\"security\""
            ":[{\"ecu\":\"pcm\",\"level\":\"03\",\"seed_bytes\":4,\"key_byt"
            "es\":8,\"delay_ms\":10000,\"key_window_ms\":500}]},\"files\":{"
            "\"a.json\":{\"havne_signals\":1,\"signals\":[{\"key\":\"rpm\","
            "\"name\":\"Engine Speed\",\"ecu\":\"pcm\",\"read\":{\"service"
            "\":\"22\",\"number\":\"F40C\"},\"field\":{\"byte\":0,\"bits\":"
            "16},\"unit\":\"rpm\"}]},\"guest.wasm\":{\"kind\":\"wasm\",\"ba"
            "se64\":\"AGFzbQEAAAABDgNgAAF/YAJ/fwF/YAAAAwcGAAABAAECBQQBAQEBB"
            "0IGCWVjdWlvX2FiaQAADGVjdWlvX2J1ZmZlcgABBWJlZ2luAAIMbmV4dF9yZXF"
            "1ZXN0AAMGYWNjZXB0AAQDZW5kAAUKHgYEAEEBCwUAQYAICwQAQQALBABBAAsEA"
            "EEACwIACw==\"}}}";
        ecuio_plugin_t* plugin = NULL;
        ecuio_plugin_info_t about;
        ecuio_plugin_conversation_t talk;
        ecuio_plugin_service_t service;
        ecuio_plugin_security_level_t level;
        ecuio_plugin_conversation_ecu_t ecu;
        int ok = 0;
        memset(&ecu, 0, sizeof ecu);
        ecu.struct_size = sizeof ecu;
        memset(&about, 0, sizeof about);
        about.struct_size = sizeof about;
        memset(&talk, 0, sizeof talk);
        talk.struct_size = sizeof talk;
        memset(&service, 0, sizeof service);
        service.struct_size = sizeof service;
        memset(&level, 0, sizeof level);
        level.struct_size = sizeof level;
        if (ecuio_plugin_load(kTalker, sizeof kTalker - 1, NULL, NULL,
                              &plugin) != ECUIO_OK) {
            return 1;
        }
        ok = ecuio_plugin_info(plugin, &about) == ECUIO_OK &&
             about.tiers ==
                 (ECUIO_PLUGIN_TIER_DATA | ECUIO_PLUGIN_TIER_CONVERSATION) &&
             ecuio_plugin_conversation(plugin, &talk) == ECUIO_OK &&
             talk.code_bytes == 139u && talk.budget == 1000u &&
             talk.ecu_count == 1u && talk.service_count == 2u &&
             talk.level_count == 1u &&
             ecuio_plugin_conversation_ecu(plugin, 0, &ecu) == ECUIO_OK &&
             ecu.rx_id == 0x7E8u && ecu.tx_id == 0x7E0u &&
             strcmp(ecu.id, "pcm") == 0 &&
             ecuio_plugin_conversation_ecu(plugin, 1, &ecu) ==
                 ECUIO_ERR_INVALID_ARGUMENT &&
             ecuio_plugin_service(plugin, 1, &service) == ECUIO_OK &&
             service.struct_size == sizeof service && service.sid == 0x27u &&
             service.sub_count == 2u && service.subs[0] == 0x03u &&
             service.subs[1] == 0x04u &&
             ecuio_plugin_service(plugin, 2, &service) ==
                 ECUIO_ERR_INVALID_ARGUMENT &&
             ecuio_plugin_security_level(plugin, 0, &level) == ECUIO_OK &&
             level.struct_size == sizeof level && level.rx_id == 0x7E8u &&
             level.level == 0x03u && level.seed_bytes == 4u &&
             level.key_bytes == 8u && level.delay_ms == 10000u &&
             level.key_window_ms == 500u &&
             ecuio_plugin_security_level(plugin, 1, &level) ==
                 ECUIO_ERR_INVALID_ARGUMENT;
        if (ecuio_plugin_free(plugin) != ECUIO_OK || !ok) {
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
