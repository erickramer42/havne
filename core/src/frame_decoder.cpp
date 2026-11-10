// File/layer: core/src/frame_decoder.cpp | C++17, Qt-free; a frame's decode
//             against a set (ADR-0024 section 4)
// Testing: UNIT: CoreTest (test_core)

#include "frame_decoder.h"

#include "computed.h"    /* computed signals' programs */
#include "obd2_reply.h"  /* a packed Service 01 reply's records */
#include "pid_support.h" /* ecuio_service01_keyed */

#include <algorithm>
#include <cstring>

namespace {

bool is_text(const ecuio_signal_def_t& def)
{
    return def.kind == ECUIO_VALUE_TEXT || def.kind == ECUIO_VALUE_HEX;
}

/* Which defs decode_frame decodes: every one, only those keyed by a
 * Service 01 PID (a packed reply's records), or every other */
enum class defs_of { all, records, rest };

/* ADR-0039 section 6: def i's decode of a frame its key matched, for
 * ecuio_session_get_absent_signals: carried, or one more frame too
 * short for it (saturating at the threshold). Other failures say
 * nothing about the reply's length. */
void note_decode(ecuio_frame_decoder& d, uint32_t i, int rc)
{
    if (!d.carried) {
        return;
    }
    if (rc == ECUIO_DECODE_OK) {
        if (d.carried[i].load(std::memory_order_relaxed) == 0) {
            d.carried[i].store(1, std::memory_order_relaxed);
        }
    } else if (rc == ECUIO_DECODE_ERR_PAST_PAYLOAD) {
        const uint8_t n = d.short_frames[i].load(std::memory_order_relaxed);
        if (n < ECUIO_ABSENT_AFTER_FRAMES) {
            d.short_frames[i].store(static_cast<uint8_t>(n + 1),
                                    std::memory_order_relaxed);
        }
    }
}

/* Decodes frame (len bytes of it) against the set into the buffers from
 * value n and text slot slot on, each def in the set's order; only the
 * defs the index finds for the frame are checked */
void decode_frame(ecuio_frame_decoder& d, const ecuio_frame_t* frame,
                  uint16_t len, uint32_t& n, uint32_t& slot,
                  defs_of which = defs_of::all)
{
    const ecuio_signal_set_t* set = d.set;
    uint32_t* found = d.def_found.data();
    const uint32_t count = ecuio_def_index_find(d.def_index, *frame, found);
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t i = found[k];
        const ecuio_signal_def_t& def = set->defs[i];
        if (which != defs_of::all &&
            ecuio_service01_keyed(def) != (which == defs_of::records)) {
            continue;
        }
        if (!ecuio_def_applies_to_frame(&def, frame)) {
            continue;
        }
        ecuio_value_t v{};
        v.signal_id = def.id;
        v.kind = def.kind;
        if (is_text(def)) {
            if (slot >= d.text_slots) {
                continue;
            }
            char* out =
                d.text_scratch.data() + size_t{slot} * ECUIO_DECODE_TEXT_MAX;
            const int rc = ecuio_decode_text(&def, frame->data, len, out,
                                             ECUIO_DECODE_TEXT_MAX);
            note_decode(d, i, rc);
            if (rc != ECUIO_DECODE_OK) {
                continue;
            }
            v.text = out;
            v.text_len = static_cast<uint32_t>(strlen(out));
            ++slot;
        } else {
            const int rc =
                ecuio_decode_signal(&def, frame->data, len, &v.value);
            note_decode(d, i, rc);
            if (rc != ECUIO_DECODE_OK) {
                continue;
            }
            if (def.kind == ECUIO_VALUE_ENUM) {
                /* The raw number, and its label from the set's table,
                 * which outlives the call */
                v.text =
                    ecuio_enum_label(set, &def, static_cast<uint32_t>(v.value));
                v.text_len = v.text != nullptr
                                 ? static_cast<uint32_t>(strlen(v.text))
                                 : 0u;
            }
            if (!d.latest.empty()) {
                d.latest[i] = {v.value, frame->timestamp_us, d.decodes};
            }
        }
        d.values[n++] = v;
    }
}

