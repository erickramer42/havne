// File/layer: core/src/obdb_import.cpp | C++17, Qt-free, core: ecuio_obdb_import (obdb.h, ADR-0051 section 7)
// Testing: UNIT: ObdbImportTest (test_core); FUZZ: fuzz_obdb_import
//
// Reads an OBDb signal set (the shape of OBDb's .schemas signals.json, the
// semantics of its python/can/ decoder) and writes a signal file
// (docs/signal-files.md). What converts:
// - commands: Service 01 and 22 on 11-bit IDs, hdr the request ID and rax
//   the reply's (rx = hdr + 8 for 7E0 to 7E7 when OBDb gives none, as
//   ISO 15765-4 does); a module per reply ID, at most ECUIO_MAX_CHANNELS
//   with obd2's when the file extends it; a model year's commands by
//   their filter (obdb_import.h's year_takes);
// - signals: the field by field_of, sign, mul / div as the scale, add as
//   the offset, decimals from the scale's step; the unit by kUnits; a map
//   as an enumeration, each label its description; the path's first part
//   as the group; the description as notes;
// - synthetics: a ratio as a computed signal.
// Each difference is a note: min and max (OBDb holds a value within them),
// nullmin and nullmax, a cut or guarded name or label, a reply ID taken
// from the standard; each signal left out says why. The output is loaded
// before it's returned, so what this gives the caller always loads.

#include "ecuio/obdb.h"

#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "error_detail.h"
#include "json.h"
#include "obdb_import.h"
#include "request_engine.h"
#include "obdb_import_words.h"
#include "obdb_importer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace ecuio_obdb_impl;

ecuio_status_t ecuio_obdb_import(const char* bytes, size_t length,
                                 const ecuio_obdb_options_t* options,
                                 ecuio_obdb_note_fn note, void* user,
                                 char** out_json, size_t* out_length)
{
    ecuio_detail_clear();
    if (out_json == nullptr || out_length == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out_json = nullptr;
    *out_length = 0;
    if (bytes == nullptr && length > 0) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    ecuio_obdb_options_t o{};
    if (options != nullptr) {
        o = *options;
    }
    if ((o.flags & ~ECUIO_OBDB_ALONE) != 0) {
        ecuio_detail_set("an option flag this core doesn't define");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    for (const uint32_t r : o.reserved) {
        if (r != 0) {
            ecuio_detail_set("the options' reserved words must be zero");
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    try {
        ecuio_json::Value root;
        ecuio_json::Error error;
        ecuio_json::Limits limits;
        limits.max_bytes = ECUIO_OBDB_MAX_BYTES;
        if (!ecuio_json::parse(bytes != nullptr ? bytes : "", length, &root,
                               &error, limits)) {
            ecuio_detail_set("%s", error.to_string().c_str());
            return error.words == "out of memory" ? ECUIO_ERR_OUT_OF_MEMORY
                                                  : ECUIO_ERR_INVALID_ARGUMENT;
        }
        importer im(o, note, user);
        const std::string text = im.run(root);
        // What the caller gets always loads: a refusal here is the
        // importer's own mistake, or a name the caller gave
        ecuio_signal_file_t* check = nullptr;
        const ecuio_status_t st =
            ecuio_signal_file_load(text.data(), text.size(), &check);
        if (st != ECUIO_OK) {
            const std::string why = ecuio_last_error_detail();
            ecuio_detail_set("the converted file is refused: %s", why.c_str());
            return st;
        }
        ecuio_signal_file_free(check);
        auto* p = static_cast<char*>(std::malloc(text.size() + 1));
        if (p == nullptr) {
            ecuio_detail_set("out of memory");
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        std::memcpy(p, text.c_str(), text.size() + 1);
        *out_json = p;
        *out_length = text.size();
        return ECUIO_OK;
    } catch (const canceled&) {
        ecuio_detail_set("the import was canceled");
        return ECUIO_ERR_CANCELED;
    } catch (const not_obdb& n) {
        if (n.line == 0) {
            ecuio_detail_set("%s", n.words.c_str());
        } else {
            ecuio_detail_set("line %u, column %u: %s",
                             static_cast<unsigned>(n.line),
                             static_cast<unsigned>(n.column), n.words.c_str());
        }
        return ECUIO_ERR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    } catch (...) {
        ecuio_detail_set("the set couldn't be read");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
}

void ecuio_obdb_free(char* json)
{
    std::free(json);
}
