// File/layer: fuzz/fuzz_condition_file.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_condition_file

#include <ecuio/conditions.h>
#include <ecuio/core.h>
#include <ecuio/profile.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>

#include "condition_eval.h" /* core/src: the evaluator a session judges by */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

/* A buffer holds text terminated inside it, zero after the NUL */
bool text_in(const char* buf, size_t cap)
{
    const void* nul = memchr(buf, 0, cap);
    if (nul == nullptr) {
        return false;
    }
    for (const char* p = static_cast<const char*>(nul); p < buf + cap; ++p) {
        if (*p != 0) {
            return false;
        }
    }
    return true;
}

bool key_text(const char* k, size_t max)
{
    const size_t n = strlen(k);
    if (n == 0 || n > max) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const char c = k[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

/* What any term the loader keeps must be */
void check_term(const ecuio_condition_term_t& t)
{
    check(t.reserved0 == 0 && t.reserved[0] == 0 && t.reserved[1] == 0);
    check(text_in(t.unit, sizeof t.unit));
    check(std::isfinite(t.level) && std::isfinite(t.clear));
    for (uint32_t k = t.reference_count; k < ECUIO_CONDITION_MAX_REFERENCES;
         ++k) {
        const ecuio_condition_reference_t zero{};
        check(memcmp(&t.references[k], &zero, sizeof zero) == 0);
    }
    if (t.kind != ECUIO_TERM_SIGNAL) {
        check(t.kind == ECUIO_TERM_LINK || t.kind == ECUIO_TERM_CHECK);
        check(t.kind == ECUIO_TERM_LINK
                  ? t.test >= ECUIO_TERM_LINK_NOT_OK &&
                        t.test <= ECUIO_TERM_LINK_HARD_FAILURE
                  : t.test >= ECUIO_CHECK_GAP && t.test <= ECUIO_CHECK_ISOTP);
        check(t.level == 0.0 && t.clear == 0.0 && t.max_age_us == 0 &&
              t.flags == 0 && t.reference_count == 0 && t.unit[0] == '\0');
        return;
    }
    check(t.test >= ECUIO_TERM_ABOVE && t.test <= ECUIO_TERM_DIFFERS);
    check((t.flags & ~(ECUIO_TERM_HAS_CLEAR | ECUIO_TERM_HAS_UNIT)) == 0);
    const bool ordered =
        t.test == ECUIO_TERM_ABOVE || t.test == ECUIO_TERM_BELOW;
    const bool has_unit = (t.flags & ECUIO_TERM_HAS_UNIT) != 0;
    check(has_unit || (!ordered && t.unit[0] == '\0'));
    if ((t.flags & ECUIO_TERM_HAS_CLEAR) != 0) {
        check(ordered);
        check(t.test == ECUIO_TERM_ABOVE ? t.clear <= t.level
                                         : t.clear >= t.level);
    } else {
        check(t.clear == 0.0);
    }
    check(t.max_age_us == 0 || (t.max_age_us >= ECUIO_COMPUTED_MIN_AGE_US &&
                                t.max_age_us <= ECUIO_COMPUTED_MAX_AGE_US));
    check(t.reference_count >= 1 &&
          t.reference_count <= ECUIO_CONDITION_MAX_REFERENCES);
    for (uint32_t k = 0; k < t.reference_count; ++k) {
        const ecuio_condition_reference_t& r = t.references[k];
        check(text_in(r.text, sizeof r.text) && r.text[0] != '\0');
        if (r.kind == ECUIO_REFERENCE_KEY) {
            check(r.id == 0 && strncmp(r.text, "key:", 4) == 0 &&
                  key_text(r.text + 4, 31));
        } else {
            check(r.kind == ECUIO_REFERENCE_ID && r.id <= UINT16_MAX);
            check(strncmp(r.text, "id:", 3) == 0 ||
                  strncmp(r.text, "obd2.", 5) == 0);
        }
        for (uint32_t j = 0; j < k; ++j) {
            const ecuio_condition_reference_t& o = t.references[j];
            check(o.kind != r.kind ||
                  (r.kind == ECUIO_REFERENCE_ID ? o.id != r.id
                                                : strcmp(o.text, r.text) != 0));
        }
    }
}

/* How a condition binds to a set: every answer within the contract, and
 * a term that names a def names one the set holds */
void check_binding(const ecuio_condition_set_t* set, uint32_t i,
                   const ecuio_condition_info_t& info,
                   const ecuio_signal_set_t* signals,
                   const ecuio_signal_file_t* file)
{
    ecuio_condition_binding_t b;
    memset(&b, 0xA5, sizeof b);
    check(ecuio_condition_set_resolve(set, i, signals, file, &b) == ECUIO_OK);
    check(b.term_count == info.term_count && b.reserved0 == 0 &&
          b.reserved[0] == 0 && b.reserved[1] == 0);
    check(b.why <= ECUIO_CONDITION_WHY_ENUM_ORDER);
    const bool disabled = (info.flags & ECUIO_CONDITION_INFO_DISABLED) != 0;
    check(disabled == (b.why == ECUIO_CONDITION_WHY_DISABLED));
    check((b.why == ECUIO_CONDITION_WHY_NONE || disabled)
              ? b.why_term == UINT32_MAX
              : b.why_term < info.term_count);
    for (uint32_t k = 0; k < ECUIO_CONDITION_MAX_TERMS; ++k) {
        check((b.reference[k] == UINT32_MAX) == (b.signal_id[k] == UINT32_MAX));
        if (b.signal_id[k] == UINT32_MAX) {
            continue;
        }
        check(k < info.term_count && signals != nullptr);
        ecuio_condition_term_t t;
        check(ecuio_condition_set_get_term(set, i, k, &t) == ECUIO_OK);
        check(t.kind == ECUIO_TERM_SIGNAL &&
              b.reference[k] < t.reference_count);
        /* The reference taken names the def, and none before it names
         * one */
        const auto with_id = [&](uint32_t id) {
            const ecuio_signal_def_t* found = nullptr;
            for (uint32_t d = 0; d < signals->count; ++d) {
                if (signals->defs[d].id == id) {
                    found = &signals->defs[d];
                }
            }
            return found;
        };
        const auto named = [&](const ecuio_condition_reference_t& r) {
            if (r.kind == ECUIO_REFERENCE_KEY) {
                return ecuio_signal_file_find(file, r.text + 4);
            }
            return r.id < ECUIO_UDS_GENERIC_SIG_ID_BASE ? with_id(r.id)
                                                        : nullptr;
        };
        const ecuio_signal_def_t* def = with_id(b.signal_id[k]);
        check(def != nullptr);
        check(named(t.references[b.reference[k]]) == def);
        for (uint32_t j = 0; j < b.reference[k]; ++j) {
            check(named(t.references[j]) == nullptr);
        }
        if (b.why == ECUIO_CONDITION_WHY_NONE) {
            check((def->flags & ECUIO_DEF_FLAG_COMPUTED) != 0 ||
                  (def->kind != ECUIO_VALUE_TEXT &&
                   def->kind != ECUIO_VALUE_HEX));
            check(strncmp(t.unit, def->unit, sizeof def->unit) == 0);
        }
    }
}

/* A run of made-up frames through the evaluator, as a session's worker
 * would feed it (ADR-0063 sections 2 and 3). The frames come from the
 * input's bytes by a generator, so a file gives one run: times that
 * stand still, creep and jump past every hold and age bound, and
 * samples on and around each term's level and clear, now and then one
 * that is no number or an infinity, and now and then a frame stamped a
 * little before the time reached. Three evaluators take them: two the
 * same frames, which must agree event for event; the third also a frame
 * with no values halfway to each one, which must change nothing, since
 * a deadline is settled at its own time by whichever frame comes next.
 * Each run is also held, frame for frame, to the reference below. */
struct made_up {
    uint64_t state;
    uint32_t next()
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<uint32_t>(state >> 16);
    }
};

/* A run's events in a form the frames' grouping can't change: a
 * condition's changes at one time as one event, as one call gives them
 * (two calls give two), counted and summed in no order across
 * conditions */
struct canon {
    ecuio_condition_event_t last[ECUIO_CONDITION_MAX_CONDITIONS] = {};
    bool held[ECUIO_CONDITION_MAX_CONDITIONS] = {};
    uint64_t count = 0;
    uint64_t sum = 0;

    void flush(uint32_t c)
    {
        const ecuio_condition_event_t& ev = last[c];
        uint64_t v = 0;
        memcpy(&v, &ev.value, sizeof v);
        uint64_t h = ev.timestamp_us * 1099511628211ull;
        h ^= (uint64_t{ev.condition} << 48) ^ (uint64_t{ev.state} << 40) ^
             (uint64_t{ev.flags} << 44) ^ (uint64_t{ev.term} << 36) ^ v;
        sum += h * 1099511628211ull + (h >> 29);
        ++count;
    }
    void take(const ecuio_condition_event_t& ev)
    {
        const uint32_t c = ev.condition;
        if (held[c] && last[c].timestamp_us == ev.timestamp_us) {
            const uint32_t fired = last[c].flags & ECUIO_CONDITION_FIRED;
            last[c] = ev;
            last[c].flags |= fired;
            return;
        }
        if (held[c]) {
            flush(c);
        }
        last[c] = ev;
        held[c] = true;
    }
    void finish()
    {
        for (uint32_t c = 0; c < ECUIO_CONDITION_MAX_CONDITIONS; ++c) {
            if (held[c]) {
                flush(c);
            }
        }
    }
};

void check_events(const ecuio_condition_eval& e, uint32_t n, uint64_t now_us)
{
    check(e.dropped == 0 && n <= e.events.size());
    uint32_t per[ECUIO_CONDITION_MAX_CONDITIONS] = {};
    for (uint32_t i = 0; i < n; ++i) {
        const ecuio_condition_event_t& ev = e.events[i];
        check(ev.condition < e.conditions.size());
        const ecuio_condition_eval::condition& c = e.conditions[ev.condition];
        check(c.available != 0 && ev.term < c.term_count);
        check(++per[ev.condition] <= ecuio_condition_eval::kEventsPerCondition);
        check(ev.state == ECUIO_CONDITION_OFF ||
              ev.state == ECUIO_CONDITION_ON);
        check((ev.flags & ~(ECUIO_CONDITION_STALE | ECUIO_CONDITION_FIRED)) ==
              0);
        check((ev.flags & ECUIO_CONDITION_FIRED) == 0 ||
              ev.state == ECUIO_CONDITION_ON);
        check(ev.timestamp_us <= now_us && !std::isnan(ev.value));
        check(ev.generation == e.generation && ev.arbitration_id == 0 &&
              ev.code == 0 && ev.reserved0 == 0 && ev.reserved == 0);
        /* By time, then by index, each (condition, time) once */
        if (i > 0) {
            const ecuio_condition_event_t& b = e.events[i - 1];
            check(b.timestamp_us < ev.timestamp_us ||
                  (b.timestamp_us == ev.timestamp_us &&
                   b.condition < ev.condition));
        }
    }
    /* A condition's state is its newest event's */
    for (uint32_t i = n; i-- > 0;) {
        const ecuio_condition_event_t& ev = e.events[i];
        bool newest = true;
        for (uint32_t j = i + 1; j < n; ++j) {
            newest = newest && e.events[j].condition != ev.condition;
        }
        if (newest) {
            const ecuio_condition_state_t s = e.state_of(ev.condition);
            check(s.state == ev.state && s.since_us == ev.timestamp_us &&
                  s.flags == (ev.flags & ECUIO_CONDITION_STALE) &&
                  memcmp(&s.value, &ev.value, sizeof s.value) == 0);
        }
    }
}

/* Section 2's rules once more, written plainly, to hold the evaluator
 * to: every term of every condition is read at every frame and every
 * deadline is looked for from scratch, with no index, no list of what
 * changed and nothing kept between frames but the samples and the
 * states. It is built from the set's public getters alone. */
struct reference {
    struct term {
        uint32_t test = 0;
        double level = 0.0;
        double clear = 0.0;
        bool has_clear = false;
        uint64_t max_age_us = 0;
        uint32_t signal_id = 0;
        bool judged = false; /* sampled, and within its age bound */
        bool truth = false;
        double value = 0.0;
        uint64_t time_us = 0;
    };
    struct condition {
        bool available = false;
        bool any = false;
        uint64_t for_us = 0;
        uint64_t clear_for_us = 0;
        uint32_t term_count = 0;
        term terms[ECUIO_CONDITION_MAX_TERMS];
        bool on = false;
        bool stale = true;
        /* What its terms have said, unbroken, since toward_since_us: 0
         * what the state is, 1 ON, 2 OFF */
        int toward = 0;
        uint64_t toward_since_us = 0;
        uint32_t toward_terms = 0;
        uint64_t fired = 0;
        uint64_t since_us = 0;
        double value = 0.0;
    };
    struct event {
        uint64_t time_us = 0;
        uint32_t condition = 0;
        bool on = false;
        bool stale = false;
        bool fired = false;
        /* The terms that may have settled it, as bits: one sample that
         * changes two terms on its signal names either, and nothing a
         * caller sees tells them apart */
        uint32_t terms = 0;
        double value = 0.0;
    };
    std::vector<condition> conditions;
    std::vector<event> events; /* the last frame's */
    uint64_t reached_us = 0;   /* time never goes back */

    reference(const ecuio_condition_set_t* set,
              const ecuio_signal_set_t* signals,
              const ecuio_signal_file_t* file)
    {
        conditions.resize(ecuio_condition_set_count(set));
        for (uint32_t i = 0; i < conditions.size(); ++i) {
            condition& c = conditions[i];
            ecuio_condition_info_t info;
            ecuio_condition_binding_t bound;
            check(ecuio_condition_set_get(set, i, &info) == ECUIO_OK);
            check(ecuio_condition_set_resolve(set, i, signals, file, &bound) ==
                  ECUIO_OK);
            c.available = bound.why == ECUIO_CONDITION_WHY_NONE;
            c.any = (info.flags & ECUIO_CONDITION_INFO_ANY) != 0;
            c.for_us = info.for_us;
            c.clear_for_us = info.clear_for_us;
            c.term_count = info.term_count;
            for (uint32_t k = 0; k < info.term_count; ++k) {
                ecuio_condition_term_t t;
                check(ecuio_condition_set_get_term(set, i, k, &t) == ECUIO_OK);
                /* A link or a check term is not judged in a session */
                c.available = c.available && t.kind == ECUIO_TERM_SIGNAL;
                c.terms[k].test = t.test;
                c.terms[k].level = t.level;
                c.terms[k].clear = t.clear;
                c.terms[k].has_clear = (t.flags & ECUIO_TERM_HAS_CLEAR) != 0;
                c.terms[k].max_age_us =
                    t.max_age_us != 0 ? t.max_age_us : ECUIO_VALUE_STALE_US;
                c.terms[k].signal_id = bound.signal_id[k];
            }
        }
    }

    void tell(uint32_t index, uint64_t at_us, uint32_t terms, bool fired)
    {
        condition& c = conditions[index];
        uint32_t first = 0;
        while ((terms >> first & 1u) == 0) {
            ++first;
        }
        c.since_us = at_us;
        c.value = c.terms[first].value;
        if (events.empty() || events.back().condition != index ||
            events.back().time_us != at_us) {
            events.emplace_back();
            events.back().time_us = at_us;
            events.back().condition = index;
        }
        event& e = events.back();
        e.on = c.on;
        e.stale = c.stale;
        e.fired = e.fired || fired;
        e.terms = terms;
        e.value = c.value;
    }

    /* The condition's terms taken together at at_us, after `terms`
     * changed */
    void weigh(uint32_t index, uint64_t at_us, uint32_t terms)
    {
        condition& c = conditions[index];
        uint32_t yes = 0;
        uint32_t no = 0;
        uint32_t unknown = 0;
        for (uint32_t k = 0; k < c.term_count; ++k) {
            ++(c.terms[k].judged ? (c.terms[k].truth ? yes : no) : unknown);
        }
        const bool settled = c.any ? yes > 0 : no > 0;
        if (!settled && unknown > 0) {
            c.toward = 0;
            if (!c.stale) {
                c.stale = true;
                tell(index, at_us, terms, false);
            }
            return;
        }
        const bool holds = c.any ? yes > 0 : no == 0;
        bool changed = c.stale;
        bool fired = false;
        c.stale = false;
        if (holds == c.on) {
            c.toward = 0;
        } else {
            const int toward = holds ? 1 : 2;
            if (c.toward != toward) {
                c.toward = toward;
                c.toward_since_us = at_us;
                c.toward_terms = terms;
            }
            if ((holds ? c.for_us : c.clear_for_us) == 0) {
                c.toward = 0;
                c.on = holds;
                c.fired += holds ? 1 : 0;
                fired = holds;
                changed = true;
            }
        }
        if (changed) {
            tell(index, at_us, terms, fired);
        }
    }

    void turn(uint32_t index, uint64_t at_us)
    {
        condition& c = conditions[index];
        c.on = c.toward == 1;
        c.fired += c.on ? 1 : 0;
        c.toward = 0;
        tell(index, at_us, c.toward_terms, c.on);
    }

    void judge(uint64_t now_us, const ecuio_value_t* values, uint32_t count)
    {
        events.clear();
        if (now_us < reached_us) {
            now_us = reached_us;
        }
        reached_us = now_us;
        for (uint32_t index = 0; index < conditions.size(); ++index) {
            condition& c = conditions[index];
            if (!c.available) {
                continue;
            }
            /* What came due before this frame, earliest first, a hold's
             * end before an age bound at the same time */
            for (;;) {
                const uint64_t end =
                    c.toward == 0
                        ? UINT64_MAX
                        : c.toward_since_us +
                              (c.toward == 1 ? c.for_us : c.clear_for_us);
                uint64_t oldest = UINT64_MAX;
                uint32_t who = 0;
                for (uint32_t k = 0; k < c.term_count; ++k) {
                    const term& t = c.terms[k];
                    if (t.judged && t.time_us + t.max_age_us < oldest) {
                        oldest = t.time_us + t.max_age_us;
                        who = k;
                    }
                }
                if (end < now_us && end <= oldest) {
                    turn(index, end);
                } else if (oldest < now_us) {
                    c.terms[who].judged = false;
                    weigh(index, oldest, 1u << who);
                } else {
                    break;
                }
            }
            /* The frame's values, each read by every term on its signal */
            uint32_t settled = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const ecuio_value_t& v = values[i];
                if (v.kind == ECUIO_VALUE_TEXT || v.kind == ECUIO_VALUE_HEX ||
                    std::isnan(v.value)) {
                    continue;
                }
                uint32_t changed = 0;
                for (uint32_t k = 0; k < c.term_count; ++k) {
                    term& t = c.terms[k];
                    if (t.signal_id != v.signal_id) {
                        continue;
                    }
                    bool truth = t.truth;
                    if (t.test == ECUIO_TERM_ABOVE) {
                        truth = v.value > t.level ||
                                (truth && t.has_clear && v.value > t.clear);
                    } else if (t.test == ECUIO_TERM_BELOW) {
                        truth = v.value < t.level ||
                                (truth && t.has_clear && v.value < t.clear);
                    } else {
                        truth = (v.value == t.level) ==
                                (t.test == ECUIO_TERM_EQUALS);
                    }
                    if (truth != t.truth || !t.judged) {
                        changed |= 1u << k;
                    }
                    t.truth = truth;
                    t.judged = true;
                    t.value = v.value;
                    t.time_us = now_us;
                }
                if (changed != 0) {
                    settled = changed;
                }
            }
            if (settled != 0) {
                weigh(index, now_us, settled);
            }
            /* A hold that ends exactly at this frame, if its values
             * didn't break it */
            if (c.toward != 0 &&
                c.toward_since_us +
                        (c.toward == 1 ? c.for_us : c.clear_for_us) ==
                    now_us) {
                turn(index, now_us);
            }
        }
        std::stable_sort(
            events.begin(), events.end(), [](const event& a, const event& b) {
                return a.time_us != b.time_us ? a.time_us < b.time_us
                                              : a.condition < b.condition;
            });
    }

    /* The evaluator's last frame gave these events and stands in these
     * states */
    void agrees(const ecuio_condition_eval& e, uint32_t n) const
    {
        check(n == events.size());
        for (uint32_t i = 0; i < n; ++i) {
            const ecuio_condition_event_t& got = e.events[i];
            const event& want = events[i];
            check(got.timestamp_us == want.time_us &&
                  got.condition == want.condition);
            check((got.state == ECUIO_CONDITION_ON) == want.on);
            check(((got.flags & ECUIO_CONDITION_STALE) != 0) == want.stale &&
                  ((got.flags & ECUIO_CONDITION_FIRED) != 0) == want.fired);
            check(got.term < ECUIO_CONDITION_MAX_TERMS &&
                  (want.terms >> got.term & 1u) != 0);
            check(memcmp(&got.value, &want.value, sizeof got.value) == 0);
        }
        check(e.conditions.size() == conditions.size());
        for (uint32_t i = 0; i < conditions.size(); ++i) {
            const condition& c = conditions[i];
            const ecuio_condition_state_t s = e.state_of(i);
            check((e.conditions[i].available != 0) == c.available);
            check((s.state == ECUIO_CONDITION_ON) == c.on &&
                  s.fired == c.fired && s.since_us == c.since_us &&
                  memcmp(&s.value, &c.value, sizeof s.value) == 0);
            check(s.flags == (!c.available ? ECUIO_CONDITION_UNAVAILABLE
                              : c.stale    ? ECUIO_CONDITION_STALE
                                           : 0u));
        }
    }
};

