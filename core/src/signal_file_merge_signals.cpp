// File/layer: core/src/signal_file_merge_signals.cpp | C++17, Qt-free, every platform
// Testing: UNIT: MergeTest (test_signals); FUZZ: fuzz_signal_merge
//
// ADR-0065 section 9's rules for a signal of a merged set: a later part's
// signal takes the place of an earlier part's of the same key or the same
// address and keeps its keys, and a computed signal the set can't compute
// is left out with a note, never a refusal. Keys are one space: a
// signal's own key and the keys it replaced never meet.

#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "json.h"
#include "signal_expr.h"
#include "signal_file_build.h"
#include "signal_file_merge.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ecuio_merge {

using ecuio_json::Member;
using ecuio_json::Value;
using ecuio_sf::q;

/* The keys an expression of the merged set reads: every alive signal's
 * own key and the keys it replaced, each under a provisional ID from its
 * place, which the loaded set gives it again */
struct merged_scope final : ecuio_sf::expr_scope {
    merged_scope(const std::vector<item>& all,
                 const std::unordered_map<std::string, size_t>& own_keys,
                 const std::unordered_map<std::string, size_t>& second_keys,
                 const std::vector<size_t>& places)
        : items(all), own(own_keys), second(second_keys), rank(places)
    {}

    size_t item_of(const std::string& key) const
    {
        auto at = own.find(key);
        if (at == own.end()) {
            at = second.find(key);
            if (at == second.end()) {
                return kNone;
            }
        }
        return at->second;
    }

    bool find(const std::string& key, uint16_t* id,
              ecuio_sf::expr_kind* kind) const override
    {
        const size_t i = item_of(key);
        if (i == kNone) {
            return false;
        }
        *id = static_cast<uint16_t>(ECUIO_USER_SIG_ID_BASE + rank[i]);
        *kind = items[i].computed ? ecuio_sf::expr_kind::computed
                : items[i].text   ? ecuio_sf::expr_kind::text
                                  : ecuio_sf::expr_kind::number;
        return true;
    }

    ecuio_sf::absent why_absent(const std::string& key) const override
    {
        return needs != nullptr && std::find(needs->begin(), needs->end(),
                                             key) != needs->end()
                   ? ecuio_sf::absent::unmet
                   : ecuio_sf::absent::unknown;
    }

    const std::vector<item>& items;
    const std::unordered_map<std::string, size_t>& own;
    const std::unordered_map<std::string, size_t>& second;
    const std::vector<size_t>& rank;
    /* The "needs" of the part whose signal is compiling */
    const std::vector<std::string>* needs = nullptr;
};

namespace {

/* A member's text, "" when it is absent or no string */
std::string text_of_member(const Value& v, const char* key)
{
    const Value* m = v.find(key);
    return m != nullptr && m->type == ecuio_json::Type::string ? m->text
                                                               : std::string();
}

} // namespace

/* Part p's signals into the list, in the part's order */
void merger::take_signals(uint32_t p)
{
    part& pt = parts_[p];
    for (const auto& k : pt.file->keys) {
        pt.def_of.emplace(k.first, k.second);
    }
    for (Member& m : pt.root.members) {
        if (m.key != "signals") {
            continue;
        }
        for (Value& v : m.value.items) {
            item it;
            if (!read_signal(p, v, &it)) {
                continue;
            }
            it.v = std::move(v);
            items_.push_back(std::move(it));
            place_of_.push_back(kNone);
            place(items_.size() - 1);
        }
    }
}

/* What the rules read of a signal: false for one whose module is left
 * out, counted on that module's note */
bool merger::read_signal(uint32_t p, const Value& v, item* out)
{
    const part& pt = parts_[p];
    out->part = p;
    out->key = text_of_member(v, "key");
    out->name = text_of_member(v, "name");
    out->unit = text_of_member(v, "unit");
    const std::string kind = text_of_member(v, "kind");
    out->computed = kind == "computed";
    out->text = kind == "text" || kind == "hex";
    /* A signal that waits for a need has no def in its part's set */
    const auto d = pt.def_of.find(out->key);
    if (d != pt.def_of.end()) {
        out->def = pt.file->defs[d->second];
        ecuio_signal_key_t address{};
        if (ecuio_signal_address_key(&out->def, &address) == ECUIO_OK) {
            out->address = address.text;
        }
        if (out->def.id < ECUIO_USER_SIG_ID_BASE) {
            out->base_id = out->def.id;
        }
    }
    if (out->computed) {
        return true;
    }
    out->module = module_for(p, v);
    return out->module != kNone;
}

/* The module a signal of part p reads, as the loader finds it for the
 * part alone: its "ecu", else the part's first, else the base's first */
