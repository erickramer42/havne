// File/layer: cli/vin_line.h | havne-cli, a VIN's decode and an ECU's identity as lines (ADR-0043, ADR-0021)
// Testing: UNIT (tests/test_cli.cpp, CliVinTest: a decode with each
//          field, a refusal, an identity line with every outcome and its
//          VIN); `vin`'s own output in the golden run
//          (tests/golden/cli/vin.jsonl)
//
// What the app's activity log says of a car's identity, as JSON: the
// `vin` command's line for a typed VIN, and `stream`'s identity line,
// one per ECU once each item has an outcome, with its VIN's decode.
// The decode is core's (vin.h): the model year and check digit by 49
// CFR 565.15, the make by NHTSA vPIC's manufacturer codes.

#ifndef HAVNE_CLI_VIN_LINE_H
#define HAVNE_CLI_VIN_LINE_H

#include "diagnostics_line.h" /* the outcome words identity shares */
#include "jsonl.h"

#include "ecuio/core.h"
#include "ecuio/vin.h"

#include <cstdint>
#include <cstring>

namespace cli {

static_assert(ECUIO_IDENTITY_ANSWERED == ECUIO_DIAG_ANSWERED &&
                  ECUIO_IDENTITY_REFUSED == ECUIO_DIAG_REFUSED &&
                  ECUIO_IDENTITY_UNANSWERED == ECUIO_DIAG_UNANSWERED &&
                  ECUIO_IDENTITY_UNASKED == ECUIO_DIAG_UNASKED,
              "identity's outcomes take diagnostics' words");

// A VIN's fields after an open object: "vin", then "decoded" and, when
// decoded, the year (null when position 10 is no year code), whether
// that year is the cycle's earlier one, the make (null when vPIC lists
// none), the manufacturer code, the check digit it works out to and
// whether position 9 holds it, and the vPIC release
inline void vin_fields(Line& l, const char* vin, uint32_t latest_model_year)
{
    l.key("vin");
    l.string(vin, SIZE_MAX, 64);
    ecuio_vin_info_t v;
    const bool decoded =
        ecuio_vin_decode(vin, latest_model_year, &v) == ECUIO_OK;
    l.key("decoded");
    l.raw(decoded ? "true" : "false");
    if (!decoded) {
        return;
    }
    l.key("model_year");
    if (v.model_year == 0) {
        l.raw("null");
    } else {
        l.u64(v.model_year);
    }
    l.key("year_earlier");
    l.raw((v.flags & ECUIO_VIN_YEAR_EARLIER) ? "true" : "false");
    const char* make = ecuio_vin_make(v.wmi);
    l.key("make");
    if (*make == '\0') {
        l.raw("null");
    } else {
        l.string(make, SIZE_MAX);
    }
    l.key("wmi");
    l.string(v.wmi, sizeof v.wmi);
    const char check[2] = {v.check_digit, '\0'};
    l.key("check_digit");
    l.string(check, sizeof check);
    l.key("check_matches");
    l.raw((v.flags & ECUIO_VIN_CHECK_MATCHES) ? "true" : "false");
    l.key("source");
    l.string(ecuio_vin_source(), SIZE_MAX);
}

// The `vin` command's line, without its newline
inline void vin_line(Line& l, const char* vin, uint32_t latest_model_year)
{
    l.raw("{\"type\":\"vin\"");
    vin_fields(l, vin, latest_model_year);
    l.ch('}');
}

// One ECU's identity once complete: each item by name, its request in
// frames.csv's form, its outcome (a refusal with its reply) and, when
// answered, its text; then "vin", the VIN core takes from it
// (ecuio_identity_vin) with its decode, or null with none
inline void identity_line(Line& l, const ecuio_channel_identity_t& id,
                          uint32_t latest_model_year)
{
    l.raw("{\"type\":\"identity\"");
    l.key("rx");
    l.hex_id(id.rx_id);
    l.key("generation");
    l.u64(id.generation);
    l.key("items");
    l.ch('[');
    const uint32_t n = id.items < ECUIO_IDENTITY_MAX_ITEMS
                           ? id.items
                           : ECUIO_IDENTITY_MAX_ITEMS;
    for (uint32_t i = 0; i < n; ++i) {
        const ecuio_identity_item_t& it = id.list[i];
        const uint8_t len = it.length <= sizeof it.request ? it.length : 0;
        if (i > 0) {
            l.ch(',');
        }
        l.ch('{');
        l.key("name");
        l.string(ecuio_identity_request_name(it.request, len), SIZE_MAX);
        l.key("request");
        l.hex_bytes(it.request, len);
        diagnostics_outcome(l, it.outcome, it.request[0], it.nrc);
        if (it.outcome == ECUIO_IDENTITY_ANSWERED) {
            l.key("text");
            l.string(it.text, sizeof it.text);
        }
        l.ch('}');
    }
    l.ch(']');
    char vin[ECUIO_VIN_TEXT_LEN];
    l.key("vin");
    if (ecuio_identity_vin(&id, vin, sizeof vin) == ECUIO_OK &&
        vin[0] != '\0') {
        l.ch('{');
        vin_fields(l, vin, latest_model_year);
        l.ch('}');
    } else {
        l.raw("null");
    }
    l.ch('}');
}

} // namespace cli

#endif // HAVNE_CLI_VIN_LINE_H
