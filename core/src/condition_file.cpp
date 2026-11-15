// File/layer: core/src/condition_file.cpp | C++17, Qt-free, every platform
// Testing: UNIT: ConditionFileTest (test_signals); FUZZ: fuzz_condition_file
//
// The condition-file loader (ADR-0063 section 5): a file's JSON (json.h)
// into a condition set, everything a file can get wrong refused here with
// the line and column, never the file's path, in the signal-file
// loader's rules and words (signal_file_build.h). What only a set of
// signals can settle is ecuio_condition_set_resolve's.

#include "ecuio/conditions.h"
#include "ecuio/signal_sets.h"
#include "condition_file.h"
#include "error_detail.h"
#include "json.h"
#include "signal_file_build.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>

namespace {

using ecuio_json::number_text;
using ecuio_json::Type;
using ecuio_json::Value;
using ecuio_sf::copy_text;
using ecuio_sf::expect;
using ecuio_sf::finite_of;
using ecuio_sf::only_keys;
using ecuio_sf::q;
using ecuio_sf::refuse;
using ecuio_sf::text_of;

using condition = ecuio_condition_set::condition;

constexpr uint32_t kNone = UINT32_MAX;

bool flag_of(const Value& v, const std::string& what)
{
    expect(v, Type::boolean, what);
    return v.boolean;
}

/* Seconds from lo_us to hi_us, as microseconds */
uint32_t duration_of(const Value& v, uint32_t lo_us, uint32_t hi_us,
                     const std::string& what)
{
    const double seconds = finite_of(v, what);
    if (seconds < lo_us / 1e6 || seconds > hi_us / 1e6) {
        refuse(v, what + " takes " + number_text(lo_us / 1e6) + " to " +
                      number_text(hi_us / 1e6) + " seconds");
    }
    return static_cast<uint32_t>(std::llround(seconds * 1e6));
}

/* A link term's states from ECUIO_TERM_LINK_NOT_OK, a check term's kinds
 * from ECUIO_CHECK_GAP and an alert's levels from ECUIO_ALERT_WARNING,
 * each in its values' order */
const char* const kLinkWords[] = {"not_ok", "timeout", "request_failed",
                                  "hard_failure"};
const char* const kCheckWords[] = {"gap", "rate_drop", "late_id", "overflow",
                                   "isotp"};
const char* const kLevelWords[] = {"warning", "danger"};
static_assert(ECUIO_TERM_LINK_HARD_FAILURE == ECUIO_TERM_LINK_NOT_OK + 3 &&
                  ECUIO_TERM_LINK_TIMEOUT == ECUIO_TERM_LINK_NOT_OK + 1,
              "kLinkWords follows the link states");
static_assert(ECUIO_CHECK_RATE_DROP == ECUIO_CHECK_GAP + 1 &&
                  ECUIO_CHECK_LATE_ID == ECUIO_CHECK_GAP + 2 &&
                  ECUIO_CHECK_OVERFLOW == ECUIO_CHECK_GAP + 3 &&
                  ECUIO_CHECK_ISOTP == ECUIO_CHECK_GAP + 4,
              "kCheckWords follows the check's kinds");
static_assert(ECUIO_ALERT_DANGER == ECUIO_ALERT_WARNING + 1,
              "kLevelWords follows the alert levels");

/* The index of v's text among words, or refused naming them all */
template <size_t N>
uint32_t word_of(const Value& v, const char* const (&words)[N],
                 const std::string& what)
{
    expect(v, Type::string, what);
    std::string list;
    for (uint32_t i = 0; i < N; ++i) {
        if (v.text == words[i]) {
            return i;
        }
        list += (i == 0 ? "" : i + 1 == N ? " or " : ", ");
        list += words[i];
    }
    refuse(v, what + " " + q(v.text) + " isn't " + list);
}

/* The file's "checks": the frames check's thresholds for the whole file */
void read_checks(const Value& v, ecuio_check_options_t& out)
{
    expect(v, Type::object, "\"checks\"");
    only_keys(v, {"gap", "rate_drop", "settle"}, "\"checks\"");
    const auto seconds = [&](const char* key, uint64_t& us) {
        if (const Value* s = v.find(key)) {
            const std::string what = std::string("\"checks\"'s ") + key;
            const double value = finite_of(*s, what);
            /* The range first: llround of a value past it is unspecified */
            const bool in_range =
                value > 0.0 && value <= ECUIO_CONDITION_MAX_CHECK_US / 1e6;
            const long long n = in_range ? std::llround(value * 1e6) : 0;
            if (n < 1) {
                refuse(*s, what + " takes more than 0, up to 3600 seconds");
            }
            us = static_cast<uint64_t>(n);
        }
    };
    seconds("gap", out.gap_us);
    seconds("settle", out.settle_us);
    if (const Value* r = v.find("rate_drop")) {
        const double value = finite_of(*r, "\"checks\"'s rate_drop");
        const bool in_range =
            value > 1.0 && value <= ECUIO_CONDITION_MAX_RATE_DROP_X100 / 100.0;
        const long long n = in_range ? std::llround(value * 100) : 0;
        if (n <= 100) {
            refuse(*r, "\"checks\"'s rate_drop takes a multiple above 1, up "
                       "to 1000, read to hundredths");
        }
        out.rate_drop_x100 = static_cast<uint32_t>(n);
    }
}

/* "id:<n>": decimal, 0 to 65535, no sign and no leading zero */
bool id_of(const std::string& digits, uint32_t* id)
{
    if (digits.empty() || digits.size() > 5 ||
        (digits.size() > 1 && digits[0] == '0')) {
        return false;
    }
    uint32_t n = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') {
            return false;
        }
        n = n * 10 + static_cast<uint32_t>(c - '0');
    }
    *id = n;
    return n <= UINT16_MAX;
}

