// File/layer: cli/page_lines.h | havne-cli, plugins check's pages (ADR-0052's appendix on pages)
// Testing: UNIT: CliPluginTest (test_cli); goldens plugins-check-pages.jsonl
//
// A plugin's Dashboard pages as plugins check judges them: each page
// through the layout check (layout_check.h, the app's reading rules), its
// key: references against the plugin's variants, its name against the
// form's other pages. Public headers only.

#ifndef HAVNE_CLI_PAGE_LINES_H
#define HAVNE_CLI_PAGE_LINES_H

#include "jsonl.h"
#include "layout_check.h"

#include "ecuio/plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace cli {

static_assert(ECUIO_PLUGIN_PAGE_MAX_BYTES ==
                  static_cast<unsigned>(layout::kFileMaxBytes),
              "a plugin's page is a layout file: core and the check share "
              "one bound");

constexpr uint16_t kPagesCore[3] = {0, 9, 0};

// One page as the check found it; words says why it fails, "" when it
// passes
struct PageCheck {
    std::string file;
    std::string form;
    std::string name; // trimmed; "" for none
    layout::Verdict verdict;
    std::vector<std::string> unknown_keys;
    std::vector<std::string> variants; // those holding every key: ref
    bool reads_plugin = false;         // a cell reads a key: ref
    std::string words;
};

// The page's name and its cells' key: references, each once, in order
inline void page_refs(const std::string& text, std::string* name,
                      std::vector<std::string>* keys)
{
    layout::Json d;
    if (!layout::Parser(text).parse(&d)) {
        return;
    }
    const layout::Json* pages = d.get("pages");
    if (pages == nullptr || !pages->is_array() || pages->arr.empty()) {
        return;
    }
    const layout::Json& page = pages->arr.front();
    const layout::Json* n = page.get("name");
    if (n != nullptr && n->is_string()) {
        *name = layout::utf8_of(layout::js_trim(n->str));
    }
    const layout::Json* cells = page.get("cells");
    if (cells == nullptr || !cells->is_array()) {
        return;
    }
    for (const layout::Json& c : cells->arr) {
        const layout::Json* refs = c.get("refs");
        if (refs == nullptr || !refs->is_array()) {
            continue;
        }
        for (const layout::Json& r : refs->arr) {
            if (!r.is_string() || r.str.compare(0, 4, u"key:") != 0) {
                continue;
            }
            const std::string key = layout::utf8_of(r.str.substr(4));
            bool known = false;
            for (const std::string& k : *keys) {
                known = known || k == key;
            }
            if (!known) {
                keys->push_back(key);
            }
        }
    }
}

// Which keys no variant has, and which variants have them all
inline void judge_keys(const ecuio_plugin_t* p, uint32_t variant_count,
                       const std::vector<std::string>& keys, PageCheck* c)
{
    for (uint32_t v = 0; v < variant_count; ++v) {
        bool all = true;
        for (const std::string& k : keys) {
            all = all && ecuio_plugin_variant_find(p, v, k.c_str()) != nullptr;
        }
        if (all) {
            char id[ECUIO_PLUGIN_VARIANT_ID_MAX + 1] = {};
            ecuio_plugin_variant(p, v, id, sizeof id);
            c->variants.push_back(id);
        }
    }
    for (const std::string& k : keys) {
        bool found = false;
        for (uint32_t v = 0; v < variant_count && !found; ++v) {
            found = ecuio_plugin_variant_find(p, v, k.c_str()) != nullptr;
        }
        if (!found) {
            c->unknown_keys.push_back(k);
        }
    }
}

// The words of a page that reads to a verdict other than whole
inline std::string verdict_words(const layout::Verdict& v)
{
    if (v.problem == "dropped") {
        return std::to_string(v.dropped_cells) +
               (v.dropped_cells == 1 ? " cell doesn't" : " cells don't") +
               " read: a field of the wrong kind or past its bound";
    }
    return layout::layout_words(v);
}

