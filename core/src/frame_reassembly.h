// File/layer: core/src/frame_reassembly.h | internal, C++17, Qt-free; ISO-TP
//             messages from frames as they crossed the bus, each with the
//             frames that carried it (ADR-0054 section 1)
// Testing: UNIT: ExportTest, ScrubFramesTest (test_core)
//
// What a scrubbed export reassembles before it classifies: ecuio_export
// over a recording's frames.csv, ecuio_scrub_frames over frames in memory.
// Both take the reader's rules, so the same frames are left out either way.

#ifndef ECUIO_FRAME_REASSEMBLY_H
#define ECUIO_FRAME_REASSEMBLY_H

#include "isotp.h"
#include "recording_check.h" /* iso15765_id */
#include "scrub.h"           /* segment_reply_id */

#include "ecuio/core.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace ecuio_frame_reassembly {

/* A frame by its place and time: a recording's row and host_us, or a
 * frame in memory and its timestamp_us */
struct seg {
    uint64_t index = 0;
    uint64_t host_us = 0;
};

/* A message's end: whole (reassembled from its frames) or torn (its frames
 * no message uses) */
struct message_end {
    bool whole = false;
    uint32_t id = 0;
    bool extended = false;
    std::vector<seg> frames;
    std::vector<uint8_t> data;
};

/* ISO-TP messages from frames as they crossed the bus, each with the
 * frames that carried it: the reader's rules (two First Frames on one ID
 * end both, N_Cr on each seg's host_us) */
class reassembly
{
public:
    reassembly() : iso_(new isotp_rx) { iso_->timed = true; }

    /* A frame as it crossed the bus on ISO 15765-4's IDs: no adapter
     * reassembled it, and its first nibble is a PCI's */
    static bool on_the_wire(const ecuio_frame_t& f)
    {
        return (f.flags &
                (ECUIO_FRAME_FLAG_REASSEMBLED | ECUIO_FRAME_FLAG_ERROR)) == 0 &&
               f.data_length >= 1 && f.data_length <= 8 &&
               (f.data[0] >> 4) <= 3 &&
               ecuio_check::iso15765_id(f.arbitration_id,
                                        (f.flags & ECUIO_FRAME_FLAG_EXTENDED) !=
                                            0);
    }

    /* A frame on a diagnostic reply ID that no adapter reassembled and
     * that is too long for classic ISO-TP: a CAN FD frame, whose ISO-TP
     * form (ISO 15765-2:2016's Single and First Frame escapes) isn't read,
     * so its message can't be classified. A scrubbed export leaves it out,
     * as it does a torn segment there. */
    static bool unread_reply(const ecuio_frame_t& f)
    {
        return (f.flags &
                (ECUIO_FRAME_FLAG_REASSEMBLED | ECUIO_FRAME_FLAG_ERROR)) == 0 &&
               f.data_length > 8 &&
               ecuio_scrub::segment_reply_id(
                   f.arbitration_id,
                   (f.flags & ECUIO_FRAME_FLAG_EXTENDED) != 0);
    }

    /* Row s's frame f, which on_the_wire takes: the messages it ends go in
     * out. False for a frame that stands alone (a Flow Control). */
    bool take(const seg& s, const ecuio_frame_t& f,
              std::vector<message_end>& out)
    {
        const auto now = static_cast<int64_t>(s.host_us);
        if (isotp_expire(*iso_, now) > 0) {
            reconcile(out);
        }
        isotp_can c;
        c.id = f.arbitration_id;
        c.len = static_cast<uint8_t>(f.data_length);
        std::memcpy(c.data, f.data, f.data_length);
        const bool ext = (f.flags & ECUIO_FRAME_FLAG_EXTENDED) != 0;
        const isotp_pci kind = isotp_kind(c);
        if (kind == isotp_pci::first) {
            isotp_rx_slot* open = isotp_rx_find(*iso_, c.id);
            if (open != nullptr) {
                open->active = false;
                ++iso_->dropped;
                torn(c.id, ext, &s, out);
                return true;
            }
        }
        isotp_message m;
        switch (isotp_take(*iso_, c, now, &m)) {
        case isotp_result::complete: {
            if (kind == isotp_pci::single) {
                torn(c.id, ext, nullptr, out); /* one it ended, if any */
            }
            message_end e;
            e.whole = true;
            e.id = c.id;
            e.extended = ext;
            e.frames = take_held(c.id);
            e.frames.push_back(s);
            e.data.assign(m.data, m.data + m.len);
            out.push_back(std::move(e));
            break;
        }
        case isotp_result::first:
            held_.push_back(held{c.id, ext, {s}});
            break;
        case isotp_result::pending:
            for (held& h : held_) {
                if (h.id == c.id) {
                    h.frames.push_back(s);
                }
            }
            break;
        case isotp_result::flow:
            return false;
        case isotp_result::malformed:
        case isotp_result::dropped:
            /* A malformed Single Frame leaves the ID's slot open; closing
             * it tears the whole message, so its later segments are torn
             * too, never pending into a message no frame list holds */
            if (isotp_rx_slot* open = isotp_rx_find(*iso_, c.id)) {
                open->active = false;
                ++iso_->dropped;
            }
            torn(c.id, ext, &s, out);
            break;
        }
        reconcile(out);
        return true;
    }

