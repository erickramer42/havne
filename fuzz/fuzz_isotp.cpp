// File/layer: fuzz/fuzz_isotp.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_isotp

#include <ecuio/core.h>

#include "isotp.h" /* core/src: pure */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <memory>

namespace {

struct digest {
    uint32_t results[6] = {};
    uint32_t message_bytes = 0;
    uint32_t frames_sent = 0;
    uint32_t round_trips = 0;
    bool operator==(const digest& o) const
    {
        return memcmp(results, o.results, sizeof results) == 0 &&
               message_bytes == o.message_bytes &&
               frames_sent == o.frames_sent && round_trips == o.round_trips;
    }
};

/* 32 KB and 4 KB: kept off the stack, as their transports keep them */
isotp_rx g_rx;
isotp_tx g_tx;

struct reader {
    const uint8_t* p;
    size_t n;
    bool more() const { return n > 0; }
    uint8_t byte()
    {
        if (n == 0) {
            return 0;
        }
        --n;
        return *p++;
    }
};

void check_slots(const isotp_rx& r)
{
    for (const isotp_rx_slot& s : r.slots) {
        if (s.active && (s.have > s.expected || s.expected < 8 ||
                         s.expected > ECUIO_ISOTP_MAX || s.next_seq > 15)) {
            abort();
        }
    }
}

void receive(reader in, digest* d)
{
    g_rx = isotp_rx{};
    g_rx.timed = true;
    int64_t now = 0;
    uint32_t singles = 0;
    while (in.more()) {
        isotp_can f;
        f.id = 0x7E8u + (in.byte() & 0x07u); /* eight IDs: every slot */
        f.len = static_cast<uint8_t>(in.byte() % 9);
        for (uint8_t i = 0; i < f.len; ++i) {
            f.data[i] = in.byte();
        }
        now += static_cast<int64_t>(in.byte()) * 1000; /* 0 to 255 ms */
        isotp_message m;
        const isotp_result res = isotp_take(g_rx, f, now, &m);
        const uint8_t k = static_cast<uint8_t>(res);
        if (k > static_cast<uint8_t>(isotp_result::dropped)) {
            abort();
        }
        ++d->results[k];
        if (res == isotp_result::complete) {
            if (m.data == nullptr || m.len == 0 || m.len > ECUIO_ISOTP_MAX ||
                m.id != f.id) {
                abort();
            }
            if (isotp_kind(f) == isotp_pci::single) {
                ++singles;
                if (m.len > 7 || m.data != f.data + 1)
                    abort();
            }
            /* An exact-size copy: ASan sees a read past the message */
            std::unique_ptr<uint8_t[]> copy(new uint8_t[m.len]);
            memcpy(copy.get(), m.data, m.len);
            d->message_bytes += m.len;
        }
        check_slots(g_rx);
        if (in.byte() & 1) {
            (void)isotp_expire(g_rx, now);
            for (const isotp_rx_slot& s : g_rx.slots) {
                if (s.active && now - s.last_us > ECUIO_ISOTP_N_CR_US) {
                    abort();
                }
            }
        }
    }
    /* each completed message began as a Single Frame or a First Frame */
    if (g_rx.completed > singles + g_rx.first_frames)
        abort();
    (void)isotp_end_all(g_rx);
    for (const isotp_rx_slot& s : g_rx.slots) {
        if (s.active)
            abort();
    }
}

void round_trip(reader in, digest* d)
{
    g_rx = isotp_rx{}; /* untimed: the input's waits are the sender's */
    g_tx = isotp_tx{};
    const uint16_t len = static_cast<uint16_t>(
        1 + ((in.byte() << 8 | in.byte()) % ECUIO_ISOTP_MAX));
    std::unique_ptr<uint8_t[]> msg(new uint8_t[len]);
    for (uint16_t i = 0; i < len; ++i) {
        msg[i] = static_cast<uint8_t>(i * 31 + len);
    }
    int64_t now = 0;
    isotp_can f;
    if (!isotp_tx_start(g_tx, 0x7E8, msg.get(), len, now, &f))
        abort();
    if (f.len != 8)
        abort();
    ++d->frames_sent;
    isotp_message m;
    isotp_result res = isotp_take(g_rx, f, now, &m);
    if (len <= 7) {
        if (res != isotp_result::complete || m.len != len ||
            memcmp(m.data, msg.get(), len) != 0 ||
            g_tx.state != isotp_tx_state::idle) {
            abort();
        }
        ++d->round_trips;
        return;
    }
    if (res != isotp_result::first)
        abort();

    uint8_t expect_seq = 1;
    int64_t last_cf = -1;
    int64_t stmin = 0;
    uint32_t in_block = 0; /* frames since the last ClearToSend */
    uint8_t block = 0;
    bool ended = false;
    while (!ended && in.more()) {
        if (g_tx.state == isotp_tx_state::awaiting_flow) {
            /* the receiver's Flow Control, as the input has it */
            isotp_can fc;
            fc.id = 0x7E0;
            fc.len = static_cast<uint8_t>(3 + in.byte() % 6);
            fc.data[0] = static_cast<uint8_t>(0x30 | (in.byte() % 4));
            fc.data[1] = in.byte();
            fc.data[2] = in.byte();
            now += static_cast<int64_t>(in.byte()) * 500;
            const isotp_flow_result fr = isotp_tx_flow(g_tx, fc, now);
            if (fr == isotp_flow_result::go) {
                block = fc.data[1];
                in_block = 0;
                stmin = isotp_stmin_us(fc.data[2]);
            } else if (fr == isotp_flow_result::overflow ||
                       fr == isotp_flow_result::abandoned) {
                if (g_tx.state != isotp_tx_state::idle)
                    abort();
                ended = true;
            } else if (fr == isotp_flow_result::ignored) {
                abort(); /* a Flow Control frame was due */
            }
            continue;
        }
        now += static_cast<int64_t>(in.byte()) * 100;
        const isotp_tx_step st = isotp_tx_next(g_tx, now, &f);
        if (st == isotp_tx_step::timed_out) {
            ended = true;
        } else if (st == isotp_tx_step::frame) {
            ++d->frames_sent;
            if (f.len != 8 || (f.data[0] >> 4) != 2 ||
                (f.data[0] & 0x0F) != expect_seq) {
                abort();
            }
            /* the first frame after ClearToSend goes at once, the rest
             * STmin apart, at most a block of them */
            if (in_block > 0 && now - last_cf < stmin)
                abort();
            if (block != 0 && ++in_block > block)
                abort();
            if (block == 0)
                ++in_block;
            expect_seq = static_cast<uint8_t>((expect_seq + 1) & 0x0F);
            last_cf = now;
            res = isotp_take(g_rx, f, now, &m);
            if (g_tx.state == isotp_tx_state::idle) {
                /* the last frame: the message is whole and the same */
                if (res != isotp_result::complete || m.len != len ||
                    memcmp(m.data, msg.get(), len) != 0) {
                    abort();
                }
                ++d->round_trips;
                ended = true;
            } else if (res != isotp_result::pending) {
                abort();
            }
        } else if (st == isotp_tx_step::idle) {
            abort(); /* only the paths above end a send */
        }
    }
    if (ended && g_tx.state != isotp_tx_state::idle)
        abort();
    if (ended &&
        isotp_tx_next(g_tx, now + 1000000, &f) != isotp_tx_step::idle) {
        abort(); /* nothing more after the end */
    }
}

void run(const uint8_t* data, size_t size, digest* d)
{
    if (size == 0) {
        return;
    }
    reader in{data + 1, size - 1};
    if (data[0] & 1) {
        round_trip(in, d);
    } else {
        receive(in, d);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::unique_ptr<uint8_t[]> copy(new uint8_t[size ? size : 1]);
    if (size > 0) {
        memcpy(copy.get(), data, size);
    }
    digest a, b;
    run(copy.get(), size, &a);
    run(copy.get(), size, &b);
    if (!(a == b))
        abort();
    return 0;
}