/* One reference: id:<n>, key:<key> or obd2.<name>, the last read as
 * that def's id */
ecuio_condition_reference_t reference_of(const Value& v,
                                         const std::string& what)
{
    ecuio_condition_reference_t r{};
    const std::string text =
        text_of(v, 1, sizeof r.text - 1, what + "'s reference");
    copy_text(r.text, sizeof r.text, text);
    if (text.compare(0, 3, "id:") == 0) {
        r.kind = ECUIO_REFERENCE_ID;
        if (!id_of(text.substr(3), &r.id)) {
            refuse(v, what + "'s reference " + q(text) +
                          " takes a whole number from 0 to 65535 after id:");
        }
    } else if (text.compare(0, 4, "key:") == 0) {
        r.kind = ECUIO_REFERENCE_KEY;
        if (!ecuio_sf::key_ok(text.substr(4))) {
            refuse(v, what + "'s reference " + q(text) +
                          " takes 1 to 31 of a-z, 0-9 and _ after key:");
        }
    } else if (text.compare(0, 5, "obd2.") == 0) {
        const ecuio_signal_set_t* base = ecuio_obd2_signal_set();
        const long d = ecuio_sf::obd2_def(base, text.substr(5));
        if (d < 0) {
            refuse(v, what + "'s reference " + q(text) +
                          " isn't one of obd2's keys");
        }
        r.kind = ECUIO_REFERENCE_ID;
        r.id = base->defs[d].id;
    } else {
        refuse(v, what + "'s reference " + q(text) +
                      " isn't id:<number>, key:<key> or obd2.<name>");
    }
    return r;
}

bool same_signal(const ecuio_condition_reference_t& a,
                 const ecuio_condition_reference_t& b)
{
    if (a.kind != b.kind) {
        return false;
    }
    return a.kind == ECUIO_REFERENCE_ID ? a.id == b.id
                                        : std::strcmp(a.text, b.text) == 0;
}

