// File/layer: fuzz/fuzz_signal_file.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_signal_file

#include <ecuio/core.h>
#include <ecuio/profile.h>
#include <ecuio/signals.h>

#include "computed.h"      /* core/src: the reference's machine */
#include "frame_decoder.h" /* core/src: the set's decode, chains and all */
#include "json.h"          /* core/src: the file with an expression put in */

#include "fuzz_same_tables.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace {

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

/* What any gauge the loader keeps must be: a dial for this def */
void check_gauge(const ecuio_signal_gauge_t* g, const ecuio_signal_def_t& d)
{
    if (g == nullptr) {
        return;
    }
    const bool computed = (d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0;
    check(computed || d.kind == ECUIO_VALUE_UINT || d.kind == ECUIO_VALUE_INT);
    check(std::isfinite(g->min) && std::isfinite(g->max) && g->min < g->max);
    check((g->flags & ~(ECUIO_GAUGE_WARN_LOW | ECUIO_GAUGE_WARN_HIGH)) == 0);
    check(g->reserved[0] == 0 && g->reserved[1] == 0 && g->reserved[2] == 0);
    const bool low = (g->flags & ECUIO_GAUGE_WARN_LOW) != 0;
    const bool high = (g->flags & ECUIO_GAUGE_WARN_HIGH) != 0;
    check(low ? (std::isfinite(g->warn_low) && g->warn_low >= g->min &&
                 g->warn_low <= g->max)
              : g->warn_low == 0.0);
    check(high ? (std::isfinite(g->warn_high) && g->warn_high >= g->min &&
                  g->warn_high <= g->max)
               : g->warn_high == 0.0);
    check(!(low && high) || g->warn_low < g->warn_high);
    double lo = 0.0;
    double hi = 0.0;
    if (ecuio_def_range(&d, &lo, &hi) == ECUIO_DECODE_OK) {
        check(g->max >= lo && g->min <= hi);
    }
}

/* The chain's reference, apart from the decoder's plan and passes: each
 * number def's newest value and time, from the batches; a computed def
 * at a frame's time worked out by walking its inputs (a number within its
 * bound, a computed one walked the same way at the same time, else no
 * value) through core's machine, fed when an input is one of the frame's
 * numbers or a fed computed def. Nothing is kept between frames but the
 * numbers' values. */
constexpr uint32_t kRefDefs = ECUIO_SIGNAL_FILE_MAX_SIGNALS + 256;

struct chain_ref {
    const ecuio_signal_set_t* set = nullptr;
    double leaf_value[kRefDefs];
    uint64_t leaf_us[kRefDefs];
    bool leaf_seen[kRefDefs];
    bool leaf_now[kRefDefs];
    uint8_t state[kRefDefs];
    double value[kRefDefs];
    bool fed[kRefDefs];
    uint64_t now = 0;

    void reset(const ecuio_signal_set_t* s)
    {
        set = s;
        memset(leaf_seen, 0, sizeof leaf_seen);
    }

    void frame(uint64_t at_us)
    {
        now = at_us;
        memset(leaf_now, 0, sizeof leaf_now);
        memset(state, 0, sizeof state);
    }

    void take(uint32_t def, double v)
    {
        leaf_value[def] = v;
        leaf_us[def] = now;
        leaf_seen[def] = true;
        leaf_now[def] = true;
    }

    uint32_t index_of(uint16_t id) const
    {
        uint32_t i = 0;
        while (i < set->count && set->defs[i].id != id) {
            ++i;
        }
        return i;
    }

    void walk(uint32_t i)
    {
        if (state[i] != 0) {
            return;
        }
        state[i] = 1; /* a loaded set has no circle to meet it again */
        const ecuio_computed_t& c = set->computed[set->defs[i].table_first];
        double in[ECUIO_COMPUTED_MAX_INPUTS] = {};
        bool f = false;
        for (uint32_t k = 0; k < c.input_count; ++k) {
            const uint32_t j = index_of(c.input_ids[k]);
            check(j < set->count);
            in[k] = std::numeric_limits<double>::quiet_NaN();
            if (set->defs[j].flags & ECUIO_DEF_FLAG_COMPUTED) {
                walk(j);
                in[k] = value[j];
                f = f || fed[j];
            } else {
                const uint64_t bound = c.max_age_us[k] != 0
                                           ? c.max_age_us[k]
                                           : ECUIO_VALUE_STALE_US;
                if (leaf_seen[j] && now - leaf_us[j] <= bound) {
                    in[k] = leaf_value[j];
                }
                f = f || leaf_now[j];
            }
        }
        double out = 0.0;
        value[i] = ecuio_computed_run(c, in, &out)
                       ? out
                       : std::numeric_limits<double>::quiet_NaN();
        fed[i] = f;
        state[i] = 2;
    }
};

/* The defs at each end of a set whose names are compiled */
const uint32_t kRefEnds = 64;

/* Compiling the name a's expressions read d by reads d. A text def's is
 * refused, and a computed def's at the chain's bound as one level too
 * deep. */
void check_ref_reads(const ecuio_signal_file_t* a, const ecuio_signal_def_t& d,
                     const char* name)
{
    ecuio_expr_check_t read{};
    read.struct_size = sizeof read;
    const ecuio_status_t st =
        ecuio_signal_file_check_expr(a, nullptr, name, strlen(name), &read);
    if (st == ECUIO_ERR_OUT_OF_MEMORY) {
        return;
    }
    const std::string why = ecuio_last_error_detail();
    if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
        check(st == ECUIO_ERR_INVALID_ARGUMENT);
        return;
    }
    if (st != ECUIO_OK) {
        check((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0 &&
              why.find("computed signals deep") != std::string::npos);
        return;
    }
    check(read.program.input_count == 1 && read.program.input_ids[0] == d.id);
}

ecuio_json::Value text_value(const std::string& text)
{
    ecuio_json::Value v;
    v.type = ecuio_json::Type::string;
    v.text = text;
    return v;
}

/* Whether text loads, its detail kept; *program the computed signal
 * key's when it does */
bool loads(const std::string& text, const std::string& key,
           ecuio_computed_t* program, std::string* detail)
{
    ecuio_signal_file_t* f = nullptr;
    const ecuio_status_t st =
        ecuio_signal_file_load(text.data(), text.size(), &f);
    *detail = ecuio_last_error_detail();
    if (st != ECUIO_OK) {
        check(st == ECUIO_ERR_INVALID_ARGUMENT ||
              st == ECUIO_ERR_OUT_OF_MEMORY);
        return false;
    }
    const ecuio_signal_def_t* def = ecuio_signal_file_find(f, key.c_str());
    check(def != nullptr && (def->flags & ECUIO_DEF_FLAG_COMPUTED) != 0);
    *program = ecuio_signal_file_set(f)->computed[def->table_first];
    ecuio_signal_file_free(f);
    return true;
}

/* A file's JSON with the tool's keys check_expr_against_load reads: the
 * expression, and the signal it is put in */
struct expr_case {
    ecuio_json::Value root;
    ecuio_json::Value* signals = nullptr;
    ecuio_json::Value* version = nullptr;
    std::string expr;
    std::string key;
    ecuio_json::Value* edited = nullptr; /* the file's signal of that key */

    /* False for bytes with no "x-expr", and for a text that written
     * again is no longer the file: nothing follows from either */
    bool read(const char* bytes, size_t size)
    {
        using ecuio_json::Type;
        ecuio_json::Error error;
        if (!ecuio_json::parse(bytes, size, &root, &error)) {
            return false;
        }
        const ecuio_json::Value* x_expr = root.find("x-expr");
        if (x_expr == nullptr || x_expr->type != Type::string) {
            return false;
        }
        expr = x_expr->text;
        for (ecuio_json::Member& m : root.members) {
            if (m.key == "signals") {
                signals = &m.value;
            } else if (m.key == "havne_signals") {
                version = &m.value;
            }
        }
        check(signals != nullptr && version != nullptr &&
              signals->type == Type::array);
        ecuio_signal_file_t* again = nullptr;
        const std::string same = ecuio_json::write(root);
        if (same.size() > ECUIO_SIGNAL_FILE_MAX_BYTES ||
            ecuio_signal_file_load(same.data(), same.size(), &again) !=
                ECUIO_OK) {
            return false;
        }
        ecuio_signal_file_free(again);
        const ecuio_json::Value* x_key = root.find("x-key");
        key =
            x_key != nullptr && x_key->type == Type::string ? x_key->text : "";
        return true;
    }

    /* A name no def of the file's set has, key set to one no signal has */
    std::string unused_name(const ecuio_signal_file_t* a)
    {
        const ecuio_signal_set_t* set = ecuio_signal_file_set(a);
        for (uint32_t n = 0;; ++n) {
            key = "zz_check" + std::to_string(n);
            const std::string name = "Zz check " + std::to_string(n);
            bool taken = ecuio_signal_file_find(a, key.c_str()) != nullptr;
            for (uint32_t i = 0; i < set->count && !taken; ++i) {
                taken = name == set->defs[i].name;
            }
            if (!taken) {
                return name;
            }
        }
    }

    /* The expression put in the JSON as a computed signal: in key's
     * place when the file has that key, else a new signal's, after the
     * file's. False when the file has no room for one more. */
    bool place(const ecuio_signal_file_t* a)
    {
        using ecuio_json::Member;
        if (ecuio_signal_file_find(a, key.c_str()) != nullptr) {
            for (ecuio_json::Value& s : signals->items) {
                const ecuio_json::Value* k = s.find("key");
                if (k != nullptr && k->text == key) {
                    edited = &s;
                }
            }
            check(edited != nullptr);
        }
        ecuio_json::Value signal;
        signal.type = ecuio_json::Type::object;
        if (edited != nullptr) {
            signal.members.push_back(Member{"key", 0, 0, text_value(key)});
            signal.members.push_back(
                Member{"name", 0, 0, *edited->find("name")});
        } else {
            if (signals->items.size() >= ECUIO_SIGNAL_FILE_MAX_SIGNALS) {
                return false;
            }
            const std::string name = unused_name(a);
            signal.members.push_back(Member{"key", 0, 0, text_value(key)});
            signal.members.push_back(Member{"name", 0, 0, text_value(name)});
        }
        signal.members.push_back(Member{"kind", 0, 0, text_value("computed")});
        signal.members.push_back(Member{"expr", 0, 0, text_value(expr)});
        if (edited == nullptr) {
            signals->items.push_back(signal);
        } else {
            *edited = signal;
        }
        return true;
    }

    /* The JSON's text at a format */
    std::string at_format(uint32_t f)
    {
        version->type = ecuio_json::Type::number;
        version->integer = true;
        version->in_int64 = true;
        version->whole = f;
        version->number = f;
        return ecuio_json::write(root);
    }

    /* A check refused is a file refused. A new signal's refusal is the
     * check's: the same words, and for the expression's own the same
     * character. */
    void refused(const std::string& words, uint32_t at)
    {
        ecuio_computed_t program{};
        std::string detail;
        /* At the newest grammar, format 2's, and never past the file's
         * own format otherwise: format 3 reads names and addresses anew */
        const std::string with = at_format(
            std::max<uint32_t>(static_cast<uint32_t>(version->whole), 2u));
        if (with.size() > ECUIO_SIGNAL_FILE_MAX_BYTES) {
            return;
        }
        check(!loads(with, key, &program, &detail));
        if (detail == "out of memory" || edited != nullptr ||
            words.find(" replaces ") != std::string::npos) {
            return;
        }
        const size_t colon = detail.find(": ");
        check(colon != std::string::npos);
        const std::string said = detail.substr(colon + 2);
        const std::string own = "the expression ";
        if (words.compare(0, own.size(), own) == 0 &&
            said.compare(0, 6, "expr '") == 0) {
            const std::string tail = " " + words.substr(own.size()) +
                                     " (at character " + std::to_string(at) +
                                     ")";
            check(said.size() > tail.size() &&
                  said.compare(said.size() - tail.size(), tail.size(), tail) ==
                      0);
        } else {
            check(said == words);
        }
    }

    /* A check that passes names the format the file loads at, and no
     * older one, and its program is the loaded signal's. format is the
     * file's own. */
    void passed(const ecuio_expr_check_t& out, uint32_t format)
    {
        ecuio_computed_t program{};
        std::string detail;
        check(out.format >= ECUIO_SIGNAL_FILE_FORMAT_OLDEST &&
              out.format <= 2u);
        check(out.depth >= 1 && out.depth <= ECUIO_COMPUTED_MAX_DEPTH);
        check(ecuio_computed_check(out.program));
        const std::string with = at_format(std::max(format, out.format));
        if (with.size() > ECUIO_SIGNAL_FILE_MAX_BYTES) {
            return;
        }
        if (!loads(with, key, &program, &detail)) {
            check(detail == "out of memory");
            return;
        }
        check(memcmp(&program, &out.program, sizeof program) == 0);
        if (out.format > format) {
            const std::string older = at_format(format);
            check(!loads(older, key, &program, &detail));
        }
    }
};

/* An expression checked against the file, and the file loaded with the
 * expression in it (ADR-0062): they must agree. The file's "x-expr" (a
 * tool's own key, which the loader passes by) is the expression, and
 * "x-key" the signal it is for: one of the file's keys puts it in that
 * signal's place, anything else makes it a new signal's. A check that
 * passes names the format the file loads at, and no older one, and its
 * program is the loaded signal's; one that refuses is a file refused,
 * for a new signal in the check's own words. */
void check_expr_against_load(const ecuio_signal_file_t* a, const char* bytes,
                             size_t size)
{
    expr_case c;
    if (!c.read(bytes, size) || !c.place(a)) {
        return;
    }
    ecuio_expr_check_t out{};
    out.struct_size = sizeof out;
    const ecuio_status_t st = ecuio_signal_file_check_expr(
        a, c.key.c_str(), c.expr.data(), c.expr.size(), &out);
    const std::string words = ecuio_last_error_detail();
    if (st == ECUIO_ERR_OUT_OF_MEMORY) {
        return;
    }
    check(st == ECUIO_OK || st == ECUIO_ERR_INVALID_ARGUMENT);
    check(out.struct_size == sizeof out && out.flags == 0);
    check((st == ECUIO_OK) == (out.at == 0));
    check((st == ECUIO_OK) == words.empty());
    check(words.find('\n') == std::string::npos);
    if (st != ECUIO_OK) {
        c.refused(words, out.at);
    } else {
        c.passed(out, ecuio_signal_file_format(a));
    }
}

/* Its text is the bytes it was read from, and its source has a name,
 * every text ended within its room, and no flag but share-alike, or
 * for a set with parts the merged flag alone */
void check_text_and_source(const ecuio_signal_file_t* a, const char* bytes,
                           size_t size)
{
    size_t text_length = 0;
    const char* written = ecuio_signal_file_text(a, &text_length);
    check(text_length == size && memcmp(written, bytes, size) == 0 &&
          written[size] == '\0');
    ecuio_signal_source_t source{};
    source.struct_size = sizeof source;
    check(ecuio_signal_file_source(a, &source) == ECUIO_OK);
    const bool merged = ecuio_signal_file_part_count(a) > 0;
    check(source.name[0] != '\0' &&
          source.flags == (merged ? ECUIO_SOURCE_MERGED
                                  : source.flags & ECUIO_SOURCE_SHARE_ALIKE));
    for (const char* t : {source.name, source.url, source.commit,
                          source.license, source.modified}) {
        check(memchr(t, '\0', ECUIO_SOURCE_TEXT_LEN) != nullptr);
    }
}

/* A format 1 file compiles format 1's steps alone and reads no
 * computed input (ADR-0062), and each computed def of the file's
 * own keeps its expression (a base set's, obd2's Boost, has none) */
void check_format(const ecuio_signal_file_t* a, const ecuio_signal_set_t* set)
{
    const uint32_t format = ecuio_signal_file_format(a);
    check(format >= ECUIO_SIGNAL_FILE_FORMAT_OLDEST &&
          format <= ECUIO_SIGNAL_FILE_FORMAT);
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0) {
            check(ecuio_signal_file_expr(a, &d)[0] == '\0');
            continue;
        }
        check((ecuio_signal_file_expr(a, &d)[0] != '\0') ==
              (ecuio_signal_file_key(a, &d)[0] != '\0'));
        const ecuio_computed_t& p = set->computed[d.table_first];
        for (uint32_t s = 0; format == 1 && s < p.step_count; ++s) {
            check(p.steps[s] <= ECUIO_COMPUTED_DIVIDE);
        }
        for (uint32_t k = 0; format == 1 && k < p.input_count; ++k) {
            for (uint32_t j = 0; j < set->count; ++j) {
                check(set->defs[j].id != p.input_ids[k] ||
                      (set->defs[j].flags & ECUIO_DEF_FLAG_COMPUTED) == 0);
            }
        }
    }
}

