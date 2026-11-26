/* File/layer: tests/test_c_abi_plugin.c | strict C part of test_c_abi
 * Testing: UNIT: the conversation tier, a wrapped signal file and its variant, compile and link, run as c_abi and c_abi_shared
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

int c_abi_plugin(void)
{
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
        ecuio_plugin_fingerprint_t print;
        ecuio_plugin_publisher_t publisher;
        ecuio_consent_line_t line;
        size_t terms = 7u;
        int ok = 0;
        memset(&line, 0, sizeof line);
        line.struct_size = (uint32_t)sizeof line;
        memset(&publisher, 0, sizeof publisher);
        publisher.struct_size = sizeof publisher;
        memset(&print, 0, sizeof print);
        print.struct_size = sizeof print - 1u;
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
        /* The fingerprint: a short struct refused, then the whole one */
        ok = ok && sizeof(ecuio_plugin_fingerprint_t) == 72u &&
             offsetof(ecuio_plugin_fingerprint_t, fingerprint) == 8u &&
             ecuio_plugin_fingerprint(plugin, &print) ==
                 ECUIO_ERR_INVALID_ARGUMENT &&
             print.struct_size == sizeof print - 1u;
        print.struct_size = sizeof print;
        ok = ok && ecuio_plugin_fingerprint(plugin, &print) == ECUIO_OK &&
             print.struct_size == sizeof print && print.flags == 0u &&
             (print.fingerprint[0] | print.fingerprint[31]) != 0;
        /* A format 2 plugin names no publisher */
        ok = ok && sizeof(ecuio_plugin_publisher_t) == 520u &&
             offsetof(ecuio_plugin_publisher_t, terms_sha256) == 456u &&
             offsetof(ecuio_plugin_publisher_t, flags) == 488u &&
             ecuio_plugin_publisher(plugin, &publisher) ==
                 ECUIO_ERR_INVALID_ARGUMENT &&
             ecuio_plugin_terms(plugin, NULL, 0, &terms) ==
                 ECUIO_ERR_INVALID_ARGUMENT &&
             terms == 7u;
        /* The operation tier's getters: a plugin without it has no plan */
        {
            ecuio_plugin_operation_t plan;
            ecuio_plugin_operation_step_t step;
            ecuio_plugin_operation_ident_t ident;
            uint32_t plans = 7u;
            memset(&plan, 0, sizeof plan);
            plan.struct_size = sizeof plan;
            memset(&step, 0, sizeof step);
            step.struct_size = sizeof step;
            memset(&ident, 0, sizeof ident);
            ident.struct_size = sizeof ident;
            ok = ok && ECUIO_PLUGIN_TIER_OPERATION == 0x4u &&
                 ECUIO_OPERATION_KIND_DUMP == 1u &&
                 ECUIO_OPERATION_CLASS_READ == 1u &&
                 ECUIO_OPERATION_CLASS_WRITE == 2u && ECUIO_STEP_READ == 1u &&
                 ECUIO_STEP_UPLOAD == 2u &&
                 ECUIO_STEP_SESSION_PROGRAMMING == 2u &&
                 ECUIO_STEP_SESSION_EXTENDED == 3u &&
                 ECUIO_PLUGIN_MAX_OPERATIONS == 8u &&
                 ECUIO_PLUGIN_MAX_STEPS == 64u &&
                 ECUIO_PLUGIN_MAX_IDENTS == 8u &&
                 ecuio_plugin_operation_count(plugin, &plans) == ECUIO_OK &&
                 plans == 0u &&
                 ecuio_plugin_operation_count(NULL, &plans) ==
                     ECUIO_ERR_NULL_ARGUMENT &&
                 ecuio_plugin_operation(plugin, 0, &plan) ==
                     ECUIO_ERR_INVALID_ARGUMENT &&
                 ecuio_plugin_operation_step(plugin, 0, 0, &step) ==
                     ECUIO_ERR_INVALID_ARGUMENT &&
                 ecuio_plugin_operation_ident(plugin, 0, 0, &ident) ==
                     ECUIO_ERR_INVALID_ARGUMENT &&
                 ecuio_plugin_operation(plugin, 0, NULL) ==
                     ECUIO_ERR_NULL_ARGUMENT;
        }
        /* The consent store's record, and its calls' NULL refusals */
        ok = ok && sizeof(ecuio_consent_record_t) == 288u &&
             offsetof(ecuio_consent_record_t, given_utc) == 8u &&
             offsetof(ecuio_consent_record_t, id) == 16u &&
             offsetof(ecuio_consent_record_t, fingerprint) == 64u &&
             offsetof(ecuio_consent_record_t, publisher) == 128u &&
             offsetof(ecuio_consent_record_t, reserved) == 256u &&
             ecuio_consent_store_open(NULL, NULL) == ECUIO_ERR_NULL_ARGUMENT &&
             ecuio_consent_store_close(NULL) == ECUIO_ERR_NULL_ARGUMENT &&
             ECUIO_ERR_CONSENT_REQUIRED == 23;
        /* A consent line: static text, its size by the pointer width */
        ok = ok &&
             sizeof(ecuio_consent_line_t) == 32u + 3u * sizeof(const char*) &&
             offsetof(ecuio_consent_line_t, key) == 16u &&
             offsetof(ecuio_consent_line_t, reserved) ==
                 16u + 3u * sizeof(const char*) &&
             ecuio_consent_line(ECUIO_CONSENT_SET_CONVERSATION, 0, &line) ==
                 ECUIO_OK &&
             line.id == ECUIO_CONSENT_LINE_HAVNE_1 &&
             strcmp(line.key, "asks") == 0 && line.heading[0] == 0;
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
    return 0;
}
