// File/layer: core/include/ecuio/sim.h | C ABI, public interface
// Testing: UNIT: SimEcuTest (test_obd2): the answers, the URI and the
//          faults; SimRecordingTest (test_obd2): a recording's answers
//          and its loading; SimTest (test_socketcan): the contract's
//          arguments on every platform; HARNESS: SimBusTest (test_vcan,
//          Linux): a requesting socketcan:// session against it on vcan0,
//          the mock's car and a recording

#ifndef ECUIO_SIM_H
#define ECUIO_SIM_H

#include "ecuio/core.h"    /* ecuio_status_t */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The simulated ECU (ADR-0046): an engine ECU answering OBD-II requests
 * on a virtual CAN bus (Linux's vcan), as 0x7E8 to 0x7DF and 0x7E0,
 * through core's ISO-TP, so a requesting socketcan:// session (ADR-0040)
 * runs end to end with no hardware.
 *
 * The URI, one of:
 *   mock://<label>[?values=drive|canned][&vin=<VIN>][&faults=<f>:<n>[,...]]
 *   replay://<absolute path>[?loop=0|1][&from=<s>][&to=<s>][&vin=<VIN>]
 *            [&faults=<f>:<n>[,...]]
 * mock://, the mock's car. values: drive (the default) answers Service 01
 * from the mock's drive cycle, canned the fakes' one reading per linear
 * J1979 PID. vin: what Service 09 item 02 answers, a made-up one by
 * default.
 * replay://, a recording (its folder or its frames.csv): each request
 * gets the engine ECU's reply the recording held for it at the simulated
 * ECU's time since create, the car's own bitmaps, identification and
 * codes; a request it holds no reply for is refused, and a clear always
 * (a recording can't say what follows one). loop=1 goes round the
 * recording; by default its last replies hold. from and to: a stretch of
 * it alone, in the seconds the recording viewer and havne-cli view show
 * (at most three decimals), its first row at the ECU's start. vin: what
 * 09 02 answers instead of the recording's.
 * faults, both: silent, late, bad_sequence and stalled, each every nth
 * reply (1-10000), the bitmaps never broken.
 */
typedef struct ecuio_sim ecuio_sim_t;

/* flags */
#define ECUIO_SIM_FAILED  0x00000001u   /* the bus failed (the interface
                                           went down or away): no longer
                                           answering */

/* POD, 96 bytes, no hidden padding; pinned by the assertions below.
 * Counts since create. */
typedef struct ecuio_sim_stats {
    uint64_t requests;       /* whole requests taken */
    uint64_t replies;        /* replies sent, late ones included */
    uint64_t refused;        /* negative replies among them */
    uint64_t silent;         /* replies a fault held back */
    uint64_t late;
    uint64_t bad_sequence;
    uint64_t stalled;
    uint64_t unflowed;       /* segmented replies the tester's Flow
                                Control never came for */
    uint64_t clears;         /* Service 04s obeyed */
    uint32_t flags;          /* ECUIO_SIM_* */
    uint32_t reserved[5];    /* zero; ABI growth space */
} ecuio_sim_stats_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_sim_stats_t) == 96, "ecuio_sim_stats_t size is ABI");
static_assert(offsetof(ecuio_sim_stats_t, flags) == 72, "sim stats flags offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_sim_stats_t) == 96, "ecuio_sim_stats_t size is ABI");
_Static_assert(offsetof(ecuio_sim_stats_t, flags) == 72, "sim stats flags offset is ABI");
#endif

/*
 * Starts a simulated ECU on interface ifname, answering from return
 * until destroy.
 *
 * CONTRACTS:
 * - NULL uri, ifname or out: ECUIO_ERR_NULL_ARGUMENT.
 * - A URI that isn't mock:// or replay://, a query key the scheme
 *   doesn't take, a bad value, or a recording's path that isn't
 *   absolute: ECUIO_ERR_BAD_URI, on every platform.
 * - Off Linux: ECUIO_ERR_NO_TRANSPORT.
 * - replay://: a frames.csv that won't open or whose row won't parse
 *   (the detail names the line, never the path), or one with no reply
 *   from the engine ECU: ECUIO_ERR_LOAD_FAILED; loop=1 on a recording
 *   whose replies share one time: ECUIO_ERR_BAD_URI.
 * - ifname not a vcan interface, or down, or a socket that won't open:
 *   ECUIO_ERR_LOAD_FAILED. A real CAN controller is refused: the
 *   simulated ECU would answer for a car's own.
 * - ecuio_last_error_detail() says why, on the calling thread.
 */
ecuio_status_t ecuio_sim_create(const char* uri, const char* ifname,
                                ecuio_sim_t** out);

/* Its counts so far, from any thread. NULL sim or out:
 * ECUIO_ERR_NULL_ARGUMENT. */
ecuio_status_t ecuio_sim_get_stats(const ecuio_sim_t* sim,
                                   ecuio_sim_stats_t* out);

/* Stops answering (a reply under way ends) and frees it. NULL is a
 * no-op. */
void ecuio_sim_destroy(ecuio_sim_t* sim);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_SIM_H */
