// File/layer: core/src/request_engine_state.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// The engine's constants, the request, scan entry, channel and engine
// structs, and the reply and add results.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestEngineTest, RequestTableTest (test_obd2);
//          HARNESS: every transport's suites

#ifndef ECUIO_REQUEST_ENGINE_STATE_H
#define ECUIO_REQUEST_ENGINE_STATE_H

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "diagnostics.h"
#include "obd2_reply.h"
#include "pid_support.h"
#include "request_talk.h"
#include "request_timing.h"

#include <algorithm>
#include <cstring>
#include <stdint.h>
#include <utility>
#include <vector>

/* Timing (ADR-0015, appendix of 2026-10-23). The wait is the server's
 * response time P2 (50 ms) plus 50 ms for the adapter: the longest reply
 * on the first vehicle took 83 ms. A "response pending" reply extends it
 * to P2* (5 s) plus the same margin. The floor is the least time between
 * the starts of two writes on one channel: this default, or the driver's
 * (ecuio_request_engine::gap_us, the URI's ?gap=). The walk's budget is
 * each channel's own, from its first bitmap, and fits the whole chain,
 * eight bitmaps, at a full wait each: an adapter that asks one channel
 * at a time leaves a walking channel the turn (request_turns.h). No
 * bitmap request goes out after it. */
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
constexpr uint8_t ECUIO_NRC_SUBFUNCTION_NOT_IN_ACTIVE_SESSION = 0x7E;
constexpr uint8_t ECUIO_NRC_NOT_IN_ACTIVE_SESSION = 0x7F;

/* Session control (ADR-0060): the one session core enters, the one a
 * close returns to, and tester present. ISO 14229-2's S3 server time is
 * 5 s; tester present goes after a quiet ECUIO_ENGINE_KEEPALIVE_US by
 * default, and a channel quiet for S3 has lost its session. */
constexpr uint8_t ECUIO_SESSION_DEFAULT = 0x01;
constexpr uint8_t ECUIO_SESSION_EXTENDED = 0x03;
constexpr uint8_t ECUIO_TESTER_PRESENT = 0x3E;
constexpr uint8_t ECUIO_SESSION_CONTROL = 0x10;
constexpr int64_t ECUIO_ENGINE_KEEPALIVE_US = 2000000;
constexpr int64_t ECUIO_ENGINE_KEEPALIVE_MIN_US = 500000;
constexpr int64_t ECUIO_ENGINE_KEEPALIVE_MAX_US = 4500000;
constexpr int64_t ECUIO_ENGINE_S3_US = 5000000;
static_assert(ECUIO_KEY_WINDOW_SESSION_US == ECUIO_ENGINE_S3_US - 1000000,
              "a key sent inside its window reaches the ECU a second "
              "before S3 runs out");

/* A channel's session is one of core.h's ECUIO_SESSION_STATE_*; a module
 * that loses it to 7E or 7F ECUIO_SESSION_LOSSES_MAX times within the
 * window reads in default from then on (ABANDONED). */
constexpr int64_t ECUIO_ENGINE_LOSS_WINDOW_US =
    int64_t{ECUIO_SESSION_LOSS_WINDOW_MS} * 1000;

/* A count that stops at the ABI's 65535 */
inline void ecuio_engine_count16(uint16_t& n)
{
    if (n < UINT16_MAX) {
        ++n;
    }
}

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
 * 0x100-wide block four times over, at eight bytes an entry. */
constexpr uint32_t ECUIO_ENGINE_SCAN_MAX = 1024;

/* What became of a scanned DID. */
constexpr uint8_t ECUIO_SCAN_UNASKED = 0;      /* not asked yet, or asked
                                                * again: once after the
                                                * channel's round, or after
                                                * a recovery */
constexpr uint8_t ECUIO_SCAN_FOUND = 1;        /* answered positive */
constexpr uint8_t ECUIO_SCAN_REFUSED = 2;      /* answered negative */
constexpr uint8_t ECUIO_SCAN_UNANSWERED = 3;   /* its wait ran out */

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
    uint8_t swept;        /* 1: counted in its channel's sweep under way (a
                             focus's minimum time, ecuio_engine_count_sweep) */
    uint8_t reply_frames; /* a table row's last answer in CAN frames (a
                             negative one is one), 0 before any
                             (reply_segmented) */
};

struct ecuio_scan_entry {
    uint16_t did;
    uint8_t channel;
    uint8_t outcome;    /* ECUIO_SCAN_* */
    uint8_t nrc;        /* the negative reply's code, when refused */
    uint8_t polled;     /* 1 once found and in the rotation */
    uint8_t retried;    /* 1 once asked again after its channel's round;
                           a recovery's rescan clears it */
};

