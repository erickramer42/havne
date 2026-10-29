// File/layer: fuzz/fuzz_elm327_reply.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ — arbitrary bytes from an ELM327 (serial, TCP, an app's
//          Bluetooth stream) through the line reader, the line kinds, the
//          frame parser and ISO-TP reassembly, in either addressing mode,
//          and the value parsers on each line: every line is within
//          ELM327_LINE_MAX and terminated, a frame line has 1 to 8 bytes,
//          a whole message fits a frame and never more than its First
//          Frame said, the values stay in range, two runs agree

#include <ecuio/core.h>

#include "elm327_protocol.h" /* core/src: the ELM327 wire, pure */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <memory>

namespace {

/* What one run saw, to compare two runs of the same bytes. */
struct digest {
    uint32_t lines = 0;
    uint32_t prompts = 0;
    uint32_t frames = 0;
    uint32_t messages = 0;
    uint64_t message_bytes = 0;
    uint32_t dropped = 0;
    uint32_t malformed = 0;
    uint64_t values = 0;
    uint32_t kinds[16] = {};

    bool operator==(const digest& o) const
    {
        return lines == o.lines && prompts == o.prompts &&
               frames == o.frames && messages == o.messages &&
               message_bytes == o.message_bytes && dropped == o.dropped &&
               malformed == o.malformed && values == o.values &&
               memcmp(kinds, o.kinds, sizeof(kinds)) == 0;
    }
};

/* The reassembly is 32 KB: the transport keeps it off the stack, so the
 * harness does too. */
elm327_isotp g_isotp;

void run(const uint8_t* data, size_t size, bool ext29, uint8_t pp,
         digest* d)
{
    g_isotp = elm327_isotp{};
    elm327_reader r;
    for (size_t i = 0; i < size; ++i) {
        const elm327_step s = elm327_take(r, data[i]);
        if (s.line) {
            ++d->lines;
            if (r.len > ELM327_LINE_MAX || r.line[r.len] != '\0' ||
                strlen(r.line) > r.len) {
                abort();
            }
            elm327_can_line f;
            const elm327_line kind = elm327_classify(r, ext29, &f);
            if (static_cast<uint8_t>(kind) >
                static_cast<uint8_t>(elm327_line::text)) {
                abort();
            }
            ++d->kinds[static_cast<uint8_t>(kind)];
            if (kind == elm327_line::frame) {
                ++d->frames;
                if (f.len < 1 || f.len > 8) abort();
                if (!ext29 && f.id > 0x7FFu) abort();
                if (ext29 && f.id > 0x1FFFFFFFu) abort();
                elm327_message m;
                const elm327_isotp_result res =
                    elm327_isotp_take(g_isotp, f, &m);
                if (res == elm327_isotp_result::complete) {
                    ++d->messages;
                    if (m.data == nullptr || m.len == 0 || m.len > 4095 ||
                        m.len > ECUIO_FRAME_DATA_MAX || m.id != f.id) {
                        abort();
                    }
                    /* An exact-size copy: ASan sees a read past the
                     * message */
                    std::unique_ptr<uint8_t[]> copy(new uint8_t[m.len]);
                    memcpy(copy.get(), m.data, m.len);
                    d->message_bytes += m.len;
                }
                for (const elm327_isotp_slot& slot : g_isotp.slots) {
                    if (slot.active &&
                        (slot.have > slot.expected || slot.expected < 8 ||
                         slot.expected > 4095 || slot.next_seq > 15)) {
                        abort();
                    }
                }
            }
            uint32_t mv = 0;
            if (elm327_parse_millivolts(r.line, r.len, &mv)) {
                if (mv > 999999u) abort();
                d->values += mv;
            }
            uint8_t value = 0;
            bool on = false;
            if (elm327_pps_value(r.line, r.len, pp, &value, &on)) {
                d->values += value + (on ? 256u : 0u);
            }
        }
        if (s.prompt) {
            ++d->prompts;
            (void)elm327_isotp_end_reply(g_isotp);
            for (const elm327_isotp_slot& slot : g_isotp.slots) {
                if (slot.active) abort();
            }
        }
    }
    d->dropped = g_isotp.dropped;
    d->malformed = g_isotp.malformed;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    /* Many replies' worth; a longer input only repeats them. */
    if (size < 1 || size > 65536) return 0;

    /* The first byte picks the addressing mode and the AT PPS parameter
     * asked for; the rest is what the adapter sent, in one chunk (the
     * reader takes a byte at a time, so chunking can't matter). */
    const bool ext29 = (data[0] & 0x80) != 0;
    const uint8_t pp = static_cast<uint8_t>(data[0] & 0x7F);
    digest a;
    digest b;
    run(data + 1, size - 1, ext29, pp, &a);
    run(data + 1, size - 1, ext29, pp, &b);
    if (!(a == b)) abort();
    return 0;
}