void read_signal_term(const Value& t, ecuio_condition_term_t& out,
                      const std::string& what)
{
    only_keys(t,
              {"signal", "unit", "above", "below", "equals", "differs", "clear",
               "max_age"},
              what);
    out.kind = ECUIO_TERM_SIGNAL;

    const Value& list = *t.find("signal");
    expect(list, Type::array, what + "'s signal");
    if (list.items.empty() ||
        list.items.size() > ECUIO_CONDITION_MAX_REFERENCES) {
        refuse(list, what + "'s signal lists " +
                         std::to_string(list.items.size()) +
                         " references; it takes 1 to 8");
    }
    for (const Value& item : list.items) {
        const ecuio_condition_reference_t r = reference_of(item, what);
        for (uint32_t k = 0; k < out.reference_count; ++k) {
            if (same_signal(out.references[k], r)) {
                refuse(item, what + "'s reference " + q(r.text) +
                                 " names the signal " +
                                 q(out.references[k].text) +
                                 " named before it");
            }
        }
        out.references[out.reference_count++] = r;
    }

    static const struct {
        const char* key;
        uint32_t test;
    } kTests[] = {{"above", ECUIO_TERM_ABOVE},
                  {"below", ECUIO_TERM_BELOW},
                  {"equals", ECUIO_TERM_EQUALS},
                  {"differs", ECUIO_TERM_DIFFERS}};
    const Value* level = nullptr;
    const char* test = "";
    for (const auto& k : kTests) {
        if (const Value* v = t.find(k.key)) {
            if (level != nullptr) {
                refuse(*v, what + " takes one of \"above\", \"below\", "
                                  "\"equals\" and \"differs\", not two");
            }
            level = v;
            test = k.key;
            out.test = k.test;
        }
    }
    if (level == nullptr) {
        refuse(t, what + " needs one of \"above\", \"below\", \"equals\" "
                         "and \"differs\"");
    }
    out.level = finite_of(*level, what + "'s " + test);
    const bool ordered =
        out.test == ECUIO_TERM_ABOVE || out.test == ECUIO_TERM_BELOW;

    if (const Value* unit = t.find("unit")) {
        copy_text(out.unit, sizeof out.unit,
                  text_of(*unit, 0, sizeof out.unit - 1, what + "'s unit"));
        out.flags |= ECUIO_TERM_HAS_UNIT;
    } else if (ordered) {
        refuse(t, what + " compares with a level, so it needs the level's "
                         "\"unit\" (\"\" for a number that has none)");
    }

    if (const Value* clear = t.find("clear")) {
        if (!ordered) {
            refuse(*clear, what + "'s clear goes with above or below");
        }
        out.clear = finite_of(*clear, what + "'s clear");
        const bool above = out.test == ECUIO_TERM_ABOVE;
        if (above ? out.clear > out.level : out.clear < out.level) {
            refuse(*clear, what + "'s clear " + number_text(out.clear) +
                               " is " + (above ? "above" : "below") +
                               " its level " + number_text(out.level) +
                               ": it lies on the other side");
        }
        out.flags |= ECUIO_TERM_HAS_CLEAR;
    }

    if (const Value* age = t.find("max_age")) {
        out.max_age_us =
            duration_of(*age, ECUIO_COMPUTED_MIN_AGE_US,
                        ECUIO_COMPUTED_MAX_AGE_US, what + "'s max_age");
    }
}

/* One term: exactly one of a signal, the link and a check */
void read_term(const Value& t, ecuio_condition_term_t& out,
               const std::string& what)
{
    expect(t, Type::object, what);
    for (const char* join : {"all", "any"}) {
        if (const Value* v = t.find(join)) {
            refuse(*v,
                   what + " holds \"" + join + "\": terms join one level deep");
        }
    }
    const Value* signal = t.find("signal");
    const Value* link = t.find("link");
    const Value* check = t.find("check");
    const int kinds = (signal != nullptr ? 1 : 0) + (link != nullptr ? 1 : 0) +
                      (check != nullptr ? 1 : 0);
    if (kinds != 1) {
        refuse(t, what + (kinds == 0 ? " needs" : " takes") +
                      " one of \"signal\", \"link\" and \"check\"" +
                      (kinds == 0 ? "" : ", not two"));
    }
    out = ecuio_condition_term_t{};
    if (signal != nullptr) {
        read_signal_term(t, out, what);
    } else if (link != nullptr) {
        only_keys(t, {"link"}, what);
        out.kind = ECUIO_TERM_LINK;
        out.test = ECUIO_TERM_LINK_NOT_OK +
                   word_of(*link, kLinkWords, what + "'s link");
    } else {
        only_keys(t, {"check"}, what);
        out.kind = ECUIO_TERM_CHECK;
        out.test =
            ECUIO_CHECK_GAP + word_of(*check, kCheckWords, what + "'s check");
    }
}