void judge_set(const ecuio_condition_set_t* set,
               const ecuio_signal_set_t* signals,
               const ecuio_signal_file_t* file, uint64_t seed)
{
    reference plain(set, signals, file);
    reference plain_dense(set, signals, file);
    std::unique_ptr<ecuio_condition_eval> a =
        ecuio_condition_eval::make(set, signals, file, 1);
    std::unique_ptr<ecuio_condition_eval> b =
        ecuio_condition_eval::make(set, signals, file, 1);
    std::unique_ptr<ecuio_condition_eval> dense =
        ecuio_condition_eval::make(set, signals, file, 1);
    check(a->conditions.size() == ecuio_condition_set_count(set));
    for (uint32_t i = 0; i < a->conditions.size(); ++i) {
        ecuio_condition_binding_t bound;
        check(ecuio_condition_set_resolve(set, i, signals, file, &bound) ==
              ECUIO_OK);
        const ecuio_condition_state_t s = a->state_of(i);
        check(s.state == ECUIO_CONDITION_OFF && s.fired == 0 &&
              s.since_us == 0);
        check((s.flags == ECUIO_CONDITION_STALE) ==
              (a->conditions[i].available != 0));
        check(a->conditions[i].available == 0 ||
              bound.why == ECUIO_CONDITION_WHY_NONE);
    }
    if (a->terms.empty()) {
        return;
    }
    static const uint32_t kSteps[] = {0,       1,       999,     50000,
                                      250000,  1000000, 2999999, 3000000,
                                      3000001, 9000000, 61000000};
    made_up r{seed | 1};
    uint64_t now = 1000000;
    static canon sparse_events;
    static canon dense_events;
    sparse_events = canon{};
    dense_events = canon{};
    for (int step = 0; step < 300; ++step) {
        const uint32_t dt =
            kSteps[r.next() % (sizeof kSteps / sizeof kSteps[0])];
        ecuio_value_t values[4] = {};
        const uint32_t count = r.next() % 4;
        for (uint32_t k = 0; k < count; ++k) {
            const ecuio_condition_eval::term& t =
                a->terms[r.next() % a->terms.size()];
            const double around = (r.next() & 1) != 0 ? t.level : t.clear;
            static const double kOff[] = {-1e6, -1.0, -1e-9, 0.0,
                                          1e-9, 1.0,  1e6};
            double v = around + kOff[r.next() % 7];
            if (!std::isfinite(v)) {
                v = around;
            }
            const uint32_t odd = r.next() % 48;
            if (odd < 3) {
                v = odd == 0   ? std::numeric_limits<double>::quiet_NaN()
                    : odd == 1 ? std::numeric_limits<double>::infinity()
                               : -std::numeric_limits<double>::infinity();
            }
            values[k].signal_id = a->signals[t.signal].id;
            values[k].kind = ECUIO_VALUE_UINT;
            values[k].value = v;
            for (uint32_t j = 0; j < k; ++j) {
                /* A frame carries a def once */
                if (values[j].signal_id == values[k].signal_id) {
                    values[k].signal_id = 0;
                    values[k].kind = ECUIO_VALUE_TEXT;
                }
            }
        }
        /* The frame's stamp: on from the time reached, or now and then
         * a little before it, as two channels' frames can arrive */
        const uint64_t at =
            r.next() % 16 == 0 ? now - (r.next() % 5000 + 1) : now + dt;
        /* The dense run's extra frame, with nothing in it */
        const uint64_t half = at >= now ? now + (at - now) / 2 : at;
        const uint32_t between = dense->judge(half, nullptr, 0);
        check_events(*dense, between, std::max(now, half));
        plain_dense.judge(half, nullptr, 0);
        plain_dense.agrees(*dense, between);
        for (uint32_t i = 0; i < between; ++i) {
            dense_events.take(dense->events[i]);
        }
        now = std::max(now, at);
        const uint32_t n = a->judge(at, values, count);
        check(b->judge(at, values, count) == n);
        check(n == 0 || memcmp(a->events.data(), b->events.data(),
                               n * sizeof(ecuio_condition_event_t)) == 0);
        check_events(*a, n, now);
        plain.judge(at, values, count);
        plain.agrees(*a, n);
        const uint32_t m = dense->judge(at, values, count);
        check_events(*dense, m, now);
        plain_dense.judge(at, values, count);
        plain_dense.agrees(*dense, m);
        for (uint32_t i = 0; i < n; ++i) {
            sparse_events.take(a->events[i]);
        }
        for (uint32_t i = 0; i < m; ++i) {
            dense_events.take(dense->events[i]);
        }
    }
    /* The same events, whichever frames settled them */
    sparse_events.finish();
    dense_events.finish();
    check(sparse_events.count == dense_events.count &&
          sparse_events.sum == dense_events.sum);
    for (uint32_t i = 0; i < a->conditions.size(); ++i) {
        const ecuio_condition_state_t x = a->state_of(i);
        const ecuio_condition_state_t y = dense->state_of(i);
        check(memcmp(&x, &y, sizeof x) == 0);
    }
}

} // namespace