/* Appends each computed value a frame value feeds (ADR-0051 section 5),
 * from each input's newest value, when every input has one no older than
 * its bound at the frame's time. A division by zero or a non-finite
 * result gives none. */
void compute_values(ecuio_frame_decoder& d, const ecuio_frame_t* frame,
                    uint32_t& n)
{
    for (const ecuio_frame_decoder::computed_plan& c : d.computed) {
        const uint32_t inputs = c.program->input_count;
        bool fed = false;
        bool fresh = true;
        double in[ECUIO_COMPUTED_MAX_INPUTS] = {};
        for (uint32_t k = 0; k < inputs; ++k) {
            const ecuio_frame_decoder::latest_value& l = d.latest[c.inputs[k]];
            fed = fed || l.decode == d.decodes;
            if (l.decode == 0 || l.time_us > frame->timestamp_us ||
                frame->timestamp_us - l.time_us > c.max_age_us[k]) {
                fresh = false;
            }
            in[k] = l.value;
        }
        ecuio_value_t v{};
        if (!fed || !fresh || !ecuio_computed_run(*c.program, in, &v.value)) {
            continue;
        }
        const ecuio_signal_def_t& def = d.set->defs[c.def];
        v.signal_id = def.id;
        v.kind = def.kind;
        d.values[n++] = v;
    }
}

/* The computed defs' plans (ADR-0051 section 5), from a set the
 * validator passed: each input's def index and age bound */
void plan_computed(ecuio_frame_decoder& d, const ecuio_signal_set_t* set)
{
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& def = set->defs[i];
        if ((def.flags & ECUIO_DEF_FLAG_COMPUTED) == 0) {
            continue;
        }
        ecuio_frame_decoder::computed_plan c;
        c.def = i;
        c.program = &set->computed[def.table_first];
        for (uint32_t k = 0; k < c.program->input_count; ++k) {
            for (uint32_t j = 0; j < set->count; ++j) {
                if (set->defs[j].id == c.program->input_ids[k]) {
                    c.inputs[k] = j;
                    break;
                }
            }
            const uint32_t age = c.program->max_age_us[k];
            c.max_age_us[k] = age != 0 ? age : ECUIO_VALUE_STALE_US;
        }
        d.computed.push_back(c);
    }
    if (!d.computed.empty()) {
        d.latest.resize(set->count);
    }
}

} // namespace

/* On one message ID, every unkeyed text def matches every frame, and each
 * key shape (offset and length) adds its largest group sharing one key. A
 * packed Service 01 reply decodes a record per PID within one frame's
 * slots, so the 41 <PID> keys add their ECUIO_OBD2_MAX_RECORDS largest
 * groups. The answer is the largest total over the IDs. */
uint32_t ecuio_text_slots_for(const ecuio_signal_set_t* set)
{
    /* ecuio_service01_keyed's shape: offset 0, two bytes */
    constexpr uint32_t kService01Shape = 2;
    struct text_key {
        uint32_t message_id;
        uint32_t shape; /* 0 unkeyed; else offset << 8 | length */
        uint32_t key;
    };
    std::vector<text_key> keys;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (!is_text(d)) {
            continue;
        }
        const bool keyed = d.mux_length > 0;
        keys.push_back(
            text_key{d.message_id,
                     keyed ? (uint32_t{d.mux_offset} << 8 | d.mux_length) : 0u,
                     keyed ? d.mux_key : 0u});
    }
    std::sort(keys.begin(), keys.end(),
              [](const text_key& a, const text_key& b) {
                  if (a.message_id != b.message_id) {
                      return a.message_id < b.message_id;
                  }
                  if (a.shape != b.shape) {
                      return a.shape < b.shape;
                  }
                  return a.key < b.key;
              });
    uint32_t best = 0;
    size_t i = 0;
    while (i < keys.size()) {
        const uint32_t id = keys[i].message_id;
        uint32_t total = 0;
        while (i < keys.size() && keys[i].message_id == id) {
            const uint32_t shape = keys[i].shape;
            uint32_t largest = 0;
            std::vector<uint32_t> records; /* the 41 <PID> keys' runs */
            while (i < keys.size() && keys[i].message_id == id &&
                   keys[i].shape == shape) {
                uint32_t run = 0;
                const uint32_t key = keys[i].key;
                while (i < keys.size() && keys[i].message_id == id &&
                       keys[i].shape == shape && keys[i].key == key) {
                    ++run;
                    ++i;
                }
                /* Unkeyed defs all match: their shape is one run */
                largest = std::max(largest, run);
                if (shape == kService01Shape && (key >> 8) == 0x41u) {
                    records.push_back(run);
                }
            }
            std::sort(records.begin(), records.end(),
                      [](uint32_t a, uint32_t b) { return a > b; });
            uint32_t packed = 0;
            for (size_t r = 0; r < records.size() && r < ECUIO_OBD2_MAX_RECORDS;
                 ++r) {
                packed += records[r];
            }
            total += std::max(largest, packed);
        }
        best = std::max(best, total);
    }
    return best;
}

