// File/layer: core/src/request_engine.h | internal, not ABI-exported;
//             included by j2534_transport.cpp (its driver) and
//             profile_registry.cpp (the request limit)
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestEngineTest, DiagnosticsTest, ClearTest (test_obd2);
//          HARNESS: J2534AdoptionTest (test_j2534_stream); J2534HealthTest
//          (test_j2534_health); BENCH

#ifndef ECUIO_REQUEST_ENGINE_H
#define ECUIO_REQUEST_ENGINE_H

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "diagnostics.h"
#include "obd2_reply.h"
#include "pid_support.h"
#include "request_timing.h"

#include <stdint.h>

/* The request engine (ADR-0015): what to request, derived from the
 * defs, and when, paced by the replies. It does no I/O and reads no
 * clock: its driver passes the time in, writes the requests and hands
 * over what it reads, so every rule here is testable without a vendor
 * DLL.
 *
 * A request is [service ID][key]: 01 <PID>, or 22 <DID high> <DID low>.
 * Each belongs to one channel, an index into the driver's channel
 * table. A channel has at most one request outstanding, as ISO 15765
 * physical addressing expects, and walks its own rotation, so channels
 * progress independently.
 *
 * Each channel's rotation runs two rate classes (ADR-0039 section 3): every
 * request is fast or slow, and the channel sends
 * ECUIO_ENGINE_FAST_PER_SLOW fast ones, then one slow one, each class in
 * table order from its own cursor. A class with nothing to send gives
 * its turn to the other, so a table of one class rotates in table order.
 * Requests are fast until ecuio_engine_classify says otherwise; the
 * driver classifies only the obd2 set's (obd2_rates.h).
 *
 * The probe (section 4) runs on the same pacing. Each channel with Service 01
 * requests first walks the supported-PID chain (01 00, then 01 20 while
 * each bitmap's last bit says the next exists, up to 01 E0), one request
 * at a time through the channel's one-shot slot, and its rotation starts
 * when the walk ends. A bitmap whose wait ends with nothing is the
 * walk's end; one whose wait ends after a reply naming another PID
 * (an answer that came late, docs/bench-sessions/2026-10-29-android.md)
 * is asked once more, within the walk's budget. While the probe is
 * on, a request leaves the
 * rotation only on an explicit answer: its PID's bit clear in an answered
 * bitmap, or a negative reply that won't change within the session.
 *
 * The DID scan (ADR-0020) runs on the same slot after the walk: each
 * scanned DID is asked once, 22 <DID>, taking turns with the channel's
 * rotation so the listed DIDs keep refreshing while a range is scanned.
 * A positive reply puts the DID in the rotation (while the table has
 * room); a negative one counts it refused; one that times out is asked
 * again when the driver calls ecuio_engine_rescan, as the link recovers.
 * The scan runs whether or not the probe is on. With scan_first (the
 * URI's ?scan=first), a channel with DIDs left to scan sends its
 * one-shots back to back and its rotation waits until they are asked:
 * a sweep of a range at the adapter's pace, the found DIDs refreshing
 * only once it ends.
 *
 * Identification (ADR-0021) runs on the same slot, ahead of the scan:
 * when the driver asks for it (ecuio_engine_identify), every channel
 * asks ECUIO_IDENT_REQUESTS once each, Service 09's vehicle information
 * and the 0x22 identification DIDs, taking turns with the rotation like
 * the scan. A positive reply's data is kept as text, a negative one as
 * its code, a timeout as unanswered, asked again on ecuio_engine_rescan.
 * Nothing it learns enters the rotation.
 *
 * Diagnostics (ADR-0037) runs on the same slot, after identification
 * and ahead of the scan: when the driver asks for it
 * (ecuio_engine_diagnose), every channel asks readiness (01 01), the
 * stored, pending and permanent codes (03, 07, 0A: one byte, no key) and
 * the freeze frame's PIDs (02 00 00); the freeze frame's code and values
 * follow as its answers say one is stored. The answers go into a report
 * per channel (diagnostics.h); ecuio_engine_diagnose_again asks it all
 * again.
 *
 * Packing (ADR-0039 Appendix A), when the driver turns it on with the
 * probe: a rotation request whose turn comes carries with it the next
 * requests of its class, up to ECUIO_ENGINE_PACK_MAX PIDs in one
 * request (01 p1 ... p6), each a Service 01 PID the bitmaps list, not a
 * bitmap, and one whose own reply has shown its length (no longer than
 * J1979's, so core's decode splits the packed reply the same way,
 * obd2_reply.h). Until a PID has answered alone it goes alone, so the
 * first lap after the walk is unpacked. A positive reply whose first
 * record's PID is one of the pack's answers it; a PID it leaves out has
 * no value that pass and isn't dropped. A pack answered with silence, a
 * negative reply or one record ECUIO_ENGINE_PACK_FAILURES times in a row
 * puts the channel back to one PID per request until the link recovers
 * (ecuio_engine_rescan): an ECU that went quiet (the ignition off) left
 * its packs unanswered along with everything else, so packing is judged
 * again once it answers. An ECU that refuses packs answers the rest, so
 * its link never recovers and its channel stays unpacked. */

/* Timing (ADR-0015, appendix of 2026-10-23). The wait is the server's
 * response time P2 (50 ms) plus 50 ms for the adapter: the longest reply
 * on the first vehicle took 83 ms. A "response pending" reply extends it
 * to P2* (5 s) plus the same margin. The floor is the least time between
 * two requests on one channel, set before any vehicle has run the
 * engine. The walk's budget fits the whole chain, eight bitmaps, at a
 * full wait each; no bitmap request goes out after it. */
constexpr int64_t ECUIO_ENGINE_WAIT_US = 100000;
constexpr int64_t ECUIO_ENGINE_PENDING_WAIT_US = 5050000;
constexpr int64_t ECUIO_ENGINE_GAP_US = 20000;
constexpr int64_t ECUIO_ENGINE_PROBE_BUDGET_US = 1000000;

/* Negative responses: 7F <service> <code> (ISO 14229-1). */
constexpr uint8_t ECUIO_NEGATIVE_RESPONSE = 0x7F;
constexpr uint8_t ECUIO_NRC_SERVICE_NOT_SUPPORTED = 0x11;
constexpr uint8_t ECUIO_NRC_SUBFUNCTION_NOT_SUPPORTED = 0x12;
constexpr uint8_t ECUIO_NRC_REQUEST_OUT_OF_RANGE = 0x31;
constexpr uint8_t ECUIO_NRC_SECURITY_ACCESS_DENIED = 0x33;
constexpr uint8_t ECUIO_NRC_RESPONSE_PENDING = 0x78;
constexpr uint8_t ECUIO_NRC_NOT_IN_ACTIVE_SESSION = 0x7F;

/* Fast requests a channel sends for each slow one (ADR-0039 section 3). */
constexpr uint32_t ECUIO_ENGINE_FAST_PER_SLOW = 2;

/* ecuio_engine_due's answers besides a table index. */
constexpr int32_t ECUIO_ENGINE_NONE = -1;   /* nothing to send */
constexpr int32_t ECUIO_ENGINE_ONCE = -2;   /* the channel's one-shot */
constexpr int32_t ECUIO_ENGINE_PACK = -3;   /* the channel's packed request */

/* Packing (ADR-0039 Appendix A): J1979's most PIDs in one request; the
 * failed packs in a row that end packing on a channel; a PID's length
 * when its own reply was longer than J1979's, which the split can't
 * tell from a packed reply, so it's never packed. */
constexpr uint32_t ECUIO_ENGINE_PACK_MAX = 6;
constexpr uint32_t ECUIO_ENGINE_PACK_FAILURES = 3;
constexpr uint8_t ECUIO_ENGINE_PID_LONG = 0xFF;

/* A bitmap reply: 41 <PID> and four bytes, one bit per PID. */
constexpr uint8_t ECUIO_BITMAP_REPLY_LEN = 6;

/* The most DIDs a session scans, over every channel (ADR-0020): a
 * 0x100-wide block four times over, at six bytes an entry. */
constexpr uint32_t ECUIO_ENGINE_SCAN_MAX = 1024;

/* What became of a scanned DID. */
constexpr uint8_t ECUIO_SCAN_UNASKED = 0;      /* not asked yet, or asked
                                                * again after a recovery */
constexpr uint8_t ECUIO_SCAN_FOUND = 1;        /* answered positive */
constexpr uint8_t ECUIO_SCAN_REFUSED = 2;      /* answered negative */
constexpr uint8_t ECUIO_SCAN_UNANSWERED = 3;   /* its wait ran out */

/* What identification asks each channel's ECU (ADR-0021), in this
 * order: SAE J1979 Service 09's vehicle information (the VIN, the
 * calibration IDs and their verification numbers, the ECU's name), then
 * ISO 14229-1's identification DIDs (Annex C: part, software and
 * hardware numbers, the supplier, the serial, the VIN again as F190).
 * The name is what the activity log shows beside the value. */
struct ecuio_ident_request {
    uint8_t bytes[3];
    uint8_t len;
    const char* name;
};

constexpr ecuio_ident_request ECUIO_IDENT_REQUESTS[] = {
    {{0x09, 0x02, 0x00}, 2, "VIN"},
    {{0x09, 0x04, 0x00}, 2, "CALID"},
    {{0x09, 0x06, 0x00}, 2, "CVN"},
    {{0x09, 0x0A, 0x00}, 2, "ECU name"},
    {{0x22, 0xF1, 0x87}, 3, "part number"},
    {{0x22, 0xF1, 0x88}, 3, "software number"},
    {{0x22, 0xF1, 0x89}, 3, "software version"},
    {{0x22, 0xF1, 0x8A}, 3, "supplier"},
    {{0x22, 0xF1, 0x8C}, 3, "ECU serial"},
    {{0x22, 0xF1, 0x90}, 3, "VIN (F190)"},
    {{0x22, 0xF1, 0x91}, 3, "hardware number"},
    {{0x22, 0xF1, 0x94}, 3, "supplier software number"},
    {{0x22, 0xF1, 0x95}, 3, "supplier software version"},
};
constexpr uint32_t ECUIO_IDENT_COUNT =
    sizeof(ECUIO_IDENT_REQUESTS) / sizeof(ECUIO_IDENT_REQUESTS[0]);
static_assert(ECUIO_IDENT_COUNT <= ECUIO_IDENTITY_MAX_ITEMS,
              "the identification list must fit the report");

/* The reply's data starts here for both services: after 49 <item> and
 * the item count, or after 62 <DID high> <DID low>. */
constexpr uint16_t ECUIO_IDENT_DATA_OFFSET = 3;

/* What diagnostics asks first, in this order (ADR-0037 section 2): PID
 * 41, this drive cycle's monitors, right after PID 01's, unless the
 * support bitmaps leave it out; the freeze frame's code (02 02 00) and
 * values (02 <pid> 00) join the list as the answers call for them. */
