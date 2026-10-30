// File/layer: core/src/uds_generic.cpp | C++17, Qt-free, every platform
// Testing: UNIT: UdsGenericTest (test_obd2); HARNESS through
//          J2534AdoptionTest; FUZZ: fuzz_uds_query. See uds_generic.h.

#include "uds_generic.h"

#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "error_detail.h"
#include "uri_query.h"

#include <cstdio>
#include <cstring>

namespace {

/* A view into the URI. */
struct view {
    const char* s;
    size_t n;
};

struct type_spec {
    const char* name;
    uint8_t kind;
    uint8_t bits;   /* 0 for the text kinds */
};

/* Every name holds a letter past 'f' and no '-', so none reads as a DID,
 * a range or an ECU, and a two-token entry is told apart by its last
 * token. */
const type_spec kTypes[] = {
    {"hex", ECUIO_VALUE_HEX, 0},    {"text", ECUIO_VALUE_TEXT, 0},
    {"u8", ECUIO_VALUE_UINT, 8},    {"u16", ECUIO_VALUE_UINT, 16},
    {"u24", ECUIO_VALUE_UINT, 24},  {"u32", ECUIO_VALUE_UINT, 32},
    {"i8", ECUIO_VALUE_INT, 8},     {"i16", ECUIO_VALUE_INT, 16},
    {"i24", ECUIO_VALUE_INT, 24},   {"i32", ECUIO_VALUE_INT, 32},
};

const char* const kShape =
    "a DID like 1E1C, an ECU and a type as in 7E9:1E1C:u16, or a range "
    "like 1E00-1EFF";
const char* const kTypeNames =
    "a type of hex, text, u8, u16, u24, u32, i8, i16, i24 or i32";
const char* const kEcuShape =
    "an ECU response ID of 9 to 7FF, or 29-bit 18DAxxyy";
const char* const kRangeOrder = "a range whose end isn't before its start";

/* One entry as listed: a DID, or the range first..last (a range of one
 * is still scanned, not polled). */
struct entry {
    uint32_t rx = 0;
    uint16_t first = 0;
    uint16_t last = 0;
    bool range = false;
    const type_spec* type = &kTypes[0];
};

/* One DID of the set after dedupe. */
struct item {
    uint32_t rx;
    uint16_t did;
    const type_spec* type;
    bool scan;
};

const type_spec* type_named(view v)
{
    for (const type_spec& t : kTypes) {
        if (v.n == strlen(t.name) && strncmp(v.s, t.name, v.n) == 0) {
            return &t;
        }
    }
    return nullptr;
}

/* 1 to max_digits hex digits, nothing else. */
bool parse_hex(view v, size_t max_digits, uint32_t* out)
{
    if (v.n == 0 || v.n > max_digits) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < v.n; ++i) {
        const char c = v.s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | d;
    }
    *out = value;
    return true;
}

/* Cuts v at the first sep into head and tail; false when sep is absent. */
bool cut(view v, char sep, view* head, view* tail)
{
    const char* p = static_cast<const char*>(memchr(v.s, sep, v.n));
    if (p == nullptr) {
        return false;
    }
    *head = {v.s, static_cast<size_t>(p - v.s)};
    *tail = {p + 1, v.n - static_cast<size_t>(p - v.s) - 1};
    return true;
}

/* The refusal: what the value should have been, and the text at fault. */
struct refusal {
    const char* wanted;
    view shown;
};

void refuse(const refusal& r)
{
    const int shown = static_cast<int>((r.shown.n < 32) ? r.shown.n : 32);
    ecuio_detail_set("?did= needs %s, not '%.*s'", r.wanted, shown, r.shown.s);
}

