// File/layer: cli/diagnostics_line.h | havne-cli, an ECU's diagnostics as a line (ADR-0037)
// Testing: UNIT (tests/test_cli.cpp, CliDiagnosticsTest: every outcome, a
//          cut list, the freeze frame's values); the mock's line in the
//          golden run (tests/golden/cli/mock.jsonl)
//
// One ECU's report once a read has completed: readiness (the MIL, the
// confirmed-code count and each monitor by name), the three code lists,
// each code as it's written ("P0301"), and the freeze frame's values by
// the obd2 def that decoded them, in its unit, as decoded. An item that
// was refused carries the negative reply in frames.csv's form.

#ifndef HAVNE_CLI_DIAGNOSTICS_LINE_H
#define HAVNE_CLI_DIAGNOSTICS_LINE_H

#include "jsonl.h"

#include "ecuio/core.h"
#include "ecuio/signals.h"

#include <cstdint>

namespace cli {

// An item's outcome: "answered", "refused" (with its "reply", "7F 0A
// 11"), "unanswered" or "unasked"
inline void diagnostics_outcome(Line& l, uint8_t outcome, uint8_t service,
                                uint8_t nrc)
{
    l.key("outcome");
    switch (outcome) {
    case ECUIO_DIAG_ANSWERED:
        l.string("answered", SIZE_MAX);
        break;
    case ECUIO_DIAG_REFUSED: {
        l.string("refused", SIZE_MAX);
        const uint8_t reply[3] = {0x7F, service, nrc};
        l.key("reply");
        l.hex_bytes(reply, sizeof reply);
        break;
    }
    case ECUIO_DIAG_UNANSWERED:
        l.string("unanswered", SIZE_MAX);
        break;
    default:
        l.string("unasked", SIZE_MAX);
        break;
    }
}

// The whole line, without its newline; l.finish() adds it
inline void diagnostics_line(Line& l, const ecuio_channel_diagnostics_t& d)
{
    l.raw("{\"type\":\"diagnostics\"");
    l.key("rx");
    l.hex_id(d.rx_id);
    l.key("generation");
    l.u64(d.generation);

    l.key("readiness");
    l.ch('{');
    diagnostics_outcome(l, d.readiness, 0x01, d.readiness_nrc);
    if (d.readiness == ECUIO_DIAG_ANSWERED) {
        l.key("mil");
        l.raw((d.flags & ECUIO_DIAG_MIL_ON) ? "true" : "false");
        l.key("dtc_count");
        l.u64(d.dtc_count);
        l.key("compression");
        l.raw((d.flags & ECUIO_DIAG_COMPRESSION) ? "true" : "false");
        l.key("monitors");
        l.ch('[');
        const uint32_t n = d.monitor_count < ECUIO_DIAG_MAX_MONITORS
                               ? d.monitor_count
                               : ECUIO_DIAG_MAX_MONITORS;
        for (uint32_t i = 0; i < n; ++i) {
            if (i) {
                l.ch(',');
            }
            l.ch('{');
            l.key("name");
            l.string(ecuio_monitor_name(d.monitors[i].id, d.flags), SIZE_MAX);
            l.key("complete");
            l.raw((d.monitors[i].flags & ECUIO_MONITOR_COMPLETE) ? "true"
                                                                 : "false");
            l.ch('}');
        }
        l.ch(']');
    }
    l.ch('}');

    static const char* const kLists[ECUIO_DIAG_LISTS] = {"stored", "pending",
                                                         "permanent"};
    static const uint8_t kServices[ECUIO_DIAG_LISTS] = {0x03, 0x07, 0x0A};
    for (uint32_t k = 0; k < ECUIO_DIAG_LISTS; ++k) {
        const ecuio_dtc_list_t& list = d.lists[k];
        l.key(kLists[k]);
        l.ch('{');
        diagnostics_outcome(l, list.outcome, kServices[k], list.nrc);
        if (list.outcome == ECUIO_DIAG_ANSWERED) {
            l.key("reported");
            l.u64(list.reported);
            l.key("codes");
            l.ch('[');
            const uint32_t held = list.held < ECUIO_DIAG_MAX_DTCS
                                      ? list.held
                                      : ECUIO_DIAG_MAX_DTCS;
            for (uint32_t i = 0; i < held; ++i) {
                char code[8] = {};
                (void)ecuio_dtc_format(list.codes[i], code, sizeof code);
                if (i) {
                    l.ch(',');
                }
                l.string(code, sizeof code);
            }
            l.ch(']');
        }
        l.ch('}');
    }

    l.key("freeze");
    l.ch('{');
    diagnostics_outcome(l, d.freeze, 0x02, d.freeze_nrc);
    if (d.freeze == ECUIO_DIAG_ANSWERED) {
        l.key("code");
        if (d.flags & ECUIO_DIAG_FREEZE_FRAME) {
            char code[8] = {};
            (void)ecuio_dtc_format(d.freeze_dtc, code, sizeof code);
            l.string(code, sizeof code);
        } else {
            l.raw("null");
        }
        l.key("values");
        l.ch('[');
        const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
        const uint32_t n = d.freeze_count < ECUIO_DIAG_MAX_FREEZE
                               ? d.freeze_count
                               : ECUIO_DIAG_MAX_FREEZE;
        for (uint32_t i = 0; i < n; ++i) {
            const ecuio_freeze_value_t& f = d.freeze_values[i];
            if (i) {
                l.ch(',');
            }
            l.ch('{');
            l.key("id");
            l.u64(f.def_id);
            for (uint32_t k = 0; obd2 != nullptr && k < obd2->count; ++k) {
                if (obd2->defs[k].id == f.def_id) {
                    l.key("name");
                    l.string(obd2->defs[k].name, sizeof obd2->defs[k].name);
                    l.key("unit");
                    l.string(obd2->defs[k].unit, sizeof obd2->defs[k].unit);
                    break;
                }
            }
            l.key("value");
            l.number(f.value);
            l.ch('}');
        }
        l.ch(']');
    }
    l.ch('}');
    l.ch('}');
}

} // namespace cli

#endif /* HAVNE_CLI_DIAGNOSTICS_LINE_H */
