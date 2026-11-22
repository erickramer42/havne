// File/layer: core/src/signal_file_merge_notes.cpp | C++17, Qt-free, every platform
// Testing: UNIT: MergeTest (test_signals); FUZZ: fuzz_signal_merge
//
// The notes of a merge (ADR-0065 section 9, ecuio_signal_file_note): what
// the merge did that no part said outright, each a sentence naming both
// parts and both signals. The merge keeps records; the sentences are
// written here once the list is settled, each within ECUIO_NOTE_TEXT_LEN
// by the bounds of what it names.

#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "json.h"
#include "signal_expr.h"
#include "signal_file_build.h"
#include "signal_file_merge.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ecuio_merge {

using ecuio_json::Value;
using ecuio_sf::q;

/* What the sentences count, worked out once for all the notes */
struct note_counts {
    /* A name's alive computed readers */
    std::unordered_map<std::string, uint32_t> readers;
    /* A part's alive readers of one obd2 def, by (part, def ID): the
     * first of them and how many */
    std::unordered_map<uint64_t, std::pair<size_t, uint32_t>> of_base;
};

namespace {

uint64_t base_code(uint32_t part, uint16_t id)
{
    return (uint64_t{part} << 32) | id;
}

/* Text with each control character a space: a sentence is one line */
std::string plain(std::string text)
{
    for (char& c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            c = ' ';
        }
    }
    return text;
}

std::string counted(uint32_t n, const char* one, const char* many)
{
    return std::to_string(n) + (n == 1 ? one : many);
}

std::string upper(std::string text)
{
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}

} // namespace

std::string cut(const std::string& text, size_t max)
{
    if (text.size() <= max) {
        return text;
    }
    size_t end = max;
    /* Back to the start of the character the cut would split */
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end) + "...";
}

/* A part by its name, short enough for two in a sentence */
std::string merger::part_name(uint32_t part) const
{
    return plain(cut(parts_[part].file->name, 40));
}

/* "'Name' (Part name)" */
std::string merger::named(size_t item) const
{
    const struct item& it = items_[item];
    return q(it.name) + " (" + part_name(it.part) + ")";
}

/* A module as a sentence names it: its name, else its id in upper case */
std::string merger::module_label(size_t module) const
{
    const std::string& name = modules_[module].name;
    if (!name.empty() && name.size() <= 31) {
        return plain(name);
    }
    if (!ids_.id(module).empty()) {
        return upper(ids_.id(module));
    }
    char hex[32];
    std::snprintf(hex, sizeof hex, "the ECU at %X",
                  static_cast<unsigned>(ids_.addr(module).rx));
    return hex;
}

/* "PCM 22 4A10, byte 0": the module, the request and the field's byte */
std::string merger::address_words(const item& it) const
{
    std::string out = module_label(it.module);
    if (const Value* read = it.v.find("read")) {
        const Value* service = read->find("service");
        const Value* number = read->find("number");
        if (service != nullptr && number != nullptr) {
            out += " " + service->text + " " + upper(number->text);
        }
    }
    if (const Value* field = it.v.find("field")) {
        if (const Value* byte = field->find("byte")) {
            out += ", byte " + std::to_string(byte->whole);
        }
    }
    return out;
}

/* The signal in the set where item was: itself, or what took its place,
 * to the end; kNone when that one is left out */
size_t merger::standing(size_t item) const
{
    while (!items_[item].alive && items_[item].stands != kNone) {
        item = items_[item].stands;
    }
    return items_[item].alive && !items_[item].left ? item : kNone;
}

std::string merger::replaced_words(const note& n,
                                   const note_counts& counts) const
{
    const item& it = items_[n.item];
    const item& was = items_[n.other];
    std::string out = named(n.item) + " replaces " + named(n.other);
    if (n.kind == ECUIO_NOTE_REPLACED_BY_ADDRESS) {
        out += ": both read " + address_words(it);
    } else {
        out += " by its key " + q(n.key);
        if (!it.address.empty() && !was.address.empty()) {
            out += "; it reads another address";
        }
    }
    /* What the replaced signal's readers now read */
    const auto read = counts.readers.find(n.key);
    if (read != counts.readers.end()) {
        out += "; " +
               counted(read->second, " computed signal reads ",
                       " computed signals read ") +
               q(n.key);
        if (it.unit != was.unit) {
            out += "; unit " + q(it.unit) + ", was " + q(was.unit);
        }
    }
    return out;
}