/* [<ecu>:]<DID>[-<DID>][:<type>] into *out; false with *why set. */
bool parse_entry(view v, entry* out, refusal* why)
{
    view tok[3];
    size_t n = 0;
    view rest = v;
    for (;;) {
        view head, tail;
        if (!cut(rest, ':', &head, &tail)) {
            if (n == 3) {
                *why = {kShape, v};
                return false;
            }
            tok[n++] = rest;
            break;
        }
        if (n == 3) {
            *why = {kShape, v};
            return false;
        }
        tok[n++] = head;
        rest = tail;
    }

    view ecu = {nullptr, 0};
    view did = tok[0];
    *out = entry{};
    if (n == 3) {
        ecu = tok[0];
        did = tok[1];
        out->type = type_named(tok[2]);
        if (out->type == nullptr) {
            *why = {kTypeNames, tok[2]};
            return false;
        }
    } else if (n == 2) {
        /* DID:type, or ECU:DID when the last token reads as a DID or holds
         * a range's '-'; a last token that is neither is a misspelled type. */
        const type_spec* t = type_named(tok[1]);
        uint32_t as_did = 0;
        if (t != nullptr) {
            out->type = t;
        } else if (parse_hex(tok[1], 4, &as_did) ||
                   memchr(tok[1].s, '-', tok[1].n) != nullptr) {
            ecu = tok[0];
            did = tok[1];
        } else {
            *why = {kTypeNames, tok[1]};
            return false;
        }
    }

    if (ecu.s != nullptr) {
        uint32_t rx = 0;
        uint32_t tx = 0;
        if (!parse_hex(ecu, 8, &rx) || !ecuio_uds_tx_for_rx(rx, &tx)) {
            *why = {kEcuShape, ecu};
            return false;
        }
        out->rx = rx;
    } else {
        out->rx = ECUIO_OBD2_ECU_ARB_ID;
    }

    view first = did;
    view last = {nullptr, 0};
    const bool range = cut(did, '-', &first, &last);
    uint32_t a = 0;
    uint32_t b = 0;
    if (!parse_hex(first, 4, &a) || (range && !parse_hex(last, 4, &b))) {
        *why = {kShape, v};
        return false;
    }
    if (range && b < a) {
        *why = {kRangeOrder, did};
        return false;
    }
    out->first = static_cast<uint16_t>(a);
    out->last = static_cast<uint16_t>(range ? b : a);
    out->range = range;
    return true;
}

/* Adds one DID, polled or scanned, keeping a repeat once: a DID both
 * alone and in a range is polled. polled and scanned count the items. */
void add_item(std::vector<item>& items, uint32_t rx, uint16_t did,
              const type_spec* type, bool scan, uint32_t& polled,
              uint32_t& scanned)
{
    for (item& it : items) {
        if (it.rx == rx && it.did == did) {
            if (it.scan && !scan) {
                it.scan = false;
                it.type = type;
                --scanned;
                ++polled;
            }
            return;
        }
    }
    items.push_back(item{rx, did, type, scan});
    if (scan) {
        ++scanned;
    } else {
        ++polled;
    }
}

bool refuse_too_many_polled(uint32_t polled)
{
    ecuio_detail_set("?did= lists %u DIDs to poll, more than the %u a "
                     "session asks for; scan a range instead",
                     static_cast<unsigned>(polled),
                     static_cast<unsigned>(ECUIO_PROFILE_MAX_REQUESTS));
    return false;
}

bool refuse_too_many_scanned(uint32_t scanned)
{
    ecuio_detail_set("?did= scans %u DIDs, more than the %u a session scans",
                     static_cast<unsigned>(scanned),
                     static_cast<unsigned>(ECUIO_ENGINE_SCAN_MAX));
    return false;
}

ecuio_signal_def_t def_for(const item& it, uint16_t id)
{
    ecuio_signal_def_t d = {};
    d.id = id;
    d.message_id = it.rx;
    d.mux_offset = 0;
    d.mux_length = 3;
    d.mux_key = 0x620000u | it.did;
    d.kind = it.type->kind;
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    d.start_byte = 3;
    d.bit_length = it.type->bits;
    d.byte_length = 0;   /* text kinds: to the end of the reply */
    d.scale = 1.0;
    d.offset = 0.0;
    if (it.rx == ECUIO_OBD2_ECU_ARB_ID) {
        (void)std::snprintf(d.name, sizeof d.name, "DID %04X", it.did);
    } else {
        (void)std::snprintf(d.name, sizeof d.name, "0x%X DID %04X", it.rx,
                            it.did);
    }
    d.unit[0] = '\0';
    d.decimal_places = 0;
    return d;
}

const ecuio_signal_set_t kTemplate = [] {
    ecuio_signal_set_t s = {};
    s.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    return s;
}();

} // namespace

bool ecuio_uds_tx_for_rx(uint32_t rx, uint32_t* out_tx)
{
    /* 8 is out: its request ID would be 0, which no channel may carry. */
    if (rx >= 9 && rx <= 0x7FFu) {
        *out_tx = rx - 8;
        return true;
    }
    if ((rx >> 16) == 0x18DAu) {
        *out_tx = 0x18DA0000u | ((rx & 0xFFu) << 8) | ((rx >> 8) & 0xFFu);
        return true;
    }
    return false;
}

