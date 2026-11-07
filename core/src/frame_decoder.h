// File/layer: core/src/frame_decoder.h | internal, C++17, Qt-free; a
//             frame's decode against a set (ADR-0024 section 4)
// Testing: UNIT: CoreTest (test_core)
//
// What one frame decodes to against a set, as a session delivers it: every
// def the frame matches and decodes, in the set's order, a packed Service
// 01 reply record by record (ADR-0039 Appendix A), then each computed def
// one of them feeds (ADR-0051 section 5). The session decodes through one
// on its worker; the reader (ADR-0049 section 5) decodes a recording's
// frames through one of its own, so both give the same values. prepare
// sizes every buffer once, so a decode allocates nothing.

#ifndef ECUIO_FRAME_DECODER_H
#define ECUIO_FRAME_DECODER_H

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "def_index.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

struct ecuio_frame_decoder {
    /* A def's newest number (decode 0 is none yet) and the frame time and
     * decode it came from, for the computed defs that read it */
    struct latest_value {
        double value = 0.0;
        uint64_t time_us = 0;
        uint64_t decode = 0;
    };
    struct computed_plan {
        uint32_t def = 0; /* the computed def's index */
        const ecuio_computed_t* program = nullptr;
        uint32_t inputs[ECUIO_COMPUTED_MAX_INPUTS] =
            {}; /* the inputs' def indexes */
        uint64_t max_age_us[ECUIO_COMPUTED_MAX_INPUTS] = {};
    };

    /* The set prepare took; NULL decodes nothing */
    const ecuio_signal_set_t* set = nullptr;
    /* One value per def, and text_slots slots of ECUIO_DECODE_TEXT_MAX
     * bytes, one per text value a frame can decode. A decode's values
     * and their text stay valid until the next decode. */
    std::vector<ecuio_value_t> values;
    std::vector<char> text_scratch;
    uint32_t text_slots = 0;
    /* The defs each frame can match (def_index.h), and room for one
     * frame's finds, one per def */
    ecuio_def_index def_index;
    std::vector<uint32_t> def_found;
    /* Per def, with prepare's absent (ADR-0039 section 6): the frames
     * whose key matched but ended before its field, counted up to
     * ECUIO_ABSENT_AFTER_FRAMES, and whether any carried it. Written by
     * the decode, read on another thread (relaxed). */
    std::unique_ptr<std::atomic<uint8_t>[]> short_frames;
    std::unique_ptr<std::atomic<uint8_t>[]> carried;
    /* A packed Service 01 reply's split (obd2_reply.h): the message IDs a
     * Service 01 def listens on, the only frames split; the frame each
     * record decodes from; and each PID's length as this ECU sends it
     * alone, where shorter than J1979's. The count of replies that
     * wouldn't split is read on another thread. */
    std::vector<uint32_t> service01_ids;
    std::unique_ptr<ecuio_frame_t> record_frame;
    uint8_t learned_lengths[256] = {};
    std::atomic<uint64_t> unsplit_replies{0};
    /* Computed signals (ADR-0051 section 5), sized only when the set has a
     * computed def: each def's newest value, and one plan per computed
     * def; decodes counts the decodes, the first 1 */
    std::vector<latest_value> latest;
    std::vector<computed_plan> computed;
    uint64_t decodes = 0;

    /* Sizes the buffers for set, which must stay valid and unchanged
     * while this decodes; absent keeps the absent signals' counts. A NULL
     * set, or one with no defs, decodes nothing. Throws bad_alloc. */
    void prepare(const ecuio_signal_set_t* set, bool absent);

    /* Decodes frame into values: the number of values, each def at most
     * once. Allocates nothing. */
    uint32_t decode(const ecuio_frame_t* frame);
};

/* The most text values one frame can decode to against set (ADR-0024
 * section 4). Throws bad_alloc. */
uint32_t ecuio_text_slots_for(const ecuio_signal_set_t* set);

#endif /* ECUIO_FRAME_DECODER_H */
