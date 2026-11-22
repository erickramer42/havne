// File/layer: core/src/signal_file_format3.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileFormat3Test, PluginTest (test_signals);
//          FUZZ: fuzz_signal_file
//
// Signal-file format 3's gates (ADR-0065 section 9), called from the
// loader's build: what a format 3 set refuses that formats 1 and 2 take.
// Format 3 keeps format 2's grammar and changes what a file means, so a
// set is held to one format whole: a plugin variant's is its newest file's.

#include "ecuio/signals.h"
#include "json.h"
#include "signal_file_build.h"
#include "signal_file_parts.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace ecuio_sf {

void named_as_base(const parts& in, const entry& e, size_t place,
                   const ecuio_signal_def_t& base_def)
{
    const uint32_t own =
        place < in.formats.size() ? in.formats[place] : in.format;
    if (own >= 3) {
        return;
    }
    /* Only a variant mixes formats: read alone, the file would replace */
    refuse(*e.v, "signal " + q(e.key) + " is named as obd2's " +
                     q(base_def.name) + ", which a format " +
                     std::to_string(own) +
                     " file replaces and a format 3 set reads beside it: "
                     "this variant holds a format 3 file, so give every "
                     "file of it \"havne_signals\": 3");
}

void one_signal_each_address(const std::vector<entry>& entries,
                             const ecuio_signal_file& f)
{
    std::unordered_map<std::string, size_t> first; /* an address's first */
    for (size_t i = 0; i < entries.size(); ++i) {
        ecuio_signal_key_t key{};
        /* A computed def reads no address; a def with no room for its
         * text is the validator's to refuse */
        if (ecuio_signal_address_key(&f.defs[entries[i].def], &key) !=
                ECUIO_OK ||
            key.text[0] == '\0') {
            continue;
        }
        const auto at = first.emplace(key.text, i);
        if (!at.second) {
            refuse(
                *entries[i].v,
                "signal " + q(entries[i].key) + " reads the address signal " +
                    q(entries[at.first->second].key) + " reads (" + key.text +
                    "): a format 3 file gives one address to one signal");
        }
    }
}

} // namespace ecuio_sf
