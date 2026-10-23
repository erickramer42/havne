// File/layer: fuzz/fuzz_decode_signal.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ — arbitrary def geometry and payloads against the
//          decoder and the per-def validator, under ASan+UBSan

#include <ecuio/signals.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <memory>

namespace {

/* Bytes 0..13 configure the def, the rest are the payload. A zeroed
 * def with bit_length 16 is structurally valid, so the mutations below
 * bend each dimension both ways, valid and invalid alike. */
constexpr size_t kConfigBytes = 14;
/* start_byte is uint16_t, so payloads past a CAN FD frame's 64 bytes,
 * and past the ECUIO_FRAME_DATA_MAX a frame holds, are reachable; so
 * is every mux_offset. payload_len is uint16_t too. */
constexpr size_t kMaxPayload = 65535;

bool same_double_bits(double a, double b)
{
    /* Bitwise: NaN != NaN under ==, and a NaN result must still be
     * deterministic bit for bit */
    return memcmp(&a, &b, sizeof a) == 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < kConfigBytes + 1) return 0;

    ecuio_signal_def_t def;
    memset(&def, 0, sizeof def);
    def.scale      = 1.0;
    def.kind       = data[0] % 6;                       /* 0..5, incl. unknown */
    def.byte_order = data[1] & 3u;                      /* 0..3, incl. unknown */
    def.start_byte = (uint16_t)(data[2] | ((uint16_t)data[3] << 8));
    def.bit_offset = data[4] & 0x0Fu;                  /* 0..15, incl. >7 */
    def.byte_length = (uint16_t)((data[4] >> 4) * 400u); /* 0..6000, past the frame */
    def.bit_length = data[5];                          /* 0..255 */
    def.mux_length = data[6] % 5;                      /* 0..4, incl. >3 */
    def.mux_offset = data[7];
    def.mux_key    = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
                     ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
    /* scale 1, offset 0: identity arithmetic, so a NaN can only come
     * from the payload's own float bits */

    const size_t avail = size - kConfigBytes;
    size_t plen = (size_t)data[12] | ((size_t)data[13] << 8);
    plen %= (avail < kMaxPayload ? avail : kMaxPayload) + 1;

    /* Exactly plen bytes on the heap, so ASan reports any read past
     * payload_len; a larger zeroed buffer would hide one. new[] of 0
     * still returns a pointer, so plen 0 tests the bounds, not NULL. */
    std::unique_ptr<uint8_t[]> payload(new uint8_t[plen]);
    memcpy(payload.get(), data + kConfigBytes, plen);

    const int vrc = ecuio_validate_signal(&def);

    /* signals.h: *out_value is written only on ECUIO_DECODE_OK. The
     * sentinel is finite, so a spurious write can't hide behind NaN. */
    const double sentinel = -12345.5e30;
    double out1 = sentinel, out2 = sentinel;
    const int rc1 = ecuio_decode_signal(&def, payload.get(),
                                        (uint16_t)plen, &out1);
    const int rc2 = ecuio_decode_signal(&def, payload.get(),
                                        (uint16_t)plen, &out2);

    /* Every return code is one of the six, now and forever */
    switch (rc1) {
    case ECUIO_DECODE_OK:
    case ECUIO_DECODE_ERR_NULL:
    case ECUIO_DECODE_ERR_PAST_PAYLOAD:
    case ECUIO_DECODE_ERR_BAD_PARAMS:
    case ECUIO_DECODE_ERR_UNSUPPORTED:
    case ECUIO_DECODE_ERR_MUX_MISMATCH:
        break;
    default:
        abort();
    }

    /* Determinism: two identical calls agree, NaN bits included */
    if (rc1 != rc2 || !same_double_bits(out1, out2)) abort();

    /* Nothing written unless the decode succeeded */
    if (rc1 != ECUIO_DECODE_OK && out1 != sentinel) abort();

    /* The text decoder: the same six codes, deterministic, the text
     * terminated inside its buffer, and nothing written on failure. */
    char t1[48], t2[48];
    memset(t1, 0x5A, sizeof t1);
    memset(t2, 0x5A, sizeof t2);
    const int tr1 = ecuio_decode_text(&def, payload.get(), (uint16_t)plen,
                                      t1, sizeof t1);
    const int tr2 = ecuio_decode_text(&def, payload.get(), (uint16_t)plen,
                                      t2, sizeof t2);
    switch (tr1) {
    case ECUIO_DECODE_OK:
    case ECUIO_DECODE_ERR_NULL:
    case ECUIO_DECODE_ERR_PAST_PAYLOAD:
    case ECUIO_DECODE_ERR_BAD_PARAMS:
    case ECUIO_DECODE_ERR_UNSUPPORTED:
    case ECUIO_DECODE_ERR_MUX_MISMATCH:
        break;
    default:
        abort();
    }
    if (tr1 != tr2 || memcmp(t1, t2, sizeof t1) != 0) abort();
    if (tr1 == ECUIO_DECODE_OK) {
        if (memchr(t1, '\0', sizeof t1) == NULL) abort();
    } else if (memchr(t1, '\0', sizeof t1) != NULL) {
        abort();   /* untouched: the fill has no NUL */
    }

    /* A def the validator accepts fails only on the payload, bounds or
     * the mux key, in the decoder for its kind, and the other decoder
     * refuses it (the ValidatorAcceptsExactlyWhatTheDecoderDecodes
     * contract, over a space no hand-written grid covers) */
    if (vrc == ECUIO_VALIDATE_OK) {
        const bool text = def.kind == ECUIO_VALUE_TEXT ||
                          def.kind == ECUIO_VALUE_HEX;
        const int mine = text ? tr1 : rc1;
        const int other = text ? rc1 : tr1;
        if (mine != ECUIO_DECODE_OK &&
            mine != ECUIO_DECODE_ERR_PAST_PAYLOAD &&
            mine != ECUIO_DECODE_ERR_MUX_MISMATCH) {
            abort();
        }
        if (other != ECUIO_DECODE_ERR_UNSUPPORTED) abort();
    }

    return 0;
}
