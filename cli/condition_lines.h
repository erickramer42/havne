// File/layer: cli/condition_lines.h | havne-cli, a condition file's lines (ADR-0063)
// Testing: UNIT: CliConditionTest (test_cli)
//
// What `havne-cli conditions check` writes besides core's loader
// (conditions.h): the arguments it takes, and the lines that say what the
// loader read and how each condition's references meet a set of signals.

#ifndef HAVNE_CLI_CONDITION_LINES_H
#define HAVNE_CLI_CONDITION_LINES_H

#include "jsonl.h"

#include "ecuio/conditions.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace cli {

// conditions check <file> [--signals <file> | --profile <name>]
struct ConditionsCheckArgs {
    const char* file = nullptr;
    const char* signals = nullptr;
    const char* profile = nullptr;
};

// The arguments after "conditions check". False with why set for a usage
// error: no file or two, an option without its value, both sets named,
// anything else starting "--"
inline bool parse_conditions_check(int argc, const char* const* argv,
                                   ConditionsCheckArgs* out, std::string* why)
{
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        const bool option = a.compare(0, 2, "--") == 0;
        if (a == "--signals" || a == "--profile") {
            const char*& slot = a == "--signals" ? out->signals : out->profile;
            if (i + 1 >= argc || argv[i + 1][0] == '\0' || slot != nullptr) {
                *why = a + " takes one " +
                       (a == "--signals" ? "signal file" : "profile name");
                return false;
            }
            slot = argv[++i];
        } else if (option) {
            *why = "unexpected '" + a + "'";
            return false;
        } else if (out->file != nullptr) {
            *why = "conditions check takes one file";
            return false;
        } else {
            out->file = argv[i];
        }
    }
    if (out->file == nullptr || out->file[0] == '\0') {
        *why = "conditions check takes a condition file";
        return false;
    }
    if (out->signals != nullptr && out->profile != nullptr) {
        *why = "conditions check takes --signals or --profile, not both";
        return false;
    }
    return true;
}

// A signal term's comparison as the file writes it; "unknown" for one
// this CLI doesn't know
inline const char* term_test_word(uint32_t test)
{
    switch (test) {
    case ECUIO_TERM_ABOVE:
        return "above";
    case ECUIO_TERM_BELOW:
        return "below";
    case ECUIO_TERM_EQUALS:
        return "equals";
    case ECUIO_TERM_DIFFERS:
        return "differs";
    default:
        return "unknown";
    }
}

// A link term's state as the file writes it
inline const char* term_link_word(uint32_t test)
{
    switch (test) {
    case ECUIO_TERM_LINK_NOT_OK:
        return "not_ok";
    case ECUIO_TERM_LINK_TIMEOUT:
        return "timeout";
    case ECUIO_TERM_LINK_REQUEST_FAILED:
        return "request_failed";
    case ECUIO_TERM_LINK_HARD_FAILURE:
        return "hard_failure";
    default:
        return "unknown";
    }
}

// A check term's kind as the file writes it
inline const char* term_check_word(uint32_t test)
{
    switch (test) {
    case ECUIO_CHECK_GAP:
        return "gap";
    case ECUIO_CHECK_RATE_DROP:
        return "rate_drop";
    case ECUIO_CHECK_LATE_ID:
        return "late_id";
    case ECUIO_CHECK_OVERFLOW:
        return "overflow";
    case ECUIO_CHECK_ISOTP:
        return "isotp";
    default:
        return "unknown";
    }
}

// Why a condition can't be judged against the set (ECUIO_CONDITION_WHY_*)
inline const char* condition_why_word(uint32_t why)
{
    switch (why) {
    case ECUIO_CONDITION_WHY_DISABLED:
        return "disabled";
    case ECUIO_CONDITION_WHY_NO_SIGNAL:
        return "no_signal";
    case ECUIO_CONDITION_WHY_UNIT:
        return "unit";
    case ECUIO_CONDITION_WHY_NOT_A_NUMBER:
        return "not_a_number";
    case ECUIO_CONDITION_WHY_ENUM_ORDER:
        return "enum_order";
    default:
        return "unknown";
    }
}