constexpr uint8_t ECUIO_DIAG_FIRST[][4] = {
    /* the request, then its length */
    {0x01, 0x01, 0x00, 2}, {0x01, 0x41, 0x00, 2}, {0x03, 0x00, 0x00, 1},
    {0x07, 0x00, 0x00, 1}, {0x0A, 0x00, 0x00, 1}, {0x02, 0x00, 0x00, 3},
};
constexpr uint32_t ECUIO_DIAG_FIRST_COUNT =
    sizeof(ECUIO_DIAG_FIRST) / sizeof(ECUIO_DIAG_FIRST[0]);

/* The first items, the freeze frame's code, and a value request per
 * freeze-frame value at most */
constexpr uint32_t ECUIO_DIAG_MAX_ITEMS =
    ECUIO_DIAG_FIRST_COUNT + 1 + ECUIO_DIAG_MAX_FREEZE;

/* A channel's clear: what its one-shot asks next */
constexpr uint8_t ECUIO_CLEAR_STEP_NONE = 0;
constexpr uint8_t ECUIO_CLEAR_STEP_SPEED = 1;   /* 01 0C */
constexpr uint8_t ECUIO_CLEAR_STEP_CLEAR = 2;   /* 04, past the gate */
constexpr uint8_t ECUIO_CLEAR_STEP_VEHICLE = 3; /* 01 0D, under ALLOW_RUNNING */

struct ecuio_diag_item {
    uint8_t request[3];
    uint8_t len;     /* 1 to 3 */
    uint8_t outcome; /* ECUIO_DIAG_UNASKED .. _UNANSWERED */
    uint8_t nrc;
};

struct ecuio_request {
    uint8_t bytes[1 + ECUIO_ENGINE_PACK_MAX]; /* the service ID, then the
                                               * key; a pack's PIDs */
    uint8_t len;        /* 2 or 3 in the rotation; a one-shot's 1 to 3; a
                           pack's 3 to 7 */
    uint8_t channel;
    uint8_t dropped;    /* 0 in the rotation, else why not (ECUIO_DROP_*) */
    uint8_t nrc;        /* the negative reply's code that dropped it */
    uint8_t slow;       /* 1: the slow class, else fast (ADR-0039 section 3) */
    uint8_t reply_len;  /* a pack's reply, its records' lengths added; 0
                           for every other request */
    uint8_t parked;     /* 1: out of the rotation while a focus leaves it
                           out (ADR-0056 section 2); back on a count of 0 */
    uint8_t slow_default; /* the class classify or judge_duplicates gave
                             it, which a focus's end restores */
};

struct ecuio_scan_entry {
    uint16_t did;
    uint8_t channel;
    uint8_t outcome;    /* ECUIO_SCAN_* */
    uint8_t nrc;        /* the negative reply's code, when refused */
    uint8_t polled;     /* 1 once found and in the rotation */
};

struct ecuio_engine_channel {
    int32_t outstanding = ECUIO_ENGINE_NONE;   /* a table index, the
                                                * one-shot, or none */
    int64_t deadline_us = 0;    /* when that request's wait ends */
    int64_t sent_us = 0;        /* when it went out (ADR-0042) */
    int64_t next_send_us = 0;   /* the floor: no request before this */
    uint32_t cursor = 0;        /* where the fast class resumes */
    uint32_t slow_cursor = 0;   /* where the slow class resumes */
    uint32_t fast_run = 0;      /* fast requests since the last slow one,
                                 * at most ECUIO_ENGINE_FAST_PER_SLOW */
    uint32_t requests = 0;      /* this channel's requests in the rotation */
    bool in_step = true;        /* no request has timed out since the last
                                 * positive reply: see ecuio_engine_on_frame */
    /* The one-shot: what goes out outside the rotation, one at a time,
     * whoever it's for (the walk's next bitmap, an identification item,
     * a scanned DID). identifying and scanning say it's taken; the walk
     * keeps its own next request below, so starting a walk never touches
     * a slot that's taken. */
    ecuio_request once = {};
    /* The supported-PID walk. Bits for PID p sit as the bitmaps carry
     * them: bit 0x80 >> ((p - 1) % 8) of byte (p - 1) / 8. */
    bool walking = false;       /* the walk has a bitmap to ask */
    ecuio_request walk = {};    /* that bitmap; the one-shot copies it */
    bool walk_stray = false;    /* while it was out, a reply named another
                                 * Service 01 PID */
    bool walk_reasked = false;  /* it was asked again for one */
    bool probed = false;        /* a walk has started */
    bool walked = false;        /* the chain was answered to its end */
    uint8_t ranges = 0;         /* bit i: the bitmap at 0x20 * i answered */
    uint8_t known[32] = {};     /* PIDs whose support the ECU stated */
    uint8_t supported[32] = {}; /* of those, the ones it supports */
    /* The DID scan: the one-shot carries a scan entry while scanning. */
    bool scanning = false;      /* the one-shot is a scanned DID */
    bool scan_turn = false;     /* the scan sends next, not the rotation */
    uint32_t scan_cursor = 0;   /* where the scan looks for its next entry */
    uint32_t scan_index = 0;    /* the entry out, while scanning */
    uint32_t scan_pending = 0;  /* this channel's unasked entries */
    uint32_t generation = 0;    /* bumped whenever the above, or a
                                 * request's dropped, changes */
    /* Identification: the one-shot carries an item while identifying. */
    ecuio_identity_item_t ident[ECUIO_IDENTITY_MAX_ITEMS] = {};
    uint32_t ident_count = 0;   /* items in ident; 0 = not identifying */
    uint32_t ident_pending = 0; /* unasked items */
    bool identifying = false;   /* the one-shot is an item */
    uint32_t ident_index = 0;   /* the item out, while identifying */
    uint32_t ident_generation = 0;   /* bumped as each item settles */
    /* Diagnostics: the one-shot carries an item while diagnosing. */
    ecuio_diag_item diag_items[ECUIO_DIAG_MAX_ITEMS] = {};
    uint32_t diag_count = 0;     /* items listed; 0 = not diagnosing */
    uint32_t diag_pending = 0;   /* unasked items */
    bool diagnosing = false;     /* the one-shot is an item */
    uint32_t diag_index = 0;     /* the item out, while diagnosing */
    uint8_t freeze_bits[4] = {}; /* 02 00 00's answer: PIDs 01 to 20 */
    ecuio_channel_diagnostics_t diag = {}; /* the report, but rx_id */
    /* Clearing (ADR-0038): the one-shot carries the clear's request
     * while clearing; clear_step is what it asks next. */
    uint8_t clear_step = ECUIO_CLEAR_STEP_NONE;
    uint32_t clear_flags = 0; /* the call's ECUIO_CLEAR_ALLOW_* */
    bool clearing = false; /* the one-shot is the clear's request */
    /* Packing (ADR-0039 Appendix A): each PID's data length as its own
     * reply showed it (0 not yet, ECUIO_ENGINE_PID_LONG longer than
     * J1979's); the pack out or last sent, and its requests' rows. */
    uint8_t pid_len[256] = {};
    ecuio_request pack = {};
    uint32_t pack_rows[ECUIO_ENGINE_PACK_MAX] = {};
    uint32_t pack_count = 0;
    uint32_t pack_failures = 0; /* failed packs in a row */
    bool packed = false;        /* a pack was answered with several records */
    bool unpacked = false;      /* packing ended until a rescan */
};

struct ecuio_request_engine {
    ecuio_request requests[ECUIO_PROFILE_MAX_REQUESTS] = {};
    uint32_t count = 0;
    ecuio_engine_channel chans[ECUIO_MAX_CHANNELS] = {};
    uint32_t chan_count = 0;
    bool probe = false;         /* section 4 on: walks run, explicit answers drop */
    int64_t walk_until_us = 0;  /* the walk's budget ends */
    ecuio_scan_entry scan[ECUIO_ENGINE_SCAN_MAX] = {};
    uint32_t scan_count = 0;
    bool scan_first = false;    /* the scan doesn't take turns with the rotation */
    /* The obd2 set, whose defs decode the freeze frame (ADR-0037 section 4);
     * set by ecuio_engine_diagnose */
    const ecuio_signal_set_t* diag_set = nullptr;
    /* ADR-0039 section 3: PID pairs (keep, demote), two bytes each, that can
     * read one sensor; set by ecuio_engine_set_duplicates */
    const uint8_t* duplicates = nullptr;
    uint32_t duplicate_count = 0;
    bool pack = false;          /* several PIDs per request (the driver's) */
    /* ADR-0056 section 2: a focus is on. Every request it parked stays
     * parked, the kept ones run fast, and a request added later starts
     * parked, until ecuio_engine_unfocus. */
    bool focused = false;
    /* A request joined parked under the focus (ecuio_engine_add): the
     * driver resolves the picks again, and a pick that names it unparks
     * it (ecuio_take_focus). Cleared by the focus and its end. */
    bool focus_joined = false;
    /* Reply timing (ADR-0042): the driver's, NULL for none. Before each
     * frame it hands over, a driver whose frames carry stamps of their
     * own sets stamp_us (session time on the frames' clock) and
     * stamp_breaks (how often that clock has taken the host's delta);
     * otherwise the intervals are on the host's clock, from now_us. */
    ecuio_timing_table* timing = nullptr;
    bool stamp_set = false;
    int64_t stamp_us = 0;
    uint32_t stamp_breaks = 0;
};

/* Whether request r is asked in the rotation: not dropped by the probe
 * and not parked by a focus (ADR-0056 section 2) */
inline bool ecuio_engine_in_rotation(const ecuio_request& r)
{
    return r.dropped == 0 && r.parked == 0;
}

/* A rotation request's answer or refusal, for reply timing */
inline void ecuio_engine_time_reply(ecuio_request_engine* e, uint32_t c,
                                    uint32_t row, bool positive, bool packed,
                                    int64_t now_us)
{
    if (e->timing == nullptr || row >= ECUIO_PROFILE_MAX_REQUESTS) {
        return;
    }
    timing_row& r = e->timing->rows[row];
    const int64_t latency = now_us - e->chans[c].sent_us;
    if (positive) {
        timing_answered(r, latency, e->stamp_set ? e->stamp_us : now_us,
                        e->stamp_breaks, packed);
    } else {
        timing_refused(r, latency, packed);
    }
}

/* How a frame on a channel relates to its outstanding request. */
enum class ecuio_reply {
    none,       /* answers no outstanding request */
    positive,   /* the service ID + 0x40, then the key: complete */
    negative,   /* 7F, the service ID, a code: complete */
    pending     /* 7F, the service ID, 0x78: still waiting, for longer */
};

enum class ecuio_engine_add_result { added, duplicate, refused, full };

/* Section 3: the services a request may carry. 0x01 (current data), 0x02 (the
 * freeze frame), 0x03, 0x07 and 0x0A (stored, pending and permanent
 * trouble codes; ADR-0037), 0x09 (vehicle information) and 0x22
 * (ReadDataByIdentifier); nothing that clears codes, changes a session,
 * unlocks or writes. Every request enters the table through
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

/* Section 4: what a negative reply's code says of its request, as
 * ECUIO_DROP_*: NOT_SUPPORTED for 0x12 (the first vehicle's answer to an
 * unsupported PID) and 0x31; SERVICE for 0x11; ACCESS for 0x33 and 0x7F,
 * since the engine never unlocks or changes the session; 0 for any code
 * that can pass, like 0x22 (conditions not correct) or 0x21 (busy). */
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