/* "when": one term, or all or any over one to four */
void read_when(const Value& when, condition& c, const std::string& what)
{
    const std::string w = what + "'s when";
    expect(when, Type::object, w);
    const Value* all = when.find("all");
    const Value* any = when.find("any");
    if (all == nullptr && any == nullptr) {
        c.info.term_count = 1;
        read_term(when, c.terms[0], w);
        return;
    }
    only_keys(when, {"all", "any"}, w);
    if (all != nullptr && any != nullptr) {
        refuse(*any, w + " takes \"all\" or \"any\", not both");
    }
    const Value& list = all != nullptr ? *all : *any;
    const std::string join = w + (all != nullptr ? "'s all" : "'s any");
    expect(list, Type::array, join);
    if (list.items.empty() || list.items.size() > ECUIO_CONDITION_MAX_TERMS) {
        refuse(list, join + " lists " + std::to_string(list.items.size()) +
                         " terms; it takes 1 to 4");
    }
    if (any != nullptr) {
        c.info.flags |= ECUIO_CONDITION_INFO_ANY;
    }
    for (const Value& item : list.items) {
        const uint32_t i = c.info.term_count++;
        read_term(item, c.terms[i], what + "'s term " + std::to_string(i + 1));
    }
}

void read_uses(const Value& v, condition& c, const std::string& what)
{
    if (const Value* alert = v.find("alert")) {
        const std::string a = what + "'s alert";
        expect(*alert, Type::object, a);
        only_keys(*alert, {"level", "sound", "banner"}, a);
        const Value* level = alert->find("level");
        if (level == nullptr) {
            refuse(*alert, a + " needs a \"level\": warning or danger");
        }
        c.info.alert_level =
            ECUIO_ALERT_WARNING + word_of(*level, kLevelWords, a + "'s level");
        c.info.uses |= ECUIO_CONDITION_USE_ALERT;
        if (const Value* sound = alert->find("sound")) {
            c.info.uses |= flag_of(*sound, a + "'s sound")
                               ? ECUIO_CONDITION_ALERT_SOUND
                               : 0u;
        }
        if (const Value* banner = alert->find("banner")) {
            c.info.uses |= flag_of(*banner, a + "'s banner")
                               ? ECUIO_CONDITION_ALERT_BANNER
                               : 0u;
        }
    }
    if (const Value* record = v.find("record")) {
        const std::string r = what + "'s record";
        expect(*record, Type::object, r);
        only_keys(*record, {"before", "after"}, r);
        c.info.uses |= ECUIO_CONDITION_USE_RECORD;
        if (const Value* before = record->find("before")) {
            c.info.before_us = duration_of(
                *before, 0, ECUIO_CONDITION_MAX_BEFORE_US, r + "'s before");
        }
        if (const Value* after = record->find("after")) {
            c.info.after_us = duration_of(
                *after, 0, ECUIO_CONDITION_MAX_AFTER_US, r + "'s after");
        }
    }
    if (const Value* mark = v.find("mark")) {
        c.info.uses |=
            flag_of(*mark, what + "'s mark") ? ECUIO_CONDITION_USE_MARK : 0u;
    }
}

void read_condition(const Value& v, ecuio_condition_set& set)
{
    expect(v, Type::object, "a condition");
    const Value* key = v.find("key");
    if (key == nullptr) {
        refuse(v, "a condition needs a \"key\"");
    }
    condition c;
    c.key = text_of(*key, 1, ECUIO_CONDITION_KEY_MAX, "a condition's key");
    if (!ecuio_sf::key_ok(c.key)) {
        refuse(*key, "key " + q(c.key) + " takes 1 to 31 of a-z, 0-9 and _");
    }
    for (const condition& x : set.conditions) {
        if (x.key == c.key) {
            refuse(*key, "key " + q(c.key) + " is given twice");
        }
    }
    const std::string what = "condition " + q(c.key);
    only_keys(v,
              {"key", "name", "enabled", "when", "for", "clear_for", "alert",
               "record", "mark"},
              what);

    const Value* name = v.find("name");
    if (name == nullptr) {
        refuse(v, what + " needs a \"name\"");
    }
    c.name = text_of(*name, 1, ECUIO_CONDITION_NAME_MAX, what + "'s name");
    ecuio_sf::no_formula(*name, what + "'s name");
    for (const condition& x : set.conditions) {
        if (x.name == c.name) {
            refuse(*name, what + " has the name " + q(c.name) +
                              ", which condition " + q(x.key) +
                              " has too: a name names one condition");
        }
    }

    if (const Value* enabled = v.find("enabled")) {
        c.info.flags |= flag_of(*enabled, what + "'s enabled")
                            ? 0u
                            : ECUIO_CONDITION_INFO_DISABLED;
    }
    const Value* when = v.find("when");
    if (when == nullptr) {
        refuse(v, what + " needs a \"when\"");
    }
    read_when(*when, c, what);
    if (const Value* hold = v.find("for")) {
        c.info.for_us =
            duration_of(*hold, 0, ECUIO_CONDITION_MAX_FOR_US, what + "'s for");
    }
    if (const Value* hold = v.find("clear_for")) {
        c.info.clear_for_us = duration_of(*hold, 0, ECUIO_CONDITION_MAX_FOR_US,
                                          what + "'s clear_for");
    }
    read_uses(v, c, what);
    set.conditions.push_back(std::move(c));
}

