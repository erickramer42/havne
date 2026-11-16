// File/layer: fuzz/fuzz_validate_set.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: arbitrary sets against the set validator: the code
//          range, the index semantics, the out contract, determinism;
//          computed tables whose inputs read each other (ADR-0062),
//          and a set that passes decoded

#include <ecuio/signals.h>

#include "frame_decoder.h" /* core/src: a passed set's decode */

#include <cmath>
#include <limits>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace {

/* Sized to the most the input can ask for (8 defs, and 8 channels
 * over ECUIO_MAX_CHANNELS, so the count's refusal is reached), so
 * however the validator walks them, it reads initialized memory. */
constexpr uint32_t kMaxDefs = 8;
constexpr uint32_t kMaxChannels = ECUIO_MAX_CHANNELS + 8;
constexpr uint32_t kDefBytes = 6;
constexpr uint32_t kChannelBytes = 4;
constexpr size_t kHeadBytes = 5;

/* No code: the input is shorter than the set it asks for */
constexpr int kTooShort = 1000;

/* Chain mode: two numbers (ids 1 and 2), then up to 12 computed defs (ids
 * 10 to 21), each with a program of its own whose inputs are any of ids
 * 0 to 23, with or without an age bound. The validator answers OK, a
 * table range (a def), a computed input or a chain (a program), or no
 * memory, in a fixed number of passes; a set it passes decodes the rest
 * of the input as frames: every value finite, each def once, the
 * frame's numbers before its computed values, each part in the set's
 * order, two decoders alike, and the batch the reference below expects. */
constexpr uint32_t kChainMax = 12;

/* The chain's reference, apart from the decoder: a computed def's value is
 * the sum of its inputs (chain mode's programs add them in order), a
 * number input its newest value within the bound at the frame's time, a
 * computed input walked the same way at the same time, no value (NaN)
 * through any step; fed when an input is the frame's number or a fed
 * computed def. Nothing is kept between frames but the numbers' values. */
struct chain_ref {
    const ecuio_signal_def_t* defs;
    uint32_t count;
    const ecuio_computed_t* computed;
    double leaf_value[2] = {};
    uint64_t leaf_us[2] = {};
    bool leaf_seen[2] = {};
    uint32_t fed_leaf = 0;
    uint64_t now = 0;
    uint8_t state[2 + kChainMax] = {};
    double value[2 + kChainMax] = {};
    bool fed[2 + kChainMax] = {};

    void take(uint32_t leaf, uint8_t raw, uint64_t at_us)
    {
        leaf_value[leaf] = raw;
        leaf_us[leaf] = at_us;
        leaf_seen[leaf] = true;
        fed_leaf = leaf;
        now = at_us;
        memset(state, 0, sizeof state);
    }

    uint32_t index_of(uint16_t id) const
    {
        uint32_t i = 0;
        while (i < count && defs[i].id != id)
            ++i;
        return i;
    }

    void walk(uint32_t i)
    {
        if (state[i] != 0)
            return;
        state[i] = 1; /* a validated set has no circle to meet it again */
        const ecuio_computed_t& c = computed[defs[i].table_first];
        double sum = 0.0;
        bool f = false;
        for (uint32_t k = 0; k < c.input_count; ++k) {
            const uint32_t j = index_of(c.input_ids[k]);
            double x = std::numeric_limits<double>::quiet_NaN();
            if (defs[j].flags & ECUIO_DEF_FLAG_COMPUTED) {
                walk(j);
                x = value[j];
                f = f || fed[j];
            } else {
                const uint64_t bound = c.max_age_us[k] != 0
                                           ? c.max_age_us[k]
                                           : ECUIO_VALUE_STALE_US;
                if (leaf_seen[j] && now - leaf_us[j] <= bound)
                    x = leaf_value[j];
                f = f || j == fed_leaf;
            }
            sum = k == 0 ? x : sum + x;
        }
        value[i] = sum;
        fed[i] = f;
        state[i] = 2;
    }
};

