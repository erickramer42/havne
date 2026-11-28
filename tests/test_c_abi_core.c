/* File/layer: tests/test_c_abi_core.c | strict C part of test_c_abi
 * Testing: UNIT: the version, validation, profile, session and values refusals, compile and link, run as c_abi and c_abi_shared
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

static int c_abi_awake(void);

int c_abi_core(void)
{
    const ecuio_signal_set_t* set = NULL;
    ecuio_validation_t validation;
    ecuio_session_info_t info;
    ecuio_channel_conversation_t conversation;
    uint32_t wait_ms = 7;
    uint32_t version_major = 0, version_minor = 0, version_patch = 0;

    if (ecuio_version(&version_major, &version_minor, &version_patch) !=
            ECUIO_OK ||
        version_major != ECUIO_VERSION_MAJOR ||
        version_minor != ECUIO_VERSION_MINOR ||
        version_patch != ECUIO_VERSION_PATCH ||
        ecuio_version(NULL, &version_minor, &version_patch) !=
            ECUIO_ERR_NULL_ARGUMENT) {
        return 1;
    }
    if (ecuio_session_get_channel_conversation(NULL, 0, &conversation) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_security_store(NULL, "s.json", NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_security_unlock(NULL, 0, 0x01, 0, &wait_ms) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        wait_ms != 0 || ECUIO_ERR_TOO_SOON != 22 ||
        ECUIO_SUPPORT_CONVERSATION != 0x40u ||
        ECUIO_SUPPORT_CLOSED != 0x80u || ECUIO_SUPPORT_NO_ROOM != 0x100u ||
        ECUIO_MODULE_REASON_RANGE != 0x1u || ECUIO_MODULE_REASON_READ != 0x2u ||
        ECUIO_MODULE_REASON_CONVERSATION != 0x4u ||
        ECUIO_MODULE_REASON_VISIT != 0x8u ||
        ECUIO_DIAG_NOT_OPENED != 0x400u) {
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
        ECUIO_VALIDATE_ERR_CHANNEL_SESSION != -27 ||
        sizeof(ECUIO_TIMING_COLUMNS) < 2u ||
        (ECUIO_SESSION_INFO_REPLAY_FAULTS & ECUIO_SESSION_INFO_LISTEN_ONLY) !=
            0u ||
        (ECUIO_SESSION_INFO_REQUESTS & ECUIO_SESSION_INFO_REPLAY_FAULTS) !=
            0u ||
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
    return c_abi_awake();
}

/* The keep-awake calls: a mock holds nothing, and the refusals */
static int c_abi_awake(void)
{
    ecuio_session_t* session = NULL;
    uint32_t what = 99u;
    int bad = 0;
    if (ECUIO_AWAKE_SYSTEM != 0x1u || ECUIO_AWAKE_WHY_MAX != 128 ||
        ecuio_session_keep_awake(NULL, ECUIO_AWAKE_SYSTEM, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_get_awake(NULL, &what) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_session_create("mock://", &session) != ECUIO_OK) {
        return 1;
    }
    bad = ecuio_session_keep_awake(session, ECUIO_AWAKE_SYSTEM, "c_abi") !=
              ECUIO_OK ||
          ecuio_session_get_awake(session, &what) != ECUIO_OK || what != 0u ||
          ecuio_session_keep_awake(session, 0x2u, NULL) !=
              ECUIO_ERR_INVALID_ARGUMENT ||
          ecuio_session_get_awake(session, NULL) != ECUIO_ERR_NULL_ARGUMENT;
    ecuio_session_destroy(session);
    return bad;
}
