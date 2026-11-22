// File/layer: core/src/signal_file_merge.h | internal, C++17, Qt-free
// Testing: UNIT: MergeTest (test_signals); FUZZ: fuzz_signal_merge
//
// The merge of signal files into one set (ADR-0065 section 9,
// ecuio_signal_file_merge): compose, then load. Each part's own text is
// read again, section 9's rules are applied to its signals as JSON, one
// format 3 text is written and the signal-file loader reads it. The
// loaded file is the result, so the text reads back to the same set by
// construction and the loader's rules are the only ones: a signal's new
// member needs nothing here, a new top-level key needs a line in write().

#ifndef ECUIO_SIGNAL_FILE_MERGE_H
#define ECUIO_SIGNAL_FILE_MERGE_H

#include "ecuio/signals.h"
#include "ecu_join.h"
#include "json.h"
#include "signal_file_build.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ecuio_merge {

constexpr size_t kNone = SIZE_MAX;

struct merged_scope; /* signal_file_merge_signals.cpp */
struct note_counts;  /* signal_file_merge_notes.cpp */

/* A rule of the call refused, in words */
struct refusal {
    std::string words;
};

/* One part as given: its loaded file and its text parsed again */
struct part {
    const ecuio_signal_file* file = nullptr;
    ecuio_json::Value root;
    std::vector<ecuio_ecus::ecu> ecus; /* its own, by its ids */
    bool left_out = false;             /* of the other ID width */
    /* Its ECU ids as the merged set's modules, kNone for one left out,
     * which has its note's place in left_note */
    std::unordered_map<std::string, size_t> module_of;
    std::unordered_map<std::string, size_t> left_note;
    /* Its keys as its loaded file's defs: none for a signal that waits */
    std::unordered_map<std::string, uint32_t> def_of;
};

/* The merged set's modules by id and address, joined as a put joins a
 * file's ECUs to a plugin's (ecu_join.h) */
class module_ids : public ecuio_ecus::joined
{
public:
    module_ids() : joined({}) {}

    size_t size() const { return have_.size(); }
    const std::string& id(size_t i) const { return have_[i].id; }
    const ecuio_ecus::address& addr(size_t i) const { return have_[i].addr; }
    void set_id(size_t i, const std::string& id) { have_[i].id = id; }

    size_t add(const std::string& id, const ecuio_ecus::address& a)
    {
        ecuio_ecus::ecu e;
        e.id = id;
        e.addr = a;
        have_.push_back(std::move(e));
        return have_.size() - 1;
    }

    /* The module at a, or kNone */
    size_t place(const ecuio_ecus::address& a) const
    {
        const ecuio_ecus::ecu* e = at_address(a);
        return e != nullptr ? static_cast<size_t>(e - have_.data()) : kNone;
    }

    /* Whether a module answers on rx, whatever it is asked on */
    bool answers(uint32_t rx) const
    {
        for (const ecuio_ecus::ecu& e : have_) {
            if (e.addr.rx == rx) {
                return true;
            }
        }
        return false;
    }

    /* id, or "<id>_2" and on when a module has it */
    std::string unused(const std::string& id) const
    {
        return taken(id) ? free_id(id) : id;
    }
};

/* What a module has besides its id and addresses */
struct module {
    std::string name; /* the earliest part's that gives one */
    uint32_t part = ECUIO_SIGNAL_PART_NONE; /* whose id stands */
    bool made_up = false; /* its id is "obd2", for want of any part's */
};

/* One signal of the merged list */
struct item {
    ecuio_json::Value v; /* as its part wrote it */
    uint32_t part = 0;
    std::string key;
    std::string name;
    std::string unit;
    std::string address;      /* its address key, "" for a computed signal */
    ecuio_signal_def_t def{}; /* its part's def: what it asks */
    size_t module = kNone;    /* the module it reads; kNone computed */
    bool computed = false;
    bool text = false;      /* kind text or hex: no expression reads it */
    uint16_t base_id = 0;   /* alone, its part put it in this base def's
                             place; 0 for any other */
    bool alive = true;      /* in the list: no later signal took its place */
    bool left = false;      /* left out of the set, its place kept */
    size_t stands = kNone;  /* the signal that took its place */
    size_t program = kNone; /* its compiled program's place, computed */
    std::vector<std::string> replaced; /* the keys it stands for besides */
    std::vector<std::string> reads;    /* the names its expression reads */
};