int chain_mode(const uint8_t* p, size_t size)
{
    if (size < 1)
        return kTooShort;
    const uint32_t m = p[0] % (kChainMax + 1);
    const size_t needed = 1 + size_t{m} * 6;
    if (size < needed)
        return kTooShort;
    static ecuio_signal_def_t defs[2 + kChainMax];
    static ecuio_computed_t computed[kChainMax];
    memset(defs, 0, sizeof defs);
    memset(computed, 0, sizeof computed);
    for (uint16_t i = 0; i < 2; ++i) {
        defs[i].id = static_cast<uint16_t>(i + 1);
        defs[i].kind = ECUIO_VALUE_UINT;
        defs[i].message_id = 0x123u + i;
        defs[i].bit_length = 8;
        defs[i].scale = 1.0;
        defs[i].name[0] = 'N'; /* N0, N1: a name of its own */
        defs[i].name[1] = static_cast<char>('0' + i);
    }
    const uint8_t* q = p + 1;
    for (uint32_t k = 0; k < m; ++k, q += 6) {
        ecuio_signal_def_t& d = defs[2 + k];
        d.id = static_cast<uint16_t>(10 + k);
        d.flags = ECUIO_DEF_FLAG_COMPUTED;
        d.scale = 1.0;
        d.name[0] = 'C'; /* Ca to Cl */
        d.name[1] = static_cast<char>('a' + k);
        d.table_first = static_cast<uint16_t>(q[0] % (m + 1)); /* m: past it */
        ecuio_computed_t& c = computed[k];
        c.input_count = static_cast<uint8_t>(1 + (q[1] & 3u));
        for (uint32_t i = 0; i < c.input_count; ++i) {
            c.input_ids[i] = static_cast<uint16_t>(q[2 + i] % 24);
            c.max_age_us[i] = (q[2 + i] & 0x80u) ? 1000000u : 0u;
            c.steps[c.step_count++] =
                static_cast<uint8_t>(ECUIO_COMPUTED_PUSH_INPUT | i);
            if (i > 0) {
                c.steps[c.step_count++] = ECUIO_COMPUTED_ADD;
            }
        }
    }
    ecuio_signal_set_t set;
    memset(&set, 0, sizeof set);
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 2 + m;
    set.defs = defs;
    set.computed_count = m;
    set.computed = m > 0 ? computed : nullptr;

    ecuio_validation_t v1, v2;
    memset(&v1, 0xA5, sizeof v1);
    const int rc = ecuio_validate_signal_set(&set, &v1);
    if (ecuio_validate_signal_set(&set, &v2) != rc || v1.index != v2.index ||
        v1.code != rc)
        abort();
    switch (rc) {
    case ECUIO_VALIDATE_OK:
    case ECUIO_VALIDATE_ERR_OUT_OF_MEMORY:
        if (v1.index != UINT32_MAX)
            abort();
        break;
    case ECUIO_VALIDATE_ERR_TABLE_RANGE:
        if (v1.index < 2 || v1.index >= set.count)
            abort();
        break;
    case ECUIO_VALIDATE_ERR_COMPUTED_INPUT:
    case ECUIO_VALIDATE_ERR_COMPUTED_CHAIN:
        if (v1.index >= m)
            abort();
        break;
    default:
        abort();
    }
    if (rc != ECUIO_VALIDATE_OK)
        return rc;

    ecuio_frame_decoder a;
    ecuio_frame_decoder b;
    a.prepare(&set, false);
    b.prepare(&set, false);
    chain_ref ref{defs, set.count, computed};
    ecuio_frame_t f;
    memset(&f, 0, sizeof f);
    uint64_t t = 0;
    for (const uint8_t* r = q; r + 2 <= p + size; r += 2) {
        const uint32_t leaf = r[0] & 1u;
        f.arbitration_id = 0x123u + leaf;
        f.data[0] = r[1];
        f.data_length = 1;
        t += uint64_t{r[0]} / 2 * 20000u; /* up to 2.5 s on */
        f.timestamp_us = t;
        const uint32_t n = a.decode(&f);
        if (b.decode(&f) != n || n > set.count)
            abort();
        for (uint32_t i = 0; i < n; ++i) {
            const ecuio_value_t& x = a.values[i];
            if (!std::isfinite(x.value) ||
                memcmp(&x.value, &b.values[i].value, sizeof x.value) != 0 ||
                x.signal_id != b.values[i].signal_id)
                abort();
        }
        /* The batch the reference expects: the frame's number, then each
         * computed def fed this frame and with a value, in the set's
         * order, each its sum of inputs worked out from the leaves */
        ref.take(leaf, r[1], t);
        if (n < 1 || a.values[0].signal_id != defs[leaf].id ||
            a.values[0].value != r[1])
            abort();
        uint32_t at = 1;
        for (uint32_t i = 2; i < set.count; ++i) {
            ref.walk(i);
            if (!ref.fed[i] || std::isnan(ref.value[i]))
                continue;
            if (at >= n || a.values[at].signal_id != defs[i].id ||
                a.values[at].value != ref.value[i])
                abort();
            ++at;
        }
        if (at != n)
            abort();
    }
    return rc;
}


/* Two valid defs with ids of their own, no channel: a set the validator
 * takes. Names are one each in a set, so a harness that names no def
 * never passes two; this input keeps that from coming back unseen. */