/* Every def has the name an expression reads it by, or is one no
 * expression names: a base set's def that is no Service 01 field, or
 * a signal whose key starts with a digit */
void check_expr_refs(const ecuio_signal_file_t* a,
                     const ecuio_signal_set_t* set)
{
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        char name[ECUIO_EXPR_REF_LEN];
        const ecuio_status_t named =
            ecuio_signal_file_expr_ref(a, &d, name, sizeof name);
        const char* key = ecuio_signal_file_key(a, &d);
        /* A key that starts with a digit is a number to an expression */
        const bool keyed = key[0] != '\0' && (key[0] < '0' || key[0] > '9');
        if (named != ECUIO_OK) {
            check(named == ECUIO_ERR_INVALID_ARGUMENT && !keyed &&
                  name[0] == '\0');
            continue;
        }
        check(memchr(name, '\0', sizeof name) != nullptr);
        check(keyed ? strcmp(name, key) == 0 : strncmp(name, "obd2.", 5) == 0);
        if (i < kRefEnds || i + kRefEnds >= set->count) {
            check_ref_reads(a, d, name);
        }
    }
}

/* The profile name is one the registry takes, and the set registers */
void check_registers(const ecuio_signal_file_t* a,
                     const ecuio_signal_set_t* set)
{
    const char* profile = ecuio_signal_file_profile(a);
    const char* kind =
        ecuio_signal_file_part_count(a) > 0 ? "merged." : "user.";
    check(strncmp(profile, kind, strlen(kind)) == 0);
    check(strlen(profile) <= ECUIO_PROFILE_NAME_MAX);
    check(ecuio_profile_register(profile, set, nullptr) == ECUIO_OK);
    check(ecuio_profile_unregister(profile) == ECUIO_OK);
}