void read_file(const Value& root, ecuio_condition_set& set)
{
    expect(root, Type::object, "the file");
    if (root.members.empty() ||
        root.members.front().key != "havne_conditions") {
        refuse(root, "a condition file starts with \"havne_conditions\"");
    }
    const Value& version = root.members.front().value;
    expect(version, Type::number, "\"havne_conditions\"");
    /* in_int64 is a whole number as written: 1.0 isn't */
    if (!version.in_int64 || version.whole != ECUIO_CONDITION_FILE_FORMAT) {
        refuse(version, "\"havne_conditions\" is format " +
                            (version.in_int64 ? std::to_string(version.whole)
                                              : std::string("?")) +
                            ", and this Havne reads format " +
                            std::to_string(ECUIO_CONDITION_FILE_FORMAT));
    }
    only_keys(root, {"havne_conditions", "name", "checks", "conditions"},
              "the file");
    if (const Value* name = root.find("name")) {
        set.name = text_of(*name, 1, 63, "\"name\"");
    }
    if (const Value* checks = root.find("checks")) {
        read_checks(*checks, set.checks);
    }
    const Value* list = root.find("conditions");
    if (list == nullptr) {
        refuse(root, "the file needs \"conditions\", a list");
    }
    expect(*list, Type::array, "\"conditions\"");
    if (list->items.size() > ECUIO_CONDITION_MAX_CONDITIONS) {
        refuse(*list, "\"conditions\" lists " +
                          std::to_string(list->items.size()) +
                          " conditions; a file takes at most " +
                          std::to_string(ECUIO_CONDITION_MAX_CONDITIONS));
    }
    set.conditions.reserve(list->items.size());
    for (const Value& item : list->items) {
        read_condition(item, set);
    }
}

/* The def a reference names in signals, or NULL */
const ecuio_signal_def_t* def_of(const ecuio_condition_reference_t& r,
                                 const ecuio_signal_set_t* signals,
                                 const ecuio_signal_file_t* file)
{
    if (r.kind == ECUIO_REFERENCE_KEY) {
        return ecuio_signal_file_find(file, r.text + 4);
    }
    if (signals == nullptr || r.id >= ECUIO_UDS_GENERIC_SIG_ID_BASE) {
        return nullptr;
    }
    for (uint32_t i = 0; i < signals->count; ++i) {
        if (signals->defs[i].id == r.id) {
            return &signals->defs[i];
        }
    }
    return nullptr;
}

