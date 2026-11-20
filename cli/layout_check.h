// File/layer: cli/layout_check.h | havne-cli, a dashboard layout file's check (ADR-0057's appendix on the layout file)
// Testing: UNIT: CliLayoutTest (test_cli), the shared corpus tests/data/layouts
//
// `havne-cli layout check <file>`: the reading rules of the app's
// DashboardBook (app/qml/DashboardBook.qml: parse, pageProblem,
// cellProblem, documentProblem, importProblem), ported to C++ so a layout
// file can be checked with no app. The port is held equal to the QML by
// the corpus in tests/data/layouts: tst_dashboardbook and test_cli read
// each case to the verdicts beside it, so a rule changed on one side
// fails the other. A rule here is the QML's rule, in the QML's order.
//
// The JSON is read as JavaScript's JSON.parse reads it (the QML's reader):
// strings are UTF-16 units, a lone surrogate escape is kept, a repeated
// key takes its last value, a number past a double is a read that fails.
// Sizes are the QML's too: a stored text's bytes are those of what
// JSON.stringify gives, so serialize() writes that form.

#ifndef HAVNE_CLI_LAYOUT_CHECK_H
#define HAVNE_CLI_LAYOUT_CHECK_H

#include "jsonl.h"
#include "layout_json.h"
#include "layout_text.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace cli {
namespace layout {

constexpr int kMaxPages = 10;
constexpr int kMaxCells = 24;
constexpr int kMaxRefs = 8;
constexpr int kMaxLabel = 40;
constexpr int kMaxColumns = 12;
constexpr int kMaxRows = 48;
constexpr int kFormat1MaxRows = 16;
constexpr int kFormat1MaxBytes = 65536;
constexpr int kMaxBytes = kFormat1MaxBytes + 2 * kMaxPages * kMaxCells;
constexpr int kPhoneSpanScale = 3;
constexpr int kMaxFiles = 32;
constexpr int kMaxFileName = 80;
// What the form and files keys may add to a stored document's bound
constexpr int kFileKeysBytes = 16384;
constexpr int kFileMaxBytes = kMaxBytes + kFileKeysBytes;

struct OptionRule {
    const char* name;
    enum Type { Number, Positive, Whole, Bool, Text, Any } type;
    int min;
    int max;
};

inline const OptionRule* option_rules(size_t* count)
{
    static const OptionRule rules[] = {
        {"from", OptionRule::Number, 0, 0},
        {"to", OptionRule::Number, 0, 0},
        {"coldBelow", OptionRule::Number, 0, 0},
        {"warnFrom", OptionRule::Number, 0, 0},
        {"dangerFrom", OptionRule::Number, 0, 0},
        {"rise", OptionRule::Number, 0, 0},
        {"warnBelow", OptionRule::Number, 0, 0},
        {"dangerBelow", OptionRule::Number, 0, 0},
        {"majorStep", OptionRule::Positive, 0, 0},
        {"minorStep", OptionRule::Positive, 0, 0},
        {"labelDivisor", OptionRule::Positive, 0, 0},
        {"decimals", OptionRule::Whole, 0, 6},
        {"count", OptionRule::Whole, 1, 32},
        {"cells", OptionRule::Whole, 2, 64},
        {"digits", OptionRule::Whole, 1, 8},
        {"window", OptionRule::Whole, 10, 120},
        {"peak", OptionRule::Bool, 0, 0},
        {"progress", OptionRule::Bool, 0, 0},
        {"vertical", OptionRule::Bool, 0, 0},
        {"marks", OptionRule::Bool, 0, 0},
        {"caption", OptionRule::Text, 0, 40},
        {"description", OptionRule::Text, 0, 80},
        {"peakLabel", OptionRule::Text, 0, 8},
        {"accent", OptionRule::Any, 0, 0},
        {"color", OptionRule::Any, 0, 0},
    };
    *count = sizeof rules / sizeof rules[0];
    return rules;
}

inline bool option_fits(const OptionRule& r, const Json& v)
{
    switch (r.type) {
    case OptionRule::Number:
        return v.kind == Json::Num && std::isfinite(v.num);
    case OptionRule::Positive:
        return v.kind == Json::Num && std::isfinite(v.num) && v.num > 0;
    case OptionRule::Whole:
        return is_integer(&v) && v.num >= r.min && v.num <= r.max;
    case OptionRule::Bool:
        return v.kind == Json::Bool;
    case OptionRule::Text:
        return v.kind == Json::Str &&
               code_points(v.str) <= static_cast<size_t>(r.max);
    case OptionRule::Any:
        return v.kind == Json::Str;
    }
    return false;
}

// Whether a cell may be kept (DashboardBook.cellProblem is ""); rows the
// bound on its height
inline bool cell_fits(const Json& c, int rows)
{
    if (!c.is_object()) {
        return false;
    }
    const Json* kind = c.get("kind");
    if (kind == nullptr || !kind->is_string() || kind->str.empty()) {
        return false;
    }
    const Json* w = c.get("w");
    if (!is_integer(w) || w->num < 1 || w->num > kMaxColumns) {
        return false;
    }
    const Json* h = c.get("h");
    if (!is_integer(h) || h->num < 1 || h->num > rows) {
        return false;
    }
    const Json* refs = c.get("refs");
    if (refs == nullptr || !refs->is_array()) {
        return false;
    }
    for (const Json& r : refs->arr) {
        if (!r.is_string()) {
            return false;
        }
    }
    if (refs->arr.size() > static_cast<size_t>(kMaxRefs)) {
        return false;
    }
    const Json* label = c.get("label");
    if (label != nullptr &&
        (!label->is_string() ||
         code_points(label->str) > static_cast<size_t>(kMaxLabel))) {
        return false;
    }
    const Json* unit = c.get("unit");
    if (unit != nullptr && !unit->is_string()) {
        return false;
    }
    const Json* options = c.get("options");
    if (options == nullptr) {
        return true;
    }
    if (!options->is_object()) {
        return false;
    }
    size_t n = 0;
    const OptionRule* rules = option_rules(&n);
    for (size_t i = 0; i < n; ++i) {
        const Json* v = options->get(rules[i].name);
        if (v != nullptr && !option_fits(rules[i], *v)) {
            return false;
        }
    }
    const Json* from = options->get("from");
    const Json* to = options->get("to");
    return from == nullptr || to == nullptr || !(from->num >= to->num);
}

// Whether a page's own fields may be kept (DashboardBook.pageProblem)
inline bool page_fits(const Json& p)
{
    if (!p.is_object()) {
        return false;
    }
    const Json* name = p.get("name");
    if (name == nullptr || !name->is_string()) {
        return false;
    }
    const std::u16string trimmed = js_trim(name->str);
    if (trimmed.empty() ||
        code_points(trimmed) > static_cast<size_t>(kMaxLabel)) {
        return false;
    }
    const Json* cells = p.get("cells");
    return cells != nullptr && cells->is_array();
}

// DashboardBook.documentProblem is "": the document reads back as it is
inline bool document_fits(const Json& d)
{
    const Json* format = d.get("format");
    const Json* pages = d.get("pages");
    if (!d.is_object() || !is_integer(format) || format->num != 2 ||
        pages == nullptr || !pages->is_array() || pages->arr.empty() ||
        pages->arr.size() > static_cast<size_t>(kMaxPages)) {
        return false;
    }
    for (const Json& p : pages->arr) {
        if (!page_fits(p)) {
            return false;
        }
        const Json* cells = p.get("cells");
        if (cells->arr.size() > static_cast<size_t>(kMaxCells)) {
            return false;
        }
        for (const Json& c : cells->arr) {
            if (!cell_fits(c, kMaxRows)) {
                return false;
            }
        }
    }
    return serialized_bytes(d) <= static_cast<size_t>(kMaxBytes);
}

struct File {
    std::string name; // UTF-8
    std::string sha256;
};

struct Verdict {
    // "" (read), "dropped" (read with pages or cells left out), "none"
    // (nothing to read), "damaged", "newer" or "otherform"
    std::string problem;
    std::string as;   // the form it was read as
    std::string form; // the file's own, "" for none
    bool read = false; // problem is "" or "dropped": the counts hold
    int pages = 0;
    int cells = 0;
    int dropped_pages = 0;
    int dropped_cells = 0;
    std::vector<File> files;
    std::vector<std::string> notes;
    bool has_first = false;
    int first_w = 0;
    int first_h = 0;
};

inline std::string utf8_of(const std::u16string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned u = s[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < s.size() &&
            s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            put_utf8(&out, 0x10000 + ((u - 0xD800) << 10) + (s[i + 1] - 0xDC00));
            ++i;
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            put_utf8(&out, 0xFFFD);
        } else {
            put_utf8(&out, u);
        }
    }
    return out;
}