/* The file merged alone (ADR-0065 section 9): refused in the merge's own
 * words, a merged set always, or one part's set, a format 3 file's its
 * own tables */
void check_merged_alone(const ecuio_signal_file_t* a,
                        const ecuio_signal_set_t* set)
{
    static const std::string own_fault = "the merged set doesn't read back";
    const ecuio_signal_file_t* parts[] = {a};
    ecuio_signal_file_t* m = reinterpret_cast<ecuio_signal_file_t*>(1);
    const ecuio_status_t s = ecuio_signal_file_merge(parts, 1, nullptr, &m);
    const std::string words = ecuio_last_error_detail();
    if (s != ECUIO_OK) {
        check(m == nullptr);
        check(s == ECUIO_ERR_OUT_OF_MEMORY ||
              (s == ECUIO_ERR_INVALID_ARGUMENT && !words.empty()));
        check(words.compare(0, own_fault.size(), own_fault) != 0);
        return;
    }
    check(m != nullptr && ecuio_signal_file_part_count(a) == 0);
    check(ecuio_signal_file_part_count(m) == 1);
    if (ecuio_signal_file_format(a) == 3) {
        check(same_tables(ecuio_signal_file_set(m), set));
    }
    ecuio_signal_file_free(m);
}

/* Each def reads a payload as long as its field needs, through the
 * decoder its kind takes, without a fault. payload keeps each def's
 * key. */
