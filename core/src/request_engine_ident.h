// File/layer: core/src/request_engine_ident.h | internal, not ABI-exported;
//             a part of the request engine, included by
//             request_engine.h, whose description covers it
// Identification (ADR-0021): the list each channel asks, the VIN rule,
// the replies' text and the channel's report.
// C++17, Qt-free, no platform headers.
// Testing: UNIT: RequestEngineTest (test_obd2), CoreTest (test_core);
//          HARNESS: each transport's identification cases

#ifndef ECUIO_REQUEST_ENGINE_IDENT_H
#define ECUIO_REQUEST_ENGINE_IDENT_H

#include "request_engine_state.h"

/* What identification asks each channel's ECU (ADR-0021), in this
 * order: SAE J1979 Service 09's vehicle information (the VIN, the
 * calibration IDs and their verification numbers, the ECU's name), then
 * ISO 14229-1's identification DIDs (Annex C: part, software and
 * hardware numbers, the supplier, the serial, the VIN again as F190).
 * The name is what the activity log shows beside the value. */
struct ecuio_ident_request {
    uint8_t bytes[3];
    uint8_t len;
    const char* name;
};

constexpr ecuio_ident_request ECUIO_IDENT_REQUESTS[] = {
    {{0x09, 0x02, 0x00}, 2, "VIN"},
    {{0x09, 0x04, 0x00}, 2, "CALID"},
    {{0x09, 0x06, 0x00}, 2, "CVN"},
    {{0x09, 0x0A, 0x00}, 2, "ECU name"},
    {{0x22, 0xF1, 0x87}, 3, "part number"},
    {{0x22, 0xF1, 0x88}, 3, "software number"},
    {{0x22, 0xF1, 0x89}, 3, "software version"},
    {{0x22, 0xF1, 0x8A}, 3, "supplier"},
    {{0x22, 0xF1, 0x8C}, 3, "ECU serial"},
    {{0x22, 0xF1, 0x90}, 3, "VIN (F190)"},
    {{0x22, 0xF1, 0x91}, 3, "hardware number"},
    {{0x22, 0xF1, 0x94}, 3, "supplier software number"},
    {{0x22, 0xF1, 0x95}, 3, "supplier software version"},
};
constexpr uint32_t ECUIO_IDENT_COUNT =
    sizeof(ECUIO_IDENT_REQUESTS) / sizeof(ECUIO_IDENT_REQUESTS[0]);
static_assert(ECUIO_IDENT_COUNT <= ECUIO_IDENTITY_MAX_ITEMS,
              "the identification list must fit the report");

/* The reply's data starts here for both services: after 49 <item> and
 * the item count, or after 62 <DID high> <DID low>. */
constexpr uint16_t ECUIO_IDENT_DATA_OFFSET = 3;

/* A report's VIN (ADR-0027 section 3) and whether it is in: 09 02's
 * answer, else 22 F190's, each kept to its ASCII letters and digits in
 * upper case and taken only at 17 of them. F190's is in only once 09 02
 * has an outcome, since 09 02 still out could answer one of its own. */
struct ecuio_ident_vin {
    char text[ECUIO_VIN_TEXT_LEN];
    bool in;
};

inline bool ecuio_ident_item_vin(const ecuio_identity_item_t& it,
                                 char (&out)[ECUIO_VIN_TEXT_LEN])
{
    if (it.outcome != ECUIO_IDENTITY_ANSWERED) {
        return false;
    }
    char vin[ECUIO_VIN_TEXT_LEN];
    uint32_t n = 0;
    for (uint32_t k = 0; k < it.text_length && k < ECUIO_IDENTITY_TEXT_LEN;
         ++k) {
        char c = it.text[k];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        } else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            continue;
        }
        if (n == ECUIO_VIN_TEXT_LEN - 1) {
            return false;
        }
        vin[n++] = c;
    }
    if (n != ECUIO_VIN_TEXT_LEN - 1) {
        return false;
    }
    vin[n] = '\0';
    std::memcpy(out, vin, sizeof vin);
    return true;
}

inline ecuio_ident_vin ecuio_identity_vin_of(const ecuio_channel_identity_t& id)
{
    ecuio_ident_vin v{};
    char from_service09[ECUIO_VIN_TEXT_LEN] = "";
    char from_f190[ECUIO_VIN_TEXT_LEN] = "";
    bool service09_out = false;
    const uint32_t items = id.items < ECUIO_IDENTITY_MAX_ITEMS
                               ? id.items
                               : ECUIO_IDENTITY_MAX_ITEMS;
    for (uint32_t i = 0; i < items; ++i) {
        const ecuio_identity_item_t& it = id.list[i];
        if (it.length == 2 && it.request[0] == 0x09 && it.request[1] == 0x02) {
            service09_out = it.outcome == ECUIO_IDENTITY_UNASKED;
            (void)ecuio_ident_item_vin(it, from_service09);
        } else if (it.length == 3 && it.request[0] == 0x22 &&
                   it.request[1] == 0xF1 && it.request[2] == 0x90) {
            (void)ecuio_ident_item_vin(it, from_f190);
        }
    }
    if (from_service09[0] != '\0') {
        std::memcpy(v.text, from_service09, sizeof v.text);
        v.in = true;
    } else if (from_f190[0] != '\0') {
        std::memcpy(v.text, from_f190, sizeof v.text);
        v.in = !service09_out;
    }
    return v;
}

