// File/layer: fuzz/fuzz_validate_set.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: arbitrary sets against the set validator: the code
//          range, the index semantics, the out contract, determinism

#include <ecuio/signals.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace {

/* Sized past the valid limits (8 defs over any count the input can
 * ask for, 8 channels over ECUIO_MAX_CHANNELS), so however the
 * validator walks them, it reads initialized memory. */
constexpr uint32_t kMaxDefs = 8;
constexpr uint32_t kMaxChannels = 8;
constexpr uint32_t kDefBytes = 6;
constexpr uint32_t kChannelBytes = 4;
constexpr size_t kHeadBytes = 5;

/* No code: the input is shorter than the set it asks for */
constexpr int kTooShort = 1000;

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

    const uint32_t count = data[0] % 9;                 /* 0..8 */
    const uint32_t channel_count = data[3] % 6;         /* 0..5, incl. >4 */
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

/* The harness still reaches a set of several defs that passes, and the
 * duplicate name's refusal through its repeat bit */
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