// clang-format off
constexpr uint8_t kTwoDefs[] = {
    2, ECUIO_SIGNAL_SET_LAYOUT, 0, 0, 0, /* count, layout, reserved, channels */
    1, ECUIO_VALUE_UINT, 8, 0, ECUIO_BYTE_ORDER_LE, 0,
    2, ECUIO_VALUE_UINT, 8, 0, ECUIO_BYTE_ORDER_LE, 0,
};
// clang-format on

/* The set data describes, validated twice and checked; its code, or
 * kTooShort */
int validated(const uint8_t* data, size_t size)
{
    if (size < kHeadBytes)
        return kTooShort;
    if (data[4] & 4u)
        return chain_mode(data + kHeadBytes, size - kHeadBytes);

    const uint32_t count = data[0] % 9;                 /* 0..8 */
    /* 0 to kMaxChannels, past ECUIO_MAX_CHANNELS among them */
    const uint32_t channel_count = data[3] % (kMaxChannels + 1);
    const bool channels_null = (data[4] & 1u) != 0;
    const bool defs_null = (data[4] & 2u) != 0;

    const size_t needed = kHeadBytes + (size_t)count * kDefBytes +
                          (size_t)channel_count * kChannelBytes;
    if (size < needed)
        return kTooShort;

    /* Every def starts from the valid baseline and bends from there.
     * p advances only for the defs the input carries: consuming
     * kMaxDefs worth of bytes when count is smaller would run off the
     * end of the fuzz input, which is the harness's bug, not core's. */
    static ecuio_signal_def_t defs[kMaxDefs];
    memset(defs, 0, sizeof defs);
    const uint8_t* p = data + kHeadBytes;
    for (uint32_t i = 0; i < kMaxDefs; ++i) {
        defs[i].scale = 1.0;
        if (i >= count) continue;
        defs[i].id = (uint16_t)(p[0] % 4);              /* duplicates reachable */
        defs[i].kind = p[1] % 5;
        defs[i].bit_length = p[2];
        defs[i].bit_offset = p[3] & 0x0Fu;
        defs[i].byte_order = p[4] & 3u;
        defs[i].mux_length = (p[4] >> 2) & 3u;
        /* A name of its own from the def's place ("s0" to "s7"), so
         * several defs can pass; one bit takes the def before's (a
         * duplicate name), another starts it as a formula does */
        const bool repeats = (p[4] & 0x80u) != 0 && i > 0;
        defs[i].name[0] = (p[4] & 0x40u) != 0 ? '=' : 's';
        defs[i].name[1] = (char)('0' + (repeats ? i - 1 : i));
        if (p[5] != 0) defs[i].reserved[0] = p[5];      /* reserved injection */
        p += kDefBytes;
    }

    static ecuio_channel_decl_t channels[kMaxChannels];
    memset(channels, 0, sizeof channels);
    for (uint32_t i = 0; i < kMaxChannels && i < channel_count; ++i) {
        channels[i].rx_id = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                            (((uint32_t)p[2] & 1u) << 29);  /* past 0x1FFFFFFF */
        channels[i].tx_id = (uint32_t)p[0] | ((uint32_t)p[3] << 8) |
                            (((uint32_t)p[2] & 2u) << 28);
        if (p[2] & 4u) channels[i].flags = p[2];        /* reserved injection */
        if (p[2] & 8u) channels[i].reserved = 1u;
        p += kChannelBytes;
    }

    ecuio_signal_set_t set;
    memset(&set, 0, sizeof set);
    set.layout_version = data[1];
    set.count = count;
    /* NULL defs is valid with count 0, ERR_NULL with count > 0 */
    set.defs = defs_null ? nullptr : defs;
    set.channel_count = channel_count;
    set.reserved0 = data[2];               /* reserved injection */
    set.channels = channels_null ? nullptr : channels;

    /* Garbage everywhere the contract says is overwritten or must be
     * zeroed, so omission is visible */
    ecuio_validation_t v1, v2;
    memset(&v1, 0xA5, sizeof v1);
    memset(&v2, 0x5A, sizeof v2);
    const int rc1 = ecuio_validate_signal_set(&set, &v1);
    const int rc2 = ecuio_validate_signal_set(&set, &v2);

    /* Determinism, the out code, and the reserved bytes written zero */
    if (rc1 != rc2) abort();
    if (v1.code != rc1) abort();
    if (v1.index != v2.index) abort();
    if (v1.reserved[0] != 0 || v1.reserved[1] != 0) abort();

    /* One switch does both jobs: an unknown code (the retired 2 and 5
     * included) lands in default, and each known code carries its own
     * index rule from signals.h. Classified by code, not by sign:
     * DUPLICATE_ID is negative and names a def. */
    switch (rc1) {
    /* Names no single element */
    case ECUIO_VALIDATE_OK:
    case ECUIO_VALIDATE_ERR_NULL:
    case ECUIO_VALIDATE_ERR_LAYOUT:
    case ECUIO_VALIDATE_ERR_SET_RESERVED:
    case ECUIO_VALIDATE_ERR_CHANNELS_NULL:
    case ECUIO_VALIDATE_ERR_CHANNEL_COUNT:
        if (v1.index != UINT32_MAX) abort();
        break;

    /* Names a channel */
    case ECUIO_VALIDATE_ERR_CHANNEL_ID:
    case ECUIO_VALIDATE_ERR_CHANNEL_RESERVED:
    case ECUIO_VALIDATE_ERR_DUPLICATE_RX:
        if (v1.index >= channel_count) abort();
        break;

    /* Names a def: every per-def code, plus DUPLICATE_ID's and
     * DUPLICATE_NAME's second of the pair */
    case ECUIO_VALIDATE_ERR_BIT_LENGTH:
    case ECUIO_VALIDATE_ERR_BIT_OFFSET:
    case ECUIO_VALIDATE_ERR_NAME:
    case ECUIO_VALIDATE_ERR_UNIT:
    case ECUIO_VALIDATE_ERR_DECIMALS:
    case ECUIO_VALIDATE_ERR_MUX_LENGTH:
    case ECUIO_VALIDATE_ERR_MUX_KEY:
    case ECUIO_VALIDATE_ERR_BIG_ENDIAN:
    case ECUIO_VALIDATE_ERR_KIND:
    case ECUIO_VALIDATE_ERR_FLOAT32:
    case ECUIO_VALIDATE_ERR_RESERVED:
    case ECUIO_VALIDATE_ERR_BYTE_ORDER:
    case ECUIO_VALIDATE_ERR_FORMULA:
    case ECUIO_VALIDATE_ERR_DUPLICATE_ID:
    case ECUIO_VALIDATE_ERR_DUPLICATE_NAME:
        if (v1.index >= count) abort();
        break;

    default:
        abort();   /* not a code this core defines */
    }

    return rc1;
}