bool ecuio_uds_query_has_did(const ecuio_uri_query& query)
{
    for (size_t i = 0; i < query.count; ++i) {
        if (query.params[i].is("did")) {
            return true;
        }
    }
    return false;
}

ecuio_status_t ecuio_uds_build(const ecuio_uri_query& query,
                               ecuio_uds_session* out)
{
    try {
        std::vector<item> items;
        bool any = false;
        uint32_t polled = 0;
        uint32_t scanned = 0;
        for (size_t i = 0; i < query.count; ++i) {
            const ecuio_uri::param& p = query.params[i];
            if (!p.is("did")) {
                continue;
            }
            if (p.val_len == 0) {
                ecuio_detail_set("?did= needs at least one DID");
                return ECUIO_ERR_BAD_URI;
            }
            view rest = {p.val, p.val_len};
            for (;;) {
                view head, tail;
                const bool more = cut(rest, ',', &head, &tail);
                const view one = more ? head : rest;
                entry en;
                refusal why = {kShape, one};
                if (one.n == 0 || !parse_entry(one, &en, &why)) {
                    refuse(why);
                    return ECUIO_ERR_BAD_URI;
                }
                any = true;
                /* The limits are checked as the list grows, a range before
                 * it expands, so a 0000-FFFF never costs 65536 lookups. */
                const uint32_t span = en.last - en.first + 1u;
                if (en.range && span > ECUIO_ENGINE_SCAN_MAX) {
                    refuse_too_many_scanned(span);
                    return ECUIO_ERR_BAD_URI;
                }
                for (uint32_t did = en.first; did <= en.last; ++did) {
                    add_item(items, en.rx, static_cast<uint16_t>(did), en.type,
                             en.range, polled, scanned);
                }
                if (polled > ECUIO_PROFILE_MAX_REQUESTS) {
                    refuse_too_many_polled(polled);
                    return ECUIO_ERR_BAD_URI;
                }
                if (scanned > ECUIO_ENGINE_SCAN_MAX) {
                    refuse_too_many_scanned(scanned);
                    return ECUIO_ERR_BAD_URI;
                }
                if (!more) {
                    break;
                }
                rest = tail;
            }
        }
        if (!any) {
            ecuio_detail_set("?did= needs at least one DID");
            return ECUIO_ERR_BAD_URI;
        }

        out->defs.clear();
        out->channels.clear();
        out->scan.clear();
        for (const item& it : items) {
            bool known = false;
            for (const ecuio_channel_decl_t& c : out->channels) {
                known = known || c.rx_id == it.rx;
            }
            if (!known) {
                uint32_t tx = 0;
                (void)ecuio_uds_tx_for_rx(it.rx, &tx);   /* parse checked it */
                out->channels.push_back(ecuio_channel_decl_t{it.rx, tx});
            }
        }
        if (out->channels.size() > ECUIO_MAX_CHANNELS) {
            ecuio_detail_set("?did= names %u ECUs, more than the %u channels "
                             "a session has",
                             static_cast<unsigned>(out->channels.size()),
                             static_cast<unsigned>(ECUIO_MAX_CHANNELS));
            return ECUIO_ERR_BAD_URI;
        }
        out->defs.reserve(items.size());
        for (int pass = 0; pass < 2; ++pass) {
            const bool scan = pass == 1;   /* polled first, then scanned */
            for (const item& it : items) {
                if (it.scan != scan) {
                    continue;
                }
                const uint16_t id = static_cast<uint16_t>(
                    ECUIO_UDS_GENERIC_SIG_ID_BASE + out->defs.size());
                out->defs.push_back(def_for(it, id));
                if (scan) {
                    out->scan.push_back(ecuio_uds_scan_target{it.rx, it.did});
                }
            }
        }
        out->polled = polled;
        out->set = ecuio_signal_set_t{};
        out->set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
        out->set.count = static_cast<uint32_t>(out->defs.size());
        out->set.defs = out->defs.data();
        out->set.channel_count = static_cast<uint32_t>(out->channels.size());
        out->set.channels = out->channels.data();
        return ECUIO_OK;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

const ecuio_signal_set_t* ecuio_uds_generic_template_set()
{
    return &kTemplate;
}
