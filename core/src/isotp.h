// File/layer: core/src/isotp.h | internal, C++17, Qt-free, header-only;
//             no platform headers
// Testing: UNIT: IsoTpTest (test_socketcan)
//
// ISO 15765-2 on classic CAN (ADR-0040 section 1): a message of up to
// 4095 bytes in CAN frames, each frame's first byte or two its Protocol
// Control Information (PCI). A Single Frame carries 1 to 7 bytes; a
// longer message is a First Frame (its length, then 6 bytes), then
// Consecutive Frames of 7, numbered 1 to 15 and round again to 0, paced
// by the receiver's Flow Control frame. Pure: no I/O, no clock (the
// caller passes the time in), no allocation, so every rule is a unit
// test on every platform.

#ifndef ECUIO_ISOTP_H
#define ECUIO_ISOTP_H

#include "ecuio/core.h"

#include <cstdint>
#include <cstring>

/* Timeouts, ISO 15765-4's Table 6 (legislated OBD): N_As, a frame's
 * transmission; N_Bs, a sender waiting for Flow Control; N_Cr, a
 * receiver waiting for the next Consecutive Frame. */
constexpr int64_t ECUIO_ISOTP_N_AS_US = 25000;
constexpr int64_t ECUIO_ISOTP_N_BS_US = 75000;
constexpr int64_t ECUIO_ISOTP_N_CR_US = 150000;

/* Messages under way at once: one per reply ID, and ISO 15765-4 has up
 * to eight ECUs answering in parallel. Past this the oldest gives way. */
constexpr uint32_t ECUIO_ISOTP_SLOTS = 8;

/* The longest message: a First Frame's 12-bit length. ISO 15765-2's
 * escape to a 32-bit length isn't taken, since a longer message doesn't
 * fit a frame (ECUIO_FRAME_DATA_MAX). */
constexpr uint16_t ECUIO_ISOTP_MAX = 4095;
static_assert(ECUIO_ISOTP_MAX <= ECUIO_FRAME_DATA_MAX,
              "a whole message fits one ecuio_frame_t");

/* The bytes past a message in a frame core sends: ISO 15765-4 has every
 * diagnostic frame 8 bytes long and leaves the padding undefined. */
constexpr uint8_t ECUIO_ISOTP_PAD = 0x00;

/* Wait frames a sender takes in a row before it gives up. ISO 15765-2
 * bounds them only at the receiver (N_WFTmax), so a receiver that waits
 * forever would hold a send forever; this bound is core's. */
constexpr uint32_t ECUIO_ISOTP_WAIT_MAX = 10;

/* One classic CAN frame: its ID and 0 to 8 bytes. */
struct isotp_can {
    uint32_t id = 0;
    uint8_t len = 0;
    uint8_t data[8] = {};
};

enum class isotp_pci : uint8_t {
    single = 0,
    first = 1,
    consecutive = 2,
    flow = 3,
    other = 4 /* an empty frame, or a PCI type ISO 15765-2 doesn't define */
};

inline isotp_pci isotp_kind(const isotp_can& f)
{
    if (f.len == 0) {
        return isotp_pci::other;
    }
    const uint8_t t = static_cast<uint8_t>(f.data[0] >> 4);
    return t <= 3 ? static_cast<isotp_pci>(t) : isotp_pci::other;
}

/* The receiver's Flow Control frame (ISO 15765-4's Table 7): ClearToSend,
 * block size 0 and STmin 0, so the ECU sends the rest at once; never a
 * wait frame. Sent once per First Frame, on isotp_flow_control_id's ID. */
inline isotp_can isotp_flow_control(uint32_t to_id)
{
    isotp_can f;
    f.id = to_id;
    f.len = 8;
    std::memset(f.data, ECUIO_ISOTP_PAD, sizeof f.data);
    f.data[0] = 0x30;
    f.data[1] = 0x00;
    f.data[2] = 0x00;
    return f;
}

/* The ID a Flow Control frame goes out on for a reply from rx to a
 * request on tx: tx, unless tx is the functional request ID (0x7DF,
 * 0x18DB33F1), when it's the physical request ID paired with rx (8 below
 * an 11-bit one, the swapped pair of an 18DA one). ISO 15765-4:2005
 * section 6.3.2.1: the physical request ID "shall only be used for
 * physically addressed FlowControl frames", and an ECU answering a
 * functional request waits for its Flow Control there (6.3.1). */