// A file's check: DashboardBook.importProblem's verdicts. as is the form
// loading it, "phone" or "desktop", or "" to take the file's own form
// ("desktop", noted "assumed-desktop", when it names none). text holds
// valid UTF-8 with no BOM
inline Verdict check(const std::string& text, const std::string& as)
{
    Verdict v;
    v.as = as.empty() ? "desktop" : as;
    if (text.empty()) {
        v.problem = "none";
        return v;
    }
    if (text.size() > static_cast<size_t>(kFileMaxBytes)) {
        v.problem = "damaged";
        return v;
    }
    Json d;
    if (!Parser(text).parse(&d) || !d.is_object()) {
        v.problem = "damaged";
        return v;
    }
    const Json* declared_p = d.get("form");
    const Json* listed_p = d.get("files");
    const bool has_declared = declared_p != nullptr;
    const bool has_listed = listed_p != nullptr;
    const Json declared = has_declared ? *declared_p : Json();
    const Json listed = has_listed ? *listed_p : Json();
    d.erase("form");
    d.erase("files");
    const bool declared_ok =
        has_declared && declared.is_string() &&
        (declared.str == u"phone" || declared.str == u"desktop");
    std::string form = as;
    std::vector<std::string> notes;
    if (form.empty()) {
        form = declared_ok ? utf8_of(declared.str) : "desktop";
        if (!has_declared) {
            notes.push_back("assumed-desktop");
        }
    }
    v.as = form;

    // parse: the stored text's rules, over what a write would store
    const size_t bytes = serialized_bytes(d);
    if (bytes > static_cast<size_t>(kMaxBytes)) {
        v.problem = "damaged";
        return v;
    }
    const Json* format = d.get("format");
    if (!is_integer(format) || format->num < 1) {
        v.problem = "damaged";
        return v;
    }
    if (format->num > 2) {
        v.problem = "newer";
        return v;
    }
    Json* pages = nullptr;
    for (size_t i = 0; i < d.keys.size(); ++i) {
        if (d.keys[i] == u"pages") {
            pages = &d.vals[i];
        }
    }
    if (pages == nullptr || !pages->is_array() ||
        pages->arr.size() > static_cast<size_t>(kMaxPages)) {
        v.problem = "damaged";
        return v;
    }
    for (const Json& p : pages->arr) {
        const Json* cells = p.is_object() ? p.get("cells") : nullptr;
        if (cells != nullptr && cells->is_array() &&
            cells->arr.size() > static_cast<size_t>(kMaxCells)) {
            v.problem = "damaged";
            return v;
        }
    }
    const int rows = format->num == 1 ? kFormat1MaxRows : kMaxRows;
    const size_t pages_read = pages->arr.size();
    std::vector<Json> kept;
    for (Json& p : pages->arr) {
        if (page_fits(p)) {
            kept.push_back(std::move(p));
        }
    }
    pages->arr = std::move(kept);
    size_t cells_dropped = 0;
    for (Json& p : pages->arr) {
        for (size_t i = 0; i < p.keys.size(); ++i) {
            if (p.keys[i] != u"cells") {
                continue;
            }
            std::vector<Json> kept_cells;
            for (Json& c : p.vals[i].arr) {
                if (cell_fits(c, rows)) {
                    kept_cells.push_back(std::move(c));
                } else {
                    ++cells_dropped;
                }
            }
            p.vals[i].arr = std::move(kept_cells);
        }
    }
    if (format->num == 1) {
        if (bytes > static_cast<size_t>(kFormat1MaxBytes) ||
            serialized_bytes(d) > static_cast<size_t>(kFormat1MaxBytes)) {
            v.problem = "damaged";
            return v;
        }
        // The upgrade: the form is the key the text came from
        for (size_t i = 0; i < d.keys.size(); ++i) {
            if (d.keys[i] == u"format") {
                d.vals[i].num = 2;
            }
        }
        if (form == "phone") {
            for (Json& p : pages->arr) {
                for (size_t i = 0; i < p.keys.size(); ++i) {
                    if (p.keys[i] != u"cells") {
                        continue;
                    }
                    for (Json& c : p.vals[i].arr) {
                        for (size_t k = 0; k < c.keys.size(); ++k) {
                            if (c.keys[k] == u"w") {
                                c.vals[k].num = std::fmin(
                                    kMaxColumns, c.vals[k].num * kPhoneSpanScale);
                            } else if (c.keys[k] == u"h") {
                                c.vals[k].num *= kPhoneSpanScale;
                            }
                        }
                    }
                }
            }
        }
    }
    if (!document_fits(d)) {
        v.problem = "damaged";
        return v;
    }

    // importProblem: the file's own keys
    if ((has_declared && !declared_ok) ||
        (has_listed &&
         (!listed.is_array() ||
          listed.arr.size() > static_cast<size_t>(kMaxFiles)))) {
        v.problem = "damaged";
        return v;
    }
    std::vector<File> files;
    if (has_listed) {
        for (const Json& f : listed.arr) {
            const Json* name = f.is_object() ? f.get("name") : nullptr;
            const Json* sha = f.is_object() ? f.get("sha256") : nullptr;
            bool hex = sha != nullptr && sha->is_string() && sha->str.size() == 64;
            for (size_t i = 0; hex && i < sha->str.size(); ++i) {
                const char16_t c = sha->str[i];
                hex = (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f');
            }
            if (name == nullptr || !name->is_string() ||
                js_trim(name->str).empty() ||
                code_points(name->str) > static_cast<size_t>(kMaxFileName) ||
                !hex) {
                v.problem = "damaged";
                return v;
            }
            files.push_back({utf8_of(name->str), utf8_of(sha->str)});
        }
    }
    if (has_declared) {
        v.form = utf8_of(declared.str);
    }
    if (has_declared && v.form != form) {
        v.problem = "otherform";
        return v;
    }
    v.read = true;
    v.files = std::move(files);
    v.notes = notes;
    if (!has_declared) {
        v.notes.insert(v.notes.begin(), "no-form");
    }
    v.pages = static_cast<int>(pages->arr.size());
    for (const Json& p : pages->arr) {
        v.cells += static_cast<int>(p.get("cells")->arr.size());
    }
    v.dropped_pages = static_cast<int>(pages_read - pages->arr.size());
    v.dropped_cells = static_cast<int>(cells_dropped);
    v.problem = v.dropped_pages > 0 || v.dropped_cells > 0 ? "dropped" : "";
    const Json& first = pages->arr[0];
    const Json& first_cells = *first.get("cells");
    if (!first_cells.arr.empty()) {
        v.has_first = true;
        v.first_w = static_cast<int>(first_cells.arr[0].get("w")->num);
        v.first_h = static_cast<int>(first_cells.arr[0].get("h")->num);
    }
    return v;
}

// The words a refused verdict gives
inline const char* layout_words(const Verdict& v)
{
    if (v.problem == "none") {
        return "the file is empty";
    }
    if (v.problem == "newer") {
        return "the file is from a newer version of the dashboard format";
    }
    if (v.problem == "otherform") {
        return v.form == "phone"
                   ? "the file is a phone layout, and a layout is never scaled to another form"
                   : "the file is a desktop layout, and a layout is never scaled to another form";
    }
    if (v.problem == "damaged") {
        return "the file isn't a layout this version reads: not JSON, past a bound, or a field of the wrong kind";
    }
    return "";
}

inline size_t layout_line_bytes(const Verdict& v)
{
    size_t n = 1024;
    for (const File& f : v.files) {
        n += Line::string_bytes(f.name.data(), f.name.size()) + 128;
    }
    return n;
}

// The `layout` line: what the file is and what loading it would drop. The
// counts, the first cell's size and `dropped` are there when it reads;
// `needs_files` is the signal files its layout names (the check has no
// vehicle to say which are missing); `words` when it is refused
inline void layout_line(Line& l, const Verdict& v)
{
    l.raw("{\"type\":\"layout\"");
    l.key("ok");
    l.raw(v.read ? "true" : "false");
    l.key("problem");
    l.string(v.problem.c_str(), SIZE_MAX);
    l.key("as");
    l.string(v.as.c_str(), SIZE_MAX);
    l.key("form");
    if (v.form.empty()) {
        l.raw("null");
    } else {
        l.string(v.form.c_str(), SIZE_MAX);
    }
    if (v.read) {
        l.key("pages");
        l.u64(static_cast<uint64_t>(v.pages));
        l.key("cells");
        l.u64(static_cast<uint64_t>(v.cells));
        l.key("dropped");
        l.raw("{\"pages\":");
        l.u64(static_cast<uint64_t>(v.dropped_pages));
        l.raw(",\"cells\":");
        l.u64(static_cast<uint64_t>(v.dropped_cells));
        l.ch('}');
        l.key("first_cell");
        if (v.has_first) {
            l.raw("{\"w\":");
            l.u64(static_cast<uint64_t>(v.first_w));
            l.raw(",\"h\":");
            l.u64(static_cast<uint64_t>(v.first_h));
            l.ch('}');
        } else {
            l.raw("null");
        }
    }
    l.key("needs_files");
    l.ch('[');
    for (size_t i = 0; i < v.files.size(); ++i) {
        if (i > 0) {
            l.ch(',');
        }
        l.raw("{\"name\":");
        l.string_exact(v.files[i].name.data(), v.files[i].name.size());
        l.raw(",\"sha256\":");
        l.string(v.files[i].sha256.c_str(), SIZE_MAX);
        l.ch('}');
    }
    l.ch(']');
    l.key("notes");
    l.ch('[');
    for (size_t i = 0; i < v.notes.size(); ++i) {
        if (i > 0) {
            l.ch(',');
        }
        l.string(v.notes[i].c_str(), SIZE_MAX);
    }
    l.ch(']');
    l.key("words");
    if (v.read) {
        l.raw("null");
    } else {
        l.string(layout_words(v), SIZE_MAX);
    }
    l.ch('}');
}

} // namespace layout
} // namespace cli

#endif