/* Section 2: the request whose positive reply carries a def's value. The def's
 * mux key sits at payload offset 0 and opens that reply: a 2-byte
 * 41 <PID> asks for 01 <PID>, a 3-byte 62 <DID> for 22 <DID>. Any other
 * key requests nothing, though it still decodes; Service 09 values are
 * read once, never rotated. Writes out and *out_len only on true. */
inline bool ecuio_engine_request_for_def(const ecuio_signal_def_t* d,
                                         uint8_t out[3], uint8_t* out_len)
{
    if (d->mux_offset != 0) {
        return false;
    }
    const uint32_t k = d->mux_key;
    if (d->mux_length == 2 && (k >> 8) == 0x41u) {
        out[0] = 0x01;
        out[1] = static_cast<uint8_t>(k);
        *out_len = 2;
        return true;
    }
    if (d->mux_length == 3 && (k >> 16) == 0x62u) {
        out[0] = 0x22;
        out[1] = static_cast<uint8_t>(k >> 8);
        out[2] = static_cast<uint8_t>(k);
        *out_len = 3;
        return true;
    }
    return false;
}

/* An empty table over chan_count channels, at least one: a set that
 * declares none still counts its requests on channel 0. The probe is
 * off until ecuio_engine_probe. */
inline void ecuio_engine_init(ecuio_request_engine* e, uint32_t chan_count)
{
    *e = ecuio_request_engine{};
    e->chan_count = (chan_count == 0) ? 1
                  : (chan_count > ECUIO_MAX_CHANNELS) ? ECUIO_MAX_CHANNELS
                  : chan_count;
}

/* Adds a request to channel's rotation, after the ones already there.
 * The same request on the same channel is kept once. Refused: a service
 * section 3 doesn't allow, a length other than 2 or 3, a channel past the
 * table. */
inline ecuio_engine_add_result ecuio_engine_add(ecuio_request_engine* e,
                                                uint32_t channel,
                                                const uint8_t* bytes,
                                                uint8_t len)
{
    if (len < 2 || len > 3 || channel >= e->chan_count ||
        !ecuio_engine_service_allowed(bytes[0])) {
        return ecuio_engine_add_result::refused;
    }
    for (uint32_t i = 0; i < e->count; ++i) {
        const ecuio_request& r = e->requests[i];
        if (r.channel == channel && r.len == len && r.bytes[0] == bytes[0] &&
            r.bytes[1] == bytes[1] && (len < 3 || r.bytes[2] == bytes[2])) {
            return ecuio_engine_add_result::duplicate;
        }
    }
    if (e->count >= ECUIO_PROFILE_MAX_REQUESTS) {
        return ecuio_engine_add_result::full;
    }
    ecuio_request& r = e->requests[e->count++];
    r = ecuio_request{};
    for (uint8_t i = 0; i < len; ++i) {
        r.bytes[i] = bytes[i];
    }
    r.len = len;
    r.channel = static_cast<uint8_t>(channel);
    /* Under a focus a request added later (a scanned DID found) wasn't
     * there when the focus resolved its picks: it joins parked, and
     * focus_joined has the driver resolve them again, which unparks it
     * when a pick names it */
    r.parked = e->focused ? 1 : 0;
    if (r.parked == 0) {
        ++e->chans[channel].requests;
    } else {
        e->focus_joined = true;
    }
    return ecuio_engine_add_result::added;
}

/* The channel a def's request goes to: the one whose receive ID is the
 * def's message_id, or channel 0 when none is (a ?filter= override
 * replaces the set's channels with one of its own). */
inline uint32_t ecuio_engine_channel_for(const ecuio_signal_def_t* d,
                                         const uint32_t* rx_ids,
                                         uint32_t rx_count)
{
    for (uint32_t i = 0; i < rx_count; ++i) {
        if (rx_ids[i] == d->message_id) {
            return i;
        }
    }
    return 0;
}

/* Section 2: enrols every request a set's defs derive, in def order, one per
 * distinct request and channel, so several defs read from one reply
 * cost one request. rx_ids holds each channel's receive ID. True when
 * every request fit; otherwise the table holds the first
 * ECUIO_PROFILE_MAX_REQUESTS, and *overflow_def (when not NULL) names
 * the first def whose request didn't. A NULL set enrols nothing. */
inline bool ecuio_engine_derive(ecuio_request_engine* e,
                                const ecuio_signal_set_t* set,
                                const uint32_t* rx_ids, uint32_t rx_count,
                                uint32_t* overflow_def)
{
    if (set == nullptr || set->defs == nullptr) {
        return true;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        uint8_t req[3] = {};
        uint8_t len = 0;
        if (!ecuio_engine_request_for_def(&set->defs[i], req, &len)) {
            continue;
        }
        const uint32_t c = ecuio_engine_channel_for(&set->defs[i], rx_ids,
                                                    rx_count);
        if (ecuio_engine_add(e, c, req, len) ==
            ecuio_engine_add_result::full) {
            if (overflow_def != nullptr) {
                *overflow_def = i;
            }
            return false;
        }
    }
    return true;
}

/* ADR-0039 section 3: puts every Service 01 request whose PID fast() turns
 * down in the slow class. Other services, and a table never classified,
 * stay fast. */
inline void ecuio_engine_classify(ecuio_request_engine* e,
                                  bool (*fast)(uint8_t pid))
{
    for (uint32_t i = 0; i < e->count; ++i) {
        ecuio_request& r = e->requests[i];
        r.slow_default =
            (r.len == 2 && r.bytes[0] == 0x01 && !fast(r.bytes[1])) ? 1 : 0;
        r.slow = r.slow_default;
    }
}

/* ADR-0039 section 3: pairs of fast PIDs that can read one sensor, count pairs
 * of (keep, demote) bytes, which must outlive the engine. Each walk's end
 * judges them on its channel (ecuio_engine_judge_duplicates). */
inline void ecuio_engine_set_duplicates(ecuio_request_engine* e,
                                        const uint8_t* pairs, uint32_t count)
{
    e->duplicates = pairs;
    e->duplicate_count = count;
}

/* Channel c's next request of one class, from that class's cursor, or
 * ECUIO_ENGINE_NONE when the class has none in the rotation. */
inline int32_t ecuio_engine_next_of(ecuio_request_engine* e, uint32_t c,
                                    bool slow)
{
    ecuio_engine_channel& ch = e->chans[c];
    uint32_t& cursor = slow ? ch.slow_cursor : ch.cursor;
    for (uint32_t k = 0; k < e->count; ++k) {
        const uint32_t i = (cursor + k) % e->count;
        const ecuio_request& r = e->requests[i];
        if (r.channel == c && ecuio_engine_in_rotation(r) &&
            (r.slow != 0) == slow) {
            cursor = (i + 1) % e->count;
            return static_cast<int32_t>(i);
        }
    }
    return ECUIO_ENGINE_NONE;
}

/* The request r names on channel c: a table index, the one-shot or the
 * pack. */
inline const ecuio_request& ecuio_engine_request_of(
    const ecuio_request_engine* e, uint32_t c, int32_t r)
{
    return (r == ECUIO_ENGINE_ONCE)   ? e->chans[c].once
           : (r == ECUIO_ENGINE_PACK) ? e->chans[c].pack
                                      : e->requests[r];
}

/* Whether request i of channel c may go in a pack: packing on and not
 * ended, the walk over, a Service 01 PID the bitmaps list that isn't a
 * bitmap, whose own reply showed a length the split will use. */
inline bool ecuio_engine_packable(const ecuio_request_engine* e, uint32_t c,
                                  uint32_t i)
{
    const ecuio_engine_channel& ch = e->chans[c];
    const ecuio_request& r = e->requests[i];
    if (!e->pack || ch.unpacked || ch.walking || r.channel != c ||
        !ecuio_engine_in_rotation(r) || r.len != 2 || r.bytes[0] != 0x01) {
        return false;
    }
    const uint8_t pid = r.bytes[1];
    return pid % 0x20u != 0 && (ch.supported[(pid - 1u) / 8u] &
                                (0x80u >> ((pid - 1u) % 8u))) != 0 &&
           ch.pid_len[pid] != 0 && ch.pid_len[pid] != ECUIO_ENGINE_PID_LONG;
}

/* Request i, due on channel c, alone or as the first of a pack: the
 * next requests of its class from that class's cursor join it while
 * each may (ecuio_engine_packable), up to ECUIO_ENGINE_PACK_MAX; the
 * first that may not stops the pack and goes next, alone. Returns i, or
 * ECUIO_ENGINE_PACK with the pack in ch.pack, its cursor past the last
 * request it took. */
inline int32_t ecuio_engine_gather(ecuio_request_engine* e, uint32_t c,
                                   int32_t i)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (!ecuio_engine_packable(e, c, static_cast<uint32_t>(i))) {
        return i;
    }
    const bool slow = e->requests[i].slow != 0;
    uint32_t& cursor = slow ? ch.slow_cursor : ch.cursor;
    ch.pack_rows[0] = static_cast<uint32_t>(i);
    ch.pack_count = 1;
    for (uint32_t k = 0; k < e->count && ch.pack_count < ECUIO_ENGINE_PACK_MAX;
         ++k) {
        const uint32_t j = (cursor + k) % e->count;
        const ecuio_request& r = e->requests[j];
        if (r.channel != c || !ecuio_engine_in_rotation(r) ||
            (r.slow != 0) != slow) {
            continue;
        }
        if (j == static_cast<uint32_t>(i) || !ecuio_engine_packable(e, c, j)) {
            break;
        }
        ch.pack_rows[ch.pack_count++] = j;
        cursor = (j + 1) % e->count;
        k = static_cast<uint32_t>(-1); /* the cursor moved: look from it */
    }
    if (ch.pack_count < 2) {
        return i;
    }
    ch.pack = ecuio_request{};
    ch.pack.bytes[0] = 0x01;
    uint32_t reply = 1;
    for (uint32_t k = 0; k < ch.pack_count; ++k) {
        const uint8_t pid = e->requests[ch.pack_rows[k]].bytes[1];
        ch.pack.bytes[1 + k] = pid;
        reply += 1u + ch.pid_len[pid];
    }
    ch.pack.len = static_cast<uint8_t>(1 + ch.pack_count);
    ch.pack.channel = static_cast<uint8_t>(c);
    ch.pack.slow = slow ? 1 : 0;
    ch.pack.reply_len = static_cast<uint8_t>(reply);
    return ECUIO_ENGINE_PACK;
}

/* Adds did to channel's scan (ADR-0020): asked once, 22 <DID>, outside
 * the rotation. The same DID on the same channel is kept once. False
 * when the scan is full or the channel is past the table. */
inline bool ecuio_engine_scan_add(ecuio_request_engine* e, uint32_t channel,
                                  uint16_t did)
{
    if (channel >= e->chan_count) {
        return false;
    }
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        if (e->scan[i].channel == channel && e->scan[i].did == did) {
            return true;
        }
    }
    if (e->scan_count >= ECUIO_ENGINE_SCAN_MAX) {
        return false;
    }
    ecuio_scan_entry& s = e->scan[e->scan_count++];
    s = ecuio_scan_entry{};
    s.did = did;
    s.channel = static_cast<uint8_t>(channel);
    ++e->chans[channel].scan_pending;
    return true;
}