inline uint32_t isotp_flow_control_id(uint32_t rx, uint32_t tx)
{
    if (tx != 0x7DFu && tx != 0x18DB33F1u) {
        return tx;
    }
    if (rx >= 9 && rx <= 0x7FFu) {
        return rx - 8;
    }
    if ((rx >> 16) == 0x18DAu) {
        return 0x18DA0000u | ((rx & 0xFFu) << 8) | ((rx >> 8) & 0xFFu);
    }
    return tx;
}

/* STmin's byte as microseconds: 0x00 to 0x7F milliseconds, 0xF1 to 0xF9
 * 100 to 900 microseconds, and any other value, reserved, as the longest,
 * 127 ms, which ISO 15765-2 has a sender take. */
inline int64_t isotp_stmin_us(uint8_t b)
{
    if (b <= 0x7F) {
        return static_cast<int64_t>(b) * 1000;
    }
    if (b >= 0xF1 && b <= 0xF9) {
        return static_cast<int64_t>(b - 0xF0) * 100;
    }
    return 127000;
}

/* ---- The receiver --------------------------------------------------- */

struct isotp_rx_slot {
    uint32_t id = 0;
    bool active = false;
    uint16_t expected = 0; /* the message's length, from its First Frame */
    uint16_t have = 0;
    uint8_t next_seq = 0; /* the Consecutive Frame index due, 0-15 */
    uint32_t started = 0; /* when, in First Frames, to pick the oldest */
    int64_t last_us = 0;  /* its last frame's time, for N_Cr */
    uint8_t data[ECUIO_ISOTP_MAX] = {};
};

/* 32 KB: it lives with its transport, never on a worker's stack. */
struct isotp_rx {
    isotp_rx_slot slots[ECUIO_ISOTP_SLOTS];
    /* N_Cr applies: frames carry the times they arrived. An ELM327's
     * lines come in a burst at its prompt, which ends a reply instead
     * (isotp_end_all). */
    bool timed = false;
    uint32_t first_frames = 0;
    uint32_t completed = 0;
    /* Messages begun and never finished: a gap in the sequence, N_Cr, a
     * new First Frame or a Single Frame on the same ID, eviction, the
     * end of a reply. */
    uint32_t dropped = 0;
    /* Frames no message could use: a bad PCI, a short frame, a
     * Consecutive Frame with nothing begun. */
    uint32_t malformed = 0;
};

/* A whole message: its CAN ID and its bytes, the service byte first.
 * data points into the frame given (a Single Frame) or the slot (a
 * segmented message) and stays valid until the next take. */
struct isotp_message {
    uint32_t id = 0;
    const uint8_t* data = nullptr;
    uint16_t len = 0;
};

enum class isotp_result : uint8_t {
    complete,  /* *out holds a whole message */
    first,     /* a First Frame began one: a receiver that does flow
                  control sends isotp_flow_control() now */
    pending,   /* a Consecutive Frame, more to come */
    flow,      /* a Flow Control frame: a sender's business */
    malformed, /* no message could use it */
    dropped    /* it ended a message unfinished */
};

