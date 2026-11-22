// File/layer: core/src/signal_file_merge.cpp | C++17, Qt-free, every platform
// Testing: UNIT: MergeTest (test_signals), c_abi_merge;
//          FUZZ: fuzz_signal_merge
//
// ecuio_signal_file_merge (ADR-0065 section 9, signals.h): the parts read
// again, their modules joined by address, the text written and loaded.
// The rules for a signal are in signal_file_merge_signals.cpp and the
// notes' sentences in signal_file_merge_notes.cpp.

#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "error_detail.h"
#include "json.h"
#include "request_engine.h"
#include "signal_file_build.h"
#include "signal_file_merge.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace ecuio_merge {

using ecuio_json::Member;
using ecuio_json::Type;
using ecuio_json::Value;
using ecuio_sf::q;

namespace {

Value text_value(const std::string& s)
{
    Value v;
    v.type = Type::string;
    v.text = s;
    return v;
}

Value whole_value(int64_t n)
{
    Value v;
    v.type = Type::number;
    v.integer = true;
    v.in_int64 = true;
    v.whole = n;
    v.number = static_cast<double>(n);
    return v;
}

Value empty_of(Type t)
{
    Value v;
    v.type = t;
    return v;
}

void add(Value& object, const std::string& key, Value value)
{
    Member m;
    m.key = key;
    m.value = std::move(value);
    object.members.push_back(std::move(m));
}

std::string hex(uint32_t id)
{
    char text[16];
    std::snprintf(text, sizeof text, "%X", static_cast<unsigned>(id));
    return text;
}

std::string parts_words(size_t n)
{
    return std::to_string(n) + (n == 1 ? " part" : " parts");
}

/* A source's texts as a "source" block: its name always, the rest given */
Value written_source(const ecuio_signal_source_t& s)
{
    Value out = empty_of(Type::object);
    add(out, "name", text_value(s.name));
    const std::pair<const char*, const char*> more[] = {
        {"url", s.url},
        {"commit", s.commit},
        {"license", s.license},
        {"modified", s.modified}};
    for (const auto& m : more) {
        if (m.second[0] != '\0') {
            add(out, m.first, text_value(m.second));
        }
    }
    return out;
}

} // namespace

merger::merger(const ecuio_signal_file_t* const* parts, uint32_t count,
               bool base_asked)
    : base_(base_asked)
{
    parts_.resize(count);
    for (uint32_t p = 0; p < count; ++p) {
        part& pt = parts_[p];
        pt.file = parts[p];
        /* Bytes a loader took: only memory can fail them */
        ecuio_json::Limits limits;
        limits.max_bytes = ECUIO_SIGNAL_SET_MAX_BYTES;
        ecuio_json::Error error;
        if (!ecuio_json::parse(pt.file->bytes.data(), pt.file->bytes.size(),
                               &pt.root, &error, limits)) {
            throw std::bad_alloc();
        }
        pt.ecus = ecuio_ecus::ecus_of(pt.root.find("ecus"));
        base_ = base_ || pt.file->extends == "obd2";
    }
}

std::string merger::compose()
{
    if (base_) {
        /* The base's channels are the first modules, named by no part */
        const ecuio_signal_set_t* base = ecuio_obd2_signal_set();
        for (uint32_t c = 0; c < base->channel_count; ++c) {
            ecuio_ecus::address a;
            a.rx = base->channels[c].rx_id;
            a.tx = base->channels[c].tx_id;
            ids_.add(std::string(), a);
            modules_.emplace_back();
            wide_ = a.rx > 0x7FFu;
            width_set_ = true;
        }
    }
    for (uint32_t p = 0; p < parts_.size(); ++p) {
        join_modules(p);
        if (!parts_[p].left_out) {
            take_signals(p);
        }
    }
    settle_expressions();
    check_bounds();
    return write();
}

/* Part p's ECUs into the set's modules (section 9, "Modules") */
void merger::join_modules(uint32_t p)
{
    part& pt = parts_[p];
    if (!pt.ecus.empty()) {
        const bool wide = pt.ecus.front().addr.rx > 0x7FFu;
        if (width_set_ && wide != wide_) {
            pt.left_out = true;
            note n;
            n.kind = ECUIO_NOTE_PART_LEFT_OUT;
            n.part = p;
            n.count =
                static_cast<uint32_t>(pt.root.find("signals")->items.size());
            n.text = std::string(": its IDs are ") + (wide ? "29" : "11") +
                     "-bit, and the set's are " + (wide_ ? "29" : "11") +
                     "-bit";
            notes_.push_back(std::move(n));
            return;
        }
        wide_ = wide;
        width_set_ = true;
    }
    const Value* entries = pt.root.find("ecus");
    for (size_t i = 0; i < pt.ecus.size(); ++i) {
        join_module(p, pt.ecus[i], entries->items[i]);
    }
}