/* The next unasked scan entry of channel c, from its cursor round the
 * list; -1 when there is none. */
inline int32_t ecuio_engine_scan_next(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t k = 0; k < e->scan_count; ++k) {
        const uint32_t i = (ch.scan_cursor + k) % e->scan_count;
        if (e->scan[i].channel == c && e->scan[i].outcome == ECUIO_SCAN_UNASKED) {
            ch.scan_cursor = (i + 1) % e->scan_count;
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

/* Settles the scan entry channel c has out: found (and in the rotation
 * while the table has room), refused with its code, or unanswered. */
/* A found DID joins channel c's rotation, room permitting */
inline void ecuio_engine_scan_poll(ecuio_request_engine* e, uint32_t c,
                                   ecuio_scan_entry& s)
{
    const uint8_t req[3] = {0x22, static_cast<uint8_t>(s.did >> 8),
                            static_cast<uint8_t>(s.did)};
    const ecuio_engine_add_result r = ecuio_engine_add(e, c, req, 3);
    s.polled = (r == ecuio_engine_add_result::added ||
                r == ecuio_engine_add_result::duplicate) ? 1 : 0;
}

inline void ecuio_engine_scan_settle(ecuio_request_engine* e, uint32_t c,
                                     uint8_t outcome, uint8_t nrc)
{
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_scan_entry& s = e->scan[ch.scan_index];
    ch.scanning = false;
    s.outcome = outcome;
    s.nrc = nrc;
    if (outcome == ECUIO_SCAN_FOUND) {
        ecuio_engine_scan_poll(e, c, s);
    }
    if (ch.scan_pending > 0) {
        --ch.scan_pending;
    }
    ++ch.generation;
}

/* A positive reply on channel c naming a scanned DID whose wait ended
 * answers it, however late: a long reply can take longer than the wait
 * (the test car's 263-byte DA9E through the VX), and the
 * adapter hands it over only whole. The DID is found and polled, so the
 * scan completes. Called only for a reply that answers nothing else. */
inline bool ecuio_engine_scan_late(ecuio_request_engine* e, uint32_t c,
                                   const uint8_t* data, uint16_t len)
{
    if (len < 3 || data[0] != 0x62) {
        return false;
    }
    const uint16_t did = static_cast<uint16_t>((data[1] << 8) | data[2]);
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        ecuio_scan_entry& s = e->scan[i];
        if (s.channel == c && s.did == did &&
            s.outcome == ECUIO_SCAN_UNANSWERED) {
            s.outcome = ECUIO_SCAN_FOUND;
            ecuio_engine_scan_poll(e, c, s);
            ++e->chans[c].generation;
            return true;
        }
    }
    return false;
}

/* Asks every unanswered scan entry, identification item and diagnostics
 * item again: the driver's call when the link returns to ok, as the walk
 * resumes. Silence wasn't an answer. A channel mid-clear keeps its
 * report as the caller saved it until the clear ends. */
inline void ecuio_engine_rescan(ecuio_request_engine* e)
{
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        ecuio_scan_entry& s = e->scan[i];
        if (s.outcome == ECUIO_SCAN_UNANSWERED) {
            s.outcome = ECUIO_SCAN_UNASKED;
            ++e->chans[s.channel].scan_pending;
        }
    }
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        if (ch.unpacked) {
            ch.unpacked = false;
            ch.pack_failures = 0;
            ++ch.generation;
        }
        for (uint32_t i = 0; i < ch.ident_count; ++i) {
            if (ch.ident[i].outcome == ECUIO_IDENTITY_UNANSWERED) {
                ch.ident[i].outcome = ECUIO_IDENTITY_UNASKED;
                ++ch.ident_pending;
            }
        }
        for (uint32_t i = 0;
             i < ch.diag_count && ch.clear_step == ECUIO_CLEAR_STEP_NONE; ++i) {
            if (ch.diag_items[i].outcome == ECUIO_DIAG_UNANSWERED) {
                ch.diag_items[i].outcome = ECUIO_DIAG_UNASKED;
                ++ch.diag_pending;
                ch.diag.flags &= ~ECUIO_DIAG_COMPLETE;
            }
        }
    }
}

/* Identification (ADR-0021): gives every channel the list to ask, each
 * item once, through the one-shot slot ahead of the scan. Idempotent:
 * a channel already holding the list keeps its answers. */
inline void ecuio_engine_identify(ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        if (ch.ident_count > 0) {
            continue;
        }
        for (uint32_t i = 0; i < ECUIO_IDENT_COUNT; ++i) {
            ecuio_identity_item_t& it = ch.ident[i];
            it = ecuio_identity_item_t{};
            const ecuio_ident_request& q = ECUIO_IDENT_REQUESTS[i];
            for (uint8_t k = 0; k < q.len; ++k) {
                it.request[k] = q.bytes[k];
            }
            it.length = q.len;
        }
        ch.ident_count = ECUIO_IDENT_COUNT;
        ch.ident_pending = ECUIO_IDENT_COUNT;
    }
}

/* The next unasked item of channel c, in list order; -1 when none. */
inline int32_t ecuio_engine_ident_next(const ecuio_request_engine* e,
                                       uint32_t c)
{
    const ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t i = 0; i < ch.ident_count; ++i) {
        if (ch.ident[i].outcome == ECUIO_IDENTITY_UNASKED) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

/* An identification reply's data as the report shows it
 * (ecuio_identity_item_t::text): ASCII when every byte is printable or
 * padding, the printable runs joined by ", " and trailing blanks
 * dropped; else hex pairs. Writes at most cap bytes, NUL included, cut
 * at a whole pair; cap is at least 1. Returns the text's length. */
inline uint8_t ecuio_identity_format(const uint8_t* data, uint32_t len,
                                     char* out, uint32_t cap)
{
    bool printable = false;
    bool ascii = true;
    for (uint32_t i = 0; i < len; ++i) {
        if (data[i] >= 0x20 && data[i] <= 0x7E) {
            printable = true;
        } else if (data[i] != 0x00 && data[i] != 0xFF) {
            ascii = false;
        }
    }
    uint32_t w = 0;
    const uint32_t room = cap - 1;
    if (ascii && printable) {
        bool started = false;
        bool gap = false;
        for (uint32_t i = 0; i < len; ++i) {
            const uint8_t b = data[i];
            if (b < 0x20 || b > 0x7E) {
                gap = true;
                continue;
            }
            if (started && gap) {
                if (w + 2 > room) {
                    break;
                }
                out[w++] = ',';
                out[w++] = ' ';
            }
            if (w + 1 > room) {
                break;
            }
            out[w++] = static_cast<char>(b);
            started = true;
            gap = false;
        }
        while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == ',')) {
            --w;
        }
    } else {
        static const char kHex[] = "0123456789ABCDEF";
        for (uint32_t i = 0; i < len; ++i) {
            const uint32_t need = (w == 0) ? 2u : 3u;
            if (w + need > room) {
                break;
            }
            if (w > 0) {
                out[w++] = ' ';
            }
            out[w++] = kHex[data[i] >> 4];
            out[w++] = kHex[data[i] & 0x0F];
        }
    }
    out[w] = '\0';
    return static_cast<uint8_t>(w);
}

/* Settles the item channel c has out: answered, with the reply's data
 * (the whole frame, from which the text is taken past the header),
 * refused with its code, or unanswered. */
inline void ecuio_engine_ident_settle(ecuio_request_engine* e, uint32_t c,
                                      uint8_t outcome, uint8_t nrc,
                                      const uint8_t* data, uint16_t len)
{
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_identity_item_t& it = ch.ident[ch.ident_index];
    ch.identifying = false;
    it.outcome = outcome;
    it.nrc = nrc;
    it.text[0] = '\0';
    it.text_length = 0;
    if (outcome == ECUIO_IDENTITY_ANSWERED && len > ECUIO_IDENT_DATA_OFFSET) {
        it.text_length = ecuio_identity_format(
            data + ECUIO_IDENT_DATA_OFFSET,
            static_cast<uint32_t>(len - ECUIO_IDENT_DATA_OFFSET),
            it.text, sizeof(it.text));
    }
    if (ch.ident_pending > 0) {
        --ch.ident_pending;
    }
    ++ch.ident_generation;
}

/* Takes table entry i out of the rotation, for reason (ECUIO_DROP_*). */
inline void ecuio_engine_drop(ecuio_request_engine* e, uint32_t i,
                              uint8_t reason, uint8_t nrc)
{
    ecuio_request& r = e->requests[i];
    if (r.dropped != 0) {
        return;
    }
    r.dropped = reason;
    r.nrc = nrc;
    ecuio_engine_channel& ch = e->chans[r.channel];
    if (r.parked == 0) {
        --ch.requests; /* a parked one was counted out already */
    }
    ++ch.generation;
}

/* Each channel's count of requests in the rotation, after a focus
 * changed which are in it. A channel whose count changed moves its
 * generation on, so its support report is published again; one whose
 * count stayed keeps its generation: its report reads the same. */
inline void ecuio_engine_recount(ecuio_request_engine* e)
{
    uint32_t counts[ECUIO_MAX_CHANNELS] = {};
    for (uint32_t i = 0; i < e->count; ++i) {
        const ecuio_request& r = e->requests[i];
        if (ecuio_engine_in_rotation(r) && r.channel < e->chan_count) {
            ++counts[r.channel];
        }
    }
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (e->chans[c].requests != counts[c]) {
            e->chans[c].requests = counts[c];
            ++e->chans[c].generation;
        }
    }
}

/* ADR-0056 section 2: parks every request keep (one byte per table row,
 * nonzero kept) leaves out and runs the kept ones in the fast class, so
 * the rotation is as short as the picks allow. The walk, the scan,
 * identification, diagnostics, a clear and a request already out are
 * untouched: a parked request's reply still correlates by the table. A
 * dropped request stays dropped. */
inline void ecuio_engine_focus(ecuio_request_engine* e, const uint8_t* keep)
{
    for (uint32_t i = 0; i < e->count; ++i) {
        ecuio_request& r = e->requests[i];
        r.parked = keep[i] != 0 ? 0 : 1;
        if (r.parked == 0) {
            r.slow = 0;
        }
    }
    e->focused = true;
    e->focus_joined = false;
    ecuio_engine_recount(e);
}

/* The focus's end: every parked request back in the rotation, each in
 * the class it was given. Nothing moves when no focus is on. */
inline void ecuio_engine_unfocus(ecuio_request_engine* e)
{
    if (!e->focused) {
        return;
    }
    for (uint32_t i = 0; i < e->count; ++i) {
        ecuio_request& r = e->requests[i];
        r.parked = 0;
        r.slow = r.slow_default;
    }
    e->focused = false;
    e->focus_joined = false;
    ecuio_engine_recount(e);
}

inline bool ecuio_engine_pid_bit(const uint8_t* bits, uint8_t pid)
{
    return (bits[(pid - 1) / 8] & (0x80u >> ((pid - 1) % 8))) != 0;
}

/* Lists one more diagnostics item on channel ch, unasked; false when
 * the list is full. */
