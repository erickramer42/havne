// File/layer: cli/refused_signals.h | havne-cli and the app, the signals an ECU refuses (ADR-0051)
// Testing: UNIT: CliRefusedTest (test_cli); BRIDGE: BridgeTest (test_bridge)
//
// A signal its module refuses shows as refused, not blank (ADR-0051
// section 3). Core drops a request a negative reply refuses from the
// rotation and lists it in its channel's support report (at most
// ECUIO_SUPPORT_MAX_DROPPED, each with its code, the refusals before the
// requests a bitmap left out). Past those, refused_unlisted counts the
// refusals a report couldn't list, so a front end says how many more
// there are, whose signals it can't name. The signals it leaves
// without values are the defs whose reply that request asks for: on the
// channel's reply ID, keyed at the reply's start by the request's
// service + 0x40 and the rest of its bytes (41 <PID>, 62 <DID>), as the
// request engine enrolled them. A request its PID bitmap left out is
// unsupported, not refused: the app hides those signals, as
// ecuio_session_get_unsupported_signals lists them. Public headers
// only, so the app and havne-cli say the same.

#ifndef HAVNE_CLI_REFUSED_SIGNALS_H
#define HAVNE_CLI_REFUSED_SIGNALS_H

#include "jsonl.h"

#include "ecuio/core.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cli {

// A negative response code's name (ISO 14229-1 Table A.1), "" for one
// it doesn't name
inline const char* nrc_name(uint8_t nrc)
{
    switch (nrc) {
    case 0x10:
        return "generalReject";
    case 0x11:
        return "serviceNotSupported";
    case 0x12:
        return "subFunctionNotSupported";
    case 0x13:
        return "incorrectMessageLengthOrInvalidFormat";
    case 0x14:
        return "responseTooLong";
    case 0x21:
        return "busyRepeatRequest";
    case 0x22:
        return "conditionsNotCorrect";
    case 0x24:
        return "requestSequenceError";
    case 0x25:
        return "noResponseFromSubnetComponent";
    case 0x26:
        return "failurePreventsExecutionOfRequestedAction";
    case 0x31:
        return "requestOutOfRange";
    case 0x33:
        return "securityAccessDenied";
    case 0x35:
        return "invalidKey";
    case 0x36:
        return "exceedNumberOfAttempts";
    case 0x37:
        return "requiredTimeDelayNotExpired";
    case 0x78:
        return "requestCorrectlyReceivedResponsePending";
    case 0x7E:
        return "subFunctionNotSupportedInActiveSession";
    case 0x7F:
        return "serviceNotSupportedInActiveSession";
    default:
        return "";
    }
}

// A signal left without values: its def's id and the refused request
struct RefusedSignal {
    uint16_t id = 0;
    uint8_t request[3] = {}; // the service ID, then the key
    uint8_t length = 0;      // 2 or 3
    uint8_t nrc = 0;         // the negative reply's code
};

// Whether a dropped request was refused by a negative reply
inline bool refused_by_reply(const ecuio_dropped_request_t& d)
{
    return d.reason == ECUIO_DROP_NOT_SUPPORTED ||
           d.reason == ECUIO_DROP_SERVICE || d.reason == ECUIO_DROP_ACCESS;
}

// Whether def is a signal of the reply request d asks for on the
// channel whose ECU answers on rx_id. A computed def reads no reply: its
// message ID is 0 and it has no key, which the validator holds it to.
inline bool asks_for(const ecuio_signal_def_t& def, uint32_t rx_id,
                     const ecuio_dropped_request_t& d)
{
    if (def.message_id != rx_id || def.mux_offset != 0 || d.length < 1 ||
        d.length > sizeof d.request || def.mux_length != d.length) {
        return false;
    }
    uint32_t key = static_cast<uint8_t>(d.request[0] + 0x40);
    for (uint8_t k = 1; k < d.length; ++k) {
        key = (key << 8) | d.request[k];
    }
    return def.mux_key == key;
}

// The set's signals the reports' ECUs refused, in the set's order, each
// once. reports holds count channels' reports; one that has learned
// nothing (generation 0) refuses nothing.
inline std::vector<RefusedSignal>
refused_signals(const ecuio_signal_set_t* set,
                const ecuio_channel_support_t* reports, uint32_t count)
{
    std::vector<RefusedSignal> out;
    if (set == nullptr || set->defs == nullptr || reports == nullptr) {
        return out;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& def = set->defs[i];
        bool found = false;
        for (uint32_t c = 0; c < count && !found; ++c) {
            const ecuio_channel_support_t& s = reports[c];
            if (s.generation == 0) {
                continue;
            }
            const uint32_t listed =
                std::min<uint32_t>(s.dropped, ECUIO_SUPPORT_MAX_DROPPED);
            for (uint32_t k = 0; k < listed && !found; ++k) {
                const ecuio_dropped_request_t& d = s.dropped_list[k];
                if (refused_by_reply(d) && asks_for(def, s.rx_id, d)) {
                    RefusedSignal r;
                    r.id = def.id;
                    std::copy(d.request, d.request + sizeof d.request,
                              r.request);
                    r.length = d.length;
                    r.nrc = d.nrc;
                    out.push_back(r);
                    found = true;
                }
            }
        }
    }
    return out;
}