std::string merger::beside_words(const note& n, const note_counts& counts) const
{
    const item& it = items_[n.item];
    std::string out = named(n.item) + " is read beside obd2's " + q(it.name) +
                      ": a merged set replaces no obd2 signal";
    const auto read = counts.of_base.find(base_code(it.part, it.base_id));
    if (read != counts.of_base.end()) {
        out += "; " + q(items_[read->second.first].name);
        out += read->second.second == 1
                   ? " reads obd2's"
                   : " and " + std::to_string(read->second.second - 1) +
                         " more read obd2's";
    }
    return out;
}

std::string merger::sentence(const note& n, const note_counts& counts) const
{
    switch (n.kind) {
    case ECUIO_NOTE_REPLACED_BY_ADDRESS:
    case ECUIO_NOTE_REPLACED_BY_KEY:
        return replaced_words(n, counts);
    case ECUIO_NOTE_KEY_TAKEN:
        return named(n.item) + " takes the key " + q(n.key) + " from " +
               named(n.other) + ", which kept it for a signal it replaced";
    case ECUIO_NOTE_BESIDE_BASE:
        return beside_words(n, counts);
    case ECUIO_NOTE_MODULE_RENAMED:
        return "ecu " + q(n.key) + " of " + q(part_name(n.part)) + " is " +
               q(n.text) + " in the set, where " + q(n.key) +
               " is another module";
    case ECUIO_NOTE_MODULE_LEFT_OUT:
        return "ecu " + q(n.key) + " of " + q(part_name(n.part)) +
               " is left out with its " +
               counted(n.count, " signal", " signals") + n.text;
    case ECUIO_NOTE_PART_LEFT_OUT:
        return q(part_name(n.part)) + " is left out with its " +
               counted(n.count, " signal", " signals") + n.text;
    default:
        return named(n.item) + " is left out: " + plain(n.text);
    }
}

void merger::give_notes(ecuio_signal_file& f) const
{
    const std::vector<size_t> list = alive();
    const size_t first = f.defs.size() - list.size();
    std::vector<uint32_t> def_at(items_.size(), UINT32_MAX);
    note_counts counts;
    const ecuio_signal_set_t* base = ecuio_obd2_signal_set();
    for (size_t k = 0; k < list.size(); ++k) {
        const item& it = items_[list[k]];
        def_at[list[k]] = static_cast<uint32_t>(first + k);
        for (const std::string& name : it.reads) {
            ++counts.readers[name];
            const long d = name.compare(0, 5, "obd2.") == 0
                               ? ecuio_sf::obd2_def(base, name.substr(5))
                               : -1;
            if (d >= 0) {
                auto& of = counts.of_base[base_code(it.part, base->defs[d].id)];
                if (of.second++ == 0) {
                    of.first = list[k];
                }
            }
        }
    }
    f.notes.clear();
    for (const note& n : notes_) {
        const size_t stands = n.item != kNone ? standing(n.item) : kNone;
        if (n.kind == ECUIO_NOTE_BESIDE_BASE && stands != n.item) {
            continue; /* the signal is no longer beside it */
        }
        ecuio_signal_file::note out;
        out.kind = n.kind;
        out.part = n.part;
        out.count = n.count;
        if (n.other != kNone) {
            out.other_part = items_[n.other].part;
        }
        if (stands != kNone) {
            out.def = def_at[stands];
        }
        out.text = cut(sentence(n, counts), ECUIO_NOTE_TEXT_LEN - 4);
        f.notes.push_back(std::move(out));
    }
}

} // namespace ecuio_merge