    /* The messages still under way at the end are torn */
    void finish(std::vector<message_end>& out)
    {
        for (held& h : held_) {
            message_end e;
            e.id = h.id;
            e.extended = h.extended;
            e.frames = std::move(h.frames);
            out.push_back(std::move(e));
        }
        held_.clear();
    }

private:
    struct held {
        uint32_t id = 0;
        bool extended = false;
        std::vector<seg> frames;
    };

    std::vector<seg> take_held(uint32_t id)
    {
        for (size_t i = 0; i < held_.size(); ++i) {
            if (held_[i].id == id) {
                std::vector<seg> f = std::move(held_[i].frames);
                held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
                return f;
            }
        }
        return {};
    }

    /* id's message under way, and the frame last when given, as torn */
    void torn(uint32_t id, bool ext, const seg* last,
              std::vector<message_end>& out)
    {
        message_end e;
        e.id = id;
        e.extended = ext;
        e.frames = take_held(id);
        if (last != nullptr) {
            e.frames.push_back(*last);
        }
        if (!e.frames.empty()) {
            out.push_back(std::move(e));
        }
    }

    /* A message ISO-TP gave up on (N_Cr, or evicted for a newer one) */
    void reconcile(std::vector<message_end>& out)
    {
        for (size_t i = 0; i < held_.size();) {
            if (isotp_rx_find(*iso_, held_[i].id) == nullptr) {
                message_end e;
                e.id = held_[i].id;
                e.extended = held_[i].extended;
                e.frames = std::move(held_[i].frames);
                out.push_back(std::move(e));
                held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    std::unique_ptr<isotp_rx> iso_;
    std::vector<held> held_;
};

/* A frame read as ISO-TP in one addressing layout: normal (its PCI at
 * byte 0) or extended (byte 0 a target address, its PCI at byte 1). Read
 * leniently, classic or CAN FD (ISO 15765-2:2016's Single and First Frame
 * escapes), for what is left out or checked, never for what is kept. Not
 * extended on ISO 15765-4's IDs, which that standard gives normal
 * addressing (a frame there that isn't on the wire is an adapter's payload
 * with no PCI), and a Single Frame takes the escape's form in a frame
 * longer than 8 bytes and the nibble's in one of 8 or less, as ISO
 * 15765-2:2016 gives them. */
struct tp_read {
    enum kind_t { none, single, first, consecutive, other };
    kind_t kind = none;
    size_t at = 0;             /* where its payload starts */
    size_t end = 0;            /* where the frame's bytes end */
    uint32_t length = 0;       /* the message's length its PCI gives */
    bool length_valid = false; /* that length fits the frame and the form */

    /* The layouts read: normal, extended */
    static constexpr size_t kLayouts = 2;

    static tp_read of(const ecuio_frame_t& f, size_t layout)
    {
        tp_read r;
        const size_t o = layout;
        const size_t n = f.data_length;
        if (n <= o ||
            (o == 1 && ecuio_check::iso15765_id(
                           f.arbitration_id,
                           (f.flags & ECUIO_FRAME_FLAG_EXTENDED) != 0))) {
            return r;
        }
        const uint8_t* d = f.data;
        r.end = n;
        r.at = n;
        switch (d[o] >> 4) {
        case 0:
            r.kind = single;
            if ((d[o] & 0x0F) != 0 && n > 8) {
                r.kind = other; /* no Single Frame's form */
            } else if ((d[o] & 0x0F) != 0) {
                r.at = o + 1;
                r.length = d[o] & 0x0Fu;
                r.length_valid = r.at + r.length <= n;
            } else if (n > 8 && o + 2 <= n) {
                /* the escape: its length in the next byte, in a frame
                 * longer than classic CAN's alone */
                r.at = o + 2;
                r.length = d[o + 1];
                r.length_valid = r.length >= 1 && r.at + r.length <= n;
            }
            return r;
        case 1:
            r.kind = first;
            if (o + 2 > n) {
                return r;
            }
            r.length = (uint32_t{d[o] & 0x0Fu} << 8) | d[o + 1];
            r.at = o + 2;
            if (r.length == 0 && o + 6 <= n) {
                /* the escape: a 32-bit length after the 12 bits of zero;
                 * one that can't be right reads as no escape, a bad
                 * length with its payload after the 12 bits */
                const uint32_t escaped = (uint32_t{d[o + 2]} << 24) |
                                         (uint32_t{d[o + 3]} << 16) |
                                         (uint32_t{d[o + 4]} << 8) | d[o + 5];
                if (escaped > n - (o + 6) && escaped <= ECUIO_FRAME_DATA_MAX) {
                    r.length = escaped;
                    r.at = o + 6;
                }
            }
            r.length_valid =
                r.length > n - r.at && r.length <= ECUIO_FRAME_DATA_MAX;
            return r;
        case 2:
            r.kind = consecutive;
            r.at = o + 1;
            return r;
        default:
            r.kind = other;
            return r;
        }
    }
};

/* A frame tp_read reads: on no ID's reassembly, and not one an adapter
 * reassembled, an error or a remote frame */
inline bool tp_readable(const ecuio_frame_t& f)
{
    return (f.flags & (ECUIO_FRAME_FLAG_REASSEMBLED | ECUIO_FRAME_FLAG_ERROR |
                       ECUIO_FRAME_FLAG_REMOTE)) == 0 &&
           f.data_length >= 1 && f.data_length <= ECUIO_FRAME_DATA_MAX;
}

/* What a message's frames are followed for, and the most of it gathered:
 * no ISO-TP message is longer */
constexpr size_t kFollowMax = ECUIO_FRAME_DATA_MAX;

/* The keys followed at once; past that the oldest ends */
constexpr size_t kOpenMax = 1024;

inline uint64_t tp_key(const ecuio_frame_t& f, size_t layout)
{
    return uint64_t{f.arbitration_id} |
           ((f.flags & ECUIO_FRAME_FLAG_EXTENDED) != 0 ? 1ull << 32 : 0) |
           (uint64_t{layout} << 33);
}

/* Identification replies on IDs the reassembly doesn't take (a manufacturer
 * module's, or a listening capture's), read in each layout (tp_read): a
 * Single or First Frame whose payload after its PCI answers an
 * identification request (scrub::identification) is one, whatever its
 * length says, and so is each Consecutive Frame on its ID and layout
 * after it, whatever its sequence number, until a frame there that isn't
 * one or until kFollowMax bytes. Fail closed: such a frame is left out, and
 * the message's bytes, up to its length (or all of them, when its length
 * can't be right), go to the check as identity values. */
class other_id_identification
{
public:
    /* Frame f, which on_the_wire doesn't take: true when it belongs to an
     * identification reply; *done then holds a message's bytes once it's
     * whole (a Single Frame, or the Consecutive Frame that completes it) */
    bool take(const ecuio_frame_t& f, std::vector<uint8_t>* done)
    {
        done->clear();
        if (!tp_readable(f)) {
            return false;
        }
        bool ours = false;
        for (size_t layout = 0; layout < tp_read::kLayouts; ++layout) {
            ours = take(f, layout, done) || ours;
        }
        return ours;
    }

    /* The bytes of each reply that ended short of its length */
    void finish(std::vector<std::vector<uint8_t>>& out)
    {
        while (!open_.empty()) {
            end(open_.begin());
        }
        for (std::vector<uint8_t>& m : short_) {
            out.push_back(std::move(m));
        }
        short_.clear();
    }

private:
    struct reply {
        size_t expected = 0; /* its length, or kFollowMax */
        bool valid = false;  /* its length is one */
        bool given = false;
        size_t seen = 0; /* its frames' payload bytes so far */
        std::vector<uint8_t> bytes;
    };

    bool take(const ecuio_frame_t& f, size_t layout, std::vector<uint8_t>* done)
    {
        const uint64_t key = tp_key(f, layout);
        const tp_read r = tp_read::of(f, layout);
        auto it = open_.find(key);
        if (r.kind == tp_read::consecutive) {
            if (it == open_.end()) {
                return false;
            }
            reply& rp = it->second;
            const size_t n = r.end - r.at;
            rp.seen += n;
            if (!rp.given) {
                const size_t take =
                    std::min<size_t>(n, rp.expected - rp.bytes.size());
                rp.bytes.insert(rp.bytes.end(), f.data + r.at,
                                f.data + r.at + take);
                if (rp.valid && rp.bytes.size() == rp.expected) {
                    rp.given = true;
                    give(std::move(rp.bytes), done);
                }
            }
            if (rp.seen >= kFollowMax) {
                end(it);
            }
            return true;
        }
        if (it != open_.end()) {
            end(it);
        }
        const uint8_t* payload = f.data + r.at;
        const size_t held = r.end - r.at;
        if ((r.kind != tp_read::single && r.kind != tp_read::first) ||
            !ecuio_scrub::identification(payload, held)) {
            return false;
        }
        if (r.kind == tp_read::single) {
            const size_t n = r.length_valid ? r.length : held;
            give(std::vector<uint8_t>(payload, payload + n), done);
            return true;
        }
        if (open_.size() >= kOpenMax) {
            end(open_.begin());
        }
        reply& rp = open_[key];
        rp.valid = r.length_valid;
        rp.expected = r.length_valid ? r.length : kFollowMax;
        rp.seen = held;
        rp.bytes.assign(payload, payload + std::min<size_t>(held, rp.expected));
        return true;
    }

    /* A whole message to *done, or after one there to finish()'s list */
    void give(std::vector<uint8_t> m, std::vector<uint8_t>* done)
    {
        if (done->empty()) {
            *done = std::move(m);
        } else {
            short_.push_back(std::move(m));
        }
    }

    /* A follow ends: a reply not yet whole goes to finish()'s list */
    void end(const std::map<uint64_t, reply>::iterator& it)
    {
        if (!it->second.given && !it->second.bytes.empty()) {
            short_.push_back(std::move(it->second.bytes));
        }
        open_.erase(it);
    }

    std::map<uint64_t, reply> open_;
    std::vector<std::vector<uint8_t>> short_;
};

/* A message any_id_messages joined, and whether the export writes any
 * frame of it */
struct joined {
    bool written = false;
    std::vector<uint8_t> data;
};

/* ISO-TP-shaped messages on any ID, for the second check only: the frames
 * on_the_wire doesn't take, read in each layout (tp_read), a First Frame's
 * payload joined with each Consecutive Frame after it on its ID and
 * layout, whatever its sequence number, until its length (kFollowMax when
 * its length can't be right), a frame there that isn't one, or the end.
 * Joining frames that aren't one message can only fail a check, never
 * pass one; nothing more is left out. */
class any_id_messages
{
public:
    /* Frame f, which tp_readable takes; written when the export writes it.
     * The messages it ends go in out. */
    void take(const ecuio_frame_t& f, bool written, std::vector<joined>& out)
    {
        for (size_t layout = 0; layout < tp_read::kLayouts; ++layout) {
            const uint64_t key = tp_key(f, layout);
            const tp_read r = tp_read::of(f, layout);
            auto it = open_.find(key);
            if (r.kind == tp_read::consecutive) {
                if (it == open_.end()) {
                    continue;
                }
                message& m = it->second;
                const size_t take = std::min<size_t>(
                    r.end - r.at, m.expected - m.j.data.size());
                m.j.data.insert(m.j.data.end(), f.data + r.at,
                                f.data + r.at + take);
                m.j.written = m.j.written || written;
                if (m.j.data.size() >= m.expected) {
                    end(it, out);
                }
                continue;
            }
            if (it != open_.end()) {
                end(it, out);
            }
            if (r.kind != tp_read::first) {
                continue; /* a Single Frame is checked as its frame */
            }
            if (open_.size() >= kOpenMax) {
                end(open_.begin(), out);
            }
            message& m = open_[key];
            m.expected = r.length_valid ? r.length : kFollowMax;
            m.j.written = written;
            m.j.data.assign(f.data + r.at,
                            f.data + r.at +
                                std::min<size_t>(r.end - r.at, m.expected));
        }
    }

    /* The messages still open at the end, with what each holds */
    void finish(std::vector<joined>& out)
    {
        while (!open_.empty()) {
            end(open_.begin(), out);
        }
    }

private:
    struct message {
        size_t expected = 0;
        joined j;
    };

    void end(const std::map<uint64_t, message>::iterator& it,
             std::vector<joined>& out)
    {
        out.push_back(std::move(it->second.j));
        open_.erase(it);
    }

    std::map<uint64_t, message> open_;
};

} // namespace ecuio_frame_reassembly

#endif /* ECUIO_FRAME_REASSEMBLY_H */
