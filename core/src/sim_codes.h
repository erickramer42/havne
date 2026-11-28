#ifndef ECUIO_SIM_CODES_H
#define ECUIO_SIM_CODES_H

#include "mock_ecu.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

enum class ecuio_sim_codes_form : uint8_t {
    normal,
    refused,
    none,
    extended,
    silent
};

struct ecuio_sim_codes_plan {
    ecuio_sim_codes_form every = ecuio_sim_codes_form::normal;
    struct aim {
        uint32_t reply_id;
        ecuio_sim_codes_form form;
    };
    aim aims[8] = {};
    uint32_t aim_count = 0;
};

constexpr ecuio_mock_code ECUIO_SIM_MODULE_CODES[] = {
    {0x456171, 0x28},
    {0xC40100, 0x24},
};

inline ecuio_sim_codes_form
ecuio_sim_codes_form_of(const ecuio_sim_codes_plan& p, uint32_t reply_id)
{
    for (uint32_t i = 0; i < p.aim_count; ++i) {
        if (p.aims[i].reply_id == reply_id) {
            return p.aims[i].form;
        }
    }
    return p.every;
}

/* codes=<form>[@<reply ID>][,...] */
inline bool ecuio_sim_parse_codes(const char* text, size_t len,
                                  ecuio_sim_codes_plan* out, char* why,
                                  size_t why_cap)
{
    static const char* const kForms[] = {"refused", "none", "extended",
                                         "silent"};
    size_t at = 0;
    while (at <= len) {
        size_t end = at;
        while (end < len && text[end] != ',') {
            ++end;
        }
        size_t name_end = at;
        while (name_end < end && text[name_end] != '@') {
            ++name_end;
        }
        int form = -1;
        for (int i = 0; i < 4; ++i) {
            if (std::strlen(kForms[i]) == name_end - at &&
                std::strncmp(text + at, kForms[i], name_end - at) == 0) {
                form = i + 1;
            }
        }
        uint32_t reply_id = 0;
        bool aimed = name_end < end;
        if (aimed) {
            char id[16] = {};
            const size_t n = end - name_end - 1;
            char* stop = nullptr;
            if (n > 0 && n < sizeof id) {
                std::memcpy(id, text + name_end + 1, n);
                reply_id = static_cast<uint32_t>(std::strtoul(id, &stop, 16));
            }
            if (n == 0 || n >= sizeof id || stop == nullptr || *stop != '\0' ||
                reply_id == 0) {
                form = -1;
            }
        }
        if (form < 0 || (aimed && out->aim_count >= 8)) {
            std::snprintf(why, why_cap,
                          "codes: '%.*s' isn't refused, none, extended or "
                          "silent, aimed at one module by @<reply ID> or at "
                          "every one",
                          static_cast<int>(end - at < 40 ? end - at : 40),
                          text + at);
            return false;
        }
        const auto f = static_cast<ecuio_sim_codes_form>(form);
        if (aimed) {
            out->aims[out->aim_count++] = {reply_id, f};
        } else {
            out->every = f;
        }
        at = end + 1;
    }
    return true;
}

/* A node's 19 01 or 19 02 into out (at least 3 + 4 x its codes), its
 * length; 0 for no answer. codes holds count of them. */
inline uint32_t ecuio_sim_codes_answer(ecuio_sim_codes_form form,
                                       const ecuio_mock_code* codes,
                                       uint32_t count, bool extended,
                                       const uint8_t* req, uint32_t len,
                                       uint8_t* out)
{
    if (len != 3 || req[0] != 0x19 || (req[1] != 0x01 && req[1] != 0x02) ||
        form == ecuio_sim_codes_form::silent) {
        return 0;
    }
    if (form == ecuio_sim_codes_form::refused ||
        (form == ecuio_sim_codes_form::extended && !extended)) {
        out[0] = 0x7F;
        out[1] = 0x19;
        out[2] = form == ecuio_sim_codes_form::refused ? 0x11 : 0x7F;
        return 3;
    }
    if (form == ecuio_sim_codes_form::none) {
        count = 0;
    }
    uint32_t n = 0;
    out[n++] = 0x59;
    out[n++] = req[1];
    out[n++] = 0xFF;
    uint32_t held = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if ((codes[i].status & req[2]) == 0) {
            continue;
        }
        ++held;
        if (req[1] == 0x02) {
            out[n++] = static_cast<uint8_t>(codes[i].code >> 16);
            out[n++] = static_cast<uint8_t>(codes[i].code >> 8);
            out[n++] = static_cast<uint8_t>(codes[i].code);
            out[n++] = codes[i].status;
        }
    }
    if (req[1] == 0x01) {
        out[n++] = 0x00;
        out[n++] = static_cast<uint8_t>(held >> 8);
        out[n++] = static_cast<uint8_t>(held);
    }
    return n;
}

#endif
