// File/layer: app/variantshape.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest.VariantShape* (test_bridge, tests/test_bridge_variantshape.cpp); QML: QmlTest.settings_rows

#include "variantshape.h"

#include <cstring> // control

namespace variant_shape {

namespace {

bool sameText(const char* a, const char* b, size_t room)
{
    return std::strncmp(a, b, room) == 0;
}

bool sameEnum(const ecuio_signal_set_t& aSet, const ecuio_signal_def_t& a,
              const ecuio_signal_set_t& bSet, const ecuio_signal_def_t& b)
{
    if (a.table_count != b.table_count) {
        return false;
    }
    for (uint16_t i = 0; i < a.table_count; ++i) {
        const ecuio_enum_entry_t& x = aSet.enums[a.table_first + i];
        const ecuio_enum_entry_t& y = bSet.enums[b.table_first + i];
        if (x.raw != y.raw || !sameText(x.label, y.label, sizeof x.label)) {
            return false;
        }
    }
    return true;
}

// Field by field: two constants are the same by ==, as +0 and -0 are, not by their bytes
bool sameProgram(const ecuio_computed_t& a, const ecuio_computed_t& b)
{
    if (a.input_count != b.input_count || a.step_count != b.step_count ||
        a.constant_count != b.constant_count) {
        return false;
    }
    for (size_t i = 0; i < 4; ++i) {
        if (a.input_ids[i] != b.input_ids[i] || a.max_age_us[i] != b.max_age_us[i]) {
            return false;
        }
    }
    if (std::memcmp(a.steps, b.steps, sizeof a.steps) != 0) {
        return false;
    }
    for (size_t i = 0; i < 16; ++i) {
        if (a.constants[i] != b.constants[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

bool sameDef(const ecuio_signal_set_t& aSet, const ecuio_signal_def_t& a,
             const ecuio_signal_set_t& bSet, const ecuio_signal_def_t& b)
{
    if (a.message_id != b.message_id || a.mux_offset != b.mux_offset ||
        a.mux_length != b.mux_length || a.mux_key != b.mux_key ||
        a.kind != b.kind || a.byte_order != b.byte_order ||
        a.start_byte != b.start_byte || a.bit_offset != b.bit_offset ||
        a.bit_length != b.bit_length || a.byte_length != b.byte_length ||
        a.scale != b.scale || a.offset != b.offset ||
        !sameText(a.name, b.name, sizeof a.name) ||
        !sameText(a.unit, b.unit, sizeof a.unit) ||
        a.decimal_places != b.decimal_places || a.plot_rank != b.plot_rank ||
        a.group != b.group || a.flags != b.flags) {
        return false;
    }
    if (a.kind == ECUIO_VALUE_ENUM && !sameEnum(aSet, a, bSet, b)) {
        return false;
    }
    if ((a.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
        return sameProgram(aSet.computed[a.table_first], bSet.computed[b.table_first]);
    }
    return true;
}

Shape of(const ecuio_signal_file_t* file, const ecuio_signal_set_t* base)
{
    Shape shape;
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
    if (set == nullptr) {
        return shape;
    }
    shape.count = static_cast<int>(set->count);
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        const char* key = ecuio_signal_file_key(file, &d);
        if (key == nullptr || key[0] == '\0') {
            continue; // the base's
        }
        const ecuio_signal_def_t* was = nullptr;
        for (uint32_t j = 0; base != nullptr && j < base->count; ++j) {
            if (sameText(base->defs[j].name, d.name, sizeof d.name)) {
                was = &base->defs[j];
                break;
            }
        }
        if (was == nullptr) {
            ++shape.added;
        } else if (ecuio_signal_file_gauge(file, &d) != nullptr ||
                   !sameDef(*set, d, *base, *was)) {
            shape.replaced << QString::fromUtf8(d.name);
        }
    }
    return shape;
}

} // namespace variant_shape
