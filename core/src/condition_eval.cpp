// File/layer: core/src/condition_eval.cpp | C++17, Qt-free, every platform
// Testing: UNIT: ConditionEvalTest (test_signals), ConditionSessionTest
//          (test_core); FUZZ: fuzz_condition_file

#include "condition_eval.h"

#include "condition_file.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

using eval = ecuio_condition_eval;

uint32_t slot_of(uint16_t id)
{
    return (uint32_t{id} * 40503u >> 4) & (eval::kHashSlots - 1);
}

/* Whether a link term naming test holds in the link's state */
bool link_holds(uint8_t test, int32_t state)
{
    switch (test) {
    case ECUIO_TERM_LINK_NOT_OK:
        return state != ECUIO_OK;
    case ECUIO_TERM_LINK_TIMEOUT:
        return state == ECUIO_ERR_VEHICLE_TIMEOUT;
    case ECUIO_TERM_LINK_REQUEST_FAILED:
        return state == ECUIO_ERR_REQUEST_FAILED;
    case ECUIO_TERM_LINK_HARD_FAILURE:
        return state == ECUIO_ERR_TRANSPORT_HARD_FAILURE;
    default:
        return false;
    }
}

} // namespace

std::unique_ptr<ecuio_condition_eval> ecuio_condition_eval::make(
    const ecuio_condition_set_t* set, const ecuio_signal_set_t* signal_set,
    const ecuio_signal_file_t* file, uint32_t generation, bool link)
{
    std::unique_ptr<ecuio_condition_eval> e(new ecuio_condition_eval());
    e->generation = generation;
    std::fill(std::begin(e->hash), std::end(e->hash), kEmpty);
    const uint32_t count = static_cast<uint32_t>(set->conditions.size());
    e->conditions.resize(count);
    e->terms.reserve(size_t{count} * ECUIO_CONDITION_MAX_TERMS);
    e->signals.reserve(size_t{count} * ECUIO_CONDITION_MAX_TERMS);
    uint32_t available = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_condition_set::condition& from = set->conditions[i];
        condition& c = e->conditions[i];
        c.any = (from.info.flags & ECUIO_CONDITION_INFO_ANY) != 0 ? 1 : 0;
        c.for_us = from.info.for_us;
        c.clear_for_us = from.info.clear_for_us;
        ecuio_condition_binding_t b{};
        if (ecuio_condition_set_resolve(set, i, signal_set, file, &b) !=
            ECUIO_OK) {
            c.why = ECUIO_CONDITION_WHY_NO_SIGNAL;
            continue;
        }
        c.why = b.why;
        for (uint32_t k = 0;
             c.why == ECUIO_CONDITION_WHY_NONE && k < from.info.term_count;
             ++k) {
            if (from.terms[k].kind == ECUIO_TERM_LINK && !link) {
                c.why = ECUIO_CONDITION_WHY_NO_LINK;
            } else if (from.terms[k].kind == ECUIO_TERM_CHECK) {
                c.why = ECUIO_CONDITION_WHY_NO_CHECK;
            }
        }
        if (c.why != ECUIO_CONDITION_WHY_NONE) {
            continue;
        }
        c.available = 1;
        ++available;
        c.first_term = static_cast<uint32_t>(e->terms.size());
        c.term_count = from.info.term_count;
        for (uint32_t k = 0; k < from.info.term_count; ++k) {
            const ecuio_condition_term_t& t = from.terms[k];
            term out;
            out.level = t.level;
            out.clear = t.clear;
            out.has_clear = (t.flags & ECUIO_TERM_HAS_CLEAR) != 0 ? 1 : 0;
            out.max_age_us = t.max_age_us;
            out.condition = i;
            out.test = static_cast<uint8_t>(t.test);
            if (t.kind == ECUIO_TERM_LINK) {
                out.link = 1;
                out.signal = kEmpty;
                e->link_terms.push_back(static_cast<uint16_t>(e->terms.size()));
                e->terms.push_back(out);
                continue;
            }
            const uint16_t id = static_cast<uint16_t>(b.signal_id[k]);
            uint32_t s = slot_of(id);
            while (e->hash[s] != kEmpty && e->signals[e->hash[s]].id != id) {
                s = (s + 1) & (kHashSlots - 1);
            }
            if (e->hash[s] == kEmpty) {
                e->hash[s] = static_cast<uint16_t>(e->signals.size());
                signal fresh;
                fresh.id = id;
                e->signals.push_back(fresh);
            }
            signal& of = e->signals[e->hash[s]];
            out.signal = e->hash[s];
            out.next = of.first_term;
            of.first_term = static_cast<uint16_t>(e->terms.size());
            ++of.unjudged;
            e->terms.push_back(out);
        }
    }
    /* Each signal's points: its terms' levels and clears, in order */
    e->points.reserve(e->terms.size() * 2);
    for (signal& s : e->signals) {
        s.first_point = static_cast<uint32_t>(e->points.size());
        for (uint16_t k = s.first_term; k != kEmpty; k = e->terms[k].next) {
            point p;
            p.term = k;
            p.at = e->terms[k].level;
            e->points.push_back(p);
            if (e->terms[k].has_clear != 0) {
                p.at = e->terms[k].clear;
                e->points.push_back(p);
            }
        }
        s.point_count = static_cast<uint32_t>(e->points.size()) - s.first_point;
        std::sort(e->points.begin() + s.first_point, e->points.end(),
                  [](const point& a, const point& b) { return a.at < b.at; });
    }
    e->events.resize(size_t{available} * kEventsPerCondition);
    e->dirty.resize(available);
    return e;
}

