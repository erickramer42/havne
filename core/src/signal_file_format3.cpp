// File/layer: core/src/signal_file_format3.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileFormat3Test, PluginTest (test_signals);
//          FUZZ: fuzz_signal_file
//
// Signal-file format 3's part of the loader (ADR-0065 section 9), called
// from its build: what a format 3 set refuses that formats 1 and 2 take,
// and "needs", the keys a file's expressions read from another file.
// Format 3 keeps format 2's grammar and changes what a file means, so a
// set is held to one format whole: a plugin variant's is its newest file's.
//
// A set with needs is built twice. The first build judges every signal,
// each needed key standing for a number, and finds the computed signals
// that read a need or a signal left out for one. The second builds the
// rest, so the set and its IDs are those of the file without them.

#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "json.h"
#include "least_core.h"
#include "signal_file_build.h"
#include "signal_file_parts.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ecuio_sf {

using ecuio_json::Type;
using ecuio_json::Value;

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

void read_needs(const Value& root, uint32_t format,
                std::vector<std::pair<std::string, const Value*>>* out)
{
    const Value* needs = root.find("needs");
    if (needs == nullptr) {
        return;
    }
    if (format < 3) {
        refuse(*needs, "\"needs\" is signal-file format 3's, and this file is "
                       "format " +
                           std::to_string(format));
    }
    expect(*needs, Type::array, "\"needs\"");
    if (needs->items.size() > ECUIO_SIGNAL_SET_MAX_SIGNALS) {
        refuse(*needs, "\"needs\" lists " +
                           std::to_string(needs->items.size()) +
                           ", more than " +
                           std::to_string(ECUIO_SIGNAL_SET_MAX_SIGNALS));
    }
    std::unordered_set<std::string> given;
    for (const Value& v : needs->items) {
        expect(v, Type::string, "a need");
        if (!key_ok(v.text)) {
            refuse(v, "\"needs\" lists " + q(v.text) +
                          ", which is no key: 1 to 31 of a-z, 0-9 and _");
        }
        if (!given.insert(v.text).second) {
            refuse(v, "\"needs\" lists " + q(v.text) + " twice");
        }
        out->emplace_back(v.text, &v);
    }
}

namespace {

ecuio_signal_file::note left_out(const entry& e, const std::string& key,
                                 const char* why)
{
    ecuio_signal_file::note n;
    n.kind = ECUIO_NOTE_SIGNAL_LEFT_OUT;
    n.text = q(e.name) + " is left out: it reads " + q(key) + why;
    return n;
}

} // namespace

void settle_needs(const parts& in, const std::vector<entry>& entries,
                  const ecuio_signal_file& f, needs_pass& pass)
{
    std::unordered_map<uint16_t, size_t> own;  /* a signal's place by its ID */
    std::unordered_map<uint16_t, size_t> need; /* a need's by its stand-in */
    std::unordered_set<std::string> keys;
    for (size_t i = 0; i < entries.size(); ++i) {
        own.emplace(entries[i].id, i);
        keys.insert(entries[i].key);
    }
    for (size_t k = 0; k < in.needs.size(); ++k) {
        if (keys.count(in.needs[k].first) != 0) {
            refuse(*in.needs[k].second,
                   "\"needs\" lists " + q(in.needs[k].first) +
                       ", which the file defines: a need is a key another "
                       "file provides");
        }
        need.emplace(pass.ids.at(in.needs[k].first), k);
    }

    /* Those reading a need, in the list's order, then what reads them */
    std::vector<uint8_t> read(in.needs.size(), 0);
    std::vector<std::vector<size_t>> readers(entries.size());
    std::vector<size_t> queue;
    pass.waiting.assign(entries.size(), 0);
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].kind != "computed") {
            continue;
        }
        const ecuio_computed_t& p =
            f.computed[f.defs[entries[i].def].table_first];
        for (uint32_t k = 0; k < p.input_count && k < ECUIO_COMPUTED_MAX_INPUTS;
             ++k) {
            const auto needed = need.find(p.input_ids[k]);
            const auto mine = own.find(p.input_ids[k]);
            if (needed != need.end()) {
                read[needed->second] = 1;
                if (pass.waiting[i] == 0) {
                    pass.waiting[i] = 1;
                    queue.push_back(i);
                    pass.notes.push_back(
                        left_out(entries[i], in.needs[needed->second].first,
                                 ", which this file needs from another"));
                }
            } else if (mine != own.end()) {
                readers[mine->second].push_back(i);
            }
        }
    }
    for (size_t at = 0; at < queue.size(); ++at) {
        const size_t gone = queue[at];
        for (const size_t r : readers[gone]) {
            if (pass.waiting[r] == 0) {
                pass.waiting[r] = 1;
                queue.push_back(r);
                pass.notes.push_back(left_out(entries[r], entries[gone].key,
                                              ", which is left out"));
            }
        }
    }
    for (size_t k = 0; k < in.needs.size(); ++k) {
        if (read[k] == 0) {
            refuse(*in.needs[k].second,
                   "\"needs\" lists " + q(in.needs[k].first) +
                       ", which no expression of the file reads");
        }
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        if (pass.waiting[i] != 0) {
            pass.waiting_keys.push_back(entries[i].key);
        }
    }
}

/* A stand-in ID is above every ID a set's own signal takes */
static_assert(ECUIO_USER_SIG_ID_BASE + 2u * ECUIO_SIGNAL_SET_MAX_SIGNALS <=
                  UINT16_MAX + 1u,
              "a needed key's stand-in ID would meet a signal's own");

void build_around_needs(const parts& in, ecuio_signal_file& f)
{
    needs_pass pass;
    for (size_t k = 0; k < in.needs.size(); ++k) {
        pass.ids.emplace(in.needs[k].first,
                         static_cast<uint16_t>(UINT16_MAX - k));
    }
    ecuio_signal_file judged;
    build(in, judged, &pass);

    std::vector<const Value*> list = in.signals;
    if (in.own_list) {
        for (const Value& v : in.signals_at->items) {
            list.push_back(&v);
        }
    }
    parts kept = in;
    kept.needs.clear();
    kept.own_list = false;
    kept.may_be_empty = true;
    kept.signals.clear();
    kept.formats.clear();
    for (size_t i = 0; i < list.size(); ++i) {
        if (pass.waiting[i] != 0) {
            continue;
        }
        kept.signals.push_back(list[i]);
        if (i < in.formats.size()) {
            kept.formats.push_back(in.formats[i]);
        }
    }
    build(kept, f, nullptr);
    /* What a waiting signal uses, an older core refuses all the same */
    ecuio_least::raise(f.least, judged.least);
    for (const auto& n : in.needs) {
        f.needs.push_back(n.first);
    }
    f.waiting = std::move(pass.waiting_keys);
    f.notes = std::move(pass.notes);
}

} // namespace ecuio_sf
