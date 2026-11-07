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

/* Identification replies on IDs the reassembly doesn't take (a manufacturer
 * module's, or a listening capture's): a Single or First Frame whose
 * payload answers an identification request (scrub::identification) is
 * one, and so is every Consecutive Frame on its ID until that ID's next
 * Single or First Frame, whatever its sequence number. Fail closed: such a
 * frame is left out, and the message's bytes, gathered in order up to its
 * length, go to the check as identity values. */
class other_id_identification
{
public:
    /* Frame f, which on_the_wire doesn't take: true when it belongs to an
     * identification reply; *done then holds a message's bytes once it's
     * whole (a Single Frame, or the Consecutive Frame that completes it) */
    bool take(const ecuio_frame_t& f, std::vector<uint8_t>* done)
    {
        done->clear();
        if ((f.flags & (ECUIO_FRAME_FLAG_REASSEMBLED | ECUIO_FRAME_FLAG_ERROR |
                        ECUIO_FRAME_FLAG_REMOTE)) != 0 ||
            f.data_length < 1 || f.data_length > 8) {
            return false;
        }
        const uint64_t key =
            uint64_t{f.arbitration_id} |
            ((f.flags & ECUIO_FRAME_FLAG_EXTENDED) != 0 ? 1ull << 32 : 0);
        const uint8_t pci = f.data[0] >> 4;
        if (pci == 0) {
            open_.erase(key);
            const uint8_t n = f.data[0] & 0x0F;
            if (n == 0 || 1u + n > f.data_length ||
                !ecuio_scrub::identification(f.data + 1, n)) {
                return false;
            }
            done->assign(f.data + 1, f.data + 1 + n);
            return true;
        }
        if (pci == 1) {
            open_.erase(key);
            const uint16_t n =
                static_cast<uint16_t>(((f.data[0] & 0x0F) << 8) | f.data[1]);
            if (f.data_length != 8 || n < 8 ||
                !ecuio_scrub::identification(f.data + 2, 6)) {
                return false;
            }
            reply& r = open_[key];
            r.expected = n;
            r.bytes.assign(f.data + 2, f.data + 8);
            return true;
        }
        if (pci != 2) {
            return false;
        }
        auto it = open_.find(key);
        if (it == open_.end()) {
            return false;
        }
        reply& r = it->second;
        if (!r.given && r.bytes.size() < r.expected) {
            const size_t take = std::min<size_t>(f.data_length - 1u,
                                                 r.expected - r.bytes.size());
            r.bytes.insert(r.bytes.end(), f.data + 1, f.data + 1 + take);
            if (r.bytes.size() == r.expected) {
                *done = r.bytes;
                r.given = true;
            }
        }
        return true;
    }

    /* The bytes of each reply still short of its length at the end */
    void finish(std::vector<std::vector<uint8_t>>& out)
    {
        for (auto& kv : open_) {
            if (!kv.second.given) {
                out.push_back(std::move(kv.second.bytes));
            }
        }
        open_.clear();
    }

private:
    struct reply {
        uint16_t expected = 0;
        bool given = false;
        std::vector<uint8_t> bytes;
    };
    std::map<uint64_t, reply> open_;
};

} // namespace ecuio_frame_reassembly

#endif /* ECUIO_FRAME_REASSEMBLY_H */
