// File/layer: core/include/ecuio/sim.h | C ABI, public interface
// Testing: UNIT: SimEcuTest, SimRecordingTest, SimDidsTest, SimAskTest
//          (test_obd2);
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
 *          [&sessions=0|1]
 *          [&vin=<VIN>][&faults=<f>:<n>[@<reply ID>][,...]][&<timing>]
 *          [&gateway=<request ID>:<reply ID>|1][&behind=<reply ID>[,...]]
 *          [&route=<class or service>:<outcome>[@<reply ID>][,...]]
 *          [&gwopt=<knob>[,...]]
 *   replay://<absolute path>[?loop=0|1][&from=<s>][&to=<s>][&vin=<VIN>]
 *            [&faults=<f>:<n>[@<reply ID>][,...]][&<timing>]
 *   where <timing> is any of delay=<p50 us>:<p99 us>[@<reply ID>][,...],
 *   fc=<us>[@<reply ID>][,...], cf=<us>[@<reply ID>][,...], seed=<n>,
 *   busy=ignore|21[@<reply ID>][,...] and bus=<kbit/s>
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
 * session (profile.h's lifetime). sessions=1 (only with profile=): each
 * of the set's modules has the extended session too (10 03 enters it,
 * 3E 00 keeps it, S3 of silence leaves it), with no security access.
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
 * (1-10000) of each module, on that module's own count, the bitmaps
 * never broken. @<reply ID> (11-bit hex) aims a fault at the module
 * answering on it alone (faults=late:3@7EA), in place of that fault
 * for every module; at most 32 aims. A faults key given twice adds to
 * the first, a later item in place of an earlier, as the timing keys do.
 * The timing, both: delay is each reply's time from the request to its
 * first frame, a log-normal through its median p50 and its 99th
 * percentile p99 (1 <= p50 <= p99 <= 10000000), drawn on a generator of
 * each module's own from seed (a decimal, 1 by default), one draw for
 * each request the module answers; a late fault adds its time to the
 * draw, a pending reply keeps its own. fc is the time from the tester's
 * Flow Control to the first Consecutive Frame, cf the module's least gap
 * between Consecutive Frames, the tester's STmin when that is longer
 * (each 0 to 1000000). @<reply ID> aims an item at one module, in place
 * of the key's item for every module; at most 32 aims for each key. A
 * key may come again: its later items replace earlier ones, and its aims
 * count together.
 * With none of them, a reply starts at once and its frames go at the
 * tester's pace.
 * busy: what a module does with a request that comes while its reply
 * waits or is under way: ignore it, or answer 7F <service> 21 (while its
 * frames are under way on a bus, it ignores it). Without it a module on
 * a bus ignores it and an unbound one answers it as any other. bus: the
 * bus's rate (1-1000 kbit/s), each frame 111 bits (stuffing left out): a
 * reply's frames wait for it, while the sim's Flow Control, 7F 78 and
 * 7F 21 go at once and hold it after; a bus's sim only.
 * gateway, mock:// only: a gateway node of its own, on the 11-bit IDs
 * given or, with 1, on 0x6F0 answering as 0x6F8 (made up, like every
 * value of the simulated car), neither one any other node's. It answers
 * 10 01, 10 03, 3E 00 and 3E 80 on its own request ID, every other
 * service 7F <service> 11, and no functional request. behind: the
 * modules behind it by their reply IDs (modules=2's or a profile's, at
 * most 32, never the engine ECU); the gateway judges each request for
 * one before the module hears it, so a request it withholds moves
 * none of the module's counts, draws or busy time. route: what it does
 * with a request by its service byte's class (reads 01, 02, 03, 06, 07,
 * 09, 0A, 19, 21, 22, 23, 24; session 10, 27, 28, 3E, 85; write 04, 08,
 * 14, 2C, 2E, 2F, 31, 3D; prog 11, 34, 35, 36, 37, 38; other the rest)
 * or by one service byte (01-BF): forward (the default for anything not
 * named), refuse (7F <service> 22 on the module's reply ID, or the code
 * .<NRC> names, 10-FE but 78) or silent. @<reply ID> aims an entry at one
 * module behind it; the most specific entry wins: a service for the
 * module, a class for the module, a service, a class. A later entry for
 * the same aim replaces the earlier; a second route key adds; at most
 * 64 entries besides the classes. A refusal is the gateway's reply,
 * flagged ECUIO_SIM_REPLY_GATEWAY: its faults, timing and busy time
 * (each aimed at its reply ID), not the module's. A functional request
 * reaches a module whose entry forwards; one withheld is silent, unless
 * gwopt=aloud, which refuses it for each module, keeping ISO 14229-1's
 * suppressed codes (11, 12, 31, 7E, 7F) silent unless gwopt=unsuppressed
 * too. behind, route and gwopt need gateway.
 *
 * A real controller answers only under ECUIO_SIM_ALLOW_DESK_BUS
 * (ecuio_sim_create_with): a desk bus, with a tester and no car.
 */