size_t merger::module_for(uint32_t p, const Value& v)
{
    const part& pt = parts_[p];
    std::string id = text_of_member(v, "ecu");
    if (id.empty() && !pt.ecus.empty()) {
        id = pt.ecus.front().id;
    }
    if (id.empty()) {
        if (ids_.size() == 0) {
            return kNone;
        }
        /* A signal names its module by an id, so the base's first gets one */
        if (ids_.id(0).empty()) {
            ids_.set_id(0, ids_.unused("obd2"));
            modules_[0].made_up = true;
        }
        return 0;
    }
    const auto m = pt.module_of.find(id);
    if (m == pt.module_of.end()) {
        return kNone;
    }
    if (m->second == kNone) {
        ++notes_[pt.left_note.at(id)].count;
    }
    return m->second;
}

/* The signal holding key as one it replaced, or kNone */
size_t merger::holder_of(const std::string& key) const
{
    const auto held = second_.find(key);
    return held != second_.end() ? held->second : kNone;
}

/* Signal s into the list: in the place of an earlier part's signal of
 * its key, else of the earliest at its address, else at the end. Every
 * earlier part's signal of its key or at its address leaves the list. */
void merger::place(size_t s)
{
    const std::string key = items_[s].key;
    const std::string address = items_[s].address;
    const uint32_t part = items_[s].part;
    std::vector<size_t> gone; /* at its address */
    if (!address.empty()) {
        const auto there = at_.find(address);
        if (there != at_.end()) {
            for (const size_t e : there->second) {
                if (items_[e].part != part) {
                    gone.push_back(e);
                }
            }
        }
    }
    const auto same = own_.find(key);
    const size_t by_key = same != own_.end() ? same->second : kNone;
    const bool key_at_address =
        std::find(gone.begin(), gone.end(), by_key) != gone.end();

    /* An own key beats a second key: the holder kept it for a signal
     * that is gone */
    const size_t holder = holder_of(key);
    if (holder != kNone) {
        std::vector<std::string>& keys = items_[holder].replaced;
        keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
        second_.erase(key);
        if (std::find(gone.begin(), gone.end(), holder) == gone.end()) {
            note n;
            n.kind = ECUIO_NOTE_KEY_TAKEN;
            n.item = s;
            n.other = holder;
            n.part = part;
            n.key = key;
            notes_.push_back(std::move(n));
        }
    }

    size_t at = by_key != kNone ? place_of_[by_key] : kNone;
    if (at == kNone) {
        for (const size_t e : gone) {
            at = std::min(at, place_of_[e]);
        }
    }
    if (by_key != kNone && !key_at_address) {
        replace(s, by_key, false);
    }
    for (const size_t e : gone) {
        replace(s, e, true);
    }
    if (at == kNone) {
        at = order_.size();
        order_.push_back(kNone);
    }
    order_[at] = s;
    place_of_[s] = at;
    own_[key] = s;
    if (!address.empty()) {
        at_[address].push_back(s);
    }
    if (items_[s].base_id != 0) {
        note n;
        n.kind = ECUIO_NOTE_BESIDE_BASE;
        n.item = s;
        n.part = part;
        notes_.push_back(std::move(n));
    }
}

/* Signal gone leaves the list for s, which keeps its keys */
void merger::replace(size_t s, size_t gone, bool at_address)
{
    item& it = items_[s];
    item& was = items_[gone];
    was.alive = false;
    was.stands = s;
    order_[place_of_[gone]] = kNone;
    own_.erase(was.key);
    std::vector<std::string> keys = std::move(was.replaced);
    was.replaced.clear();
    keys.insert(keys.begin(), was.key);
    for (const std::string& k : keys) {
        if (k != it.key) {
            it.replaced.push_back(k);
            second_[k] = s;
        }
    }
    if (!was.address.empty()) {
        std::vector<size_t>& there = at_[was.address];
        there.erase(std::remove(there.begin(), there.end(), gone), there.end());
    }
    note n;
    n.kind = at_address ? ECUIO_NOTE_REPLACED_BY_ADDRESS
                        : ECUIO_NOTE_REPLACED_BY_KEY;
    n.item = s;
    n.other = gone;
    n.part = it.part;
    n.key = was.key;
    notes_.push_back(std::move(n));
}

/* The list as it stands: each place's signal, less those left out */
std::vector<size_t> merger::alive() const
{
    std::vector<size_t> list;
    list.reserve(order_.size());
    for (const size_t i : order_) {
        if (i != kNone && !items_[i].left) {
            list.push_back(i);
        }
    }
    return list;
}