inline bool ecuio_engine_diag_push(ecuio_engine_channel& ch, uint8_t b0,
                                   uint8_t b1, uint8_t b2, uint8_t len)
{
    if (ch.diag_count >= ECUIO_DIAG_MAX_ITEMS) {
        return false;
    }
    ecuio_diag_item& it = ch.diag_items[ch.diag_count++];
    it = ecuio_diag_item{};
    it.request[0] = b0;
    it.request[1] = b1;
    it.request[2] = b2;
    it.len = len;
    ++ch.diag_pending;
    ch.diag.flags &= ~ECUIO_DIAG_COMPLETE;
    return true;
}

/* The diag_set's first def that reads Service 01 PID pid, or nullptr */
inline const ecuio_signal_def_t*
ecuio_engine_diag_def(const ecuio_request_engine* e, uint8_t pid,
                      uint32_t from = 0)
{
    const ecuio_signal_set_t* set = e->diag_set;
    if (set == nullptr || set->defs == nullptr) {
        return nullptr;
    }
    for (uint32_t i = from; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset == 0 && d.mux_length == 2 &&
            d.mux_key == (0x4100u | pid)) {
            return &d;
        }
    }
    return nullptr;
}

/* The freeze frame's values, once one is stored: 02 <pid> 00 for each
 * PID its bitmap lists that the diag_set decodes, up to the values the
 * report holds */
inline void ecuio_engine_diag_push_values(const ecuio_request_engine* e,
                                          ecuio_engine_channel& ch)
{
    uint32_t asked = 0;
    for (uint8_t pid = 0x03; pid < 0x20 && asked < ECUIO_DIAG_MAX_FREEZE;
         ++pid) {
        if (ecuio_engine_pid_bit(ch.freeze_bits, pid) &&
            ecuio_engine_diag_def(e, pid) != nullptr &&
            ecuio_engine_diag_push(ch, 0x02, pid, 0x00, 3)) {
            ++asked;
        }
    }
}

/* A freeze-frame value's reply, 42 <pid> 00 <data>, read as the Service
 * 01 reply 41 <pid> <data> it mirrors, through each diag_set def of the
 * PID (ADR-0037 section 4) */
inline void ecuio_engine_diag_take_value(const ecuio_request_engine* e,
                                         ecuio_channel_diagnostics_t& d,
                                         const uint8_t* data, uint16_t len)
{
    uint8_t payload[16] = {0x41, data[1]};
    uint16_t n = 2;
    for (uint16_t i = 3; i < len && n < sizeof(payload); ++i) {
        payload[n++] = data[i];
    }
    for (const ecuio_signal_def_t* def = ecuio_engine_diag_def(e, data[1]);
         def != nullptr && d.freeze_count < ECUIO_DIAG_MAX_FREEZE;
         def = ecuio_engine_diag_def(
             e, data[1], static_cast<uint32_t>(def - e->diag_set->defs) + 1)) {
        double v = 0;
        if (ecuio_decode_signal(def, payload, n, &v) == 0) {
            ecuio_freeze_value_t& f = d.freeze_values[d.freeze_count++];
            f = ecuio_freeze_value_t{};
            f.def_id = def->id;
            f.value = v;
        }
    }
}

/* Settles the item channel c has out: answered, with the reply (the
 * whole frame), refused with its code, or unanswered; the report takes
 * what it says, and the freeze frame's answers list what comes next */
inline void ecuio_engine_diag_settle(ecuio_request_engine* e, uint32_t c,
                                     uint8_t outcome, uint8_t nrc,
                                     const uint8_t* data, uint16_t len)
{
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_diag_item& it = ch.diag_items[ch.diag_index];
    ecuio_channel_diagnostics_t& d = ch.diag;
    ch.diagnosing = false;
    it.outcome = outcome;
    it.nrc = nrc;
    if (ch.diag_pending > 0) {
        --ch.diag_pending;
    }
    const bool answered = outcome == ECUIO_DIAG_ANSWERED;
    switch (it.request[0]) {
    case 0x01:
        if (it.request[1] == 0x41) {
            d.cycle = outcome;
            if (answered && len >= 6) {
                ecuio_diag_take_cycle(data + 2, &d);
            }
        } else {
            d.readiness = outcome;
            d.readiness_nrc = nrc;
            if (answered && len >= 6) {
                ecuio_diag_take_readiness(data + 2, &d);
            }
        }
        break;
    case 0x03:
    case 0x07:
    case 0x0A: {
        ecuio_dtc_list_t& l =
            d.lists[it.request[0] == 0x03   ? ECUIO_DIAG_STORED
                    : it.request[0] == 0x07 ? ECUIO_DIAG_PENDING
                                            : ECUIO_DIAG_PERMANENT];
        l.outcome = outcome;
        l.nrc = nrc;
        if (answered) {
            ecuio_diag_take_codes(data, len, &l);
        }
        break;
    }
    case 0x02:
        if (it.request[1] == 0x00) {
            /* The freeze frame's PIDs: its code next, if listed, else
             * straight to its values */
            d.freeze = outcome;
            d.freeze_nrc = nrc;
            if (answered && len >= 7) {
                for (uint32_t b = 0; b < 4; ++b) {
                    ch.freeze_bits[b] = data[3 + b];
                }
                if (ecuio_engine_pid_bit(ch.freeze_bits, 0x02)) {
                    ecuio_engine_diag_push(ch, 0x02, 0x02, 0x00, 3);
                } else {
                    ecuio_engine_diag_push_values(e, ch);
                }
            }
        } else if (it.request[1] == 0x02) {
            /* 00 00: no freeze frame is stored, so no values to ask */
            if (answered && len >= 5) {
                d.freeze_dtc = static_cast<uint16_t>((data[3] << 8) | data[4]);
                if (d.freeze_dtc != 0) {
                    d.flags |= ECUIO_DIAG_FREEZE_FRAME;
                    ecuio_engine_diag_push_values(e, ch);
                }
            }
        } else if (answered && len >= 3) {
            ecuio_engine_diag_take_value(e, d, data, len);
        }
        break;
    default:
        break;
    }
    if (ch.diag_pending == 0) {
        d.flags |= ECUIO_DIAG_COMPLETE;
    }
    ++d.generation;
}

/* Lists the first items on a channel holding none, and marks its
 * report asked */
inline void ecuio_engine_diag_list(ecuio_engine_channel& ch)
{
    ch.diag.flags |= ECUIO_DIAG_ASKED;
    for (const auto& q : ECUIO_DIAG_FIRST) {
        ecuio_engine_diag_push(ch, q[0], q[1], q[2], q[3]);
    }
}

/* Diagnostics (ADR-0037): gives every channel the list to ask, through
 * the one-shot slot after identification and ahead of the scan; set
 * decodes the freeze frame (the obd2 set). Idempotent: a channel
 * already holding the list keeps its answers. */
inline void ecuio_engine_diagnose(ecuio_request_engine* e,
                                  const ecuio_signal_set_t* set)
{
    e->diag_set = set;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (e->chans[c].diag_count == 0) {
            ecuio_engine_diag_list(e->chans[c]);
        }
    }
}

/* Starts channel ch's read over: its report empty but for the last
 * clear's outcome, its generation moved on */
inline void ecuio_engine_diag_restart(ecuio_engine_channel& ch)
{
    const uint32_t generation = ch.diag.generation;
    const uint8_t clear = ch.diag.clear;
    const uint8_t clear_nrc = ch.diag.clear_nrc;
    const uint32_t clear_rpm = ch.diag.clear_rpm;
    const uint32_t clear_kmh = ch.diag.clear_kmh;
    const uint32_t read_flags =
        ch.diag.flags & (ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH);
    ch.diag = ecuio_channel_diagnostics_t{};
    ch.diag.generation = generation + 1;
    ch.diag.clear = clear;
    ch.diag.clear_nrc = clear_nrc;
    ch.diag.clear_rpm = clear_rpm;
    ch.diag.clear_kmh = clear_kmh;
    ch.diag.flags = read_flags;
    for (uint8_t& b : ch.freeze_bits) {
        b = 0;
    }
    ch.diag_count = 0;
    ch.diag_pending = 0;
    ecuio_engine_diag_list(ch);
}

/* Asks the whole list again on every channel whose last read has
 * settled ("Read again"): its report starts over, its generation moves
 * on. A channel mid-read, mid-clear, or never asked, is left as it is. */
inline void ecuio_engine_diagnose_again(ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        if (ch.diag_count == 0 || ch.diagnosing || ch.diag_pending > 0 ||
            ch.clear_step != ECUIO_CLEAR_STEP_NONE) {
            continue;
        }
        ecuio_engine_diag_restart(ch);
    }
}

/* Clears channel c's trouble codes (ADR-0038) when its report is the
 * one the caller saved: read to the end, at generation, with no clear
 * running. The clear then asks the engine speed through the one-shot,
 * and sends 04 only at 0 rpm, or under flags' ECUIO_CLEAR_ALLOW_RUNNING
 * asks the vehicle speed next (ecuio_engine_clear_settle). Any other
 * report records ECUIO_CLEAR_REPORT_CHANGED and sends nothing. False
 * then, and for a channel past the count, one that reads no
 * diagnostics, or one already clearing (which carries on). */
inline bool ecuio_engine_clear(ecuio_request_engine* e, uint32_t c,
                               uint32_t generation, uint32_t flags = 0)
{
    if (c >= e->chan_count) {
        return false;
    }
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_channel_diagnostics_t& d = ch.diag;
    if (ch.diag_count == 0 || ch.clear_step != ECUIO_CLEAR_STEP_NONE) {
        return false;
    }
    d.clear_nrc = 0;
    d.clear_rpm = 0;
    d.clear_kmh = 0;
    d.flags &= ~(ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH);
    ++d.generation;
    if ((d.flags & ECUIO_DIAG_COMPLETE) == 0 || ch.diagnosing ||
        ch.diag_pending > 0 || d.generation != generation + 1) {
        d.clear = ECUIO_CLEAR_REPORT_CHANGED;
        return false;
    }
    d.clear = ECUIO_CLEAR_NONE;
    d.flags |= ECUIO_DIAG_CLEARING;
    ch.clear_step = ECUIO_CLEAR_STEP_SPEED;
    ch.clear_flags = flags;
    return true;
}

/* Settles the request channel c's clear has out: answered with the
 * reply, refused with its code, or unanswered (ECUIO_DIAG_*). The engine
 * speed's answer sends 04 next at 0 rpm and ends the clear otherwise;
 * under ECUIO_CLEAR_ALLOW_RUNNING it is only recorded, any answer or
 * none, and the vehicle speed is asked, whose 0 km/h (or anything,
 * under ECUIO_CLEAR_ALLOW_MOVING) sends 04. 04's answer ends it, and a
 * 44 or no answer reads the report again. */
