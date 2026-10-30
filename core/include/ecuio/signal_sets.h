#ifndef ECUIO_SIGNAL_SETS_H
#define ECUIO_SIGNAL_SETS_H

#include <ecuio/signals.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported signal IDs for bridge/UI reference */
#define ECUIO_MOCK_SIG_RPM_ID      1001
#define ECUIO_MOCK_SIG_COOLANT_ID  2001
#define ECUIO_MOCK_SIG_SPEED_ID    3001
#define ECUIO_MOCK_SIG_THROTTLE_ID 4001
#define ECUIO_MOCK_SIG_IAT_ID      5001
#define ECUIO_MOCK_SIG_LOAD_ID     6001
#define ECUIO_MOCK_SIG_MAF_ID      8001
#define ECUIO_MOCK_SIG_OIL_ID      9001
#define ECUIO_MOCK_SIG_LAMBDA_ID   10001
/* CAN arbitration ID carrying the mock telemetry payload. SHARED by
 * mock_transport.cpp (encoder) and signal_sets.cpp (signal defs) so
 * the two halves of the round-trip contract cannot drift apart. */
#define ECUIO_MOCK_TELEMETRY_ARB_ID  0x0C0u
/* Mock telemetry payload length. The signals need 12 bytes and classic
 * CAN carries 8, so mock frames are CAN FD (ECUIO_FRAME_FLAG_FD) at
 * 12 bytes, a valid FD length. Shared by the encoder, the worker and
 * the tests. */
#define ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN  12u

/* ---- OBD-II v1 set (ADR-0013) ----
 * IDs occupy a distinct band from the mock set (no cross-set def.id
 * collision even though only one set is ever active per session).
 * Wire shape pinned by the fake's canned responses AND the HARNESS
 * tests in tests/test_obd2.cpp. */
#define ECUIO_OBD2_SIG_ENGINE_LOAD_ID  7001
#define ECUIO_OBD2_SIG_COOLANT_TEMP_ID 7002
#define ECUIO_OBD2_SIG_RPM_ID          7003
#define ECUIO_OBD2_SIG_SPEED_ID        7004
#define ECUIO_OBD2_SIG_IAT_ID          7005
#define ECUIO_OBD2_SIG_THROTTLE_ID     7006
#define ECUIO_OBD2_SIG_FUEL_LEVEL_ID   7007
#define ECUIO_OBD2_SIG_BATTERY_V_ID    7008
#define ECUIO_OBD2_SIG_MAF_ID          7009
#define ECUIO_OBD2_SIG_OIL_TEMP_ID     7010
/* ---- uds-generic (ADR-0020) ----
 * Its set is built per session from the URI's ?did= entries, one def
 * per DID in the order listed, with ids from this band: the first def is
 * the base, so the band holds every single DID a session can poll
 * (ECUIO_PROFILE_MAX_REQUESTS) plus every DID it can scan. Distinct from
 * the mock and obd2 bands above. */
#define ECUIO_UDS_GENERIC_SIG_ID_BASE  20000u

/* OBD-II ECU #1 physical response ID, paired with the tester request
 * ID 0x7E0. Shared by obd2_signal_sets.cpp (defs), test fixtures, and
 * the fake's canned response header so all three agree on the channel. */
#define ECUIO_OBD2_ECU_ARB_ID  0x7E8u

/* OBD-II tester request arbitration ID: physical pairing with
 * ECUIO_OBD2_ECU_ARB_ID (0x7E0 <-> 0x7E8). Consumed by the obd2 set's
 * channel declaration and the transport's fallback, so the two
 * cannot drift apart (ECUIO_MOCK_TELEMETRY_ARB_ID precedent). */
#define ECUIO_OBD2_TESTER_ARB_ID  0x7E0u

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_SIGNAL_SETS_H */
