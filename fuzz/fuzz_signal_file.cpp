// File/layer: fuzz/fuzz_signal_file.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_signal_file

#include <ecuio/core.h>
#include <ecuio/profile.h>
#include <ecuio/signals.h>

#include "computed.h"      /* core/src: the reference's machine */
#include "frame_decoder.h" /* core/src: the set's decode, chains and all */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

    /* A format 1 file compiles format 1's steps alone and reads no
     * computed input (ADR-0062), and each computed def keeps its
     * expression */
    const uint32_t format = ecuio_signal_file_format(a);
    check(format >= ECUIO_SIGNAL_FILE_FORMAT_OLDEST &&
          format <= ECUIO_SIGNAL_FILE_FORMAT);
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0) {
            check(ecuio_signal_file_expr(a, &d)[0] == '\0');
            continue;
        }
        check(ecuio_signal_file_expr(a, &d)[0] != '\0');
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

    /* The profile name is one the registry takes, and the set registers */
    const char* profile = ecuio_signal_file_profile(a);
    check(strncmp(profile, "user.", 5) == 0);
    check(strlen(profile) <= ECUIO_PROFILE_NAME_MAX);
    check(ecuio_profile_register(profile, set, nullptr) == ECUIO_OK);
    check(ecuio_profile_unregister(profile) == ECUIO_OK);

    /* Each def reads a payload as long as its field needs, through the
     * decoder its kind takes, without a fault */
    uint8_t payload[ECUIO_FRAME_DATA_MAX] = {};
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

    /* Frames carrying the keys of the first defs and of the last (a file
     * extending obd2 lists the base's first and its own after), a
     * millisecond apart, through two decoders: every value finite, each
     * def once, the frame's numbers first, the two alike, and the
     * computed values the reference expects, in the set's order */
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
        memcpy(frame.data, payload, sizeof payload);
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
        for (uint32_t c = 0; c < set->count; ++c) {
            if ((set->defs[c].flags & ECUIO_DEF_FLAG_COMPUTED) == 0) {
                continue;
            }
            ref.walk(c);
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

    ecuio_signal_file_free(a);
    ecuio_signal_file_free(b);
    return 0;
}