// An ECU's refusals its report couldn't list
struct RefusedMore {
    uint32_t rx_id = 0; // the channel's receive ID: its ECU
    uint32_t count = 0; // the refused requests past the list
};

// Whether the report's answered bitmaps leave pid out: the bitmap at PID
// 0x20 * ((pid - 1) / 0x20) answered and pid's bit clear, the rule core
// drops a request by (ECUIO_DROP_BITMAP). PID 0 judges nothing.
inline bool left_out_by_bitmap(const ecuio_channel_support_t& s, uint8_t pid)
{
    if (pid == 0) {
        return false;
    }
    const unsigned range = (pid - 1u) / 0x20u;
    return (s.ranges & (1u << range)) != 0 &&
           (s.pid_bitmap[(pid - 1u) / 8u] & (0x80u >> ((pid - 1u) % 8u))) == 0;
}

// For each report whose list is all refusals with more drops past it,
// the refusals among those: the drops past the list less the requests
// the bitmaps left out (none of which is listed, refusals coming first),
// counted by the set's defs on the channel keyed by a Service 01 PID (one
// request a PID). While a walk is still running core drops nothing by
// its bitmaps yet, so the count can read low, never high. A report with
// room to spare, or one whose list ends in a bitmap drop, has listed
// every refusal.
inline std::vector<RefusedMore>
refused_unlisted(const ecuio_signal_set_t* set,
                 const ecuio_channel_support_t* reports, uint32_t count)
{
    std::vector<RefusedMore> out;
    if (set == nullptr || set->defs == nullptr || reports == nullptr) {
        return out;
    }
    for (uint32_t c = 0; c < count; ++c) {
        const ecuio_channel_support_t& s = reports[c];
        if (s.generation == 0 || s.dropped <= ECUIO_SUPPORT_MAX_DROPPED ||
            !refused_by_reply(s.dropped_list[ECUIO_SUPPORT_MAX_DROPPED - 1])) {
            continue;
        }
        bool seen[256] = {};
        uint32_t bitmap = 0;
        for (uint32_t i = 0; i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            const uint8_t pid = static_cast<uint8_t>(d.mux_key & 0xFFu);
            if (d.message_id == s.rx_id && d.mux_offset == 0 &&
                d.mux_length == 2 && (d.mux_key >> 8) == 0x41u && !seen[pid] &&
                left_out_by_bitmap(s, pid)) {
                seen[pid] = true;
                ++bitmap;
            }
        }
        const uint32_t past = s.dropped - ECUIO_SUPPORT_MAX_DROPPED;
        if (past > bitmap) {
            RefusedMore m;
            m.rx_id = s.rx_id;
            m.count = past - bitmap;
            out.push_back(m);
        }
    }
    return out;
}

// The bytes a refused line takes, room for each signal's entry and each
// ECU's count past them
inline size_t refused_line_bytes(size_t count, size_t more = 0)
{
    return 64 + count * 160 + more * 48;
}

// havne-cli's refused line: {"type":"refused","signals":[{"id","request",
// "reply","nrc"}]}, the request and its reply in frames.csv's form, nrc
// the code's name or null; an emptied list is []. With refusals past a
// report's list, "more":[{"ecu","refused"}] after: each such ECU's ID in
// frames.csv's form and how many more it refused, whose signals the list
// can't name.
inline void refused_line(Line& l, const std::vector<RefusedSignal>& refused,
                         const std::vector<RefusedMore>& more = {})
{
    l.raw("{\"type\":\"refused\"");
    l.key("signals");
    l.ch('[');
    for (size_t i = 0; i < refused.size(); ++i) {
        const RefusedSignal& r = refused[i];
        if (i) {
            l.ch(',');
        }
        l.ch('{');
        l.key("id");
        l.u64(r.id);
        l.key("request");
        l.hex_bytes(r.request, r.length);
        const uint8_t reply[3] = {0x7F, r.request[0], r.nrc};
        l.key("reply");
        l.hex_bytes(reply, sizeof reply);
        l.key("nrc");
        const char* name = nrc_name(r.nrc);
        if (*name == '\0') {
            l.raw("null");
        } else {
            l.string(name, std::strlen(name));
        }
        l.ch('}');
    }
    l.ch(']');
    if (!more.empty()) {
        l.key("more");
        l.ch('[');
        for (size_t i = 0; i < more.size(); ++i) {
            if (i) {
                l.ch(',');
            }
            l.ch('{');
            l.key("ecu");
            l.hex_id(more[i].rx_id);
            l.key("refused");
            l.u64(more[i].count);
            l.ch('}');
        }
        l.ch(']');
    }
    l.ch('}');
}

} // namespace cli

#endif /* HAVNE_CLI_REFUSED_SIGNALS_H */
