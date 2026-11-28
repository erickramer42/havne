/* File/layer: tests/test_c_abi_diag.c | strict C part of test_c_abi
 * Testing: UNIT: group names, display convert, channel reports, the sur request, lists, inspect and the sims, compile and link, run as c_abi and c_abi_shared
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

int c_abi_diag(void)
{
    ecuio_channel_support_t support;
    ecuio_channel_identity_t identity;
    ecuio_channel_diagnostics_t diagnostics;
    ecuio_display_value_t shown;
    ecuio_sur_request_t sur_request;
    ecuio_transport_ops_t ops;
    uint32_t serial_count = 0;
    uint32_t can_count = 0;
    ecuio_sim_t* sim = NULL;
    ecuio_sim_stats_t sim_stats;
    ecuio_sim_gateway_t gateway;
    ecuio_sim_gateway_override_t route;
    ecuio_inspection_t inspection;
    const uint8_t vin_request[2] = {0x09, 0x02};
    char dtc[6];
    char uds[ECUIO_DTC_UDS_TEXT_LEN];

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
        ecuio_session_clear_diagnostics_set(NULL, NULL, 0, 0) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ECUIO_CLEAR_MOVING != 7u || ECUIO_CLEAR_NO_VEHICLE_SPEED != 8u ||
        ECUIO_CLEAR_NOT_EMISSIONS != 9u || ECUIO_CLEAR_NOT_OPENED != 10u ||
        ECUIO_ERR_IN_USE != 20 || ECUIO_ERR_REPORT_CHANGED != 17 ||
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
    if (ecuio_dtc_format_uds(0x012800u, ECUIO_DTC_FORMAT_J2012, uds,
                             sizeof uds) != ECUIO_OK ||
        strcmp(uds, "P0128-00") != 0 ||
        ecuio_dtc_title_uds(0x012800u, ECUIO_DTC_FORMAT_J1939)[0] != 0 ||
        strcmp(ecuio_dtc_status_state(0x01), "Active") != 0 ||
        strcmp(ecuio_dtc_status_class(0x08), "Stored") != 0 ||
        ecuio_dtc_status_lamp(ECUIO_DTC_STATUS_LAMP) != 1 ||
        ecuio_module_code_emissions_lists(NULL, 0x030100u, 0) != 0 ||
        ECUIO_MODULE_CODES_MASK_DEFAULT != 0xAFu ||
        ECUIO_MODULE_CODES_NOT_OPENED != 0x10u) {
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
        ECUIO_SIM_REPLY_BAD_SEQUENCE != 4u || ECUIO_SIM_REPLY_STALLED != 8u ||
        ECUIO_SIM_REPLY_GATEWAY != 16u) {
        return 1;
    }
    /* The simulated gateway's view */
    if (ecuio_sim_get_gateway(NULL, &gateway) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_sim_get_gateway_override(NULL, 0, &route) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ECUIO_SIM_CLASS_COUNT != 5u || ECUIO_SIM_ROUTE_GATED_SILENT != 5u ||
        ECUIO_SIM_GATEWAY_UNSUPPRESSED != 0x80u) {
        return 1;
    }
    ecuio_sim_destroy(NULL);
    return 0;
}