void check_each_def_decodes(const ecuio_signal_file_t* a,
                            const ecuio_signal_set_t* set, uint8_t* payload)
{
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        check_gauge(ecuio_signal_file_gauge(a, &d), d);
        for (uint32_t k = 0; k < d.mux_length; ++k) {
            payload[d.mux_offset + k] =
                static_cast<uint8_t>(d.mux_key >> (8u * (d.mux_length - 1u - k)));
        }
        double v = 0.0;
        char text[ECUIO_DECODE_TEXT_MAX];
        if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
            (void)ecuio_decode_text(&d, payload, ECUIO_FRAME_DATA_MAX, text,
                                    sizeof text);
        } else if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0) {
            check(ecuio_decode_signal(&d, payload, ECUIO_FRAME_DATA_MAX, &v) ==
                  ECUIO_DECODE_OK);
            if (d.kind == ECUIO_VALUE_ENUM) {
                (void)ecuio_enum_label(set, &d, static_cast<uint32_t>(v));
            }
        }
    }
}

/* What an evaluation is given: every number def seen so far, its
 * newest value and its age at this frame */
uint32_t evaluation_inputs(const ecuio_signal_set_t* set, const chain_ref& ref,
                           ecuio_eval_input_t* given)
{
    uint32_t givens = 0;
    for (uint32_t g = 0; g < set->count; ++g) {
        if ((set->defs[g].flags & ECUIO_DEF_FLAG_COMPUTED) == 0 &&
            ref.leaf_seen[g]) {
            given[givens] = ecuio_eval_input_t{};
            given[givens].def_id = set->defs[g].id;
            given[givens].value = ref.leaf_value[g];
            given[givens].age_us = ref.now - ref.leaf_us[g];
            ++givens;
        }
    }
    return givens;
}

