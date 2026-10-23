// File/layer: core/src/uds_generic.h | internal, not ABI-exported;
//             C++17, Qt-free, every platform (core.cpp builds the set,
//             the transports read it)
// Testing: UNIT — UdsGenericTest (test_obd2): the ?did= grammar, each
//     refusal and its words, the set it builds, that the set validates
//     and fits the engine's table; CoreTest: selection by URI, the mock's
//     DID frames. HARNESS — J2534AdoptionTest: the requests, the ECU
//     prefix and the scan against the fake. FUZZ — fuzz_uds_query.

#ifndef ECUIO_UDS_GENERIC_H
#define ECUIO_UDS_GENERIC_H

#include "ecuio/core.h"
#include "ecuio/signals.h"
#include "request_engine.h"

#include <stdint.h>
#include <vector>

struct ecuio_uri_query;

/* The uds-generic profile (ADR-0020): Service 0x22 reads of the DIDs the
 * session's URI lists, for finding a manufacturer's data on a car before
 * a curated set exists. The registry binds the name to an empty set; a
 * session gets a set of its own, built here at create from the query and
 * owned by the session (session.h), which the transports and the bridge
 * read through ecuio_session_signal_set as any other.
 *
 *   ?profile=uds-generic&did=<entry>[,<entry>...]   every did= key adds up
 *   entry := [<ecu>:]<DID>[-<DID>][:<type>]
 *   DID   := 1 to 4 hex digits, no prefix
 *   ecu   := the ECU's response ID in hex: 11-bit, 9 to 7FF, asked on the
 *            ID 8 below it (7E9 asks on 7E1); or 29-bit 18DA<a><b>, asked
 *            on 18DA<b><a>. Without one, 7E8, asked on 7E0.
 *   type  := hex (the default: the reply's bytes as pairs) | text (ASCII)
 *            | u8 u16 u24 u32 | i8 i16 i24 i32 (big-endian, scale 1)
 *
 * A single DID is polled: its def enrols 22 <DID> in the rotation
 * (request_engine.h). A range A-B is scanned: a def per DID, each asked
 * once through the engine's scan; one the ECU answers joins the rotation.
 * The same DID listed twice is kept once, and one listed alone and inside
 * a range is polled. Limits, each refused at create with the detail set:
 * ECUIO_PROFILE_MAX_REQUESTS DIDs to poll, ECUIO_ENGINE_SCAN_MAX to scan,
 * ECUIO_MAX_CHANNELS ECUs. The polled defs come first, then the scanned
 * ones, each group in the order its DIDs first appear (polled counts the
 * first group, so the transport derives the rotation from that head alone
 * and feeds the rest to the scan); ids run from
 * ECUIO_UDS_GENERIC_SIG_ID_BASE in that order.
 * Each def is keyed 62 <DID> at offset 0 with the value from byte 3,
 * named "DID 1E1C" (or "0x7E9 DID 1E1C" off the default ECU), no unit,
 * no decimals. */

constexpr const char* ECUIO_UDS_GENERIC_NAME = "uds-generic";

/* A DID to scan, on the ECU answering from rx_id. */
struct ecuio_uds_scan_target {
    uint32_t rx_id;
    uint16_t did;
};

/* A session's set and what feeds it, owned by the session. The set's
 * pointers reach into the vectors, so neither may change after build. */
struct ecuio_uds_session {
    std::vector<ecuio_signal_def_t> defs;   /* polled first, then scanned */
    uint32_t polled = 0;                    /* how many of defs are polled */
    std::vector<ecuio_channel_decl_t> channels;
    std::vector<ecuio_uds_scan_target> scan;   /* one per scanned def */
    ecuio_signal_set_t set = {};
};

/* The set's polled head: what the rotation derives from. */
inline ecuio_signal_set_t ecuio_uds_polled_set(const ecuio_uds_session& u)
{
    ecuio_signal_set_t head = u.set;
    head.count = u.polled;
    return head;
}

/* Whether the query carries a did= key at all. */
bool ecuio_uds_query_has_did(const ecuio_uri_query& query);

/* Builds *out from the query's did= keys. ECUIO_OK, or
 * ECUIO_ERR_BAD_URI with the detail set (error_detail.h) for a malformed
 * entry, an ECU the transport can't address, or a limit passed;
 * ECUIO_ERR_OUT_OF_MEMORY when the vectors can't grow. *out is
 * unspecified on failure. */
ecuio_status_t ecuio_uds_build(const ecuio_uri_query& query,
                               ecuio_uds_session* out);

/* The request ID an ECU answering from rx is asked on: 8 below an 11-bit
 * ID of 9 or more (so the request ID is never 0), the swapped pair of a
 * 29-bit 18DA<a><b>. False for any other ID. */
bool ecuio_uds_tx_for_rx(uint32_t rx, uint32_t* out_tx);

/* The set the registry binds the name to: valid, with no defs and no
 * channels. Not NULL, so a lookup tells uds-generic from raw. */
const ecuio_signal_set_t* ecuio_uds_generic_template_set();

#endif /* ECUIO_UDS_GENERIC_H */