/* Identification (ADR-0021): gives every channel the list to ask, each
 * item once, through the one-shot slot ahead of the scan. Idempotent:
 * a channel already holding the list keeps its answers. */
inline void ecuio_engine_identify(ecuio_request_engine* e)
{
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        ecuio_engine_channel& ch = e->chans[c];
        if (ch.ident_count > 0) {
            continue;
        }
        for (uint32_t i = 0; i < ECUIO_IDENT_COUNT; ++i) {
            ecuio_identity_item_t& it = ch.ident[i];
            it = ecuio_identity_item_t{};
            const ecuio_ident_request& q = ECUIO_IDENT_REQUESTS[i];
            for (uint8_t k = 0; k < q.len; ++k) {
                it.request[k] = q.bytes[k];
            }
            it.length = q.len;
        }
        ch.ident_count = ECUIO_IDENT_COUNT;
        ch.ident_pending = ECUIO_IDENT_COUNT;
    }
}

/* The next unasked item of channel c, in list order; -1 when none. */
inline int32_t ecuio_engine_ident_next(const ecuio_request_engine* e,
                                       uint32_t c)
{
    const ecuio_engine_channel& ch = e->chans[c];
    for (uint32_t i = 0; i < ch.ident_count; ++i) {
        if (ch.ident[i].outcome == ECUIO_IDENTITY_UNASKED) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

/* An identification reply's data as the report shows it
 * (ecuio_identity_item_t::text): ASCII when every byte is printable or
 * padding, the printable runs joined by ", " and trailing blanks
 * dropped; else hex pairs. Writes at most cap bytes, NUL included, cut
 * at a whole pair; cap is at least 1. Returns the text's length. */
inline uint8_t ecuio_identity_format(const uint8_t* data, uint32_t len,
                                     char* out, uint32_t cap)
{
    bool printable = false;
    bool ascii = true;
    for (uint32_t i = 0; i < len; ++i) {
        if (data[i] >= 0x20 && data[i] <= 0x7E) {
            printable = true;
        } else if (data[i] != 0x00 && data[i] != 0xFF) {
            ascii = false;
        }
    }
    uint32_t w = 0;
    const uint32_t room = cap - 1;
    if (ascii && printable) {
        bool started = false;
        bool gap = false;
        for (uint32_t i = 0; i < len; ++i) {
            const uint8_t b = data[i];
            if (b < 0x20 || b > 0x7E) {
                gap = true;
                continue;
            }
            if (started && gap) {
                if (w + 2 > room) {
                    break;
                }
                out[w++] = ',';
                out[w++] = ' ';
            }
            if (w + 1 > room) {
                break;
            }
            out[w++] = static_cast<char>(b);
            started = true;
            gap = false;
        }
        while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == ',')) {
            --w;
        }
    } else {
        static const char kHex[] = "0123456789ABCDEF";
        for (uint32_t i = 0; i < len; ++i) {
            const uint32_t need = (w == 0) ? 2u : 3u;
            if (w + need > room) {
                break;
            }
            if (w > 0) {
                out[w++] = ' ';
            }
            out[w++] = kHex[data[i] >> 4];
            out[w++] = kHex[data[i] & 0x0F];
        }
    }
    out[w] = '\0';
    return static_cast<uint8_t>(w);
}

/* Settles the item channel c has out: answered, with the reply's data
 * (the whole frame, from which the text is taken past the header),
 * refused with its code, or unanswered. */
inline void ecuio_engine_ident_settle(ecuio_request_engine* e, uint32_t c,
                                      uint8_t outcome, uint8_t nrc,
                                      const uint8_t* data, uint16_t len)
{
    ecuio_engine_channel& ch = e->chans[c];
    ecuio_identity_item_t& it = ch.ident[ch.ident_index];
    ch.identifying = false;
    it.outcome = outcome;
    it.nrc = nrc;
    it.text[0] = '\0';
    it.text_length = 0;
    if (outcome == ECUIO_IDENTITY_ANSWERED && len > ECUIO_IDENT_DATA_OFFSET) {
        it.text_length = ecuio_identity_format(
            data + ECUIO_IDENT_DATA_OFFSET,
            static_cast<uint32_t>(len - ECUIO_IDENT_DATA_OFFSET),
            it.text, sizeof(it.text));
    }
    if (ch.ident_pending > 0) {
        --ch.ident_pending;
    }
    ++ch.ident_generation;
}

/* What channel c's ECU has said it is, for
 * ecuio_session_get_channel_identity; the driver adds the channel's
 * rx_id. Complete once every item has an outcome; the VIN flagged as
 * soon as ecuio_identity_vin_of has it in. */
inline void ecuio_engine_identity(const ecuio_request_engine* e, uint32_t c,
                                  ecuio_channel_identity_t* out)
{
    const ecuio_engine_channel& ch = e->chans[c];
    *out = ecuio_channel_identity_t{};
    out->generation = ch.ident_generation;
    out->items = ch.ident_count;
    bool complete = ch.ident_count > 0;
    for (uint32_t i = 0; i < ch.ident_count; ++i) {
        out->list[i] = ch.ident[i];
        complete = complete && ch.ident[i].outcome != ECUIO_IDENTITY_UNASKED;
    }
    if (complete) {
        out->flags |= ECUIO_IDENTITY_COMPLETE;
    }
    if (ecuio_identity_vin_of(*out).in) {
        out->flags |= ECUIO_IDENTITY_VIN;
    }
}

#endif
