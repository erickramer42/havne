// File/layer: core/src/signal_expr.h | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest, SignalExprTest, PluginTest (test_signals);
//          FUZZ: fuzz_signal_file
//
// A computed signal's expression (ADR-0051 section 2, ADR-0062): the
// compiler from its text to a program's steps, and the rules for computed
// signals reading each other. Its caller names the signals an expression
// can read and says where a refusal lies, so the loader's rules and words
// serve whatever holds an expression.

#ifndef ECUIO_SIGNAL_EXPR_H
#define ECUIO_SIGNAL_EXPR_H

#include "ecuio/signals.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ecuio_sf {

/* What a name in an expression is: text covers a hex signal too */
enum class expr_kind { number, computed, text };

/* The signals an expression can read by key, and base, the obd2 set when
 * "obd2.<key>" names its defs, else NULL */
struct expr_scope {
    const ecuio_signal_set_t* base = nullptr;
    /* key's def ID and kind; false for a key no signal has */
    virtual bool find(const std::string& key, uint16_t* id,
                      expr_kind* kind) const = 0;

protected:
    ~expr_scope() = default;
};

/* An expression's refusal: the offset in its text, from 0, and the words
 * that follow the expression's name ("ends where a value should be") */
struct expr_refused {
    size_t at;
    std::string words;
};

/* text compiled at a file's format (a format 1 file meets format 2's
 * forms refused by name): its program with every age bound 0, and into
 * names the name each input was read by, in the inputs' order, with the
 * offset it is first read at into places (NULL: not wanted). Throws
 * expr_refused, and std::bad_alloc. */
ecuio_computed_t compile_expr(const std::string& text, const expr_scope& scope,
                              uint32_t format, std::vector<std::string>* names,
                              std::vector<size_t>* places = nullptr);

/* The key an expression reads obd2's def at index by, after "obd2.":
 * what obd2_def (signal_file_build.h) takes back to index. "" for a def
 * with no key: one that is no Service 01 field (the computed Boost). */
std::string obd2_key_of(const ecuio_signal_set_t* base, uint32_t index);

/* A signal as the chain rules see it: program NULL for one that isn't
 * computed, key NULL for an expression checked with no key, which
 * nothing reads. key and program must outlive the call. */
struct chain_node {
    const std::string* key;
    uint16_t id;
    const ecuio_computed_t* program;
};

/* A chain's refusal: the node it is refused at and the whole sentence;
 * a circle's nodes from that one in the order each reads the next (empty
 * for a chain too deep), and the depths settled, 0 for a node on a
 * circle or reading one */
struct chain_refused {
    size_t node;
    std::string words;
    std::vector<size_t> circle;
    std::vector<uint32_t> depths;
};

/* Each node's depth, 0 for one that isn't computed, 1 for one reading
 * no computed node. Throws chain_refused for a circle or a chain past
 * ECUIO_COMPUTED_MAX_DEPTH, and std::bad_alloc. */
std::vector<uint32_t> chain_depths(const std::vector<chain_node>& nodes);

/* What an expression past ECUIO_EXPR_MAX_BYTES is refused with, after
 * its name: the compiler's words, for a caller that judges the length
 * before it copies the text */
std::string too_long_words();

/* The loader's sentences for a computed signal's readers, shared with
 * the check of an expression against a loaded file.
 * "signal 'p''s max_age names 'a', a computed signal: ...": */
std::string aged_computed_words(const std::string& reader,
                                const std::string& key);
/* "signal 'k' replaces 'Name', which <reader> reads": a signal named as
 * a base def stands in its place. reader is quoted by the caller. One of
 * the ends below follows it. */
std::string replaced_words(const std::string& key, const std::string& name,
                           const std::string& reader);
extern const char* const kReplacedNotANumber;
extern const char* const kReplacedFormat;
extern const char* const kReplacedAged;
/* ": that puts 'Name' 9 computed signals deep: ..." */
std::string too_deep_words(const std::string& name, uint32_t depth);

} // namespace ecuio_sf

#endif /* ECUIO_SIGNAL_EXPR_H */