/* The input is a condition file, or one and then, after a 0x1E byte, a
 * signal file to resolve it against */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const char* bytes = reinterpret_cast<const char*>(data);
    const void* cut = memchr(data, 0x1E, size);
    const size_t length =
        cut != nullptr
            ? static_cast<size_t>(static_cast<const uint8_t*>(cut) - data)
            : size;

    ecuio_condition_set_t* a = nullptr;
    const ecuio_status_t sa = ecuio_condition_set_load(bytes, length, &a);
    const std::string detail_a = ecuio_last_error_detail();
    ecuio_condition_set_t* b = nullptr;
    const ecuio_status_t sb = ecuio_condition_set_load(bytes, length, &b);
    check(sa == sb);
    check(detail_a == ecuio_last_error_detail());

    if (sa != ECUIO_OK) {
        check(a == nullptr && b == nullptr);
        check(sa == ECUIO_ERR_INVALID_ARGUMENT ||
              sa == ECUIO_ERR_OUT_OF_MEMORY);
        check(!detail_a.empty());
        check(detail_a.find('\n') == std::string::npos);
        check(detail_a.find('\r') == std::string::npos);
        return 0;
    }
    check(a != nullptr && b != nullptr);
    check(length <= ECUIO_CONDITION_FILE_MAX_BYTES);

    ecuio_signal_file_t* file = nullptr;
    if (cut != nullptr) {
        (void)ecuio_signal_file_load(bytes + length + 1, size - length - 1,
                                     &file);
    }

    const uint32_t count = ecuio_condition_set_count(a);
    check(count <= ECUIO_CONDITION_MAX_CONDITIONS &&
          count == ecuio_condition_set_count(b));
    check(strlen(ecuio_condition_set_name(a)) <= 63);
    check(strcmp(ecuio_condition_set_name(a), ecuio_condition_set_name(b)) ==
          0);

    ecuio_check_options_t checks;
    memset(&checks, 0xA5, sizeof checks);
    check(ecuio_condition_set_checks(a, &checks) == ECUIO_OK);
    check(checks.gap_us <= ECUIO_CONDITION_MAX_CHECK_US &&
          checks.settle_us <= ECUIO_CONDITION_MAX_CHECK_US &&
          checks.flags == 0);
    check(checks.rate_drop_x100 == 0 ||
          (checks.rate_drop_x100 > 100 &&
           checks.rate_drop_x100 <= ECUIO_CONDITION_MAX_RATE_DROP_X100));
    for (uint64_t r : checks.reserved) {
        check(r == 0);
    }

    for (uint32_t i = 0; i < count; ++i) {
        ecuio_condition_info_t info;
        ecuio_condition_info_t other;
        memset(&info, 0xA5, sizeof info);
        check(ecuio_condition_set_get(a, i, &info) == ECUIO_OK);
        check(ecuio_condition_set_get(b, i, &other) == ECUIO_OK);
        check(key_text(info.key, ECUIO_CONDITION_KEY_MAX));
        check(strcmp(info.key, other.key) == 0 &&
              strcmp(info.name, other.name) == 0);
        const size_t name = strlen(info.name);
        check(name >= 1 && name <= ECUIO_CONDITION_NAME_MAX);
        check(strchr("=+-@\t\r", info.name[0]) == nullptr);
        for (uint32_t j = 0; j < i; ++j) {
            check(ecuio_condition_set_get(a, j, &other) == ECUIO_OK);
            check(strcmp(info.key, other.key) != 0 &&
                  strcmp(info.name, other.name) != 0);
        }
        check((info.flags & ~(ECUIO_CONDITION_INFO_DISABLED |
                              ECUIO_CONDITION_INFO_ANY)) == 0);
        const uint32_t alert =
            ECUIO_CONDITION_ALERT_SOUND | ECUIO_CONDITION_ALERT_BANNER;
        check((info.uses &
               ~(ECUIO_CONDITION_USE_ALERT | ECUIO_CONDITION_USE_RECORD |
                 ECUIO_CONDITION_USE_MARK | alert)) == 0);
        const bool alerts = (info.uses & ECUIO_CONDITION_USE_ALERT) != 0;
        check(alerts ? (info.alert_level == ECUIO_ALERT_WARNING ||
                        info.alert_level == ECUIO_ALERT_DANGER)
                     : (info.alert_level == ECUIO_ALERT_NONE &&
                        (info.uses & alert) == 0));
        check(info.term_count >= 1 &&
              info.term_count <= ECUIO_CONDITION_MAX_TERMS);
        check(info.for_us <= ECUIO_CONDITION_MAX_FOR_US &&
              info.clear_for_us <= ECUIO_CONDITION_MAX_FOR_US &&
              info.before_us <= ECUIO_CONDITION_MAX_BEFORE_US &&
              info.after_us <= ECUIO_CONDITION_MAX_AFTER_US);
        check((info.uses & ECUIO_CONDITION_USE_RECORD) != 0 ||
              (info.before_us == 0 && info.after_us == 0));
        check(info.reserved[0] == 0 && info.reserved[1] == 0);

        ecuio_condition_term_t t;
        for (uint32_t k = 0; k < info.term_count; ++k) {
            ecuio_condition_term_t u;
            memset(&t, 0xA5, sizeof t);
            check(ecuio_condition_set_get_term(a, i, k, &t) == ECUIO_OK);
            check(ecuio_condition_set_get_term(b, i, k, &u) == ECUIO_OK);
            check(memcmp(&t, &u, sizeof t) == 0);
            check_term(t);
        }
        check(ecuio_condition_set_get_term(a, i, info.term_count, &t) ==
              ECUIO_ERR_INVALID_ARGUMENT);

        check_binding(a, i, info, nullptr, nullptr);
        check_binding(a, i, info, ecuio_obd2_signal_set(), nullptr);
        check_binding(a, i, info, ecuio_mock_signal_set(), nullptr);
        if (file != nullptr) {
            check_binding(a, i, info, ecuio_signal_file_set(file), file);
            check_binding(a, i, info, ecuio_signal_file_set(file), nullptr);
        }
    }
    ecuio_condition_info_t past;
    check(ecuio_condition_set_get(a, count, &past) ==
          ECUIO_ERR_INVALID_ARGUMENT);

    /* Judged, against obd2's set and the signal file's */
    uint64_t seed = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        seed = (seed ^ data[i]) * 1099511628211ull;
    }
    judge_set(a, ecuio_obd2_signal_set(), nullptr, seed);
    if (file != nullptr) {
        judge_set(a, ecuio_signal_file_set(file), file, seed);
    }

    ecuio_signal_file_free(file);
    ecuio_condition_set_free(a);
    ecuio_condition_set_free(b);
    return 0;
}