void merger::leave_out(size_t i, const std::string& why)
{
    if (items_[i].left) {
        return;
    }
    items_[i].left = true;
    note n;
    n.kind = ECUIO_NOTE_SIGNAL_LEFT_OUT;
    n.item = i;
    n.part = items_[i].part;
    n.text = cut(why, 200);
    notes_.push_back(std::move(n));
}

/* Every computed signal compiled against the whole set's keys. One the
 * set can't compute is left out, and so is each signal computed from it:
 * a left-out signal's keys still name it meanwhile, so its readers are
 * found. */
void merger::settle_expressions()
{
    const std::vector<size_t> list = alive();
    if (list.size() > ECUIO_SIGNAL_SET_MAX_SIGNALS) {
        throw refusal{"the parts hold " + std::to_string(list.size()) +
                      " signals together, more than " +
                      std::to_string(ECUIO_SIGNAL_SET_MAX_SIGNALS)};
    }
    std::vector<size_t> rank(items_.size(), kNone);
    for (size_t k = 0; k < list.size(); ++k) {
        rank[list[k]] = k;
    }
    merged_scope scope(items_, own_, second_, rank);
    scope.base = base_ ? ecuio_obd2_signal_set() : nullptr;
    for (const size_t i : list) {
        if (items_[i].computed) {
            compile(i, scope);
        }
    }
    settle_chains(list);
    follow_left_out(list, scope);
}

void merger::compile(size_t i, merged_scope& scope)
{
    item& it = items_[i];
    scope.needs = &parts_[it.part].file->needs;
    try {
        const ecuio_computed_t program =
            ecuio_sf::compile_expr(text_of_member(it.v, "expr"), scope,
                                   ECUIO_SIGNAL_FILE_FORMAT, &it.reads);
        it.program = programs_.size();
        programs_.push_back(program);
    } catch (const ecuio_sf::expr_refused& r) {
        it.reads.clear();
        leave_out(
            i,
            (r.words.compare(0, 6, "reads ") == 0 ? "it " : "its expression ") +
                r.words);
        return;
    }
    /* An age bound is an input's that arrives; a computed one doesn't */
    const Value* ages = it.v.find("max_age");
    for (size_t k = 0; ages != nullptr && k < ages->members.size(); ++k) {
        const size_t named = scope.item_of(ages->members[k].key);
        if (named != kNone && items_[named].computed) {
            leave_out(i, "its max_age names " + q(ages->members[k].key) +
                             ", which is a computed signal in the set");
            return;
        }
    }
}

/* Circles and chains past ECUIO_COMPUTED_MAX_DEPTH, through any parts */
void merger::settle_chains(const std::vector<size_t>& list)
{
    std::vector<ecuio_sf::chain_node> nodes;
    nodes.reserve(list.size());
    for (size_t k = 0; k < list.size(); ++k) {
        const item& it = items_[list[k]];
        nodes.push_back(
            {&it.key, static_cast<uint16_t>(ECUIO_USER_SIG_ID_BASE + k),
             it.program != kNone && !it.left ? &programs_[it.program]
                                             : nullptr});
    }
    try {
        ecuio_sf::chain_depths(nodes);
    } catch (const ecuio_sf::chain_refused& r) {
        for (size_t k = 0; k < list.size(); ++k) {
            if (nodes[k].program == nullptr) {
                continue;
            }
            if (r.depths[k] == 0) {
                leave_out(list[k], "it is on a circle of computed signals, "
                                   "or reads one that is");
            } else if (r.depths[k] > ECUIO_COMPUTED_MAX_DEPTH) {
                leave_out(list[k],
                          "it sits " + std::to_string(r.depths[k]) +
                              " computed signals deep: a chain goes " +
                              std::to_string(ECUIO_COMPUTED_MAX_DEPTH) +
                              " deep at most");
            }
        }
    }
}

/* Each signal computed from one left out is left out too, to the end */
void merger::follow_left_out(const std::vector<size_t>& list,
                             const merged_scope& scope)
{
    /* A signal's readers: each with which of its names reads it */
    std::vector<std::vector<std::pair<size_t, uint32_t>>> readers(
        items_.size());
    std::vector<size_t> queue;
    for (const size_t i : list) {
        const item& it = items_[i];
        if (it.left) {
            queue.push_back(i);
        }
        for (uint32_t k = 0; k < it.reads.size(); ++k) {
            const size_t read = scope.item_of(it.reads[k]);
            if (read != kNone) {
                readers[read].emplace_back(i, k);
            }
        }
    }
    for (size_t at = 0; at < queue.size(); ++at) {
        for (const auto& r : readers[queue[at]]) {
            if (!items_[r.first].left) {
                leave_out(r.first, "it reads " +
                                       q(items_[r.first].reads[r.second]) +
                                       ", which is left out");
                queue.push_back(r.first);
            }
        }
    }
}

} // namespace ecuio_merge
