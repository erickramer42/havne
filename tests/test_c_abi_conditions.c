/* File/layer: tests/test_c_abi_conditions.c | strict C part of test_c_abi
 * Testing: UNIT: a condition file read from C, compile and link, run as c_abi and c_abi_shared
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
int c_abi_conditions(void)
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