inline void word(Line& l, const char* w)
{
    l.string(w, std::strlen(w));
}

// {"type":"condition_set", name (null with none), conditions, gap_us,
// rate_drop, settle_us (the file's, else the check's defaults, as info's
// summary names them), against: the profile the conditions are resolved
// against, null with none}
inline void condition_set_line(Line& l, const char* name, uint32_t conditions,
                               const ecuio_check_options_t& checks,
                               const char* against)
{
    l.raw("{\"type\":\"condition_set\"");
    l.key("name");
    if (name[0] != '\0') {
        word(l, name);
    } else {
        l.raw("null");
    }
    l.key("conditions");
    l.u64(conditions);
    l.key("gap_us");
    l.u64(checks.gap_us != 0 ? checks.gap_us : ECUIO_CHECK_DEFAULT_GAP_US);
    l.key("rate_drop");
    l.number((checks.rate_drop_x100 != 0 ? checks.rate_drop_x100
                                         : ECUIO_CHECK_DEFAULT_RATE_DROP_X100) /
             100.0);
    l.key("settle_us");
    l.u64(checks.settle_us != 0 ? checks.settle_us
                                : ECUIO_CHECK_DEFAULT_SETTLE_US);
    l.key("against");
    if (against != nullptr) {
        word(l, against);
    } else {
        l.raw("null");
    }
    l.ch('}');
}

// The def with an id in a set, or nullptr
inline const ecuio_signal_def_t* def_by_id(const ecuio_signal_set_t* set,
                                           uint32_t id)
{
    for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
        if (set->defs[i].id == id) {
            return &set->defs[i];
        }
    }
    return nullptr;
}

// One term: {kind: signal, signal [references], unit (null when the file
// wrote none), test, level, clear|null, max_age_us|null (the default
// bound)}, {kind: link, link} or {kind: check, check}. With a binding, a
// signal term adds resolved: null, or {reference, id, name, unit} of the
// def it took in the set.
inline void condition_term(Line& l, const ecuio_condition_term_t& t,
                           const ecuio_condition_binding_t* binding,
                           uint32_t index, const ecuio_signal_set_t* set)
{
    l.ch('{');
    l.key("kind");
    if (t.kind == ECUIO_TERM_LINK) {
        l.raw("\"link\"");
        l.key("link");
        word(l, term_link_word(t.test));
        l.ch('}');
        return;
    }
    if (t.kind == ECUIO_TERM_CHECK) {
        l.raw("\"check\"");
        l.key("check");
        word(l, term_check_word(t.test));
        l.ch('}');
        return;
    }
    l.raw("\"signal\"");
    l.key("signal");
    l.ch('[');
    for (uint32_t k = 0;
         k < t.reference_count && k < ECUIO_CONDITION_MAX_REFERENCES; ++k) {
        if (k != 0) {
            l.ch(',');
        }
        l.string(t.references[k].text, sizeof t.references[k].text);
    }
    l.ch(']');
    l.key("unit");
    if ((t.flags & ECUIO_TERM_HAS_UNIT) != 0) {
        l.string(t.unit, sizeof t.unit);
    } else {
        l.raw("null");
    }
    l.key("test");
    word(l, term_test_word(t.test));
    l.key("level");
    l.number(t.level);
    l.key("clear");
    if ((t.flags & ECUIO_TERM_HAS_CLEAR) != 0) {
        l.number(t.clear);
    } else {
        l.raw("null");
    }
    l.key("max_age_us");
    if (t.max_age_us != 0) {
        l.u64(t.max_age_us);
    } else {
        l.raw("null");
    }
    if (binding != nullptr) {
        l.key("resolved");
        const uint32_t taken = binding->reference[index];
        const ecuio_signal_def_t* def =
            def_by_id(set, binding->signal_id[index]);
        if (def == nullptr || taken >= t.reference_count) {
            l.raw("null");
        } else {
            l.ch('{');
            l.key("reference");
            l.string(t.references[taken].text, sizeof t.references[taken].text);
            l.key("id");
            l.u64(def->id);
            l.key("name");
            l.string(def->name, sizeof def->name);
            l.key("unit");
            l.string(def->unit, sizeof def->unit);
            l.ch('}');
        }
    }
    l.ch('}');
}