/* The harness still reaches a set of several defs that passes, the
 * duplicate name's refusal through its repeat bit, a channel's reserved
 * fields through theirs, and one channel more than a set holds */
bool reaches_several_defs()
{
    if (validated(kTwoDefs, sizeof kTwoDefs) != ECUIO_VALIDATE_OK)
        abort();
    uint8_t repeated[sizeof kTwoDefs];
    memcpy(repeated, kTwoDefs, sizeof kTwoDefs);
    repeated[kHeadBytes + kDefBytes + 4] |= 0x80u;
    if (validated(repeated, sizeof repeated) !=
        ECUIO_VALIDATE_ERR_DUPLICATE_NAME) {
        abort();
    }
    /* One channel, 0x7E8 and 0x6E8: taken plain, refused with either bit */
    uint8_t channel[kHeadBytes + kChannelBytes] = {
        0, ECUIO_SIGNAL_SET_LAYOUT, 0, 1, 0, 0xE8, 0x07, 0x00, 0x06};
    if (validated(channel, sizeof channel) != ECUIO_VALIDATE_OK)
        abort();
    const uint8_t bits[] = {4, 8}; /* its flags, its reserved word */
    for (const uint8_t bit : bits) {
        channel[kHeadBytes + 2] = bit;
        if (validated(channel, sizeof channel) !=
            ECUIO_VALIDATE_ERR_CHANNEL_RESERVED) {
            abort();
        }
    }
    uint8_t crowd[kHeadBytes + (ECUIO_MAX_CHANNELS + 1) * kChannelBytes] = {
        0, ECUIO_SIGNAL_SET_LAYOUT, 0, ECUIO_MAX_CHANNELS + 1, 0};
    for (uint32_t i = 0; i <= ECUIO_MAX_CHANNELS; ++i) {
        uint8_t* c = crowd + kHeadBytes + i * kChannelBytes;
        c[0] = (uint8_t)(0x10 + i); /* an ID of its own for each */
        c[1] = 0x07;
        c[3] = 0x06;
    }
    if (validated(crowd, sizeof crowd) != ECUIO_VALIDATE_ERR_CHANNEL_COUNT)
        abort();
    return true;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const bool checked = reaches_several_defs();
    (void)checked;
    (void)validated(data, size);
    return 0;
}