void merger::join_module(uint32_t p, const ecuio_ecus::ecu& e,
                         const Value& entry)
{
    part& pt = parts_[p];
    const Value* name = entry.find("name");
    size_t m = ids_.place(e.addr);
    if (m == kNone) {
        if (ids_.answers(e.addr.rx)) {
            leave_module_out(p, e,
                             ": its reply ID " + hex(e.addr.rx) +
                                 " is answered under another request ID");
            return;
        }
        if (ids_.size() >= size_t{ECUIO_MAX_CHANNELS}) {
            leave_module_out(p, e,
                             ": the set holds " +
                                 std::to_string(ECUIO_MAX_CHANNELS) +
                                 " modules, the base's counted");
            return;
        }
        m = ids_.add(std::string(), e.addr);
        modules_.emplace_back();
    }
    module& have = modules_[m];
    if (ids_.id(m).empty() || have.made_up) {
        /* The first part to name the module gives it its id */
        ids_.set_id(m, std::string());
        const std::string id = ids_.unused(e.id);
        ids_.set_id(m, id);
        have.part = p;
        have.made_up = false;
        if (id != e.id) {
            note n;
            n.kind = ECUIO_NOTE_MODULE_RENAMED;
            n.part = p;
            n.key = e.id;
            n.text = id;
            notes_.push_back(std::move(n));
        }
    }
    if (have.name.empty() && name != nullptr) {
        have.name = name->text;
    }
    pt.module_of[e.id] = m;
}

void merger::leave_module_out(uint32_t p, const ecuio_ecus::ecu& e,
                              const std::string& why)
{
    part& pt = parts_[p];
    pt.module_of[e.id] = kNone;
    pt.left_note[e.id] = notes_.size();
    note n;
    n.kind = ECUIO_NOTE_MODULE_LEFT_OUT;
    n.part = p;
    n.key = e.id;
    n.text = why;
    notes_.push_back(std::move(n));
}

/* The set's bounds (ADR-0065 section 3), each refused naming its count */
void merger::check_bounds() const
{
    const std::vector<size_t> list = alive();
    if (list.empty()) {
        throw refusal{"no signal of the " + parts_words(parts_.size()) +
                      " is left to read"};
    }
    size_t second = 0;
    size_t labels = base_ ? ecuio_obd2_signal_set()->enum_count : 0;
    for (const size_t i : list) {
        second += items_[i].replaced.size();
        const Value* values = items_[i].v.find("values");
        labels += values != nullptr ? values->members.size() : 0;
    }
    if (second > ECUIO_SIGNAL_SET_MAX_SIGNALS) {
        throw refusal{"the parts' signals replaced " + std::to_string(second) +
                      " keys, more than " +
                      std::to_string(ECUIO_SIGNAL_SET_MAX_SIGNALS)};
    }
    if (labels > ECUIO_SET_MAX_TABLE) {
        throw refusal{"the parts' enums hold " + std::to_string(labels) +
                      " labels together, more than " +
                      std::to_string(ECUIO_SET_MAX_TABLE)};
    }
    check_requests();
}

/* Each module's requests, the base's counted, as the engine's plan
 * counts them (ecuio_engine_plan): one for each request and module */
void merger::check_requests() const
{
    std::vector<uint32_t> asked; /* ecuio_engine_request_code */
    const auto ask = [&asked](const ecuio_signal_def_t& d, size_t module) {
        uint8_t request[3] = {};
        uint8_t length = 0;
        if (ecuio_engine_request_for_def(&d, request, &length)) {
            asked.push_back(ecuio_engine_request_code(
                static_cast<uint32_t>(module), request, length));
        }
    };
    for (const size_t i : alive()) {
        if (!items_[i].computed) {
            ask(items_[i].def, items_[i].module);
        }
    }
    if (base_) {
        /* The base's channels are the set's first modules, in their order */
        const ecuio_signal_set_t* base = ecuio_obd2_signal_set();
        uint32_t rx_ids[ECUIO_MAX_CHANNELS] = {};
        for (uint32_t c = 0; c < base->channel_count; ++c) {
            rx_ids[c] = base->channels[c].rx_id;
        }
        for (uint32_t d = 0; d < base->count; ++d) {
            ask(base->defs[d], ecuio_engine_channel_for(&base->defs[d], rx_ids,
                                                        base->channel_count));
        }
    }
    std::sort(asked.begin(), asked.end());
    asked.erase(std::unique(asked.begin(), asked.end()), asked.end());
    for (size_t at = 0; at < asked.size();) {
        size_t end = at;
        while (end < asked.size() && (asked[end] >> 17) == (asked[at] >> 17)) {
            ++end;
        }
        if (end - at > size_t{ECUIO_MODULE_MAX_REQUESTS}) {
            throw refusal{"the parts ask " + module_label(asked[at] >> 17) +
                          " " + std::to_string(end - at) +
                          " requests, more than " +
                          std::to_string(ECUIO_MODULE_MAX_REQUESTS) +
                          ", the base's counted"};
        }
        at = end;
    }
}