typedef struct ecuio_sim ecuio_sim_t;

/* flags */
#define ECUIO_SIM_FAILED  0x00000001u   /* the bus failed (the interface
                                           went down or away): no longer
                                           answering */
#define ECUIO_SIM_YIELDED 0x00000002u   /* another node sent on 0x7E8 (or
                                           another of its nodes' reply IDs,
                                           the gateway's too): silent from
                                           then on, requests only counted */

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
 * - faults=, delay=, fc= or cf= aiming at a reply ID none of the sim's
 *   ECUs (the gateway's included) answers on, behind= naming a reply ID
 *   no module answers on, or gateway= meeting a module's ID:
 *   ECUIO_ERR_BAD_URI, the detail naming the IDs there are; after the
 *   profile, whose modules it needs, and before NO_TRANSPORT. A gateway's
 *   key on replay:// is BAD_URI with the URI.
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

/*
 * A simulated ECU on no bus (ADR-0055): the same car, answering only
 * ecuio_sim_ask, on every platform. It never opens a socket or reaches
 * a bus; a caller that puts its replies somewhere (havne-cli's ELM327
 * emulator, a script) chooses where.
 *
 * CONTRACTS:
 * - NULL uri or out: ECUIO_ERR_NULL_ARGUMENT.
 * - options: none is defined yet; any bit is ECUIO_ERR_INVALID_ARGUMENT,
 *   after the NULL checks.
 * - The URI, its profile and its recording as ecuio_sim_create's, in
 *   that order, on every platform: BAD_URI, UNKNOWN_PROFILE,
 *   INVALID_ARGUMENT, LOAD_FAILED. bus= is BAD_URI, with the URI: an
 *   unbound sim's caller times its frames.
 * - ecuio_sim_get_stats, ecuio_sim_set_value and ecuio_sim_destroy work
 *   on it as on a sim on a bus; its flags stay 0.
 */
ecuio_status_t ecuio_sim_create_unbound(const char* uri, uint32_t options,
                                        ecuio_sim_t** out);

/* ecuio_sim_ask's time_us: the sim's own clock, steady time since
 * create */
#define ECUIO_SIM_NOW (-1)

/* The most replies one request gets: each ECU's (the engine ECU and up
 * to signals.h's ECUIO_MAX_CHANNELS modules), each with a
 * 7F <service> 78 ahead of it */
#define ECUIO_SIM_MAX_REPLIES 66u

/* ecuio_sim_reply_t flags */
#define ECUIO_SIM_REPLY_PENDING      0x00000001u /* 7F <service> 78: the
                                                    same ECU's reply follows
                                                    in a later entry */
#define ECUIO_SIM_REPLY_LATE         0x00000002u /* put off by the late
                                                    fault (delay_us) */
#define ECUIO_SIM_REPLY_BAD_SEQUENCE 0x00000004u /* segmented: its last
                                                    Consecutive Frame's
                                                    sequence number skips
                                                    one */
#define ECUIO_SIM_REPLY_STALLED      0x00000008u /* segmented: its last
                                                    Consecutive Frame comes
                                                    160 ms after the one
                                                    before */
#define ECUIO_SIM_REPLY_GATEWAY      0x00000010u /* the gateway wrote it for
                                                    the module behind it
                                                    whose reply ID it
                                                    carries */

/* One ECU's reply to a request. POD, 4136 bytes, no hidden padding;
 * pinned by the assertions below. */
typedef struct ecuio_sim_reply {
    uint32_t reply_id;    /* the 11-bit CAN ID it is sent on */
    uint32_t flags;       /* ECUIO_SIM_REPLY_* */
    int64_t delay_us;     /* when it starts, after the request: the
                             module's drawn delay (delay=), plus a
                             fault's */
    uint32_t length;      /* bytes of data: 1..4095, ISO-TP's longest */
    uint32_t fc_us;       /* fc=: the tester's Flow Control to the first
                             Consecutive Frame, 0 for at once */
    uint32_t cf_us;       /* cf=: the module's least gap between
                             Consecutive Frames; a longer STmin wins */
    uint32_t reserved[3]; /* zero; ABI growth space */
    uint8_t data[ECUIO_FRAME_DATA_MAX]; /* the whole message, from its
                                           service byte; zero past
                                           length */
} ecuio_sim_reply_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_sim_reply_t) == 4136,
              "ecuio_sim_reply_t size is ABI");
