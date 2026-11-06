// File/layer: core/src/obdb_import.h | C++17, Qt-free, core-internal
// Testing: UNIT: ObdbImportTest (test_core): the year rule over each form
//          of a filter, the field rule over each shape OBDb writes, and
//          the converter through ecuio_obdb_import
//
// The pure rules of the OBDb import (obdb.h, ADR-0051 section 7), apart
// so the plugin framework's OBDb variants (ADR-0052) take the same ones:
// which commands a model year takes, and where OBDb's bit field lies in
// a signal file's terms. The semantics are OBDb's own decoder's (its
// .schemas repository, python/can/): a field's bit index counts from the
// first byte after the service and PID or DID echo, most significant bit
// first, and blsb reverses the order of its whole bytes.

#ifndef ECUIO_OBDB_IMPORT_H
#define ECUIO_OBDB_IMPORT_H

#include "json.h"

#include <cstdint>

namespace ecuio_obdb {

/* Whether a command's "filter" takes a model year. No filter takes every
 * year, and model year 0 (none given) takes only those. A filter's
 * "years" list takes each year it names; "from" and "to" take a range,
 * both ends included, or with from past to the years outside it (from on
 * and to back), as OBDb's decoder reads them; "from" alone takes it and
 * later, "to" alone it and earlier. A filter that names no year at all
 * takes none. */
inline bool year_takes(const ecuio_json::Value* filter, uint32_t model_year)
{
    if (filter == nullptr) {
        return true;
    }
    if (model_year == 0 || filter->type != ecuio_json::Type::object) {
        return false;
    }
    auto year_of = [](const ecuio_json::Value* v, int64_t* out) {
        if (v == nullptr || v->type != ecuio_json::Type::number ||
            !v->in_int64) {
            return false;
        }
        *out = v->whole;
        return true;
    };
    const int64_t y = model_year;
    if (const ecuio_json::Value* years = filter->find("years")) {
        if (years->type == ecuio_json::Type::array) {
            for (const ecuio_json::Value& v : years->items) {
                int64_t listed = 0;
                if (year_of(&v, &listed) && listed == y) {
                    return true;
                }
            }
        }
    }
    int64_t from = 0;
    int64_t to = 0;
    const bool has_from = year_of(filter->find("from"), &from);
    const bool has_to = year_of(filter->find("to"), &to);
    if (has_from && has_to) {
        return from <= to ? (y >= from && y <= to) : (y >= from || y <= to);
    }
    if (has_from) {
        return y >= from;
    }
    if (has_to) {
        return y <= to;
    }
    return false;
}

/* A field in a signal file's terms: the data byte it starts at (after the
 * echo), its bits, and for a little-endian field the bit it starts at in
 * that byte, least significant first */
struct field {
    uint32_t byte = 0;
    uint32_t bits = 0;
    uint32_t bit = 0;
    bool little_endian = false;
};

/* What a field's bit index (bix), length (len) and blsb become, or false
 * when a signal file can't say it: a big-endian field that isn't whole
 * bytes across more than one byte, a blsb field that isn't whole bytes,
 * or a length outside 1 to 32. A field within one byte is little-endian
 * in a signal file, its bit counted from the byte's least significant;
 * whole bytes are big-endian, or little-endian under blsb (which OBDb
 * applies past 8 bits only). */
inline bool field_of(uint32_t bix, uint32_t len, bool blsb, field* out)
{
    if (len == 0 || len > 32) {
        return false;
    }
    field f;
    f.byte = bix / 8;
    f.bits = len;
    if (bix % 8 == 0 && len % 8 == 0) {
        f.little_endian = blsb && len > 8;
        *out = f;
        return true;
    }
    const uint32_t last = bix + len - 1;
    if (bix / 8 == last / 8) {
        f.little_endian = true;
        f.bit = 7 - last % 8;
        *out = f;
        return true;
    }
    return false;
}

} // namespace ecuio_obdb

#endif
