/* File/layer: tests/test_c_abi_signalfile.c | strict C part of test_c_abi
 * Testing: UNIT: a field's span, a def's key, gauges, expressions and the signal file's source, compile and link, run as c_abi and c_abi_shared
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

int c_abi_signalfile(void)
{
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
    return 0;
}