size_t ecuio_condition_eval::bytes() const
{
    return sizeof *this + terms.capacity() * sizeof(term) +
           signals.capacity() * sizeof(signal) +
           points.capacity() * sizeof(point) +
           conditions.capacity() * sizeof(condition) +
           events.capacity() * sizeof(ecuio_condition_event_t) +
           dirty.capacity() * sizeof(uint32_t) +
           link_terms.capacity() * sizeof(uint16_t);
}

ecuio_condition_eval::joined
ecuio_condition_eval::join(const condition& c) const
{
    bool unjudged = false;
    for (uint32_t k = 0; k < c.term_count; ++k) {
        const term& t = terms[c.first_term + k];
        if (t.judged == 0) {
            unjudged = true;
        } else if ((t.truth != 0) == (c.any != 0)) {
            /* One true term settles any; one false term settles all */
            return c.any != 0 ? joined::yes : joined::no;
        }
    }
    if (unjudged) {
        return joined::unjudged;
    }
    return c.any != 0 ? joined::no : joined::yes;
}

/* The time past which term t's sample can't be judged */
uint64_t ecuio_condition_eval::expiry(const term& t) const
{
    if (t.link != 0) {
        return UINT64_MAX;
    }
    const signal& s = signals[t.signal];
    return t.max_age_us != 0 ? s.time_us + t.max_age_us : s.fresh_until_us;
}

/* A deadline of condition index: its check, and the set's, come no later */
void ecuio_condition_eval::watch(uint32_t index, uint64_t deadline_us)
{
    condition& c = conditions[index];
    c.check_us = std::min(c.check_us, deadline_us);
    next_check_us = std::min(next_check_us, c.check_us);
}

/* The condition's (state, stale) changed at at_us: one event, or the one
 * it already has at that time in this call */