static_assert(offsetof(ecuio_sim_reply_t, delay_us) == 8,
              "sim reply delay_us offset is ABI");
static_assert(offsetof(ecuio_sim_reply_t, fc_us) == 20,
              "sim reply fc_us offset is ABI");
static_assert(offsetof(ecuio_sim_reply_t, cf_us) == 24,
              "sim reply cf_us offset is ABI");
static_assert(offsetof(ecuio_sim_reply_t, reserved) == 28,
              "sim reply reserved offset is ABI");
static_assert(offsetof(ecuio_sim_reply_t, data) == 40,
              "sim reply data offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_sim_reply_t) == 4136,
               "ecuio_sim_reply_t size is ABI");
_Static_assert(offsetof(ecuio_sim_reply_t, delay_us) == 8,
               "sim reply delay_us offset is ABI");
_Static_assert(offsetof(ecuio_sim_reply_t, fc_us) == 20,
               "sim reply fc_us offset is ABI");
_Static_assert(offsetof(ecuio_sim_reply_t, cf_us) == 24,
               "sim reply cf_us offset is ABI");
_Static_assert(offsetof(ecuio_sim_reply_t, reserved) == 28,
               "sim reply reserved offset is ABI");
_Static_assert(offsetof(ecuio_sim_reply_t, data) == 40,
               "sim reply data offset is ABI");
#endif

/*
 * One whole request on request_id, as a tester sends it, to a sim made
 * by ecuio_sim_create_unbound: every ECU that hears request_id (its own
 * request ID, or 0x7DF) answers as it would on a bus, faults included,
 * each reply in an entry, in the ECUs' order. time_us is the ECU's time
 * when the request comes, in microseconds since create on the caller's
 * clock (the drive cycle and a recording's replies follow it), or
 * ECUIO_SIM_NOW; a time before the last ask's is taken as the last's.
 * An ID no ECU hears gets no reply and isn't counted.
 *
 * CONTRACTS:
 * - NULL sim, request or count, or NULL replies with a capacity:
 *   ECUIO_ERR_NULL_ARGUMENT.
 * - A sim on a bus (ecuio_sim_create_with): ECUIO_ERR_INVALID_ARGUMENT,
 *   which its own worker answers; so is a length of 0 or past 4095, or
 *   a time_us below ECUIO_SIM_NOW.
 * - The request is answered once, whatever the capacity: the ECU's
 *   clock moves, the request is counted and a fault drawn, an 04 clears
 *   it. *count is every reply, at most ECUIO_SIM_MAX_REPLIES, and the
 *   first capacity are copied; pass ECUIO_SIM_MAX_REPLIES to get them
 *   all, since asking again is a second request with its own answer.
 * - One caller at a time; ecuio_sim_get_stats and ecuio_sim_set_value
 *   from any thread meanwhile.
 */
ecuio_status_t ecuio_sim_ask(ecuio_sim_t* sim, uint32_t request_id,
                             const uint8_t* request, uint32_t length,
                             int64_t time_us, ecuio_sim_reply_t* replies,
                             uint32_t capacity, uint32_t* count);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_SIM_H */
