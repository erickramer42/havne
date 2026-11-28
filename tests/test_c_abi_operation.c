/* File/layer: tests/test_c_abi_operation.c | strict C part of test_c_abi
 * Testing: UNIT: ecuio/operation.h's layouts and NULL refusals, compile and link, run as c_abi and c_abi_shared
 */

#include "ecuio/operation.h"
#include "ecuio/core.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "test_c_abi_parts.h"

int c_abi_operation(void)
{
    int ok = 1;
    ecuio_operation_request_t request;
    ecuio_operation_state_t state;

    ok = ok && sizeof(ecuio_operation_request_t) == 32u &&
         offsetof(ecuio_operation_request_t, operation) == 4u &&
         offsetof(ecuio_operation_request_t, flags) == 8u &&
         offsetof(ecuio_operation_request_t, reserved) == 12u;
    ok = ok && sizeof(ecuio_operation_state_t) == 336u &&
         offsetof(ecuio_operation_state_t, generation) == 4u &&
         offsetof(ecuio_operation_state_t, phase) == 12u &&
         offsetof(ecuio_operation_state_t, end) == 16u &&
         offsetof(ecuio_operation_state_t, step) == 24u &&
         offsetof(ecuio_operation_state_t, nrc) == 28u &&
         offsetof(ecuio_operation_state_t, routine_status) == 29u &&
         offsetof(ecuio_operation_state_t, routine_status_given) == 30u &&
         ECUIO_OPERATION_CHECK_DFI_CLAIMED == 0x400u &&
         offsetof(ecuio_operation_state_t, bytes_done) == 32u &&
         offsetof(ecuio_operation_state_t, bytes_total) == 40u &&
         offsetof(ecuio_operation_state_t, differ_offset) == 48u &&
         offsetof(ecuio_operation_state_t, name) == 56u &&
         offsetof(ecuio_operation_state_t, words) == 152u &&
         offsetof(ecuio_operation_state_t, volts_mv) == 312u &&
         offsetof(ecuio_operation_state_t, volts_min_mv) == 316u &&
         offsetof(ecuio_operation_state_t, checks) == 320u &&
         offsetof(ecuio_operation_state_t, waiting_ms) == 324u &&
         offsetof(ecuio_operation_state_t, rpm) == 328u &&
         offsetof(ecuio_operation_state_t, kmh) == 330u &&
         offsetof(ecuio_operation_state_t, reserved) == 332u;
    ok = ok && ECUIO_OPERATION_MAY_UNLOCK == 1u &&
         ECUIO_OPERATION_PARKED == 2u && ECUIO_OPERATION_PHASE_NONE == 0u &&
         ECUIO_OPERATION_PHASE_CHECKING == 1u &&
         ECUIO_OPERATION_PHASE_PRECONDITIONS == 2u &&
         ECUIO_OPERATION_PHASE_UNLOCKING == 3u &&
         ECUIO_OPERATION_PHASE_READING == 4u &&
         ECUIO_OPERATION_PHASE_COMPARING == 5u &&
         ECUIO_OPERATION_PHASE_SAVING == 6u &&
         ECUIO_OPERATION_PHASE_LEAVING == 7u &&
         ECUIO_OPERATION_PHASE_DONE == 8u;
    ok = ok && ECUIO_OPERATION_END_NONE == 0u &&
         ECUIO_OPERATION_END_SAVED == 1u &&
         ECUIO_OPERATION_END_CANCELED == 2u &&
         ECUIO_OPERATION_END_REFUSED == 3u &&
         ECUIO_OPERATION_END_NO_ANSWER == 4u &&
         ECUIO_OPERATION_END_DIFFER == 5u &&
         ECUIO_OPERATION_END_IDENTITY_CHANGED == 6u &&
         ECUIO_OPERATION_END_UNLOCK_REFUSED == 7u &&
         ECUIO_OPERATION_END_SESSION_LOST == 8u &&
         ECUIO_OPERATION_END_LINK_LOST == 9u &&
         ECUIO_OPERATION_END_FILE == 10u &&
         ECUIO_OPERATION_END_BAD_REPLY == 11u &&
         ECUIO_OPERATION_END_ENGINE_RUNNING == 12u &&
         ECUIO_OPERATION_END_MOVING == 13u &&
         ECUIO_OPERATION_END_VOLTAGE_LOW == 14u &&
         ECUIO_OPERATION_CHECK_RPM == 1u && ECUIO_OPERATION_CHECK_KMH == 2u &&
         ECUIO_OPERATION_CHECK_VOLTS == 4u &&
         ECUIO_OPERATION_CHECK_VOLTS_FROM_ECU == 8u &&
         ECUIO_OPERATION_CHECK_VOLTS_LOW == 0x10u &&
         ECUIO_OPERATION_CHECK_VOLTS_HIGH == 0x20u &&
         ECUIO_OPERATION_CHECK_NOT_HELD_AWAKE == 0x40u &&
         ECUIO_OPERATION_CHECK_ON_BATTERY == 0x80u &&
         ECUIO_OPERATION_CHECK_VOLTS_SAG == 0x100u &&
         ECUIO_OPERATION_CHECK_IDENT_UNREAD == 0x200u &&
         ECUIO_OPERATION_BATTERY_SUPPORT == 4u &&
         ECUIO_OPERATION_END_VOLTAGE_HIGH == 15u;
    if (!ok) {
        fprintf(stderr, "c_abi: an operation struct's layout moved\n");
        return 1;
    }

    memset(&request, 0, sizeof request);
    memset(&state, 0, sizeof state);
    request.struct_size = sizeof request;
    state.struct_size = sizeof state;
    ok = ok && ecuio_session_operation_start(NULL, &request, "") ==
                   ECUIO_ERR_NULL_ARGUMENT;
    ok = ok && ecuio_session_operation_cancel(NULL) == ECUIO_ERR_NULL_ARGUMENT;
    ok = ok &&
         ecuio_session_get_operation(NULL, &state) == ECUIO_ERR_NULL_ARGUMENT;
    if (!ok) {
        fprintf(stderr, "c_abi: an operation call's refusal moved\n");
        return 1;
    }
    return 0;
}