struct ecuio_engine_channel {
    int64_t deadline_us = 0;    /* when the outstanding request's wait ends */
    int64_t sent_us = 0;        /* when it went out (ADR-0042) */
    int64_t next_send_us = 0;   /* the floor: no request before this */
    int64_t walk_until_us = 0;  /* the walk's budget ends (walk_timed) */
    int32_t outstanding = ECUIO_ENGINE_NONE; /* a table index, the one-shot,
                                              * or none (after the int64s:
                                              * the padding check) */
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
    /* A talk (request_talk.h): while talk.out the one-shot carries its
     * echo bytes, and the transport reads its whole request through
     * ecuio_engine_pdu_of */
    ecuio_engine_talk_state talk;
    /* The supported-PID walk. Bits for PID p sit as the bitmaps carry
     * them: bit 0x80 >> ((p - 1) % 8) of byte (p - 1) / 8. */
    bool walking = false;       /* the walk has a bitmap to ask */
    ecuio_request walk = {};    /* that bitmap; the one-shot copies it */
    bool walk_stray = false;    /* while it was out, a reply named another
                                 * Service 01 PID */
    bool walk_reasked = false;  /* it was asked again for one */
    bool walk_timed = false;    /* walk_until_us is set; a probe clears it,
                                 * and an untimed walk is inside it */
    bool probed = false;        /* a walk has started */
    bool walked = false;        /* the chain was answered to its end */
    uint8_t ranges = 0;         /* bit i: the bitmap at 0x20 * i answered */
    uint8_t known[32] = {};     /* PIDs whose support the ECU stated */
    uint8_t supported[32] = {}; /* of those, the ones it supports */
    /* The DID scan: the one-shot carries a scan entry while scanning. */
    bool scanning = false;      /* the one-shot is a scanned DID */
    bool scan_turn = false;     /* the scan sends next, not the rotation */
    bool check_due = false;     /* ecuio_engine_ask_check: one row, once */
    uint32_t check_from = 0;    /* the row after its last that timed out */
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
    /* A focus's minimum time between sweeps (ecuio_engine_count_sweep):
     * the rows asked since this sweep started, those of them dropped
     * since, when it started, and the time before which the next one
     * doesn't */
    uint32_t sweep_asked = 0;
    uint32_t sweep_dropped = 0;
    int64_t sweep_start_us = 0;
    int64_t next_sweep_us = 0;
    /* Session control (ADR-0060): the one-shot carries 10 xx or 3E 00
     * while sessioning. session_fresh: no reply but the session's since it
     * was entered, so a 7E or 7F then isn't a lost session. last_sent_us:
     * the channel's last request, which S3 runs from. session_refused: the
     * request whose 7E or 7F made the last re-entry (len 0: none), until
     * it answers; refused_own once its loss was given back. */
    uint8_t session_step = ECUIO_SESSION_STATE_NOT_ASKED;
    uint8_t session_nrc = 0;
    bool sessioning = false;
    bool session_fresh = false;
    bool refused_own = false;
    /* The channel's requests go to a functional address (0x7DF,
     * 0x18DB33F1), which every module hears: it sends nothing while any
     * channel's window is open, and never a seed, a key or a talk longer
     * than a frame. Set once, by ecuio_build_requests. */
    bool functional = false;
    uint16_t session_entries = 0; /* each 50 03 */
    uint16_t session_lost = 0;    /* each re-entry */
    uint32_t refused_losses = 0;  /* 7E or 7F re-entries in this window */
    int64_t loss_window_us = 0;   /* when that window started */
    int64_t last_sent_us = 0;
    /* The key's window (request_talk.h): while it is open the channel
     * sends only a key */
    ecuio_key_window key;
    ecuio_request session_refused = {};
};

/* Sets channel ch's session step. A key's window lasts only while the
 * session is entered: every other step ends it (request_talk.h). */
inline void ecuio_engine_session_step(ecuio_engine_channel& ch, uint8_t step)
{
    ch.session_step = step;
    if (step != ECUIO_SESSION_STATE_ENTERED) {
        ch.key.until_us = 0;
    }
}

/* A table row with no reply-timing row: none yet, and none ever (the
 * pool was spent when it first entered the rotation) */
constexpr uint16_t ECUIO_TIMING_NONE = 0xFFFF;
constexpr uint16_t ECUIO_TIMING_SPENT = 0xFFFE;
static_assert(ECUIO_PROFILE_MAX_REQUESTS < ECUIO_TIMING_SPENT,
              "a timing row's index fits its 16 bits");