/* Why a term can't be judged on the def it bound to, or _NONE */
uint32_t why_not(const ecuio_condition_term_t& t, const ecuio_signal_def_t& d)
{
    const bool computed = (d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0;
    if (!computed &&
        (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX)) {
        return ECUIO_CONDITION_WHY_NOT_A_NUMBER;
    }
    if (!computed && d.kind == ECUIO_VALUE_ENUM &&
        (t.test == ECUIO_TERM_ABOVE || t.test == ECUIO_TERM_BELOW)) {
        return ECUIO_CONDITION_WHY_ENUM_ORDER;
    }
    if (std::strncmp(t.unit, d.unit, sizeof d.unit) != 0) {
        return ECUIO_CONDITION_WHY_UNIT;
    }
    return ECUIO_CONDITION_WHY_NONE;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_condition_set_load(const char* bytes, size_t length,
                                        ecuio_condition_set_t** out)
{
    ecuio_detail_clear();
    if (out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    if (bytes == nullptr && length > 0) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    try {
        ecuio_json::Value root;
        ecuio_json::Error error;
        ecuio_json::Limits limits;
        limits.max_bytes = ECUIO_CONDITION_FILE_MAX_BYTES;
        if (!ecuio_json::parse(bytes != nullptr ? bytes : "", length, &root,
                               &error, limits)) {
            ecuio_detail_set("%s", error.to_string().c_str());
            return error.words == "out of memory" ? ECUIO_ERR_OUT_OF_MEMORY
                                                  : ECUIO_ERR_INVALID_ARGUMENT;
        }
        std::unique_ptr<ecuio_condition_set> set(new ecuio_condition_set());
        read_file(root, *set);
        if (length > 0) {
            set->bytes.assign(bytes, length);
        }
        *out = set.release();
        return ECUIO_OK;
    } catch (const ecuio_sf::refused& r) {
        ecuio_detail_set("line %u, column %u: %s",
                         static_cast<unsigned>(r.line),
                         static_cast<unsigned>(r.column), r.words.c_str());
        return ECUIO_ERR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    } catch (...) {
        ecuio_detail_set("the file couldn't be read");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
}

void ecuio_condition_set_free(ecuio_condition_set_t* set)
{
    delete set;
}

uint32_t ecuio_condition_set_count(const ecuio_condition_set_t* set)
{
    return set != nullptr ? static_cast<uint32_t>(set->conditions.size()) : 0;
}

const char* ecuio_condition_set_name(const ecuio_condition_set_t* set)
{
    return set != nullptr ? set->name.c_str() : "";
}

ecuio_status_t ecuio_condition_set_checks(const ecuio_condition_set_t* set,
                                          ecuio_check_options_t* out)
{
    if (set == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = set->checks;
    return ECUIO_OK;
}

ecuio_status_t ecuio_condition_set_get(const ecuio_condition_set_t* set,
                                       uint32_t index,
                                       ecuio_condition_info_t* out)
{
    if (set == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (index >= set->conditions.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const condition& c = set->conditions[index];
    *out = c.info;
    out->key = c.key.c_str();
    out->name = c.name.c_str();
    return ECUIO_OK;
}

ecuio_status_t ecuio_condition_set_get_term(const ecuio_condition_set_t* set,
                                            uint32_t condition_index,
                                            uint32_t term,
                                            ecuio_condition_term_t* out)
{
    if (set == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (condition_index >= set->conditions.size() ||
        term >= set->conditions[condition_index].info.term_count) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *out = set->conditions[condition_index].terms[term];
    return ECUIO_OK;
}

ecuio_status_t ecuio_condition_set_resolve(const ecuio_condition_set_t* set,
                                           uint32_t index,
                                           const ecuio_signal_set_t* signals,
                                           const ecuio_signal_file_t* file,
                                           ecuio_condition_binding_t* out)
{
    if (set == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (index >= set->conditions.size() ||
        (file != nullptr && ecuio_signal_file_set(file) != signals)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const condition& c = set->conditions[index];
    ecuio_condition_binding_t b{};
    b.why_term = kNone;
    b.term_count = c.info.term_count;
    for (uint32_t i = 0; i < ECUIO_CONDITION_MAX_TERMS; ++i) {
        b.reference[i] = kNone;
        b.signal_id[i] = kNone;
    }
    for (uint32_t i = 0; i < c.info.term_count; ++i) {
        const ecuio_condition_term_t& t = c.terms[i];
        if (t.kind != ECUIO_TERM_SIGNAL) {
            continue;
        }
        uint32_t why = ECUIO_CONDITION_WHY_NO_SIGNAL;
        for (uint32_t k = 0; k < t.reference_count; ++k) {
            const ecuio_signal_def_t* d =
                def_of(t.references[k], signals, file);
            if (d != nullptr) {
                b.reference[i] = k;
                b.signal_id[i] = d->id;
                why = why_not(t, *d);
                break;
            }
        }
        if (why != ECUIO_CONDITION_WHY_NONE &&
            b.why == ECUIO_CONDITION_WHY_NONE) {
            b.why = why;
            b.why_term = i;
        }
    }
    if ((c.info.flags & ECUIO_CONDITION_INFO_DISABLED) != 0) {
        b.why = ECUIO_CONDITION_WHY_DISABLED;
        b.why_term = kNone;
    }
    *out = b;
    return ECUIO_OK;
}

} // extern "C"
