// File/layer: core/src/request_engine.h | internal, not ABI-exported;
//             included by j2534_transport.cpp (its driver) and
//             profile_registry.cpp (the request limit)
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestEngineTest, RequestTableTest, DiagnosticsTest,
//          ClearTest, SessionControlTest, EngineTalkTest, RequestTurnsTest
//          (test_obd2);
//          HARNESS: J2534AdoptionTest (test_j2534_stream); J2534HealthTest
//          (test_j2534_health); BENCH

#ifndef ECUIO_REQUEST_ENGINE_H
#define ECUIO_REQUEST_ENGINE_H

/* The engine's parts, each a request_engine_*.h of what it does,
 * included in dependency order. */
#include "request_engine_state.h"
#include "request_engine_ident.h"
#include "request_engine_services.h"
#include "request_engine_table.h"
#include "request_engine_rotation.h"
#include "request_engine_pack.h"
#include "request_engine_focus.h"
#include "request_engine_scan.h"
#include "request_engine_walk.h"
#include "request_engine_diag.h"
#include "request_engine_session.h"
#include "request_engine_declared.h"
#include "request_engine_quiet.h"
#include "request_engine_send.h"
#include "request_engine_reply.h"
#include "request_engine_modules.h"
#include "request_diag_visit.h"

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
 * its link never recovers and its channel stays unpacked.
 *
 * A focus can carry a minimum time between sweeps (ADR-0056 section 4,
 * ecuio_engine_focus): a sweep ends once as many rows have been asked
 * as the channel has in its rotation, a pack counting each of its rows,
 * so every pick has been asked, and the next one starts no sooner than
 * that long after this one started. A sweep that took longer waits for
 * nothing. Only the rotation waits: the walk, identification, the
 * diagnostics, a clear and the scan go out in the gap.
 *
 * A module that has stopped answering (ADR-0065's appendix of
 * 2026-11-21, request_engine_quiet.h) is quiet: while another module
 * answers, it is asked one row on a backoff up to 5 s and its lists
 * settle unanswered unasked, so its waits cost the others nothing on an
 * adapter that asks one module at a time. Any frame from it ends that. */

#endif /* ECUIO_REQUEST_ENGINE_H */