/* What a note's sentence is made from once the list is settled */
struct note {
    uint32_t kind = 0;
    size_t item = kNone;  /* the signal it is about */
    size_t other = kNone; /* the signal it took the place or a key of */
    uint32_t part = ECUIO_SIGNAL_PART_NONE;
    uint32_t count = 0;
    std::string key;  /* the key replaced or taken, or a module's id as
                         its part has it */
    std::string text; /* why a module, a part or a signal is left out, or
                         a renamed module's id in the set */
};

class merger
{
public:
    merger(const ecuio_signal_file_t* const* parts, uint32_t count,
           bool base_asked);

    /* The merged set's text, or a refusal thrown (std::bad_alloc too) */
    std::string compose();
    /* The merge's notes into the file its text loaded to */
    void give_notes(ecuio_signal_file& f) const;

private:
    /* signal_file_merge.cpp: the parts, the modules, the text */
    void join_modules(uint32_t p);
    void join_module(uint32_t p, const ecuio_ecus::ecu& e,
                     const ecuio_json::Value& entry);
    void leave_module_out(uint32_t p, const ecuio_ecus::ecu& e,
                          const std::string& why);
    void check_bounds() const;
    void check_requests() const;
    std::string write() const;
    ecuio_json::Value written_parts() const;
    ecuio_json::Value written_ecus() const;
    ecuio_json::Value written_signal(const item& it) const;

    /* signal_file_merge_signals.cpp: section 9's rules for a signal */
    void take_signals(uint32_t p);
    bool read_signal(uint32_t p, const ecuio_json::Value& v, item* out);
    size_t module_for(uint32_t p, const ecuio_json::Value& v);
    void place(size_t s);
    void replace(size_t s, size_t gone, bool at_address);
    size_t holder_of(const std::string& key) const;
    std::vector<size_t> alive() const;
    void settle_expressions();
    void compile(size_t i, merged_scope& scope);
    void settle_chains(const std::vector<size_t>& list);
    void leave_out(size_t i, const std::string& why);
    void follow_left_out(const std::vector<size_t>& list,
                         const merged_scope& scope);

    /* signal_file_merge_notes.cpp: the notes' sentences */
    std::string sentence(const note& n, const note_counts& counts) const;
    std::string replaced_words(const note& n, const note_counts& counts) const;
    std::string beside_words(const note& n, const note_counts& counts) const;
    std::string named(size_t item) const;
    std::string part_name(uint32_t part) const;
    std::string module_label(size_t module) const;
    std::string address_words(const item& it) const;
    size_t standing(size_t item) const;

    std::vector<part> parts_;
    bool base_ = false;
    bool wide_ = false; /* the set's IDs are 29-bit */
    bool width_set_ = false;
    module_ids ids_;
    std::vector<module> modules_; /* beside ids_ */
    std::vector<item> items_;
    std::vector<ecuio_computed_t> programs_; /* an item's, by its program */
    std::vector<size_t> order_; /* the list: each place's item, or kNone */
    std::unordered_map<std::string, size_t> own_;    /* an own key's item */
    std::unordered_map<std::string, size_t> second_; /* a second key's */
    std::unordered_map<std::string, std::vector<size_t>> at_; /* an address's */
    std::vector<size_t> place_of_; /* an item's place in order_ */
    std::vector<note> notes_;
};

/* Text cut to max bytes at a UTF-8 character, "..." after a cut */
std::string cut(const std::string& text, size_t max);

} // namespace ecuio_merge

#endif /* ECUIO_SIGNAL_FILE_MERGE_H */