Value merger::written_parts() const
{
    Value list = empty_of(Type::array);
    for (const part& pt : parts_) {
        const ecuio_signal_file& f = *pt.file;
        Value entry = empty_of(Type::object);
        add(entry, "profile", text_value(f.profile));
        add(entry, "name", text_value(f.name));
        add(entry, "format", whole_value(f.format));
        if (!f.plugin.empty()) {
            /* "<id>,<version>,<variant>", none of which holds a comma */
            const size_t first = f.plugin.find(',');
            const size_t second = f.plugin.find(',', first + 1);
            Value plugin = empty_of(Type::object);
            add(plugin, "id", text_value(f.plugin.substr(0, first)));
            add(plugin, "version",
                text_value(f.plugin.substr(first + 1, second - first - 1)));
            add(plugin, "variant", text_value(f.plugin.substr(second + 1)));
            add(entry, "plugin", std::move(plugin));
        }
        add(entry, "source", written_source(f.source));
        list.items.push_back(std::move(entry));
    }
    return list;
}

/* The modules a part names: a base channel none names has no entry */
Value merger::written_ecus() const
{
    Value list = empty_of(Type::array);
    for (size_t m = 0; m < ids_.size(); ++m) {
        if (ids_.id(m).empty()) {
            continue;
        }
        Value entry = empty_of(Type::object);
        add(entry, "id", text_value(ids_.id(m)));
        if (!modules_[m].name.empty()) {
            add(entry, "name", text_value(modules_[m].name));
        }
        add(entry, "rx", text_value(hex(ids_.addr(m).rx)));
        add(entry, "tx", text_value(hex(ids_.addr(m).tx)));
        if (modules_[m].part != ECUIO_SIGNAL_PART_NONE) {
            add(entry, "part", whole_value(modules_[m].part));
        }
        list.items.push_back(std::move(entry));
    }
    return list;
}

/* A signal as its part wrote it, with its part, the keys it replaced and
 * its module by the set's id */
Value merger::written_signal(const item& it) const
{
    Value out = empty_of(Type::object);
    add(out, "key", text_value(it.key));
    add(out, "part", whole_value(it.part));
    if (!it.replaced.empty()) {
        Value keys = empty_of(Type::array);
        for (const std::string& k : it.replaced) {
            keys.items.push_back(text_value(k));
        }
        add(out, "replaced", std::move(keys));
    }
    if (it.module != kNone) {
        add(out, "ecu", text_value(ids_.id(it.module)));
    }
    for (const Member& m : it.v.members) {
        if (m.key != "key" && m.key != "ecu") {
            out.members.push_back(m);
        }
    }
    return out;
}

std::string merger::write() const
{
    Value doc = empty_of(Type::object);
    add(doc, "havne_signals", whole_value(ECUIO_SIGNAL_FILE_FORMAT));
    add(doc, "name",
        text_value(parts_words(parts_.size()) +
                   (parts_.size() == 1 ? " read alone" : " read together")));
    Value source = empty_of(Type::object);
    add(source, "name", text_value("A merge of " + parts_words(parts_.size())));
    add(doc, "source", std::move(source));
    if (base_) {
        add(doc, "extends", text_value("obd2"));
    }
    add(doc, "parts", written_parts());
    Value ecus = written_ecus();
    if (!ecus.items.empty()) {
        add(doc, "ecus", std::move(ecus));
    }
    Value signals = empty_of(Type::array);
    for (const size_t i : alive()) {
        signals.items.push_back(written_signal(items_[i]));
    }
    add(doc, "signals", std::move(signals));
    std::string text = ecuio_json::write(doc);
    if (text.size() > ECUIO_SIGNAL_SET_MAX_BYTES) {
        throw refusal{"the merged set's text is " +
                      std::to_string(text.size()) + " bytes, more than " +
                      std::to_string(ECUIO_SIGNAL_SET_MAX_BYTES)};
    }
    return text;
}

