#ifndef QTTUNE_SIGNAL_SETS_H
#define QTTUNE_SIGNAL_SETS_H

#include <qttune/signals.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported signal IDs for bridge/UI reference */
#define MOCK_SIG_RPM_ID      1001
#define MOCK_SIG_COOLANT_ID  2001
#define MOCK_SIG_SPEED_ID    3001
#define MOCK_SIG_THROTTLE_ID 4001
#define MOCK_SIG_IAT_ID      5001
#define MOCK_SIG_LOAD_ID     6001
/* CAN arbitration ID carrying the mock telemetry payload. SHARED by
 * mock_transport.cpp (encoder) and signal_sets.cpp (signal defs) so
 * the two halves of the round-trip contract cannot drift apart. */
#define MOCK_TELEMETRY_ARB_ID  0x0C0u

#ifdef __cplusplus
}
#endif

#endif /* QTTUNE_SIGNAL_SETS_H */
