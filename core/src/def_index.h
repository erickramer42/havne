// File/layer: core/src/def_index.h | C++17, Qt-free, ALL platforms (internal)
// Testing: UNIT: DefIndexTest, CoreTest (test_core)
//
// The defs of a set that a frame can match (ADR-0024 section 4), found
// without checking each def. A def applies to a frame on its message ID
// and, when keyed, with its key at its offset, so the index groups the
// defs by message ID and key shape (offset and length): a frame looks up
// its ID, then reads its key once per shape on that ID. Checking every
// def costs each frame a key read per def, which a set whose defs share
// one ID (obd2's) pays in full. The decode still asks
// ecuio_def_applies_to_frame of each def found: the index narrows, the
// gate decides. Built once, at create (the set never changes after);
// a lookup allocates nothing.

#ifndef ECUIO_DEF_INDEX_H
#define ECUIO_DEF_INDEX_H

#include "ecuio/core.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <cstdint>
#include <vector>

struct ecuio_def_index {
    /* One message ID's defs of one key shape, entries [begin, end):
     * shape 0 is unkeyed, else offset << 8 | length */
    struct group {
        uint32_t message_id;
        uint32_t shape;
        uint32_t begin;
        uint32_t end;
    };
    /* A def's key (0 unkeyed) and its place in the set */
    struct entry {
        uint32_t key;
        uint32_t def;
    };
    std::vector<group> groups;  /* by message ID, then shape */
    std::vector<entry> entries; /* by group, then key, then place */
};

/* Indexes count defs. Throws bad_alloc, which create catches. */
inline void ecuio_def_index_build(ecuio_def_index& index,
                                  const ecuio_signal_def_t* defs,
                                  uint32_t count)
{
    struct item {
        uint32_t message_id;
        uint32_t shape;
        uint32_t key;
        uint32_t def;
    };
    std::vector<item> items;
    items.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_signal_def_t& d = defs[i];
        const bool keyed = d.mux_length > 0;
        items.push_back(
            item{d.message_id,
                 keyed ? (uint32_t{d.mux_offset} << 8 | d.mux_length) : 0u,
                 keyed ? d.mux_key : 0u, i});
    }
    std::sort(items.begin(), items.end(), [](const item& a, const item& b) {
        if (a.message_id != b.message_id) {
            return a.message_id < b.message_id;
        }
        if (a.shape != b.shape) {
            return a.shape < b.shape;
        }
        if (a.key != b.key) {
            return a.key < b.key;
        }
        return a.def < b.def;
    });
    index.groups.clear();
    index.entries.clear();
    index.entries.reserve(count);
    for (const item& it : items) {
        if (index.groups.empty() ||
            index.groups.back().message_id != it.message_id ||
            index.groups.back().shape != it.shape) {
            const uint32_t at = static_cast<uint32_t>(index.entries.size());
            index.groups.push_back(
                ecuio_def_index::group{it.message_id, it.shape, at, at});
        }
        index.entries.push_back(ecuio_def_index::entry{it.key, it.def});
        index.groups.back().end = static_cast<uint32_t>(index.entries.size());
    }
}

/* The places in the set of the defs that can apply to frame, in the
 * set's order, written to out, which has room for the set's count;
 * returns how many. Every def ecuio_def_applies_to_frame takes is among
 * them, and of a validated set no other. */
inline uint32_t ecuio_def_index_find(const ecuio_def_index& index,
                                     const ecuio_frame_t& frame, uint32_t* out)
{
    /* An error report's ID is its error class, not a message */
    if (frame.flags & ECUIO_FRAME_FLAG_ERROR) {
        return 0;
    }
    uint32_t len = frame.data_length;
    if (len > sizeof(frame.data)) {
        len = sizeof(frame.data);
    }
    const uint32_t id = frame.arbitration_id;
    auto g = std::lower_bound(index.groups.begin(), index.groups.end(), id,
                              [](const ecuio_def_index::group& a, uint32_t v) {
                                  return a.message_id < v;
                              });
    uint32_t n = 0;
    uint32_t runs = 0;
    for (; g != index.groups.end() && g->message_id == id; ++g) {
        const ecuio_def_index::entry* first = index.entries.data() + g->begin;
        const ecuio_def_index::entry* last = index.entries.data() + g->end;
        if (g->shape != 0) {
            /* The key as ecuio_def_applies_to_frame reads it: first
             * byte most significant, within the frame, at most 3 bytes */
            const uint32_t offset = g->shape >> 8;
            const uint32_t length = g->shape & 0xFFu;
            if (length > 3 || offset + length > len) {
                continue;
            }
            uint32_t key = 0;
            for (uint32_t i = 0; i < length; ++i) {
                key = (key << 8) | frame.data[offset + i];
            }
            first = std::lower_bound(first, last, key,
                                     [](const ecuio_def_index::entry& e,
                                        uint32_t v) { return e.key < v; });
            last = std::upper_bound(
                first, last, key,
                [](uint32_t v, const ecuio_def_index::entry& e) {
                    return v < e.key;
                });
        }
        if (first == last) {
            continue;
        }
        for (const ecuio_def_index::entry* e = first; e != last; ++e) {
            out[n++] = e->def;
        }
        ++runs;
    }
    /* Each run is in the set's order; more than one interleave */
    if (runs > 1) {
        std::sort(out, out + n);
    }
    return n;
}

#endif