namespace {

/* The call's own refusals: what it is given, before anything is read */
void check_parts(const ecuio_signal_file_t* const* parts, uint32_t count)
{
    if (count == 0) {
        throw refusal{"a merge takes at least one part"};
    }
    if (count > ECUIO_MERGE_MAX_PARTS) {
        throw refusal{std::to_string(count) + " parts are more than the " +
                      std::to_string(ECUIO_MERGE_MAX_PARTS) +
                      " a merged set holds"};
    }
    for (uint32_t p = 0; p < count; ++p) {
        const std::string which = "part " + std::to_string(p);
        if (!parts[p]->parts.empty()) {
            throw refusal{which + " is a merged set: merge its parts instead"};
        }
        if (parts[p]->bytes.empty()) {
            throw refusal{which + " has no text to merge"};
        }
        for (uint32_t e = 0; e < p; ++e) {
            if (parts[e] == parts[p]) {
                throw refusal{which + " is part " + std::to_string(e) +
                              " again: a set holds a part once"};
            }
            if (parts[e]->profile == parts[p]->profile) {
                throw refusal{which + " has the profile of part " +
                              std::to_string(e) + ", " + q(parts[p]->profile) +
                              ": a set holds a part once"};
            }
        }
    }
}

/* Whether the options name obd2 as the base; a refusal for what this core
 * doesn't know of them */
bool base_asked(const ecuio_merge_options_t* options)
{
    if (options == nullptr) {
        return false;
    }
    if (options->struct_size < sizeof(ecuio_merge_options_t)) {
        throw refusal{"the options' struct_size is " +
                      std::to_string(options->struct_size) + ", under its " +
                      std::to_string(sizeof(ecuio_merge_options_t)) + " bytes"};
    }
    if ((options->flags & ~ECUIO_MERGE_BASE_OBD2) != 0) {
        char flag[16];
        std::snprintf(
            flag, sizeof flag, "0x%x",
            static_cast<unsigned>(options->flags & ~ECUIO_MERGE_BASE_OBD2));
        throw refusal{std::string("a merge takes no flag ") + flag};
    }
    for (const uint32_t r : options->reserved) {
        if (r != 0) {
            throw refusal{"the options' reserved words are zero"};
        }
    }
    return (options->flags & ECUIO_MERGE_BASE_OBD2) != 0;
}

} // namespace

} // namespace ecuio_merge

extern "C" {

ecuio_status_t ecuio_signal_file_merge(const ecuio_signal_file_t* const* parts,
                                       uint32_t count,
                                       const ecuio_merge_options_t* options,
                                       ecuio_signal_file_t** out)
{
    ecuio_detail_clear();
    if (out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    if (parts == nullptr && count > 0) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    for (uint32_t p = 0; p < count && p < ECUIO_MERGE_MAX_PARTS; ++p) {
        if (parts[p] == nullptr) {
            ecuio_detail_set("part %u is NULL", static_cast<unsigned>(p));
            return ECUIO_ERR_NULL_ARGUMENT;
        }
    }
    try {
        const bool base = ecuio_merge::base_asked(options);
        ecuio_merge::check_parts(parts, count);
        ecuio_merge::merger merge(parts, count, base);
        const std::string text = merge.compose();
        ecuio_signal_file* made = nullptr;
        const ecuio_status_t status =
            ecuio_sf::load(text.data(), text.size(), ECUIO_SIGNAL_SET_MAX_BYTES,
                           ECUIO_SIGNAL_SET_MAX_SIGNALS, &made);
        if (status == ECUIO_ERR_OUT_OF_MEMORY) {
            return status;
        }
        if (status != ECUIO_OK) {
            /* The composer's fault, never the parts': the loader refused
             * a text made of what it had taken */
            const std::string why = ecuio_last_error_detail();
            ecuio_detail_set("the merged set doesn't read back: %s",
                             why.c_str());
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        std::unique_ptr<ecuio_signal_file> file(made);
        merge.give_notes(*file);
        *out = file.release();
        ecuio_detail_clear();
        return ECUIO_OK;
    } catch (const ecuio_merge::refusal& r) {
        ecuio_detail_set("%s", r.words.c_str());
        return ECUIO_ERR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    } catch (...) {
        ecuio_detail_set("the parts couldn't be merged");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
}

} // extern "C"