inline void ecuio_engine_clear_settle(ecuio_request_engine* e, uint32_t c,
                                      uint8_t outcome, uint8_t nrc,
                                      const uint8_t* data, uint16_t len)
{
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_channel_diagnostics_t& d = ch.diag;
    ch.clearing = false;
    bool done = true;
    bool read = false;
    const bool running_ok = (ch.clear_flags & ECUIO_CLEAR_ALLOW_RUNNING) != 0;
    const bool moving_ok = (ch.clear_flags & ECUIO_CLEAR_ALLOW_MOVING) != 0;
    if (ch.clear_step == ECUIO_CLEAR_STEP_SPEED) {
        const bool got = outcome == ECUIO_DIAG_ANSWERED && len >= 4;
        const uint32_t raw = got ? (uint32_t{data[2]} << 8) | data[3] : 0;
        if (got) {
            d.clear_rpm = (raw + 3) / 4; /* 0.25 rpm a count */
            d.flags |= ECUIO_DIAG_CLEAR_RPM;
        }
        if (running_ok) {
            ch.clear_step = ECUIO_CLEAR_STEP_VEHICLE;
            done = false;
        } else if (got && raw == 0) {
            ch.clear_step = ECUIO_CLEAR_STEP_CLEAR;
            done = false;
        } else if (got) {
            d.clear = ECUIO_CLEAR_ENGINE_RUNNING;
        } else {
            d.clear = ECUIO_CLEAR_NO_ENGINE_SPEED;
            d.clear_nrc = nrc;
        }
    } else if (ch.clear_step == ECUIO_CLEAR_STEP_VEHICLE) {
        const bool got = outcome == ECUIO_DIAG_ANSWERED && len >= 3;
        if (got) {
            d.clear_kmh = data[2]; /* 1 km/h a count */
            d.flags |= ECUIO_DIAG_CLEAR_KMH;
        }
        if (moving_ok || (got && data[2] == 0)) {
            ch.clear_step = ECUIO_CLEAR_STEP_CLEAR;
            done = false;
        } else if (got) {
            d.clear = ECUIO_CLEAR_MOVING;
        } else {
            d.clear = ECUIO_CLEAR_NO_VEHICLE_SPEED;
            d.clear_nrc = nrc;
        }
    } else if (outcome == ECUIO_DIAG_ANSWERED) {
        d.clear = ECUIO_CLEAR_CLEARED;
        read = true;
    } else if (outcome == ECUIO_DIAG_REFUSED) {
        d.clear = ECUIO_CLEAR_REFUSED;
        d.clear_nrc = nrc;
    } else {
        d.clear = ECUIO_CLEAR_UNANSWERED;
        read = true;
    }
    if (done) {
        ch.clear_step = ECUIO_CLEAR_STEP_NONE;
        d.flags &= ~ECUIO_DIAG_CLEARING;
    }
    ++d.generation;
    if (read) {
        ecuio_engine_diag_restart(ch);
    }
}

/* The next unasked diagnostics item of channel c, in list order; -1
 * when none. */
inline int32_t ecuio_engine_diag_next(const ecuio_request_engine* e, uint32_t c)
{
    const ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t i = 0; i < ch.diag_count; ++i) {
        if (ch.diag_items[i].outcome == ECUIO_DIAG_UNASKED) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

/* Settles each unasked Service 01 item of channel c whose PID the walk's
 * bitmaps state unsupported, unasked: an ECU that says it lacks PID 41
 * would only let the request wait out its time */
inline void ecuio_engine_diag_skip_unsupported(ecuio_request_engine* e,
                                               uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t i = 0; i < ch.diag_count; ++i) {
        const ecuio_diag_item& it = ch.diag_items[i];
        if (it.outcome == ECUIO_DIAG_UNASKED && it.request[0] == 0x01 &&
            it.request[1] != 0 &&
            ecuio_engine_pid_bit(ch.known, it.request[1]) &&
            !ecuio_engine_pid_bit(ch.supported, it.request[1])) {
            ch.diag_index = i;
            ecuio_engine_diag_settle(e, c, ECUIO_DIAG_UNSUPPORTED, 0, nullptr,
                                     0);
        }
    }
}

/* ADR-0039 section 3: for each duplicate pair, the demoted PID's request on
 * channel c is slow when the channel's answered bitmaps list both PIDs,
 * and fast otherwise (the pairs are of fast PIDs), so a car that answers
 * one form keeps it fast and a later walk judges again. */
inline void ecuio_engine_judge_duplicates(ecuio_request_engine* e, uint32_t c)
{
    const ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t p = 0; p < e->duplicate_count; ++p) {
        const uint8_t keep = e->duplicates[2 * p];
        const uint8_t demote = e->duplicates[2 * p + 1];
        const bool both = ecuio_engine_pid_bit(ch.supported, keep) &&
                          ecuio_engine_pid_bit(ch.supported, demote);
        for (uint32_t i = 0; i < e->count; ++i) {
            ecuio_request& r = e->requests[i];
            if (r.channel == c && r.len == 2 && r.bytes[0] == 0x01 &&
                r.bytes[1] == demote) {
                r.slow_default = both ? 1 : 0;
                if (!e->focused) {
                    r.slow = r.slow_default;
                }
            }
        }
    }
}

/* Ends channel c's walk where it stands, answered or not, and drops each
 * of its Service 01 requests whose PID the answers mark unsupported.
 * PIDs it didn't reach stay: silence isn't an answer. Then judges the
 * duplicate pairs on what the answers listed. */
inline void ecuio_engine_end_walk(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.walking = false;
    ++ch.generation;
    for (uint32_t i = 0; i < e->count; ++i) {
        const ecuio_request& r = e->requests[i];
        if (r.channel == c && r.dropped == 0 && r.bytes[0] == 0x01 &&
            r.bytes[1] != 0 && ecuio_engine_pid_bit(ch.known, r.bytes[1]) &&
            !ecuio_engine_pid_bit(ch.supported, r.bytes[1])) {
            ecuio_engine_drop(e, i, ECUIO_DROP_BITMAP, 0);
        }
    }
    ecuio_engine_judge_duplicates(e, c);
}

/* A bitmap reply to the walk on channel c: 41 <PID> b0 b1 b2 b3, PID
 * 0x20 * i covering PIDs 0x20 * i + 1 to 0x20 * i + 0x20, the last of
 * them the next bitmap. A clear last bit ends the chain, and with it
 * says every later PID is unsupported. */
inline void ecuio_engine_take_bitmap(ecuio_request_engine* e, uint32_t c,
                                     const uint8_t* data, int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.walk_stray = false;
    ch.walk_reasked = false;
    const uint32_t range = ch.walk.bytes[1] / 0x20u;
    for (uint32_t b = 0; b < 4; ++b) {
        ch.known[range * 4 + b] = 0xFF;
        ch.supported[range * 4 + b] = data[2 + b];
    }
    ch.ranges = static_cast<uint8_t>(ch.ranges | (1u << range));
    const bool next = (data[5] & 0x01u) != 0;
    if (!next) {
        for (uint32_t b = (range + 1) * 4; b < sizeof(ch.known); ++b) {
            ch.known[b] = 0xFF;
        }
    }
    if (!next || range == 7) {
        ch.walked = true;
        ecuio_engine_end_walk(e, c);
        return;
    }
    ch.walk.bytes[1] = static_cast<uint8_t>((range + 1) * 0x20u);
    if (now_us >= e->walk_until_us) {
        ecuio_engine_end_walk(e, c);
    }
}

/* Section 4: turns the probe on and starts the walk on every channel that has
 * Service 01 requests in its rotation and whose chain isn't answered to
 * its end yet. A walk resumes at the first bitmap it hasn't had, so the
 * one at connect and each after a recovery (the driver's call) together
 * ask each bitmap until it's answered. The budget starts now. A channel
 * whose one-shot is taken by an identification item or a scanned DID
 * keeps it: the walk waits in ch.walk until that one has settled
 * (ecuio_engine_due). */
inline void ecuio_engine_probe(ecuio_request_engine* e, int64_t now_us)
{
    e->probe = true;
    e->walk_until_us = now_us + ECUIO_ENGINE_PROBE_BUDGET_US;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        if (ch.walked || ch.walking) {
            continue;
        }
        bool asks_pids = false;
        for (uint32_t i = 0; i < e->count && !asks_pids; ++i) {
            const ecuio_request& r = e->requests[i];
            asks_pids = r.channel == c && r.dropped == 0 && r.bytes[0] == 0x01;
        }
        if (!asks_pids) {
            continue;
        }
        uint32_t range = 0;
        while (range < 8 && (ch.ranges & (1u << range)) != 0) {
            ++range;
        }
        if (range == 8) {
            continue;   /* every bitmap answered, so walked is set */
        }
        ch.walk = ecuio_request{};
        ch.walk.bytes[0] = 0x01;
        ch.walk.bytes[1] = static_cast<uint8_t>(range * 0x20u);
        ch.walk.len = 2;
        ch.walk.channel = static_cast<uint8_t>(c);
        ch.walk_stray = false;
        ch.walk_reasked = false;
        ch.walking = true;
        ch.probed = true;
    }
}

/* What channel c sends next: ECUIO_ENGINE_ONCE for an identification
 * item, a diagnostics item, a clear's request or a scanned DID already
 * in the one-shot (its write failed), then while its walk has a bitmap
 * to ask, then, taking turns with the rotation, for a clear's next
 * request, the next identification item, then the next diagnostics
 * item, then the next scanned DID, else the next
 * request of its rotation, or ECUIO_ENGINE_NONE while
 * one is outstanding, before the floor, or when there is nothing to
 * send. A walk out of budget ends here. Moves the rotation past the
 * request it returns; ecuio_engine_sent records what became of it. */