// The bytes a term takes on its line at most: its keys and numbers, each
// reference as written (a reference holds no character to escape), its
// unit, and the def it resolved to, a unit and a name escaped six bytes
// for each byte
inline size_t condition_term_bytes()
{
    const ecuio_condition_term_t* t = nullptr;
    const ecuio_signal_def_t* d = nullptr;
    const size_t reference = sizeof t->references[0].text + 3;
    return 320 + (ECUIO_CONDITION_MAX_REFERENCES + 1) * reference +
           6 * (sizeof t->unit + sizeof d->name + sizeof d->unit);
}

// The bytes a condition's line takes at most
inline size_t condition_def_bytes(const ecuio_condition_info_t& info)
{
    return 512 + Line::string_bytes(info.key) + Line::string_bytes(info.name) +
           ECUIO_CONDITION_MAX_TERMS * condition_term_bytes();
}

// {"type":"condition_def", key, name, enabled, join (all|any), terms
// [...], for_us, clear_for_us, alert: null or {level, sound, banner},
// record: null or {before_us, after_us}, mark}; with a binding also
// available, why (null, or disabled, no_signal, unit, not_a_number,
// enum_order) and why_term (null, or the first term that doesn't bind,
// from 0)
inline void condition_def_line(Line& l, const ecuio_condition_info_t& info,
                               const ecuio_condition_term_t* terms,
                               const ecuio_condition_binding_t* binding,
                               const ecuio_signal_set_t* set)
{
    l.raw("{\"type\":\"condition_def\"");
    l.key("key");
    word(l, info.key);
    l.key("name");
    word(l, info.name);
    l.key("enabled");
    l.raw((info.flags & ECUIO_CONDITION_INFO_DISABLED) != 0 ? "false" : "true");
    l.key("join");
    l.raw((info.flags & ECUIO_CONDITION_INFO_ANY) != 0 ? "\"any\"" : "\"all\"");
    l.key("terms");
    l.ch('[');
    for (uint32_t i = 0; i < info.term_count && i < ECUIO_CONDITION_MAX_TERMS;
         ++i) {
        if (i != 0) {
            l.ch(',');
        }
        condition_term(l, terms[i], binding, i, set);
    }
    l.ch(']');
    l.key("for_us");
    l.u64(info.for_us);
    l.key("clear_for_us");
    l.u64(info.clear_for_us);
    l.key("alert");
    if ((info.uses & ECUIO_CONDITION_USE_ALERT) != 0) {
        l.ch('{');
        l.key("level");
        l.raw(info.alert_level == ECUIO_ALERT_DANGER ? "\"danger\""
                                                     : "\"warning\"");
        l.key("sound");
        l.raw((info.uses & ECUIO_CONDITION_ALERT_SOUND) != 0 ? "true"
                                                             : "false");
        l.key("banner");
        l.raw((info.uses & ECUIO_CONDITION_ALERT_BANNER) != 0 ? "true"
                                                              : "false");
        l.ch('}');
    } else {
        l.raw("null");
    }
    l.key("record");
    if ((info.uses & ECUIO_CONDITION_USE_RECORD) != 0) {
        l.ch('{');
        l.key("before_us");
        l.u64(info.before_us);
        l.key("after_us");
        l.u64(info.after_us);
        l.ch('}');
    } else {
        l.raw("null");
    }
    l.key("mark");
    l.raw((info.uses & ECUIO_CONDITION_USE_MARK) != 0 ? "true" : "false");
    if (binding != nullptr) {
        l.key("available");
        l.raw(binding->why == ECUIO_CONDITION_WHY_NONE ? "true" : "false");
        l.key("why");
        if (binding->why == ECUIO_CONDITION_WHY_NONE) {
            l.raw("null");
        } else {
            word(l, condition_why_word(binding->why));
        }
        l.key("why_term");
        if (binding->why_term < info.term_count) {
            l.u64(binding->why_term);
        } else {
            l.raw("null");
        }
    }
    l.ch('}');
}

} // namespace cli

#endif // HAVNE_CLI_CONDITION_LINES_H
