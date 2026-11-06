// File/layer: cli/diagnostics_line.h | havne-cli, an ECU's diagnostics as a line (ADR-0037)
// Testing: UNIT: CliDiagnosticsTest, CliDtcTest, CliInspectionTest (test_cli)
//
// One ECU's report once a read has completed: readiness (the MIL, the
// confirmed-code count and each monitor by name, with this drive cycle's
// state once PID 41 has answered), PID 41's own outcome, the three code
// lists,
// each code as it's written ("P0301") with its title beside it in
// "titles" (ADR-0044; null where the list has none), and the freeze
// frame's code, its title and its values by
// the obd2 def that decoded them, in its unit, as decoded. An item that
// was refused carries the negative reply in frames.csv's form.
//
// Once every ECU's read has completed, the inspection line judges the
// car's readiness by the federal rule (ADR-0045).

#ifndef HAVNE_CLI_DIAGNOSTICS_LINE_H
#define HAVNE_CLI_DIAGNOSTICS_LINE_H

#include "jsonl.h"

#include "ecuio/core.h"
#include "ecuio/signals.h"

#include <cstdint>

namespace cli {

// An outcome's word: "answered", "refused", "unanswered", "unsupported"
// (not asked: the ECU's bitmaps leave the PID out) or "unasked"
inline const char* diagnostics_outcome_word(uint8_t outcome)
{
    switch (outcome) {
    case ECUIO_DIAG_ANSWERED:
        return "answered";
    case ECUIO_DIAG_REFUSED:
        return "refused";
    case ECUIO_DIAG_UNANSWERED:
        return "unanswered";
    case ECUIO_DIAG_UNSUPPORTED:
        return "unsupported";
    default:
        return "unasked";
    }
}

// An item's outcome, a refusal with its "reply" ("7F 0A 11")
inline void diagnostics_outcome(Line& l, uint8_t outcome, uint8_t service,
                                uint8_t nrc)
{
    l.key("outcome");
    l.string(diagnostics_outcome_word(outcome), SIZE_MAX);
    if (outcome == ECUIO_DIAG_REFUSED) {
        const uint8_t reply[3] = {0x7F, service, nrc};
        l.key("reply");
        l.hex_bytes(reply, sizeof reply);
    }
}

// A code's title (ecuio_dtc_title), or null when the list has none
inline void dtc_title(Line& l, uint16_t code)
{
    const char* title = ecuio_dtc_title(code);
    if (*title == '\0') {
        l.raw("null");
    } else {
        l.string(title, SIZE_MAX);
    }
}

// A typed code's line (`havne-cli dtc`): the code as written, its
// title or null, and where the titles come from
inline void dtc_line(Line& l, uint16_t code)
{
    char text[8] = {};
    (void)ecuio_dtc_format(code, text, sizeof text);
    l.raw("{\"type\":\"dtc\"");
    l.key("code");
    l.string(text, sizeof text);
    l.key("title");
    dtc_title(l, code);
    l.key("source");
    l.string(ecuio_dtc_title_source(), SIZE_MAX);
    l.ch('}');
}

// A verdict's word: "ready", "not_ready", "mil_on", "year_needed",
// "not_covered", "reading" or "unknown"
inline const char* inspection_word(uint8_t verdict)
{
    switch (verdict) {
    case ECUIO_INSPECTION_READY:
        return "ready";
    case ECUIO_INSPECTION_NOT_READY:
        return "not_ready";
    case ECUIO_INSPECTION_MIL_ON:
        return "mil_on";
    case ECUIO_INSPECTION_YEAR_NEEDED:
        return "year_needed";
    case ECUIO_INSPECTION_NOT_COVERED:
        return "not_covered";
    case ECUIO_INSPECTION_READING:
        return "reading";
    default:
        return "unknown";
    }
}

// The car's readiness against the federal rule (ADR-0045), every ECU's
// report judged together: the verdict, the model year it was judged for
// (null with none), the incomplete monitors by name, how many the rule
// allows that year (null with no year), the reports counted and the rule
inline void inspection_line(Line& l, const ecuio_inspection_t& i,
                            uint32_t model_year)
{
    l.raw("{\"type\":\"inspection\"");
    l.key("verdict");
    l.string(inspection_word(i.verdict), SIZE_MAX);
    l.key("model_year");
    if (model_year == 0) {
        l.raw("null");
    } else {
        l.u64(model_year);
    }
    l.key("incomplete");
    l.ch('[');
    bool first = true;
    for (uint8_t id = 0; id < 16; ++id) {
        if (((i.incomplete_ids >> id) & 1u) == 0) {
            continue;
        }
        if (!first) {
            l.ch(',');
        }
        first = false;
        l.string(ecuio_monitor_name(id, i.diag_flags), SIZE_MAX);
    }
    l.ch(']');
    l.key("allowed");
    if (model_year == 0) {
        l.raw("null");
    } else {
        l.u64(i.allowed);
    }
    l.key("reports");
    l.u64(i.reports);
    l.key("rule");
    l.string("40 CFR 85.2222", SIZE_MAX);
    l.ch('}');
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
            if (d.flags & ECUIO_DIAG_CYCLE) {
                l.key("cycle_enabled");
                l.raw((d.monitors[i].flags & ECUIO_MONITOR_CYCLE_ENABLED)
                          ? "true"
                          : "false");
                l.key("cycle_complete");
                l.raw((d.monitors[i].flags & ECUIO_MONITOR_CYCLE_COMPLETE)
                          ? "true"
                          : "false");
            }
            l.ch('}');
        }
        l.ch(']');
    }
    l.ch('}');

    /* PID 41: the report keeps no refusal's code, so no reply */
    l.key("cycle");
    l.ch('{');
    l.key("outcome");
    l.string(diagnostics_outcome_word(d.cycle), SIZE_MAX);
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
            // Each code's title beside it (ADR-0044), null with none
            l.key("titles");
            l.ch('[');
            for (uint32_t i = 0; i < held; ++i) {
                if (i) {
                    l.ch(',');
                }
                dtc_title(l, list.codes[i]);
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
            l.key("title");
            dtc_title(l, d.freeze_dtc);
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