inline int32_t ecuio_engine_due(ecuio_request_engine* e, uint32_t c,
                                int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.outstanding != ECUIO_ENGINE_NONE || now_us < ch.next_send_us) {
        return ECUIO_ENGINE_NONE;
    }
    /* Taken before a probe started the walk: it goes first, so its reply
     * settles it and no other request's reply does. */
    if (ch.identifying || ch.diagnosing || ch.scanning || ch.clearing) {
        return ECUIO_ENGINE_ONCE;
    }
    if (ch.walking) {
        if (now_us < e->walk_until_us) {
            ch.once = ch.walk;
            return ECUIO_ENGINE_ONCE;
        }
        ecuio_engine_end_walk(e, c);
    }
    const bool once_turn = ch.requests == 0 || ch.scan_turn ||
                           (e->scan_first && ch.scan_pending > 0);
    if (ch.clear_step != ECUIO_CLEAR_STEP_NONE && once_turn) {
        ch.once = ecuio_request{};
        if (ch.clear_step == ECUIO_CLEAR_STEP_SPEED) {
            ch.once.bytes[0] = ECUIO_CLEAR_SPEED_REQUEST[0];
            ch.once.bytes[1] = ECUIO_CLEAR_SPEED_REQUEST[1];
            ch.once.len = 2;
        } else if (ch.clear_step == ECUIO_CLEAR_STEP_VEHICLE) {
            ch.once.bytes[0] = ECUIO_CLEAR_VEHICLE_SPEED_REQUEST[0];
            ch.once.bytes[1] = ECUIO_CLEAR_VEHICLE_SPEED_REQUEST[1];
            ch.once.len = 2;
        } else {
            ch.once.bytes[0] = ECUIO_CLEAR_SERVICE;
            ch.once.len = 1;
        }
        ch.once.channel = static_cast<uint8_t>(c);
        ch.clearing = true;
        ch.scan_turn = false;
        return ECUIO_ENGINE_ONCE;
    }
    if (ch.ident_pending > 0 && once_turn) {
        const int32_t i = ecuio_engine_ident_next(e, c);
        if (i >= 0) {
            const ecuio_identity_item_t& it = ch.ident[i];
            ch.once = ecuio_request{};
            for (uint8_t k = 0; k < it.length; ++k) {
                ch.once.bytes[k] = it.request[k];
            }
            ch.once.len = it.length;
            ch.once.channel = static_cast<uint8_t>(c);
            ch.identifying = true;
            ch.ident_index = static_cast<uint32_t>(i);
            ch.scan_turn = false;
            return ECUIO_ENGINE_ONCE;
        }
        ch.ident_pending = 0;   /* the count had drifted; nothing is unasked */
    }
    if (ch.diag_pending > 0 && once_turn) {
        ecuio_engine_diag_skip_unsupported(e, c);
    }
    if (ch.diag_pending > 0 && once_turn) {
        const int32_t i = ecuio_engine_diag_next(e, c);
        if (i >= 0) {
            const ecuio_diag_item& it = ch.diag_items[i];
            ch.once = ecuio_request{};
            for (uint8_t k = 0; k < it.len; ++k) {
                ch.once.bytes[k] = it.request[k];
            }
            ch.once.len = it.len;
            ch.once.channel = static_cast<uint8_t>(c);
            ch.diagnosing = true;
            ch.diag_index = static_cast<uint32_t>(i);
            ch.scan_turn = false;
            return ECUIO_ENGINE_ONCE;
        }
        ch.diag_pending = 0; /* the count had drifted; nothing is unasked */
    }
    if (ch.scan_pending > 0 && once_turn) {
        const int32_t i = ecuio_engine_scan_next(e, c);
        if (i >= 0) {
            const ecuio_scan_entry& s = e->scan[i];
            ch.once = ecuio_request{};
            ch.once.bytes[0] = 0x22;
            ch.once.bytes[1] = static_cast<uint8_t>(s.did >> 8);
            ch.once.bytes[2] = static_cast<uint8_t>(s.did);
            ch.once.len = 3;
            ch.once.channel = static_cast<uint8_t>(c);
            ch.scanning = true;
            ch.scan_index = static_cast<uint32_t>(i);
            ch.scan_turn = false;
            return ECUIO_ENGINE_ONCE;
        }
        ch.scan_pending = 0;   /* the count had drifted; nothing is unasked */
    }
    ch.scan_turn = true;
    if (ch.requests == 0) {
        return ECUIO_ENGINE_NONE;
    }
    const bool slow_turn = ch.fast_run >= ECUIO_ENGINE_FAST_PER_SLOW;
    int32_t i = ecuio_engine_next_of(e, c, slow_turn);
    if (i < 0) {
        i = ecuio_engine_next_of(e, c, !slow_turn);
    }
    if (i < 0) {
        return ECUIO_ENGINE_NONE;
    }
    if (e->requests[i].slow != 0) {
        ch.fast_run = 0;
    } else if (ch.fast_run < ECUIO_ENGINE_FAST_PER_SLOW) {
        ++ch.fast_run;
    }
    return ecuio_engine_gather(e, c, i);
}

/* A pack on channel c that failed: silence, a negative reply or one
 * record. The ECUIO_ENGINE_PACK_FAILURES-th in a row ends packing on
 * the channel until ecuio_engine_rescan. */
inline void ecuio_engine_pack_failed(ecuio_request_engine* e, uint32_t c)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (++ch.pack_failures >= ECUIO_ENGINE_PACK_FAILURES && !ch.unpacked) {
        ch.unpacked = true;
        ++ch.generation;
    }
}

/* After the driver's write of request r on channel c, at now_us: a sent
 * request waits for its reply until the wait ends; one that wasn't sent
 * leaves nothing outstanding, and a one-shot stays due. The floor runs
 * from now either way. */
inline void ecuio_engine_sent(ecuio_request_engine* e, uint32_t c,
                              int32_t r, bool sent, int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    ch.outstanding = sent ? r : ECUIO_ENGINE_NONE;
    ch.deadline_us = now_us + ECUIO_ENGINE_WAIT_US;
    if (sent) {
        ch.sent_us = now_us;
    }
    ch.next_send_us = now_us + ECUIO_ENGINE_GAP_US;
}

/* Ends channel c's request once its wait has run out: true when one
 * timed out, which is that request's outcome. Its reply may still come,
 * so the channel is out of step until a positive reply. A bitmap that
 * times out ends the walk, unless a reply naming another PID came
 * while it waited: then it's asked once more, while the budget lasts.
 * A scanned DID, an identification item, a diagnostics item or a
 * clear's request that times out is unanswered. */
inline bool ecuio_engine_expire(ecuio_request_engine* e, uint32_t c,
                                int64_t now_us)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.outstanding == ECUIO_ENGINE_NONE || now_us < ch.deadline_us) {
        return false;
    }
    const int32_t out = ch.outstanding;
    const bool once = out == ECUIO_ENGINE_ONCE;
    const bool pack = out == ECUIO_ENGINE_PACK;
    ch.outstanding = ECUIO_ENGINE_NONE;
    ch.in_step = false;
    if (e->timing != nullptr && out >= 0) {
        ++e->timing->rows[out].timeouts;
    } else if (e->timing != nullptr && pack) {
        for (uint32_t k = 0; k < ch.pack_count; ++k) {
            ++e->timing->rows[ch.pack_rows[k]].timeouts;
        }
    }
    if (pack) {
        ecuio_engine_pack_failed(e, c);
    } else if (once && ch.clearing) {
        ecuio_engine_clear_settle(e, c, ECUIO_DIAG_UNANSWERED, 0, nullptr, 0);
    } else if (once && ch.identifying) {
        ecuio_engine_ident_settle(e, c, ECUIO_IDENTITY_UNANSWERED, 0,
                                  nullptr, 0);
    } else if (once && ch.diagnosing) {
        ecuio_engine_diag_settle(e, c, ECUIO_DIAG_UNANSWERED, 0, nullptr, 0);
    } else if (once && ch.scanning) {
        ecuio_engine_scan_settle(e, c, ECUIO_SCAN_UNANSWERED, 0);
    } else if (once) {
        if (ch.walk_stray && !ch.walk_reasked && now_us < e->walk_until_us) {
            ch.walk_reasked = true; /* due asks the same bitmap again */
        } else {
            ecuio_engine_end_walk(e, c);
        }
    }
    ch.walk_stray = false;
    return true;
}

/* A frame on channel c while its pack is out: a positive reply whose
 * first record's PID is one of the pack's answers it, and succeeds when
 * its records, each of a PID the pack asked at the length its own reply
 * showed, fill it exactly and number two or more; a negative reply to
 * Service 01 ends it failed; a pending one extends the wait. Out of step
 * (ecuio_engine_on_frame), a reply that would fail it answers nothing:
 * the request before's, come late, can carry one of the pack's PIDs or
 * name its service, and the pack's own may follow, its wait judging it.
 * Nothing is dropped: the probe alone drops a PID. */
inline ecuio_reply ecuio_engine_pack_reply(ecuio_request_engine* e,
                                           uint32_t c, const uint8_t* data,
                                           uint16_t len, int64_t now_us,
                                           uint8_t* out_nrc)
{
    ecuio_engine_channel& ch = e->chans[c];
    const auto asked = [&ch](uint8_t pid) {
        for (uint32_t k = 0; k < ch.pack_count; ++k) {
            if (ch.pack.bytes[1 + k] == pid) {
                return true;
            }
        }
        return false;
    };
    if (len >= 3 && data[0] == 0x41u && asked(data[1])) {
        uint32_t records = 0;
        uint32_t at = 1;
        while (at < len && asked(data[at]) &&
               at + 1u + ch.pid_len[data[at]] <= len) {
            at += 1u + ch.pid_len[data[at]];
            ++records;
        }
        const bool filled = at == len && records >= 2;
        if (!filled && !ch.in_step) {
            return ecuio_reply::none;
        }
        ch.outstanding = ECUIO_ENGINE_NONE;
        ch.in_step = true;
        if (filled) {
            ch.pack_failures = 0;
            if (!ch.packed) {
                ch.packed = true;
                ++ch.generation;
            }
        } else {
            ecuio_engine_pack_failed(e, c);
        }
        return ecuio_reply::positive;
    }
    if (len < 3 || data[0] != ECUIO_NEGATIVE_RESPONSE || data[1] != 0x01u ||
        (data[2] != ECUIO_NRC_RESPONSE_PENDING && !ch.in_step)) {
        return ecuio_reply::none;
    }
    if (out_nrc != nullptr) {
        *out_nrc = data[2];
    }
    if (data[2] == ECUIO_NRC_RESPONSE_PENDING) {
        ch.deadline_us = now_us + ECUIO_ENGINE_PENDING_WAIT_US;
        return ecuio_reply::pending;
    }
    ch.outstanding = ECUIO_ENGINE_NONE;
    ecuio_engine_pack_failed(e, c);
    return ecuio_reply::negative;
}

/* A pack's reply, for reply timing: each PID whose record came back is
 * answered, every PID refused by a refusal; a PID the reply left out
 * gets nothing that pass (ADR-0042 section 1) */
inline void ecuio_engine_time_pack(ecuio_request_engine* e, uint32_t c,
                                   ecuio_reply reply, const uint8_t* data,
                                   uint16_t len, int64_t now_us)
{
    const ecuio_engine_channel& ch = e->chans[c];
    if (e->timing == nullptr) {
        return;
    }
    if (reply == ecuio_reply::negative) {
        for (uint32_t k = 0; k < ch.pack_count; ++k) {
            ecuio_engine_time_reply(e, c, ch.pack_rows[k], false, true, now_us);
        }
        return;
    }
    if (reply != ecuio_reply::positive) {
        return;
    }
    uint32_t at = 1;
    while (at < len) {
        const uint8_t pid = data[at];
        uint32_t k = 0;
        while (k < ch.pack_count && ch.pack.bytes[1 + k] != pid) {
            ++k;
        }
        if (k == ch.pack_count || at + 1u + ch.pid_len[pid] > len) {
            return;
        }
        ecuio_engine_time_reply(e, c, ch.pack_rows[k], true, true, now_us);
        at += 1u + ch.pid_len[pid];
    }
}

/* A frame received on channel c, its payload after the CAN ID (at most
 * the first six bytes are read, except that an identification reply's
 * data becomes the item's text). A positive or negative reply completes
 * the outstanding request; a pending one keeps it outstanding and
 * extends its wait to the pending wait from now. A bitmap carries on the
 * walk; while the probe is on, a negative reply drops what its code says
 * (ecuio_engine_drop_reason): its request, or for 0x11 every request of
 * its service on the channel. A negative reply names the service but not
 * the key, so one that came late, after its request timed out, would
 * name the request now outstanding: it drops only while the channel is
 * in step. An ECU answers in order, so a positive reply, which carries
 * the key, puts the channel back in step. A scanned DID is settled by
 * either reply, in step or not: its answer counts once, and a late
 * negative reply can at worst count the DID out now as refused, while
 * the reply that was really its own still decodes through its def. An
 * identification item, a diagnostics item and a clear's request are
 * settled the same way;
 * a one-byte request (03, 07, 0A) has no key, so its service's reply
 * alone answers it. *out_nrc (when not NULL) gets the code of a negative
 * or pending reply. */
