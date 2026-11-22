// File/layer: core/src/signal_file_parts.h | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest (test_signals); FUZZ: fuzz_signal_file
//
// The pieces of a signal file that build reads (signal_file_parts.cpp),
// the loader's own: the condition and plugin loaders read
// signal_file_build.h. Each throws refused.

#ifndef ECUIO_SIGNAL_FILE_PARTS_H
#define ECUIO_SIGNAL_FILE_PARTS_H

#include "ecuio/signals.h"
#include "json.h"
#include "signal_file_build.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ecuio_sf {

/* One signal of the file as read so far */
struct entry {
    const ecuio_json::Value* v = nullptr;
    std::string key;
    std::string kind;
    uint32_t def = 0;      /* its index in the set's defs */
    uint16_t id = 0;       /* its def's id: the base def's it replaces, else
                           ECUIO_USER_SIG_ID_BASE + its place in the list */
    std::string name;      /* its "name" when that is text, else "" */
    bool replaces = false; /* it stands in a base def's place */
};

/* Format 3's gates (signal_file_format3.cpp, ADR-0065 section 9).
 * A format 3 set's signal at place, named as base_def: a second signal of
 * that name, and refused for a signal of a format 1 or 2 file, whose name
 * would replace the base def were its variant not format 3 */
void named_as_base(const parts& in, const entry& e, size_t place,
                   const ecuio_signal_def_t& base_def);
/* Two of the set's own signals at one address, refused at the later */
void one_signal_each_address(const std::vector<entry>& entries,
                             const ecuio_signal_file& f);

/* A signal's "gauge", in the signal's own unit: refused when its numbers
 * can't be a dial for this def, stored as written otherwise */
void read_gauge(const ecuio_json::Value& s, const ecuio_signal_def_t& d,
                uint32_t def, ecuio_signal_file& f, const std::string& what);

/* A signal's "group" as its group number */
uint8_t group_of(const ecuio_json::Value& v, const std::string& what);

/* The ECUs: the base's channels first, then the file's, one width,
 * at most ECUIO_MAX_CHANNELS; ids to their rx */
void read_ecus(const ecuio_json::Value* ecus, ecuio_signal_file& f,
               std::vector<std::pair<std::string, uint32_t>>* ids);

/* A signal that reads a reply: its ECU, request and field into d */
void read_field(const ecuio_json::Value& s, const std::string& kind,
                const std::vector<std::pair<std::string, uint32_t>>& ecus,
                const ecuio_signal_file& f, ecuio_signal_def_t& d,
                const std::string& what);

/* An enum's labels, ascending by raw value, into f's table */
void read_values(const ecuio_json::Value& s, ecuio_signal_file& f,
                 ecuio_signal_def_t& d, const std::string& what);

} // namespace ecuio_sf

#endif /* ECUIO_SIGNAL_FILE_PARTS_H */