/* The computed def at c worked out apart from any session
 * (ecuio_signal_file_evaluate): the reference's value to the bit, or
 * none where it has none */
void check_evaluated(const ecuio_signal_file_t* a,
                     const ecuio_signal_set_t* set, uint32_t c,
                     const ecuio_eval_input_t* given, uint32_t givens,
                     double want)
{
    ecuio_eval_result_t worked{};
    worked.struct_size = sizeof worked;
    check(ecuio_signal_file_evaluate(a,
                                     &set->computed[set->defs[c].table_first],
                                     given, givens, &worked) == ECUIO_OK);
    const bool has = (worked.flags & ECUIO_EVAL_VALUE) != 0;
    check(has == !std::isnan(want));
    check(has ? memcmp(&worked.value, &want, sizeof worked.value) == 0
              : worked.why != ECUIO_EVAL_WHY_NONE);
}

/* Frames carrying the keys of the first defs and of the last (a file
 * extending obd2 lists the base's first and its own after), a
 * millisecond apart, through two decoders: every value finite, each
 * def once, the frame's numbers first, the two alike, and the
 * computed values the reference expects, in the set's order */
void check_frames(const ecuio_signal_file_t* a, const ecuio_signal_set_t* set,
                  const uint8_t* payload)
{
    ecuio_frame_decoder da;
    ecuio_frame_decoder db;
    da.prepare(set, false);
    db.prepare(set, false);
    static chain_ref ref;
    check(set->count <= kRefDefs);
    ref.reset(set);
    static ecuio_frame_t frame;
    const uint32_t ends = 64;
    for (uint32_t i = 0; i < set->count; ++i) {
        if (i == ends && set->count > 2 * ends) {
            i = set->count - ends;
        }
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.flags & ECUIO_DEF_FLAG_COMPUTED) {
            continue;
        }
        frame = ecuio_frame_t{};
        frame.arbitration_id = d.message_id;
        frame.timestamp_us = uint64_t{i} * 1000u;
        frame.data_length = ECUIO_FRAME_DATA_MAX;
        memcpy(frame.data, payload, ECUIO_FRAME_DATA_MAX);
        for (uint32_t k = 0; k < d.mux_length; ++k) {
            frame.data[d.mux_offset + k] = static_cast<uint8_t>(
                d.mux_key >> (8u * (d.mux_length - 1u - k)));
        }
        const uint32_t n = da.decode(&frame);
        check(db.decode(&frame) == n && n <= set->count);
        ref.frame(frame.timestamp_us);
        static uint8_t seen[kRefDefs];
        memset(seen, 0, sizeof seen);
        uint32_t v = 0;
        for (; v < n; ++v) {
            const ecuio_value_t& x = da.values[v];
            check(std::isfinite(x.value) &&
                  x.signal_id == db.values[v].signal_id &&
                  memcmp(&x.value, &db.values[v].value, sizeof x.value) == 0);
            const uint32_t at = ref.index_of(x.signal_id);
            check(at < set->count && seen[at] == 0);
            seen[at] = 1;
            if (set->defs[at].flags & ECUIO_DEF_FLAG_COMPUTED) {
                break;
            }
            ref.take(at, x.value);
        }
        static ecuio_eval_input_t given[kRefDefs];
        const uint32_t givens = evaluation_inputs(set, ref, given);
        for (uint32_t c = 0; c < set->count; ++c) {
            if ((set->defs[c].flags & ECUIO_DEF_FLAG_COMPUTED) == 0) {
                continue;
            }
            ref.walk(c);
            if (ref.fed[c]) {
                check_evaluated(a, set, c, given, givens, ref.value[c]);
            }
            if (!ref.fed[c] || std::isnan(ref.value[c])) {
                continue;
            }
            check(v < n && da.values[v].signal_id == set->defs[c].id &&
                  memcmp(&da.values[v].value, &ref.value[c],
                         sizeof ref.value[c]) == 0);
            ++v;
        }
        check(v == n);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const char* bytes = reinterpret_cast<const char*>(data);
    ecuio_signal_file_t* a = nullptr;
    const ecuio_status_t sa = ecuio_signal_file_load(bytes, size, &a);
    const std::string detail_a = ecuio_last_error_detail();
    ecuio_signal_file_t* b = nullptr;
    const ecuio_status_t sb = ecuio_signal_file_load(bytes, size, &b);
    check(sa == sb);
    check(detail_a == ecuio_last_error_detail());

    if (sa != ECUIO_OK) {
        check(a == nullptr && b == nullptr);
        check(sa == ECUIO_ERR_INVALID_ARGUMENT || sa == ECUIO_ERR_OUT_OF_MEMORY);
        check(!detail_a.empty());
        check(detail_a.find('\n') == std::string::npos);
        check(detail_a.find('\r') == std::string::npos);
        return 0;
    }
    check(a != nullptr && b != nullptr);

    const ecuio_signal_set_t* set = ecuio_signal_file_set(a);
    ecuio_validation_t why{};
    check(ecuio_validate_signal_set(set, &why) == ECUIO_VALIDATE_OK);
    check(set->count == ecuio_signal_file_set(b)->count);
    check(strcmp(ecuio_signal_file_profile(a), ecuio_signal_file_profile(b)) == 0);

    check_text_and_source(a, bytes, size);
    check_format(a, set);
    check_expr_against_load(a, bytes, size);
    check_expr_refs(a, set);
    check_registers(a, set);
    check_merged_alone(a, set);
    uint8_t payload[ECUIO_FRAME_DATA_MAX] = {};
    check_each_def_decodes(a, set, payload);
    check_frames(a, set, payload);

    ecuio_signal_file_free(a);
    ecuio_signal_file_free(b);
    return 0;
}