inline ecuio_reply ecuio_engine_on_frame(ecuio_request_engine* e, uint32_t c,
                                         const uint8_t* data, uint16_t len,
                                         int64_t now_us, uint8_t* out_nrc)
{
    ecuio_engine_channel& ch = e->chans[c];
    if (ch.outstanding == ECUIO_ENGINE_NONE || len == 0) {
        (void)ecuio_engine_scan_late(e, c, data, len);
        return ecuio_reply::none;
    }
    const int32_t out = ch.outstanding;
    if (out == ECUIO_ENGINE_PACK) {
        if (ecuio_engine_scan_late(e, c, data, len)) {
            return ecuio_reply::none;
        }
        const ecuio_reply reply =
            ecuio_engine_pack_reply(e, c, data, len, now_us, out_nrc);
        ecuio_engine_time_pack(e, c, reply, data, len, now_us);
        return reply;
    }
    const ecuio_request& r = ecuio_engine_request_of(e, c, out);
    if (len >= r.len && data[0] == static_cast<uint8_t>(r.bytes[0] + 0x40) &&
        (r.len < 2 || data[1] == r.bytes[1]) &&
        (r.len < 3 || data[2] == r.bytes[2])) {
        ch.outstanding = ECUIO_ENGINE_NONE;
        ch.in_step = true;
        if (out >= 0) {
            ecuio_engine_time_reply(e, c, static_cast<uint32_t>(out), true,
                                    false, now_us);
        }
        if (out >= 0 && r.len == 2 && r.bytes[0] == 0x01) {
            /* A PID's own reply: its length, for packing */
            const uint32_t bytes = len - 2u;
            const uint32_t table = ECUIO_OBD2_PID_BYTES[r.bytes[1]];
            ch.pid_len[r.bytes[1]] =
                bytes == 0       ? 0
                : bytes <= table ? static_cast<uint8_t>(bytes)
                                 : ECUIO_ENGINE_PID_LONG;
        }
        if (out == ECUIO_ENGINE_ONCE && ch.clearing) {
            ecuio_engine_clear_settle(e, c, ECUIO_DIAG_ANSWERED, 0, data, len);
        } else if (out == ECUIO_ENGINE_ONCE && ch.identifying) {
            ecuio_engine_ident_settle(e, c, ECUIO_IDENTITY_ANSWERED, 0,
                                      data, len);
        } else if (out == ECUIO_ENGINE_ONCE && ch.diagnosing) {
            ecuio_engine_diag_settle(e, c, ECUIO_DIAG_ANSWERED, 0, data, len);
        } else if (out == ECUIO_ENGINE_ONCE && ch.scanning) {
            ecuio_engine_scan_settle(e, c, ECUIO_SCAN_FOUND, 0);
        } else if (out == ECUIO_ENGINE_ONCE) {
            if (len >= ECUIO_BITMAP_REPLY_LEN) {
                ecuio_engine_take_bitmap(e, c, data, now_us);
            } else {
                ecuio_engine_end_walk(e, c);
            }
        }
        return ecuio_reply::positive;
    }
    if (out == ECUIO_ENGINE_ONCE && ch.walking && !ch.identifying &&
        !ch.diagnosing && !ch.scanning && !ch.clearing && len >= 2 &&
        data[0] == static_cast<uint8_t>(r.bytes[0] + 0x40)) {
        ch.walk_stray = true; /* another PID's answer, not the bitmap's */
    }
    if (ecuio_engine_scan_late(e, c, data, len)) {
        return ecuio_reply::none;
    }
    if (len < 3 || data[0] != ECUIO_NEGATIVE_RESPONSE ||
        data[1] != r.bytes[0]) {
        return ecuio_reply::none;
    }
    const uint8_t code = data[2];
    if (out_nrc != nullptr) {
        *out_nrc = code;
    }
    if (code == ECUIO_NRC_RESPONSE_PENDING) {
        ch.deadline_us = now_us + ECUIO_ENGINE_PENDING_WAIT_US;
        return ecuio_reply::pending;
    }
    ch.outstanding = ECUIO_ENGINE_NONE;
    if (out >= 0) {
        ecuio_engine_time_reply(e, c, static_cast<uint32_t>(out), false, false,
                                now_us);
    }
    const uint8_t service = r.bytes[0];
    const uint8_t reason = (e->probe && ch.in_step)
                               ? ecuio_engine_drop_reason(code) : 0;
    if (reason == ECUIO_DROP_SERVICE) {
        for (uint32_t i = 0; i < e->count; ++i) {
            if (e->requests[i].channel == c &&
                e->requests[i].bytes[0] == service) {
                ecuio_engine_drop(e, i, reason, code);
            }
        }
    } else if (reason != 0 && out != ECUIO_ENGINE_ONCE) {
        ecuio_engine_drop(e, static_cast<uint32_t>(out), reason, code);
    }
    if (out == ECUIO_ENGINE_ONCE && ch.clearing) {
        ecuio_engine_clear_settle(e, c, ECUIO_DIAG_REFUSED, code, nullptr, 0);
    } else if (out == ECUIO_ENGINE_ONCE && ch.identifying) {
        ecuio_engine_ident_settle(e, c, ECUIO_IDENTITY_REFUSED, code,
                                  nullptr, 0);
    } else if (out == ECUIO_ENGINE_ONCE && ch.diagnosing) {
        ecuio_engine_diag_settle(e, c, ECUIO_DIAG_REFUSED, code, nullptr, 0);
    } else if (out == ECUIO_ENGINE_ONCE && ch.scanning) {
        ecuio_engine_scan_settle(e, c, ECUIO_SCAN_REFUSED, code);
    } else if (out == ECUIO_ENGINE_ONCE) {
        ecuio_engine_end_walk(e, c);
    }
    return ecuio_reply::negative;
}

/* Microseconds until the engine next has something to do: a wait
 * ending, or a channel's floor passing while it has something to send;
 * 0 when something is due now, INT64_MAX when nothing is. */
inline int64_t ecuio_engine_next_event_us(const ecuio_request_engine* e,
                                          int64_t now_us)
{
    int64_t next = INT64_MAX;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        const ecuio_engine_channel& ch = e->chans[c];
        if (ch.outstanding == ECUIO_ENGINE_NONE && ch.requests == 0 &&
            !ch.walking && ch.scan_pending == 0 && ch.ident_pending == 0 &&
            ch.diag_pending == 0 && ch.clear_step == ECUIO_CLEAR_STEP_NONE) {
            continue;
        }
        const int64_t at = (ch.outstanding != ECUIO_ENGINE_NONE)
                               ? ch.deadline_us : ch.next_send_us;
        const int64_t in = (at > now_us) ? at - now_us : 0;
        if (in < next) {
            next = in;
        }
    }
    return next;
}

/* What channel c has learned, for ecuio_session_get_channel_support;
 * the driver adds the channel's rx_id. */
inline void ecuio_engine_support(const ecuio_request_engine* e, uint32_t c,
                                 ecuio_channel_support_t* out)
{
    const ecuio_engine_channel& ch = e->chans[c];
    *out = ecuio_channel_support_t{};
    out->flags = (ch.probed ? ECUIO_SUPPORT_PROBED : 0u) |
                 (ch.walked ? ECUIO_SUPPORT_COMPLETE : 0u) |
                 (ch.packed ? ECUIO_SUPPORT_PACKED : 0u) |
                 (ch.unpacked ? ECUIO_SUPPORT_UNPACKED : 0u);
    out->generation = ch.generation;
    out->ranges = ch.ranges;
    for (uint32_t b = 0; b < sizeof(out->pid_bitmap); ++b) {
        out->pid_bitmap[b] = ch.supported[b];
    }
    out->pids_supported = ecuio_supported_pid_count(ch.supported);
    out->pids_through = ecuio_pids_through(ch.ranges);
    /* The list takes the refused drops first, then the bitmap's, each
     * in set order: a bitmap leaves out many PIDs on most cars, and its
     * drops would otherwise fill the list before the refusals the front
     * ends name (cli/refused_signals.h) */
    uint32_t listed = 0;
    for (int bitmap = 0; bitmap < 2; ++bitmap) {
        for (uint32_t i = 0; i < e->count; ++i) {
            const ecuio_request& r = e->requests[i];
            if (r.channel != c) {
                continue;
            }
            if (bitmap == 0) {
                ++out->requests;
                out->dropped += r.dropped != 0 ? 1u : 0u;
            }
            if (r.dropped == 0 ||
                (r.dropped == ECUIO_DROP_BITMAP) != (bitmap == 1) ||
                listed >= ECUIO_SUPPORT_MAX_DROPPED) {
                continue;
            }
            ecuio_dropped_request_t& d = out->dropped_list[listed++];
            for (uint8_t k = 0; k < r.len; ++k) {
                d.request[k] = r.bytes[k];
            }
            d.length = r.len;
            d.reason = r.dropped;
            d.nrc = r.nrc;
        }
    }
    for (uint32_t i = 0; i < e->scan_count; ++i) {
        const ecuio_scan_entry& s = e->scan[i];
        if (s.channel != c) {
            continue;
        }
        ++out->scan_total;
        if (s.outcome == ECUIO_SCAN_FOUND) {
            ++out->scan_done;
            ++out->scan_found;
            out->scan_polled = static_cast<uint16_t>(out->scan_polled + s.polled);
        } else if (s.outcome == ECUIO_SCAN_REFUSED) {
            ++out->scan_done;
            ++out->scan_refused;
        }
    }
    if (out->scan_total > 0 && out->scan_done == out->scan_total) {
        out->flags |= ECUIO_SUPPORT_SCAN_COMPLETE;
    }
}

/* What channel c's ECU has said of its diagnostics, for the session's
 * report; the driver adds the channel's rx_id. Complete once every item
 * listed has an outcome. */
inline void ecuio_engine_diagnostics(const ecuio_request_engine* e, uint32_t c,
                                     ecuio_channel_diagnostics_t* out)
{
    *out = e->chans[c].diag;
}

/* What channel c's ECU has said it is, for
 * ecuio_session_get_channel_identity; the driver adds the channel's
 * rx_id. Complete once every item has an outcome. */
inline void ecuio_engine_identity(const ecuio_request_engine* e, uint32_t c,
                                  ecuio_channel_identity_t* out)
{
    const ecuio_engine_channel& ch = e->chans[c];
    *out = ecuio_channel_identity_t{};
    out->generation = ch.ident_generation;
    out->items = ch.ident_count;
    bool complete = ch.ident_count > 0;
    for (uint32_t i = 0; i < ch.ident_count; ++i) {
        out->list[i] = ch.ident[i];
        complete = complete && ch.ident[i].outcome != ECUIO_IDENTITY_UNASKED;
    }
    if (complete) {
        out->flags |= ECUIO_IDENTITY_COMPLETE;
    }
}

#endif /* ECUIO_REQUEST_ENGINE_H */