void ecuio_condition_eval::changed(uint32_t index, uint64_t at_us,
                                   uint32_t decider, bool fired)
{
    condition& c = conditions[index];
    c.since_us = at_us;
    c.changed_term = decider;
    const term& by = terms[c.first_term + decider];
    c.value = by.link != 0 ? 0.0 : signals[by.signal].value;
    ecuio_condition_event_t* e = nullptr;
    if (c.event_frame == frame && events[c.last_event].timestamp_us == at_us) {
        e = &events[c.last_event];
        fired = fired || (e->flags & ECUIO_CONDITION_FIRED) != 0;
    } else {
        if (event_count == events.size()) {
            assert(false && "a frame gave a condition more events than its "
                            "bound");
            ++dropped;
            return;
        }
        c.last_event = event_count;
        c.event_frame = frame;
        e = &events[event_count++];
        *e = ecuio_condition_event_t{};
    }
    e->timestamp_us = at_us;
    e->condition = index;
    e->state = c.state;
    e->flags = (c.stale != 0 ? ECUIO_CONDITION_STALE : 0u) |
               (fired ? ECUIO_CONDITION_FIRED : 0u) |
               (at_link != 0 ? ECUIO_CONDITION_LINK : 0u);
    e->term = decider;
    e->value = c.value;
    e->code = by.link != 0 ? link_state : 0;
    e->generation = generation;
}

/* The condition's terms joined again at at_us, after decider changed */
void ecuio_condition_eval::rejoin(uint32_t index, uint64_t at_us,
                                  uint32_t decider)
{
    condition& c = conditions[index];
    const joined j = join(c);
    if (j == joined::unjudged) {
        /* The state is kept, and a hold under way starts over */
        c.hold = 0;
        if (c.stale == 0) {
            c.stale = 1;
            changed(index, at_us, decider, false);
        }
        return;
    }
    bool change = c.stale != 0;
    bool fired = false;
    c.stale = 0;
    const bool holds = j == joined::yes;
    const bool toward_on = c.state == ECUIO_CONDITION_OFF && holds;
    const bool toward_off = c.state == ECUIO_CONDITION_ON && !holds;
    const uint8_t want = toward_on ? 1 : toward_off ? 2 : 0;
    if (want == 0) {
        c.hold = 0;
    } else {
        const uint32_t wait = toward_on ? c.for_us : c.clear_for_us;
        if (c.hold != want) {
            c.hold = want;
            c.hold_end_us = at_us + wait;
            c.hold_term = decider;
        }
        if (wait == 0) {
            c.hold = 0;
            c.state = toward_on ? ECUIO_CONDITION_ON : ECUIO_CONDITION_OFF;
            fired = toward_on;
            c.fired += toward_on ? 1 : 0;
            change = true;
        } else {
            watch(index, c.hold_end_us);
        }
    }
    if (change) {
        changed(index, at_us, decider, fired);
    }
}

/* Term index against its signal's newest sample: its truth, and its
 * condition marked when the truth or the judgment changed */
void ecuio_condition_eval::sample(uint16_t index)
{
    term& t = terms[index];
    signal& s = signals[t.signal];
    const double v = s.value;
    const bool was_judged = t.judged != 0;
    uint8_t truth = t.truth;
    switch (t.test) {
    case ECUIO_TERM_ABOVE:
        if (v > t.level) {
            truth = 1;
        } else if (t.has_clear == 0 || v <= t.clear) {
            truth = 0;
        }
        break;
    case ECUIO_TERM_BELOW:
        if (v < t.level) {
            truth = 1;
        } else if (t.has_clear == 0 || v >= t.clear) {
            truth = 0;
        }
        break;
    case ECUIO_TERM_EQUALS:
        truth = v == t.level ? 1 : 0;
        break;
    default:
        truth = v != t.level ? 1 : 0;
        break;
    }
    if (truth == t.truth && was_judged) {
        return;
    }
    t.truth = truth;
    condition& c = conditions[t.condition];
    c.changed_term = uint32_t{index} - c.first_term;
    if (c.dirty == 0) {
        c.dirty = 1;
        dirty[dirty_count++] = t.condition;
    }
    if (!was_judged) {
        t.judged = 1;
        --s.unjudged;
        watch(t.condition, expiry(t));
    }
}

/* One value of the frame: its signal's sample, then the terms it can
 * change. A term's truth changes only at a sample on or across its
 * level or its clear, so with every term judged only the points from
 * the sample before to this one are visited. */