void ecuio_frame_decoder::prepare(const ecuio_signal_set_t* s, bool absent)
{
    if (s == nullptr || s->defs == nullptr || s->count == 0) {
        return;
    }
    values.resize(s->count);
    if (absent) {
        short_frames.reset(new std::atomic<uint8_t>[s->count]());
        carried.reset(new std::atomic<uint8_t>[s->count]());
    }
    text_slots = ecuio_text_slots_for(s);
    text_scratch.resize(size_t{text_slots} * ECUIO_DECODE_TEXT_MAX);
    ecuio_def_index_build(def_index, s->defs, s->count);
    def_found.resize(s->count);
    for (uint32_t i = 0; i < s->count; ++i) {
        const ecuio_signal_def_t& d = s->defs[i];
        if (ecuio_service01_keyed(d) &&
            std::find(service01_ids.begin(), service01_ids.end(),
                      d.message_id) == service01_ids.end()) {
            service01_ids.push_back(d.message_id);
        }
    }
    if (!service01_ids.empty()) {
        record_frame.reset(new ecuio_frame_t{});
    }
    plan_computed(*this, s);
    set = s;
}

/* A packed Service 01 reply decodes record by record (ADR-0039 Appendix
 * A): each record a frame in record_frame for the defs keyed by its PID,
 * every other def from the reply whole, so each def decodes at most once.
 * Text values take the next slot, restarting each frame; prepare gave a
 * slot for every text value a frame can decode to. */
uint32_t ecuio_frame_decoder::decode(const ecuio_frame_t* frame)
{
    if (set == nullptr) {
        return 0;
    }
    const uint16_t len = (frame->data_length <= sizeof(frame->data))
                             ? frame->data_length
                             : static_cast<uint16_t>(sizeof(frame->data));
    uint32_t n = 0;
    uint32_t slot = 0;
    ++decodes;
    if (record_frame && (frame->flags & ECUIO_FRAME_FLAG_ERROR) == 0 &&
        std::find(service01_ids.begin(), service01_ids.end(),
                  frame->arbitration_id) != service01_ids.end()) {
        ecuio_obd2_record records[ECUIO_OBD2_MAX_RECORDS];
        uint32_t count = 0;
        const ecuio_obd2_reply reply = ecuio_obd2_split(
            frame->data, len, learned_lengths, records, &count);
        if (reply == ecuio_obd2_reply::split) {
            ecuio_frame_t& r = *record_frame;
            r.arbitration_id = frame->arbitration_id;
            r.flags = frame->flags;
            r.timestamp_us = frame->timestamp_us;
            r.data[0] = 0x41u;
            for (uint32_t k = 0; k < count; ++k) {
                r.data[1] = records[k].pid;
                memcpy(r.data + 2, frame->data + records[k].start,
                       records[k].bytes);
                r.data_length = static_cast<uint16_t>(2 + records[k].bytes);
                decode_frame(*this, &r, r.data_length, n, slot,
                             defs_of::records);
            }
            decode_frame(*this, frame, len, n, slot, defs_of::rest);
            compute_values(*this, frame, n);
            return n;
        }
        if (reply == ecuio_obd2_reply::unsplit) {
            unsplit_replies.fetch_add(1, std::memory_order_relaxed);
        }
    }
    decode_frame(*this, frame, len, n, slot);
    compute_values(*this, frame, n);
    return n;
}
