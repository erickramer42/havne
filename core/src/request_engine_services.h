// File/layer: core/src/request_engine_services.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// The services a request may carry, the session requests and the
// clear's (each pinned by a static_assert), and what a refusal means.
// C++17, Qt-free, no platform headers.
// Testing: COMPILE: the static_asserts; UNIT: RequestEngineTest,
//          SessionControlTest, ClearTest (test_obd2)

#ifndef ECUIO_REQUEST_ENGINE_SERVICES_H
#define ECUIO_REQUEST_ENGINE_SERVICES_H

#include "request_engine_ident.h"

/* Section 3: the services a request may carry. 0x01 (current data), 0x02 (the
 * freeze frame), 0x03, 0x07 and 0x0A (stored, pending and permanent
 * trouble codes; ADR-0037), 0x09 (vehicle information) and 0x22
 * (ReadDataByIdentifier); nothing that clears codes, changes a session
 * (ADR-0060's session step is its own one-shot, below), unlocks or
 * writes. Every request enters the table through
 * ecuio_engine_add, which refuses the rest, and the one-shot lists are
 * checked against it below, so the rule holds whatever path enrols a
 * request. */
constexpr bool ecuio_engine_service_allowed(uint8_t sid)
{
    return sid == 0x01 || sid == 0x02 || sid == 0x03 || sid == 0x07 ||
           sid == 0x09 || sid == 0x0A || sid == 0x22;
}

constexpr bool ecuio_engine_lists_allowed()
{
    for (const ecuio_ident_request& q : ECUIO_IDENT_REQUESTS) {
        if (!ecuio_engine_service_allowed(q.bytes[0])) {
            return false;
        }
    }
    for (const auto& q : ECUIO_DIAG_FIRST) {
        if (!ecuio_engine_service_allowed(q[0])) {
            return false;
        }
    }
    return ecuio_engine_service_allowed(0x02) &&
           !ecuio_engine_service_allowed(0x04);
}
static_assert(ecuio_engine_lists_allowed(),
              "every one-shot list asks only what §3 allows, and never 04");

/* ADR-0060: the session requests the engine may send, and no other: the
 * extended session (10 03), the default one a close returns to (10 01)
 * and tester present with a reply (3E 00). Never the programming session
 * (10 02), a reset, security access (27) or a write. These never enter
 * the table, which ecuio_engine_service_allowed keeps; the session's
 * one-shot is built only through this check. */
constexpr bool ecuio_engine_session_request_allowed(uint8_t sid, uint8_t sub)
{
    return (sid == ECUIO_SESSION_CONTROL &&
            (sub == ECUIO_SESSION_DEFAULT || sub == ECUIO_SESSION_EXTENDED)) ||
           (sid == ECUIO_TESTER_PRESENT && sub == 0x00);
}
static_assert(ecuio_engine_session_request_allowed(0x10, 0x03) &&
                  ecuio_engine_session_request_allowed(0x10, 0x01) &&
                  ecuio_engine_session_request_allowed(0x3E, 0x00) &&
                  !ecuio_engine_session_request_allowed(0x10, 0x02) &&
                  !ecuio_engine_session_request_allowed(0x10, 0x04) &&
                  !ecuio_engine_session_request_allowed(0x3E, 0x80) &&
                  !ecuio_engine_session_request_allowed(0x27, 0x01) &&
                  !ecuio_engine_session_request_allowed(0x11, 0x01) &&
                  !ecuio_engine_service_allowed(0x10) &&
                  !ecuio_engine_service_allowed(0x3E) &&
                  !ecuio_engine_service_allowed(0x27) &&
                  !ecuio_engine_service_allowed(0x2E) &&
                  !ecuio_engine_service_allowed(0x2F) &&
                  !ecuio_engine_service_allowed(0x31) &&
                  !ecuio_engine_service_allowed(0x14),
              "sessions: 10 01, 10 03 and 3E 00 only, and never in the table");

/* A clear (ADR-0038) asks the engine speed, then, only at 0 rpm, sends
 * 04; with the caller's word on a running engine it asks the vehicle
 * speed too, and sends 04 at 0 km/h or with the caller's word on a
 * moving one. These are the requests it builds; 04 only it builds: 04
 * never enters the table or a list, and the table refuses it. */
constexpr uint8_t ECUIO_CLEAR_SPEED_REQUEST[2] = {0x01, 0x0C};
constexpr uint8_t ECUIO_CLEAR_VEHICLE_SPEED_REQUEST[2] = {0x01, 0x0D};
constexpr uint8_t ECUIO_CLEAR_SERVICE = 0x04;
static_assert(
    ecuio_engine_service_allowed(ECUIO_CLEAR_SPEED_REQUEST[0]) &&
        ecuio_engine_service_allowed(ECUIO_CLEAR_VEHICLE_SPEED_REQUEST[0]) &&
        !ecuio_engine_service_allowed(ECUIO_CLEAR_SERVICE),
    "a clear reads the engine and vehicle speeds, then sends 04, "
    "which nothing else may");

constexpr uint8_t ECUIO_CODES_SERVICE = 0x19;

constexpr bool ecuio_engine_codes_request_allowed(uint8_t sid, uint8_t sub)
{
    return sid == ECUIO_CODES_SERVICE && (sub == 0x01 || sub == 0x02);
}
static_assert(ecuio_engine_codes_request_allowed(0x19, 0x01) &&
                  ecuio_engine_codes_request_allowed(0x19, 0x02) &&
                  !ecuio_engine_codes_request_allowed(0x19, 0x03) &&
                  !ecuio_engine_codes_request_allowed(0x19, 0x04) &&
                  !ecuio_engine_codes_request_allowed(0x19, 0x06) &&
                  !ecuio_engine_codes_request_allowed(0x14, 0x01) &&
                  !ecuio_engine_codes_request_allowed(0x04, 0x00) &&
                  !ecuio_engine_service_allowed(ECUIO_CODES_SERVICE),
              "the codes read sends 19 01 and 19 02 alone, outside the table");

/* Section 4: what a negative reply's code says of its request, as
 * ECUIO_DROP_*: NOT_SUPPORTED for 0x12 (the first vehicle's answer to an
 * unsupported PID) and 0x31; SERVICE for 0x11; ACCESS for 0x33 and 0x7F,
 * since the engine never unlocks, and enters only the extended session it
 * was asked for; 0 for any code that can pass, like 0x22 (conditions not
 * correct) or 0x21 (busy). */
inline uint8_t ecuio_engine_drop_reason(uint8_t nrc)
{
    switch (nrc) {
    case ECUIO_NRC_SUBFUNCTION_NOT_SUPPORTED:
    case ECUIO_NRC_REQUEST_OUT_OF_RANGE:
        return ECUIO_DROP_NOT_SUPPORTED;
    case ECUIO_NRC_SERVICE_NOT_SUPPORTED:
        return ECUIO_DROP_SERVICE;
    case ECUIO_NRC_SECURITY_ACCESS_DENIED:
    case ECUIO_NRC_NOT_IN_ACTIVE_SESSION:
        return ECUIO_DROP_ACCESS;
    default:
        return 0;
    }
}

#endif
