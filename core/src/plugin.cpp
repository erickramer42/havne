// File/layer: core/src/plugin.cpp | C++17, Qt-free, every platform
// Testing: UNIT: PluginTest, PluginWrapTest, PluginConversationTest
//          (test_signals); HARNESS: RecorderTest (test_core); FUZZ:
//          fuzz_plugin_manifest
//
// The plugin loader's C API (ADR-0052's data tier): a plugin loaded
// (plugin_loader.h), its parts read back, its value fixtures, its variants
// registered as profiles.

#include "ecuio/internal.h"
#include "ecuio/plugin.h"
#include "ecuio/profile.h"
#include "error_detail.h"
#include "json.h"
#include "least_core.h"
#include "plugin_loader.h"
#include "plugin_rules.h"
#include "profile_hold.h"
#include "signal_file_build.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace ecuio_plugin_loader;

namespace {

bool copy_out(const std::string& s, char* out, size_t cap)
{
    if (out == nullptr || cap < s.size() + 1) {
        return false;
    }
    std::memcpy(out, s.c_str(), s.size() + 1);
    return true;
}

void copy_field(char* dst, size_t cap, const std::string& s)
{
    size_t n = 0;
    for (; n + 1 < cap && n < s.size(); ++n) {
        dst[n] = s[n];
    }
    dst[n] = '\0';
}

/* A value fixture file's entries, or a placed thrown with its words
 * ("line L, column C: why"); std::bad_alloc too */
std::vector<ecuio_plugin_value_t> read_values(const ecuio_plugin& p,
                                              const char* bytes, size_t length)
{
    ecuio_json::Limits limits;
    limits.max_bytes = ECUIO_SIGNAL_FILE_MAX_BYTES;
    ecuio_json::Error error;
    Value root;
    if (!ecuio_json::parse(bytes != nullptr ? bytes : "", length, &root, &error,
                           limits)) {
        throw placed_json("", error);
    }
    std::vector<ecuio_plugin_value_t> out;
    try {
        expect(root, Type::object, "a fixture file");
        if (root.members.empty() ||
            root.members.front().key != "havne_fixtures") {
            refuse(root, "a fixture file starts with \"havne_fixtures\"");
        }
        const Value& format = root.members.front().value;
        expect(format, Type::number, "\"havne_fixtures\"");
        if (!format.integer || format.whole != ECUIO_PLUGIN_FIXTURES_FORMAT) {
            refuse(format, "\"havne_fixtures\" is format " +
                               (format.integer ? std::to_string(format.whole)
                                               : std::string("?")) +
                               ", and this Havne reads format 1");
        }
        only_keys(root, {"havne_fixtures", "values"}, "a fixture file");
        const Value* list = root.find("values");
        if (list == nullptr) {
            refuse(root, "a fixture file needs \"values\"");
        }
        expect(*list, Type::array, "\"values\"");
        if (list->items.empty() ||
            list->items.size() > ECUIO_PLUGIN_MAX_VALUES) {
            refuse(*list, "\"values\" lists 1 to 4096 entries");
        }
        std::vector<std::pair<uint32_t, uint16_t>> seen;
        for (const Value& e : list->items) {
            expect(e, Type::object, "a value fixture");
            only_keys(e, {"variant", "signal", "value"}, "a value fixture");
            const Value* variant = e.find("variant");
            const Value* signal = e.find("signal");
            const Value* value = e.find("value");
            if (variant == nullptr || signal == nullptr || value == nullptr) {
                refuse(e, "a value fixture needs \"variant\", \"signal\" "
                          "and \"value\"");
            }
            const std::string id = text_of(
                *variant, 1, ECUIO_PLUGIN_VARIANT_ID_MAX, "\"variant\"");
            uint32_t index = 0;
            while (index < p.variants.size() && p.variants[index].id != id) {
                ++index;
            }
            if (index == p.variants.size()) {
                refuse(*variant, "the plugin has no variant " + q(id));
            }
            const std::string key = text_of(*signal, 1, 31, "\"signal\"");
            const ecuio_signal_def_t* def = ecuio_signal_file_find(
                p.variants[index].file.get(), key.c_str());
            if (def == nullptr) {
                refuse(*signal,
                       "variant " + q(id) + " has no signal " + q(key));
            }
            if ((def->flags & ECUIO_DEF_FLAG_COMPUTED) != 0 ||
                def->kind == ECUIO_VALUE_TEXT || def->kind == ECUIO_VALUE_HEX) {
                refuse(*signal, q(key) +
                                    " isn't a number or an enum: a value "
                                    "fixture checks a field the simulated ECU "
                                    "answers");
            }
            for (const auto& s : seen) {
                if (s.first == index && s.second == def->id) {
                    refuse(*signal,
                           q(key) + " of variant " + q(id) + " is given twice");
                }
            }
            seen.emplace_back(index, def->id);
            expect(*value, Type::number, "\"value\"");
            ecuio_plugin_value_t v{};
            v.value = value->number;
            v.variant = index;
            v.line = e.line;
            copy_field(v.signal, sizeof v.signal, key);
            v.def = def;
            out.push_back(v);
        }
    } catch (const refused& r) {
        throw placed{"line " + std::to_string(r.line) + ", column " +
                     std::to_string(r.column) + ": " + r.words};
    }
    return out;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_plugin_load(const char* manifest, size_t length,
                                 ecuio_plugin_read_t read, void* user,
                                 ecuio_plugin_t** out)
{
    ecuio_detail_clear();
    if (out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    if (manifest == nullptr && length > 0) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    try {
        loader l(manifest, length, read, user);
        std::unique_ptr<ecuio_plugin> p = l.load();
        *out = p.release();
        return ECUIO_OK;
    } catch (const placed& r) {
        ecuio_detail_set("%s", r.words.c_str());
        return r.out_of_memory ? ECUIO_ERR_OUT_OF_MEMORY
                               : ECUIO_ERR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    } catch (...) {
        ecuio_detail_set("the plugin couldn't be read");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
}

ecuio_status_t ecuio_plugin_info(const ecuio_plugin_t* plugin,
                                 ecuio_plugin_info_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out->struct_size < sizeof(ecuio_plugin_info_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof(ecuio_plugin_info_t);
    info.tiers = plugin->tiers;
    copy_field(info.id, sizeof info.id, plugin->id);
    copy_field(info.version, sizeof info.version, plugin->version);
    copy_field(info.name, sizeof info.name, plugin->name);
    info.variant_count = static_cast<uint32_t>(plugin->variants.size());
    info.fixture_count = static_cast<uint32_t>(plugin->fixtures.size());
    *out = info;
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_requires_core(const ecuio_plugin_t* plugin,
                                          uint16_t* major, uint16_t* minor,
                                          uint16_t* patch)
{
    if (plugin == nullptr || major == nullptr || minor == nullptr ||
        patch == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *major = plugin->requires_core[0];
    *minor = plugin->requires_core[1];
    *patch = plugin->requires_core[2];
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_lineage(const ecuio_plugin_t* plugin, char* lineage,
                                    size_t cap)
{
    if (plugin == nullptr || lineage == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!copy_out(plugin->lineage, lineage, cap)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_variant(const ecuio_plugin_t* plugin,
                                    uint32_t index, char* id, size_t cap)
{
    if (plugin == nullptr || id == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (index >= plugin->variants.size() ||
        !copy_out(plugin->variants[index].id, id, cap)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_fixture(const ecuio_plugin_t* plugin,
                                    uint32_t index, char* path, size_t cap)
{
    if (plugin == nullptr || path == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (index >= plugin->fixtures.size() ||
        !copy_out(plugin->fixtures[index], path, cap)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return ECUIO_OK;
}

const ecuio_signal_set_t* ecuio_plugin_variant_set(const ecuio_plugin_t* plugin,
                                                   uint32_t index)
{
    if (plugin == nullptr || index >= plugin->variants.size()) {
        return nullptr;
    }
    return ecuio_signal_file_set(plugin->variants[index].file.get());
}

const ecuio_signal_file_t*
ecuio_plugin_variant_file(const ecuio_plugin_t* plugin, uint32_t index)
{
    if (plugin == nullptr || index >= plugin->variants.size()) {
        return nullptr;
    }
    return plugin->variants[index].file.get();
}

const ecuio_signal_def_t*
ecuio_plugin_variant_find(const ecuio_plugin_t* plugin, uint32_t index,
                          const char* key)
{
    if (plugin == nullptr || index >= plugin->variants.size()) {
        return nullptr;
    }
    return ecuio_signal_file_find(plugin->variants[index].file.get(), key);
}

ecuio_status_t ecuio_plugin_conversation(const ecuio_plugin_t* plugin,
                                         ecuio_plugin_conversation_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (plugin->conversation == nullptr ||
        out->struct_size < sizeof(ecuio_plugin_conversation_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const conversation& c = *plugin->conversation;
    ecuio_plugin_conversation_t info{};
    info.struct_size = sizeof(ecuio_plugin_conversation_t);
    info.code_bytes = static_cast<uint32_t>(c.code.size());
    std::memcpy(info.code_sha256, c.sha256, sizeof info.code_sha256);
    info.budget = c.budget;
    info.ecu_count = static_cast<uint32_t>(c.ecus.size());
    info.service_count = static_cast<uint32_t>(c.services.size());
    info.level_count = static_cast<uint32_t>(c.levels.size());
    *out = info;
    return ECUIO_OK;
}

ecuio_status_t
ecuio_plugin_conversation_ecu(const ecuio_plugin_t* plugin, uint32_t index,
                              ecuio_plugin_conversation_ecu_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (plugin->conversation == nullptr ||
        index >= plugin->conversation->ecus.size() ||
        out->struct_size < sizeof(ecuio_plugin_conversation_ecu_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const conversation_ecu& from = plugin->conversation->ecus[index];
    ecuio_plugin_conversation_ecu_t ecu{};
    ecu.struct_size = sizeof(ecuio_plugin_conversation_ecu_t);
    ecu.rx_id = from.rx;
    ecu.tx_id = from.tx;
    copy_field(ecu.id, sizeof ecu.id, from.id);
    *out = ecu;
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_service(const ecuio_plugin_t* plugin,
                                    uint32_t index, ecuio_plugin_service_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (plugin->conversation == nullptr ||
        index >= plugin->conversation->services.size() ||
        out->struct_size < sizeof(ecuio_plugin_service_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_plugin_service_t service = plugin->conversation->services[index];
    service.struct_size = sizeof(ecuio_plugin_service_t);
    *out = service;
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_security_level(const ecuio_plugin_t* plugin,
                                           uint32_t index,
                                           ecuio_plugin_security_level_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (plugin->conversation == nullptr ||
        index >= plugin->conversation->levels.size() ||
        out->struct_size < sizeof(ecuio_plugin_security_level_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_plugin_security_level_t level = plugin->conversation->levels[index];
    level.struct_size = sizeof(ecuio_plugin_security_level_t);
    *out = level;
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_values(const ecuio_plugin_t* plugin,
                                   const char* bytes, size_t length,
                                   ecuio_plugin_value_t* values, uint32_t cap,
                                   uint32_t* count)
{
    ecuio_detail_clear();
    if (plugin == nullptr || count == nullptr ||
        (bytes == nullptr && length > 0) || (values == nullptr && cap > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    try {
        const std::vector<ecuio_plugin_value_t> read =
            read_values(*plugin, bytes, length);
        for (size_t i = 0; i < read.size() && i < cap; ++i) {
            values[i] = read[i];
        }
        *count = static_cast<uint32_t>(read.size());
        return ECUIO_OK;
    } catch (const placed& r) {
        ecuio_detail_set("%s", r.words.c_str());
        return r.out_of_memory ? ECUIO_ERR_OUT_OF_MEMORY
                               : ECUIO_ERR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc&) {
        ecuio_detail_set("out of memory");
        return ECUIO_ERR_OUT_OF_MEMORY;
    } catch (...) {
        ecuio_detail_set("the fixture file couldn't be read");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
}

ecuio_status_t ecuio_plugin_match(const ecuio_plugin_t* plugin,
                                  const ecuio_vin_info_t* vin,
                                  uint32_t* indices, uint32_t cap,
                                  uint32_t* count)
{
    if (plugin == nullptr || vin == nullptr || count == nullptr ||
        (indices == nullptr && cap > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    char code[ECUIO_VIN_WMI_LEN + 1] = {};
    std::memcpy(code, vin->wmi, ECUIO_VIN_WMI_LEN);
    const std::string wmi(code);
    uint32_t n = 0;
    for (size_t v = 0; v < plugin->variants.size(); ++v) {
        bool proposed = false;
        for (const match_rule& rule : plugin->match) {
            bool maker = false;
            for (const std::string& w : rule.wmi) {
                maker = maker || w == wmi;
            }
            proposed =
                proposed || (maker && rule.years.covers(vin->model_year));
        }
        if (proposed && plugin->variants[v].years.covers(vin->model_year)) {
            if (n < cap) {
                indices[n] = static_cast<uint32_t>(v);
            }
            ++n;
        }
    }
    *count = n;
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_register(ecuio_plugin_t* plugin, uint32_t index,
                                     char* name, size_t cap)
{
    if (plugin == nullptr || (name == nullptr && cap > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (index >= plugin->variants.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    variant& v = plugin->variants[index];
    if (name != nullptr && cap < v.profile.size() + 1) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_status_t rc = ecuio_profile_register(
        v.profile.c_str(), ecuio_signal_file_set(v.file.get()), nullptr);
    if (rc != ECUIO_OK) {
        return rc;
    }
    v.registered = true;
    if (name != nullptr) {
        copy_out(v.profile, name, cap);
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_free(ecuio_plugin_t* plugin)
{
    if (plugin == nullptr) {
        return ECUIO_OK;
    }
    try {
        /* Every variant's set, registered by ecuio_plugin_register or
         * under a name of the caller's */
        std::vector<const ecuio_signal_set_t*> sets;
        sets.reserve(plugin->variants.size());
        for (const variant& v : plugin->variants) {
            sets.push_back(ecuio_signal_file_set(v.file.get()));
        }
        const ecuio_status_t rc =
            ecuio_profile_unregister_unused(sets.data(), sets.size());
        if (rc != ECUIO_OK) {
            if (rc == ECUIO_ERR_IN_USE) {
                ecuio_detail_set(
                    "a session or a simulated ECU still decodes with one "
                    "of plugin %s's profiles",
                    plugin->id.c_str());
            }
            return rc;
        }
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    delete plugin;
    return ECUIO_OK;
}

} // extern "C"

ecuio_least::version ecuio_least::of_plugin(const ecuio_plugin_t* plugin)
{
    return plugin->least;
}

std::atomic<uint64_t> ecuio_plugin_rules::g_test_core{0};

extern "C" void ecuio_plugin_test_set_core_version(int on, uint32_t major,
                                                   uint32_t minor,
                                                   uint32_t patch)
{
    ecuio_plugin_rules::g_test_core.store(
        on == 0 ? 0
                : (uint64_t{1} << 48) | (uint64_t{major & 0xFFFF} << 32) |
                      (uint64_t{minor & 0xFFFF} << 16) | (patch & 0xFFFF),
        std::memory_order_relaxed);
}
