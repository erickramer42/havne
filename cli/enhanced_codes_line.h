#ifndef HAVNE_CLI_ENHANCED_CODES_LINE_H
#define HAVNE_CLI_ENHANCED_CODES_LINE_H

#include "diagnostics_line.h"
#include "jsonl.h"

#include "ecuio/core.h"

#include <cstdint>

namespace cli {

inline constexpr size_t kEnhancedCodesLineBytes = 32 * 1024;

inline const char* dtc_format_word(uint8_t format)
{
    switch (format) {
    case ECUIO_DTC_FORMAT_J2012:
        return "SAE J2012";
    case ECUIO_DTC_FORMAT_ISO14229:
        return "ISO 14229-1";
    case ECUIO_DTC_FORMAT_J1939:
        return "SAE J1939";
    case ECUIO_DTC_FORMAT_ISO11992:
        return "ISO 11992-4";
    case ECUIO_DTC_FORMAT_J2012_WWH:
        return "SAE J2012 WWH-OBD";
    default:
        return "unknown";
    }
}

inline bool codes_answered_nothing(const ecuio_module_codes_t& r)
{
    return r.count_outcome == ECUIO_DIAG_UNANSWERED &&
           r.list_outcome == ECUIO_DIAG_UNANSWERED;
}

inline void codes_string_or_null(Line& l, const char* s)
{
    if (*s == '\0') {
        l.raw("null");
    } else {
        l.string(s, SIZE_MAX);
    }
}

inline void codes_reply(Line& l, uint8_t nrc)
{
    const uint8_t reply[3] = {0x7F, 0x19, nrc};
    l.key("reply");
    l.hex_bytes(reply, sizeof reply);
}

// One array of a list's per-code values; put(i) writes code i's element
template <typename Put>
inline void codes_array(Line& l, const char* key, uint32_t n, Put put)
{
    l.key(key);
    l.ch('[');
    for (uint32_t i = 0; i < n; ++i) {
        l.raw(i ? "," : "");
        put(i);
    }
    l.ch(']');
}

inline void codes_emissions(Line& l, uint32_t lists)
{
    static const char* const kLists[] = {"stored", "pending", "permanent"};
    l.ch('[');
    bool first = true;
    for (uint32_t k = 0; k < 3; ++k) {
        if ((lists & (1u << k)) != 0) {
            l.raw(first ? "" : ",");
            l.string(kLists[k], SIZE_MAX);
            first = false;
        }
    }
    l.ch(']');
}

inline void codes_count_object(Line& l, const ecuio_module_codes_t& r)
{
    l.key("count");
    l.ch('{');
    l.key("outcome");
    l.string(diagnostics_outcome_word(r.count_outcome), SIZE_MAX);
    if (r.count_outcome == ECUIO_DIAG_REFUSED) {
        codes_reply(l, r.count_nrc);
    }
    if ((r.flags & ECUIO_MODULE_CODES_FORMAT) != 0) {
        l.key("format");
        l.string(dtc_format_word(r.format), SIZE_MAX);
        l.key("available");
        l.hex_id(r.available);
        l.key("reported");
        l.u64(r.count);
    }
    l.ch('}');
}

inline void codes_list_arrays(Line& l, const ecuio_module_codes_t& r,
                              const ecuio_channel_diagnostics_t* standard)
{
    const uint32_t n =
        r.held < ECUIO_MODULE_CODES_MAX ? r.held : ECUIO_MODULE_CODES_MAX;
    codes_array(l, "codes", n, [&](uint32_t i) {
        char text[ECUIO_DTC_UDS_TEXT_LEN + 1] = {};
        (void)ecuio_dtc_format_uds(r.codes[i].code, r.format, text,
                                   sizeof text);
        l.string(text, sizeof text);
    });
    codes_array(l, "status", n,
                [&](uint32_t i) { l.hex_id(r.codes[i].status); });
    codes_array(l, "state", n, [&](uint32_t i) {
        l.string(ecuio_dtc_status_state(r.codes[i].status), SIZE_MAX);
    });
    codes_array(l, "class", n, [&](uint32_t i) {
        codes_string_or_null(l, ecuio_dtc_status_class(r.codes[i].status));
    });
    codes_array(l, "lamp", n, [&](uint32_t i) {
        l.raw(ecuio_dtc_status_lamp(r.codes[i].status) ? "true" : "false");
    });
    codes_array(l, "emissions", n, [&](uint32_t i) {
        codes_emissions(l, ecuio_module_code_emissions_lists(
                               standard, r.codes[i].code, r.format));
    });
    codes_array(l, "titles", n, [&](uint32_t i) {
        codes_string_or_null(l, ecuio_dtc_title_uds(r.codes[i].code, r.format));
    });
}

inline void codes_list_object(Line& l, const ecuio_module_codes_t& r,
                              const ecuio_channel_diagnostics_t* standard)
{
    l.key("list");
    l.ch('{');
    l.key("outcome");
    l.string(diagnostics_outcome_word(r.list_outcome), SIZE_MAX);
    if (r.list_outcome == ECUIO_DIAG_REFUSED) {
        codes_reply(l, r.list_nrc);
    }
    if (r.list_outcome == ECUIO_DIAG_ANSWERED) {
        l.key("reported");
        l.u64(r.reported);
        if (r.reported > r.held) {
            l.key("held");
            l.u64(r.held);
        }
        codes_list_arrays(l, r, standard);
    }
    l.ch('}');
}

// {"type":"enhanced_codes", rx, ...}: a module that answered nothing, or
// could not be opened, carries an outcome alone. standard is the module's
// J1979 report (null when none was read) for the emissions marks.
inline void enhanced_codes_line(Line& l, const ecuio_module_codes_t& r,
                                const ecuio_channel_diagnostics_t* standard)
{
    l.raw("{\"type\":\"enhanced_codes\"");
    l.key("rx");
    l.hex_id(r.rx_id);
    const bool not_opened = (r.flags & ECUIO_MODULE_CODES_NOT_OPENED) != 0;
    if (not_opened || codes_answered_nothing(r)) {
        l.key("outcome");
        l.string(not_opened ? "not_opened" : "no_response", SIZE_MAX);
        l.ch('}');
        return;
    }
    l.key("read");
    l.u64(r.read_id);
    l.key("generation");
    l.u64(r.generation);
    l.key("mask");
    l.hex_id(r.mask);
    codes_count_object(l, r);
    codes_list_object(l, r, standard);
    l.ch('}');
}

// {"type":"enhanced_codes_end", modules, read, no_response, not_reached}
inline void enhanced_codes_end_line(Line& l, uint32_t modules, uint32_t read,
                                    uint32_t no_response, uint32_t not_reached)
{
    l.raw("{\"type\":\"enhanced_codes_end\"");
    l.key("modules");
    l.u64(modules);
    l.key("read");
    l.u64(read);
    l.key("no_response");
    l.u64(no_response);
    l.key("not_reached");
    l.u64(not_reached);
    l.ch('}');
}

} // namespace cli

#endif // HAVNE_CLI_ENHANCED_CODES_LINE_H
