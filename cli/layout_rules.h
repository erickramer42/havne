// File/layer: cli/layout_rules.h | havne-cli, DashboardBook's page, cell and document rules for a layout file (ADR-0057's appendix on the layout file)
// Testing: UNIT: CliLayoutTest (test_cli), the shared corpus tests/data/layouts

#ifndef HAVNE_CLI_LAYOUT_RULES_H
#define HAVNE_CLI_LAYOUT_RULES_H

#include "layout_json.h"
#include "layout_text.h"

#include <cmath>
#include <string>

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

} // namespace layout
} // namespace cli

#endif