void ecuio_condition_eval::feed(uint64_t now_us, const ecuio_value_t& v)
{
    uint32_t at = slot_of(v.signal_id);
    while (hash[at] != kEmpty && signals[hash[at]].id != v.signal_id) {
        at = (at + 1) & (kHashSlots - 1);
    }
    if (hash[at] == kEmpty || std::isnan(v.value)) {
        return; /* a NaN is no sample: no point is on either side of it */
    }
    signal& s = signals[hash[at]];
    const double before = s.value;
    const bool every = s.sampled == 0 || s.unjudged != 0;
    uint64_t gap = 0;
    if (s.sampled != 0) {
        gap = std::min(now_us - s.time_us, kPaceCapUs);
        s.mean_us = static_cast<uint32_t>(
            s.mean_us == 0 ? gap : (3 * uint64_t{s.mean_us} + gap) / 4);
    }
    const uint64_t pace = kPaceTimes * std::max(gap, uint64_t{s.mean_us});
    /* The gap's cap already holds pace to the ceiling; the min states it */
    const uint64_t bound =
        std::max(kPaceFloorUs, std::min(kPaceTimes * kPaceCapUs, pace));
    /* A shorter pace moves its terms' deadlines earlier: their checks too */
    const bool earlier = now_us + bound < s.fresh_until_us;
    s.fresh_until_us = now_us + bound;
    s.value = v.value;
    s.time_us = now_us;
    first_samples += s.sampled == 0 ? 1u : 0u;
    s.sampled = 1;
    for (uint16_t k = s.first_term; earlier && k != kEmpty; k = terms[k].next) {
        if (terms[k].judged != 0 && terms[k].max_age_us == 0) {
            watch(terms[k].condition, s.fresh_until_us);
        }
    }
    if (every) {
        for (uint16_t k = s.first_term; k != kEmpty; k = terms[k].next) {
            sample(k);
        }
        return;
    }
    const double lo = std::min(before, v.value);
    const double hi = std::max(before, v.value);
    const point* first = points.data() + s.first_point;
    const point* last = first + s.point_count;
    const point* p = std::lower_bound(
        first, last, lo, [](const point& a, double b) { return a.at < b; });
    for (; p != last && p->at <= hi; ++p) {
        sample(p->term);
    }
}

/* Condition index's deadlines before now_us, each at its own time, a
 * hold's end before an age bound at the same time; with hold_at_now a
 * hold ending exactly at now_us too */
void ecuio_condition_eval::settle_one(uint32_t index, uint64_t now_us,
                                      bool hold_at_now)
{
    condition& c = conditions[index];
    for (;;) {
        const uint64_t hold_end = c.hold != 0 ? c.hold_end_us : UINT64_MAX;
        uint64_t expiry = UINT64_MAX;
        uint32_t oldest = 0;
        for (uint32_t k = 0; k < c.term_count; ++k) {
            const term& t = terms[c.first_term + k];
            if (t.judged != 0 && this->expiry(t) < expiry) {
                expiry = this->expiry(t);
                oldest = k;
            }
        }
        const bool hold_due =
            hold_end < now_us || (hold_at_now && hold_end == now_us);
        const bool expiry_due = expiry < now_us;
        if (hold_due && (!expiry_due || hold_end <= expiry)) {
            const bool on = c.hold == 1;
            c.hold = 0;
            c.state = on ? ECUIO_CONDITION_ON : ECUIO_CONDITION_OFF;
            c.fired += on ? 1 : 0;
            changed(index, hold_end, c.hold_term, on);
        } else if (expiry_due) {
            term& t = terms[c.first_term + oldest];
            t.judged = 0;
            ++signals[t.signal].unjudged;
            rejoin(index, expiry, oldest);
        } else {
            c.check_us = std::min(hold_end, expiry);
            return;
        }
    }
}