inline isotp_rx_slot* isotp_rx_find(isotp_rx& r, uint32_t id)
{
    for (isotp_rx_slot& s : r.slots) {
        if (s.active && s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

/* A frame received at now_us. Frames shorter than 8 bytes are taken when
 * they hold what their PCI says (ECUs outside legislated OBD needn't
 * pad), except a First Frame, which ISO 15765-2 has fill its frame. */
inline isotp_result isotp_take(isotp_rx& r, const isotp_can& f, int64_t now_us,
                               isotp_message* out)
{
    isotp_rx_slot* s = isotp_rx_find(r, f.id);
    switch (isotp_kind(f)) {
    case isotp_pci::single: {
        const uint8_t n = f.data[0] & 0x0F;
        if (n == 0 || n > 7 || f.len < 1 + n) {
            ++r.malformed;
            return isotp_result::malformed;
        }
        if (s != nullptr) {
            s->active = false;
            ++r.dropped;
        }
        out->id = f.id;
        out->data = f.data + 1;
        out->len = n;
        ++r.completed;
        return isotp_result::complete;
    }
    case isotp_pci::first: {
        const uint16_t n =
            static_cast<uint16_t>(((f.data[0] & 0x0F) << 8) | f.data[1]);
        if (f.len != 8 || n < 8) {
            /* n == 0 is the escape to a 32-bit length; 1 to 7 fit a
             * Single Frame */
            ++r.malformed;
            return isotp_result::malformed;
        }
        if (s != nullptr) {
            ++r.dropped; /* the same ID starts over */
        } else {
            for (isotp_rx_slot& c : r.slots) {
                if (!c.active) {
                    s = &c;
                    break;
                }
            }
            if (s == nullptr) {
                s = &r.slots[0];
                for (isotp_rx_slot& c : r.slots) {
                    if (c.started < s->started) {
                        s = &c;
                    }
                }
                ++r.dropped; /* the oldest gives way */
            }
        }
        s->id = f.id;
        s->active = true;
        s->expected = n;
        std::memcpy(s->data, f.data + 2, 6);
        s->have = 6;
        s->next_seq = 1;
        s->started = ++r.first_frames;
        s->last_us = now_us;
        return isotp_result::first;
    }
    case isotp_pci::consecutive: {
        if (s == nullptr) {
            ++r.malformed;
            return isotp_result::malformed;
        }
        const uint16_t left = static_cast<uint16_t>(s->expected - s->have);
        const uint16_t take = left < 7 ? left : 7;
        if ((f.data[0] & 0x0F) != s->next_seq || f.len < 1 + take ||
            (r.timed && now_us - s->last_us > ECUIO_ISOTP_N_CR_US)) {
            s->active = false;
            ++r.dropped;
            return isotp_result::dropped;
        }
        std::memcpy(s->data + s->have, f.data + 1, take);
        s->have = static_cast<uint16_t>(s->have + take);
        s->next_seq = static_cast<uint8_t>((s->next_seq + 1) & 0x0F);
        s->last_us = now_us;
        if (s->have < s->expected) {
            return isotp_result::pending;
        }
        s->active = false;
        out->id = f.id;
        out->data = s->data;
        out->len = s->expected;
        ++r.completed;
        return isotp_result::complete;
    }
    case isotp_pci::flow:
        return isotp_result::flow;
    case isotp_pci::other:
        break;
    }
    ++r.malformed;
    return isotp_result::malformed;
}

/* Ends every message whose next Consecutive Frame is later than N_Cr at
 * now_us; a timed receiver's worker calls it each pass. Returns how many. */
inline uint32_t isotp_expire(isotp_rx& r, int64_t now_us)
{
    uint32_t n = 0;
    if (!r.timed) {
        return 0;
    }
    for (isotp_rx_slot& s : r.slots) {
        if (s.active && now_us - s.last_us > ECUIO_ISOTP_N_CR_US) {
            s.active = false;
            ++n;
        }
    }
    r.dropped += n;
    return n;
}

/* Ends every message under way (an ELM327's prompt: it no longer
 * listens). Returns how many. */
inline uint32_t isotp_end_all(isotp_rx& r)
{
    uint32_t n = 0;
    for (isotp_rx_slot& s : r.slots) {
        if (s.active) {
            s.active = false;
            ++n;
        }
    }
    r.dropped += n;
    return n;
}

/* ---- The sender ----------------------------------------------------- */

enum class isotp_tx_state : uint8_t {
    idle,
    awaiting_flow, /* a First Frame or a block sent: Flow Control due */
    sending        /* Consecutive Frames due, STmin apart */
};

/* 4 KB: as the receiver, it lives with its owner. */
struct isotp_tx {
    isotp_tx_state state = isotp_tx_state::idle;
    uint32_t id = 0;
    uint16_t len = 0;
    uint16_t sent = 0;
    uint8_t seq = 0;
    uint8_t block = 0;      /* the receiver's block size, 0 = no limit */
    uint8_t block_left = 0; /* Consecutive Frames left in this block */
    int64_t stmin_us = 0;
    /* awaiting_flow: N_Bs's end; sending: the next frame's earliest time */
    int64_t due_us = 0;
    uint32_t waits = 0; /* wait frames in a row */
    uint8_t data[ECUIO_ISOTP_MAX] = {};
};

inline void isotp_pad(isotp_can* f, uint8_t used)
{
    std::memset(f->data + used, ECUIO_ISOTP_PAD, sizeof f->data - used);
    f->len = 8;
}

/* Starts a message of 1 to 4095 bytes to id at now_us: *out is its Single
 * Frame (the send is over) or its First Frame (Flow Control is due within
 * N_Bs). False, nothing written, for a bad length or a send under way. */
inline bool isotp_tx_start(isotp_tx& t, uint32_t id, const uint8_t* msg,
                           uint16_t len, int64_t now_us, isotp_can* out)
{
    if (t.state != isotp_tx_state::idle || len == 0 || len > ECUIO_ISOTP_MAX) {
        return false;
    }
    out->id = id;
    if (len <= 7) {
        out->data[0] = static_cast<uint8_t>(len);
        std::memcpy(out->data + 1, msg, len);
        isotp_pad(out, static_cast<uint8_t>(1 + len));
        return true;
    }
    t.id = id;
    t.len = len;
    std::memcpy(t.data, msg, len);
    out->data[0] = static_cast<uint8_t>(0x10 | (len >> 8));
    out->data[1] = static_cast<uint8_t>(len & 0xFF);
    std::memcpy(out->data + 2, msg, 6);
    out->len = 8;
    t.sent = 6;
    t.seq = 1;
    t.waits = 0;
    t.state = isotp_tx_state::awaiting_flow;
    t.due_us = now_us + ECUIO_ISOTP_N_BS_US;
    return true;
}

enum class isotp_flow_result : uint8_t {
    go,        /* ClearToSend: Consecutive Frames from now */
    wait,      /* a wait frame: N_Bs starts again */
    overflow,  /* the receiver can't take the message: the send ends */
    abandoned, /* ECUIO_ISOTP_WAIT_MAX wait frames, or a bad Flow Control
                  frame: the send ends */
    ignored    /* no Flow Control was due */
};

/* The receiver's Flow Control frame, at now_us. */
inline isotp_flow_result isotp_tx_flow(isotp_tx& t, const isotp_can& f,
                                       int64_t now_us)
{
    if (t.state != isotp_tx_state::awaiting_flow ||
        isotp_kind(f) != isotp_pci::flow) {
        return isotp_flow_result::ignored;
    }
    if (f.len < 3) {
        t.state = isotp_tx_state::idle;
        return isotp_flow_result::abandoned;
    }
    switch (f.data[0] & 0x0F) {
    case 0: /* ClearToSend */
        t.block = f.data[1];
        t.block_left = t.block;
        t.stmin_us = isotp_stmin_us(f.data[2]);
        t.waits = 0;
        t.state = isotp_tx_state::sending;
        t.due_us = now_us;
        return isotp_flow_result::go;
    case 1: /* Wait */
        if (++t.waits >= ECUIO_ISOTP_WAIT_MAX) {
            t.state = isotp_tx_state::idle;
            return isotp_flow_result::abandoned;
        }
        t.due_us = now_us + ECUIO_ISOTP_N_BS_US;
        return isotp_flow_result::wait;
    case 2: /* Overflow */
        t.state = isotp_tx_state::idle;
        return isotp_flow_result::overflow;
    default:
        t.state = isotp_tx_state::idle;
        return isotp_flow_result::abandoned;
    }
}

enum class isotp_tx_step : uint8_t {
    frame,     /* *out is the next Consecutive Frame (the last one when
                  the state is idle again) */
    later,     /* nothing due before t.due_us */
    timed_out, /* N_Bs passed with no Flow Control: the send ends */
    idle       /* no send under way */
};

/* What the send has due at now_us. */
inline isotp_tx_step isotp_tx_next(isotp_tx& t, int64_t now_us, isotp_can* out)
{
    switch (t.state) {
    case isotp_tx_state::idle:
        return isotp_tx_step::idle;
    case isotp_tx_state::awaiting_flow:
        if (now_us > t.due_us) {
            t.state = isotp_tx_state::idle;
            return isotp_tx_step::timed_out;
        }
        return isotp_tx_step::later;
    case isotp_tx_state::sending:
        break;
    }
    if (now_us < t.due_us) {
        return isotp_tx_step::later;
    }
    const uint16_t left = static_cast<uint16_t>(t.len - t.sent);
    const uint8_t take = static_cast<uint8_t>(left < 7 ? left : 7);
    out->id = t.id;
    out->data[0] = static_cast<uint8_t>(0x20 | t.seq);
    std::memcpy(out->data + 1, t.data + t.sent, take);
    isotp_pad(out, static_cast<uint8_t>(1 + take));
    t.sent = static_cast<uint16_t>(t.sent + take);
    t.seq = static_cast<uint8_t>((t.seq + 1) & 0x0F);
    if (t.sent == t.len) {
        t.state = isotp_tx_state::idle;
    } else if (t.block != 0 && --t.block_left == 0) {
        t.state = isotp_tx_state::awaiting_flow;
        t.due_us = now_us + ECUIO_ISOTP_N_BS_US;
    } else {
        t.due_us = now_us + t.stmin_us;
    }
    return isotp_tx_step::frame;
}

#endif /* ECUIO_ISOTP_H */