/* A def that derives no request, in a def-to-row map */
constexpr uint32_t ECUIO_ROW_NONE = UINT32_MAX;

struct ecuio_request_engine {
    /* The table: room for each request the set derives and each DID its
     * scan can find, sized by ecuio_engine_init and never after */
    std::vector<ecuio_request> requests;
    /* Reply timing's pool (ADR-0065 section 3), counted whether or not
     * timing is attached: timing_of names each table row's pool row,
     * ECUIO_TIMING_NONE until it has one; timing_rows each pool row's
     * table row, in the order taken (timing_taken of them); untimed the
     * rows that entered the rotation with the pool spent. Sized with the
     * table. */
    std::vector<uint16_t> timing_of;
    std::vector<uint32_t> timing_rows;
    /* A focus's row marks, one byte for each table row: the driver's
     * scratch (ecuio_take_focus), sized with the table */
    std::vector<uint8_t> marks;
    /* Each table row's last write, 0 before any (ecuio_engine_sent; the
     * J2534 hold's overdue order), sized with the table */
    std::vector<int64_t> asked_us;
    uint32_t count = 0; /* the table's rows in use */
    uint32_t timing_taken = 0;
    uint32_t untimed = 0;
    /* The rows the focus keeps, a dropped one too (0 with none on) */
    uint32_t kept = 0;
    /* A bit for each channel whose ECU answers: set by a frame from it,
     * cleared when a wait on one of its table's requests runs out. A
     * one-shot's silence leaves it: an ECU answers nothing to an item it
     * doesn't serve. */
    uint32_t answering = 0;
    /* One for each channel, sized by ecuio_engine_init and never after */
    std::vector<ecuio_engine_channel> chans;
    uint32_t chan_count = 0;
    bool probe = false;         /* section 4 on: walks run, explicit answers drop */
    /* The set derives more requests than a rotation holds: a row takes
     * its pool row when a focus first keeps it, and with no focus every
     * row is parked (ecuio_engine_pick_only) */
    bool pick_only = false;
    /* The floor between the starts of two writes on a channel */
    int64_t gap_us = ECUIO_ENGINE_GAP_US;
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
    /* The most CAN frames a pack's reply may take (?packframes=); 0 none */
    uint8_t pack_frames = 0;
    /* ADR-0056 section 2: a focus is on. Every request it parked stays
     * parked, the kept ones run fast, and a request added later starts
     * parked, until ecuio_engine_unfocus. */
    bool focused = false;
    /* A request joined parked under the focus (ecuio_engine_add): the
     * driver resolves the picks again, and a pick that names it unparks
     * it (ecuio_take_focus). Cleared by the focus and its end. */
    bool focus_joined = false;
    /* The focus's minimum time between sweeps, 0 for none (ADR-0056
     * section 4); only a focus sets it */
    int64_t min_sweep_us = 0;
    /* The session every channel enters (ADR-0060): 0 for none, the
     * default; set only by ecuio_engine_session */
    uint8_t session = 0;
    int64_t keepalive_us = ECUIO_ENGINE_KEEPALIVE_US;
    /* Reply timing (ADR-0042): the driver's, NULL for none. Before each
     * frame it hands over, a driver whose frames carry stamps of their
     * own sets stamp_us (session time on the frames' clock) and
     * stamp_breaks (how often that clock has taken the host's delta);
     * otherwise the intervals are on the host's clock, from now_us. */
    ecuio_timing_table* timing = nullptr;
    bool stamp_set = false;
    int64_t stamp_us = 0;
    uint32_t stamp_breaks = 0;
    /* The longest request the transport sends, a talk's bound: attach
     * sets ECUIO_ENGINE_PDU_MAX where a request goes out as ISO-TP */
    uint32_t pdu_max = ECUIO_ENGINE_PDU_SINGLE;
};

/* Whether request r is asked in the rotation: not dropped by the probe
 * and not parked by a focus (ADR-0056 section 2) */
inline bool ecuio_engine_in_rotation(const ecuio_request& r)
{
    return r.dropped == 0 && r.parked == 0;
}

/* How a frame on a channel relates to its outstanding request. */
enum class ecuio_reply {
    none,       /* answers no outstanding request */
    positive,   /* the service ID + 0x40, then the key: complete */
    negative,   /* 7F, the service ID, a code: complete */
    pending     /* 7F, the service ID, 0x78: still waiting, for longer */
};

enum class ecuio_engine_add_result { added, duplicate, refused, full };

#endif