void ecuio_condition_eval::settle(uint64_t now_us, bool hold_at_now)
{
    uint64_t least = UINT64_MAX;
    for (uint32_t i = 0; i < conditions.size(); ++i) {
        condition& c = conditions[i];
        if (c.available != 0 &&
            (c.check_us < now_us || (hold_at_now && c.check_us == now_us))) {
            settle_one(i, now_us, hold_at_now);
        }
        least = std::min(least, c.check_us);
    }
    next_check_us = least;
}

/* A judge call's start: its time, never before the last, and every
 * deadline before it settled */
uint64_t ecuio_condition_eval::begin(uint64_t time_us)
{
    event_count = 0;
    ++frame;
    time_us = std::max(time_us, newest_us);
    newest_us = time_us;
    if (next_check_us < time_us) {
        settle(time_us, false);
    }
    return time_us;
}

uint32_t ecuio_condition_eval::judge(uint64_t time_us,
                                     const ecuio_value_t* values,
                                     uint32_t count)
{
    at_link = 0;
    time_us = begin(time_us);
    for (uint32_t i = 0; i < count; ++i) {
        if (values[i].kind != ECUIO_VALUE_TEXT &&
            values[i].kind != ECUIO_VALUE_HEX) {
            feed(time_us, values[i]);
        }
    }
    return finish(time_us);
}

uint32_t ecuio_condition_eval::judge_link(uint64_t time_us, int32_t state)
{
    at_link = 1;
    time_us = begin(time_us);
    set_link(state);
    const uint32_t n = finish(time_us);
    at_link = 0;
    return n;
}

void ecuio_condition_eval::set_link(int32_t state)
{
    link_state = state;
    for (const uint16_t k : link_terms) {
        term& t = terms[k];
        const uint8_t truth = link_holds(t.test, state) ? 1 : 0;
        if (truth == t.truth && t.judged != 0) {
            continue;
        }
        t.truth = truth;
        t.judged = 1;
        condition& c = conditions[t.condition];
        c.changed_term = uint32_t{k} - c.first_term;
        if (c.dirty == 0) {
            c.dirty = 1;
            dirty[dirty_count++] = t.condition;
        }
    }
}

/* A judge call's end: the conditions its inputs changed joined again,
 * then the holds ending at its time; the events in order */
uint32_t ecuio_condition_eval::finish(uint64_t time_us)
{
    for (uint32_t i = 0; i < dirty_count; ++i) {
        condition& c = conditions[dirty[i]];
        c.dirty = 0;
        rejoin(dirty[i], time_us, c.changed_term);
    }
    dirty_count = 0;
    if (next_check_us <= time_us) {
        settle(time_us, true);
    }
    /* By time, then the set's order; a condition's own are in order */
    for (uint32_t i = 1; i < event_count; ++i) {
        const ecuio_condition_event_t e = events[i];
        uint32_t at = i;
        while (at > 0 && (events[at - 1].timestamp_us > e.timestamp_us ||
                          (events[at - 1].timestamp_us == e.timestamp_us &&
                           events[at - 1].condition > e.condition))) {
            events[at] = events[at - 1];
            --at;
        }
        events[at] = e;
    }
    return event_count;
}

uint8_t ecuio_condition_eval::sampled_terms(uint32_t index) const
{
    const condition& c = conditions[index];
    uint8_t bits = 0;
    for (uint32_t k = 0; k < c.term_count && k < 8; ++k) {
        const term& t = terms[c.first_term + k];
        if (t.link != 0 || signals[t.signal].sampled != 0) {
            bits = static_cast<uint8_t>(bits | (1u << k));
        }
    }
    return bits;
}

ecuio_condition_state_t ecuio_condition_eval::state_of(uint32_t index) const
{
    const condition& c = conditions[index];
    ecuio_condition_state_t s{};
    s.state = c.state;
    s.since_us = c.since_us;
    s.fired = c.fired;
    s.value = c.value;
    s.generation = generation;
    if (c.available != 0) {
        s.flags = c.stale != 0 ? ECUIO_CONDITION_STALE : 0u;
    } else {
        s.flags = ECUIO_CONDITION_UNAVAILABLE;
        s.why = c.why;
    }
    return s;
}