// Page index judged; seen holds the form and name of the pages before it
inline PageCheck
check_page(const ecuio_plugin_t* p, uint32_t variant_count, uint32_t index,
           std::vector<std::pair<std::string, std::string>>* seen)
{
    PageCheck c;
    ecuio_plugin_page_t page{};
    page.struct_size = sizeof page;
    if (ecuio_plugin_page(p, index, &page) != ECUIO_OK) {
        c.words = "the page can't be read back from core";
        return c;
    }
    c.file = page.path;
    c.form = page.form == ECUIO_PLUGIN_PAGE_PHONE ? "phone" : "desktop";
    const std::string text = ecuio_plugin_page_text(p, index);
    c.verdict = layout::check(text, c.form);
    std::vector<std::string> keys;
    page_refs(text, &c.name, &keys);
    c.reads_plugin = !keys.empty();
    judge_keys(p, variant_count, keys, &c);
    if (!c.verdict.problem.empty()) {
        c.words = verdict_words(c.verdict);
    } else if (!c.unknown_keys.empty()) {
        c.words = "a cell reads key:" + c.unknown_keys.front() +
                  ", which no variant of the plugin has";
    }
    for (const auto& s : *seen) {
        if (c.words.empty() && s.first == c.form && s.second == c.name) {
            c.words = "another " + c.form + " page is named \"" + c.name +
                      "\": the list of pages would show two the same";
        }
    }
    seen->emplace_back(c.form, c.name);
    return c;
}

inline size_t page_line_bytes(const PageCheck& c)
{
    size_t n = 1024 + Line::string_bytes(c.file.data(), c.file.size()) +
               Line::string_bytes(c.name.data(), c.name.size()) +
               Line::string_bytes(c.words.data(), c.words.size());
    for (const std::string& k : c.unknown_keys) {
        n += Line::string_bytes(k.data(), k.size()) + 2;
    }
    return n + c.variants.size() * (ECUIO_PLUGIN_VARIANT_ID_MAX + 4);
}

inline void string_list(Line& l, const std::vector<std::string>& items)
{
    l.ch('[');
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            l.ch(',');
        }
        l.string_exact(items[i].data(), items[i].size());
    }
    l.ch(']');
}

// {"type":"page", file, form, name, ok, problem, cells, dropped_cells,
// variants, unknown_keys, reads_plugin, words}: problem is the layout
// check's ("" when it reads whole), variants those that hold all its key:
// references, reads_plugin false for a page of built-in signals alone
inline void page_line(Line& l, const PageCheck& c)
{
    l.raw("{\"type\":\"page\"");
    l.key("file");
    l.string_exact(c.file.data(), c.file.size());
    l.key("form");
    l.string(c.form.c_str(), c.form.size());
    l.key("name");
    if (c.name.empty()) {
        l.raw("null");
    } else {
        l.string_exact(c.name.data(), c.name.size());
    }
    l.key("ok");
    l.raw(c.words.empty() ? "true" : "false");
    l.key("problem");
    l.string(c.verdict.problem.c_str(), c.verdict.problem.size());
    l.key("cells");
    l.u64(static_cast<uint64_t>(c.verdict.cells));
    l.key("dropped_cells");
    l.u64(static_cast<uint64_t>(c.verdict.dropped_cells));
    l.key("variants");
    string_list(l, c.variants);
    l.key("unknown_keys");
    string_list(l, c.unknown_keys);
    l.key("reads_plugin");
    l.raw(c.reads_plugin ? "true" : "false");
    l.key("words");
    if (c.words.empty()) {
        l.raw("null");
    } else {
        l.string_exact(c.words.data(), c.words.size());
    }
    l.ch('}');
}

// Whether a plugin with pages declares a requires.core older than
// kPagesCore, which a core before it refuses the manifest's "pages" for
inline bool pages_need_newer_core(const uint16_t declared[3],
                                  uint32_t page_count)
{
    for (int i = 0; page_count > 0 && i < 3; ++i) {
        if (declared[i] != kPagesCore[i]) {
            return declared[i] < kPagesCore[i];
        }
    }
    return false;
}

// {"type":"requires_core", declared, needs, warning} for pages_need_newer_core
inline void pages_core_line(Line& l, const uint16_t declared[3])
{
    char version[24];
    std::snprintf(version, sizeof version, "%u.%u.%u", unsigned{declared[0]},
                  unsigned{declared[1]}, unsigned{declared[2]});
    l.raw("{\"type\":\"requires_core\"");
    l.key("declared");
    l.string(version, std::strlen(version));
    std::snprintf(version, sizeof version, "%u.%u.%u", unsigned{kPagesCore[0]},
                  unsigned{kPagesCore[1]}, unsigned{kPagesCore[2]});
    l.key("needs");
    l.string(version, std::strlen(version));
    l.key("warning");
    const std::string why =
        std::string("the manifest has \"pages\", which core reads from ") +
        version + " on: raise the manifest's \"requires\" core to " + version +
        ", so an older Havne names that version instead of "
        "refusing the key";
    l.string(why.c_str(), why.size());
    l.ch('}');
}

} // namespace cli

#endif /* HAVNE_CLI_PAGE_LINES_H */
