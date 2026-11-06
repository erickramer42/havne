// File/layer: core/include/ecuio/sim.h | C ABI, public interface
// Testing: UNIT: SimEcuTest, SimRecordingTest, SimDidsTest (test_obd2);
//          SimTest (test_socketcan); HARNESS: SimBusTest (test_vcan)

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
 *   mock://<label>[?values=drive|canned][&modules=1|2][&profile=<name>]
 *          [&vin=<VIN>][&faults=<f>:<n>[,...]]
 *   replay://<absolute path>[?loop=0|1][&from=<s>][&to=<s>][&vin=<VIN>]
 *            [&faults=<f>:<n>[,...]]
 * mock://, the mock's car. values: drive (the default) answers Service 01
 * from the mock's drive cycle, canned the fakes' one reading per linear
 * J1979 PID. modules=2: a second module too, on 0x7E1 answering as
 * 0x7E9, with a few made-up Service 22 DIDs of its own (D100, D101,
 * D102) and every other request refused. vin: what Service 09 item 02
 * answers, a made-up one by default.
 * profile: a registered profile (profile.h) whose set's Service 22 DIDs
 * the simulated car answers: each def keyed 62 <DID> at offset 0 is a
 * field of that DID's reply on the module whose reply ID is its
 * message_id, written by inverting the def's scale and offset into its
 * width, signedness, byte order and start byte (a text def answers its
 * name), from the value ecuio_sim_set_value gave it, raw 0 until then.
 * The set's channels are its modules: one on 0x7E0 to 0x7E8 is the
 * engine ECU, which answers the set's DIDs as well as everything else;
 * each other channel is a module answering the set's DIDs alone (7F 22
 * 31 for another, 7F <service> 11 for another service). A set with no
 * channels puts its DIDs on the engine ECU. 11-bit IDs only. Not with
 * modules=2. The set must stay valid while the sim runs, as for a
 * session (profile.h's lifetime).
 * replay://, a recording (its folder or its frames.csv): each request
 * gets the engine ECU's reply the recording held for it at the simulated
 * ECU's time since create, the car's own bitmaps, identification and
 * codes; a request it holds no reply for is refused, and a clear always
 * (a recording can't say what follows one). loop=1 goes round the
 * recording; by default its last replies hold. from and to: a stretch of
 * it alone, in the seconds the recording viewer and havne-cli view show
 * (at most three decimals), its first row at the ECU's start. vin: what
 * 09 02 answers instead of the recording's.
 * faults, both: silent, late, pending (7F <service> 78 first, the reply
 * 300 ms later), bad_sequence and stalled, each every nth reply
 * (1-10000) of each module, the bitmaps never broken.
 *
 * A real controller answers only under ECUIO_SIM_ALLOW_DESK_BUS
 * (ecuio_sim_create_with): a desk bus, with a tester and no car.
 */
typedef struct ecuio_sim ecuio_sim_t;

/* flags */
#define ECUIO_SIM_FAILED  0x00000001u   /* the bus failed (the interface
                                           went down or away): no longer
                                           answering */
#define ECUIO_SIM_YIELDED 0x00000002u   /* another node sent on 0x7E8 (or,
                                           with modules=2, 0x7E9), a reply
                                           ID: silent from then on,
                                           requests only counted */

/* ecuio_sim_create_with's options */
#define ECUIO_SIM_ALLOW_DESK_BUS 0x00000001u /* a real CAN controller is
                                                taken too: the caller says
                                                its bus is a desk's, with
                                                no car on it */

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
    uint32_t reserved0;      /* zero */
    uint64_t pending;        /* replies sent after a 7F <service> 78 */
    uint32_t reserved[2];    /* zero; ABI growth space */
} ecuio_sim_stats_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_sim_stats_t) == 96, "ecuio_sim_stats_t size is ABI");
static_assert(offsetof(ecuio_sim_stats_t, flags) == 72, "sim stats flags offset is ABI");
static_assert(offsetof(ecuio_sim_stats_t, pending) == 80,
              "sim stats pending offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_sim_stats_t) == 96, "ecuio_sim_stats_t size is ABI");
_Static_assert(offsetof(ecuio_sim_stats_t, flags) == 72, "sim stats flags offset is ABI");
_Static_assert(offsetof(ecuio_sim_stats_t, pending) == 80,
               "sim stats pending offset is ABI");
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
 * - profile=: a name not registered, ECUIO_ERR_UNKNOWN_PROFILE; a set
 *   with a 29-bit channel, one on 0x7DF, or one sharing an ID with the
 *   engine ECU's pair but not the pair itself, ECUIO_ERR_INVALID_ARGUMENT;
 *   both on every platform, after the URI and before NO_TRANSPORT.
 * - Whatever the bus, a frame another node sends on 0x7E8 silences it
 *   for good (ECUIO_SIM_YIELDED): an ECU already answers there.
 */
ecuio_status_t ecuio_sim_create(const char* uri, const char* ifname,
                                ecuio_sim_t** out);

/*
 * ecuio_sim_create with options (ECUIO_SIM_*); create is this with 0.
 *
 * CONTRACTS, beyond create's:
 * - An option bit not defined: ECUIO_ERR_INVALID_ARGUMENT, after the
 *   NULL checks.
 * - ECUIO_SIM_ALLOW_DESK_BUS: a real CAN controller is taken when it is
 *   up, not listen-only and error-active, at the rate it is up at, with
 *   or without one-shot mode (ADR-0046's appendix "the desk bus"); else
 *   ECUIO_ERR_LOAD_FAILED with the ip link commands that would change
 *   it. Only for a bus with no car on it: on a car's, the simulated ECU
 *   would answer for its engine ECU until that ECU's first reply.
 */
ecuio_status_t ecuio_sim_create_with(const char* uri, const char* ifname,
                                     uint32_t options, ecuio_sim_t** out);

/* Its counts so far, from any thread. NULL sim or out:
 * ECUIO_ERR_NULL_ARGUMENT. */
ecuio_status_t ecuio_sim_get_stats(const ecuio_sim_t* sim,
                                   ecuio_sim_stats_t* out);

/* The physical value signal_id answers with from now on: a numeric def
 * of the profile's (profile= in the URI) keyed 62 <DID>, the value its
 * field decodes to, rounded to the field's step. From any thread.
 * ECUIO_ERR_NULL_ARGUMENT for a NULL sim; ECUIO_ERR_INVALID_ARGUMENT,
 * the value unchanged, for a sim with no profile, a signal that isn't
 * one of its DIDs' numeric fields, or a value its field can't hold. */
ecuio_status_t ecuio_sim_set_value(ecuio_sim_t* sim, uint16_t signal_id,
                                   double value);

/* Stops answering (a reply under way ends) and frees it. NULL is a
 * no-op. */
void ecuio_sim_destroy(ecuio_sim_t* sim);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_SIM_H */
