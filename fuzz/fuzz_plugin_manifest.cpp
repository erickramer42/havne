// File/layer: fuzz/fuzz_plugin_manifest.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: the plugin loader (ADR-0052, ADR-0066's format 2,
//          ecuio_plugin_load) and its value fixtures (ecuio_plugin_values)
//
// The first byte picks the form: even, a packed package (the rest up to
// its first 0x1E byte is the file); odd, a folder, the rest cut at 0x1E
// bytes into manifest.json and the files the loader asks for in turn.
// What follows (the packed file's 0x1E, or the first piece the folder's
// loader didn't ask for) is a fixture file read against the plugin
// loaded. fuzz/README.md lists what each input must hold to.

#include <ecuio/core.h>
#include <ecuio/plugin.h>
#include <ecuio/profile.h>
#include <ecuio/signals.h>

#include "signal_file_build.h" /* core/src: a variant's merged text */
#include "tier2_ceiling.h"     /* core/src: what a conversation may declare */
#include "wasm_check.h"        /* core/src: a module's most bytes */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

/* The folder's files: each piece after the manifest is the next file
 * the loader asks for, whatever its path */
struct pieces {
    std::vector<std::string> files;
    size_t next = 0;

    static int read(void* user, const char* path, const char** bytes,
                    size_t* length)
    {
        auto* self = static_cast<pieces*>(user);
        check(path != nullptr && path[0] != '\0' && path[0] != '/');
        check(strchr(path, '\\') == nullptr);
        /* No part is "." or "..": the loader checks before it asks */
        const std::string p(path);
        size_t start = 0;
        for (size_t i = 0; i <= p.size(); ++i) {
            if (i == p.size() || p[i] == '/') {
                const std::string part = p.substr(start, i - start);
                check(!part.empty() && part != "." && part != "..");
                start = i + 1;
            }
        }
        if (self->next >= self->files.size()) {
            return 1;
        }
        const std::string& f = self->files[self->next++];
        *bytes = f.data();
        *length = f.size();
        return 0;
    }
};

ecuio_status_t load(const std::string& manifest, pieces* folder,
                    ecuio_plugin_t** out, std::string* words)
{
    if (folder != nullptr) {
        folder->next = 0;
    }
    const ecuio_status_t s = ecuio_plugin_load(
        manifest.data(), manifest.size(),
        folder != nullptr ? &pieces::read : nullptr, folder, out);
    *words = ecuio_last_error_detail();
    return s;
}

/* A fixture file read against both plugins alike: refused in one line
 * with the count untouched, or entries that each name a variant and a
 * numeric field of that variant's set, the first cap copied */
void check_values(const ecuio_plugin_t* a, const ecuio_plugin_t* b,
                  uint32_t variants, const std::string& bytes)
{
    uint32_t count = 0xFFFFFFFFu;
    const ecuio_status_t s =
        ecuio_plugin_values(a, bytes.data(), bytes.size(), nullptr, 0, &count);
    const std::string words = ecuio_last_error_detail();
    uint32_t again = 0xFFFFFFFFu;
    check(ecuio_plugin_values(b, bytes.data(), bytes.size(), nullptr, 0,
                              &again) == s);
    check(words == ecuio_last_error_detail());
    if (s != ECUIO_OK) {
        check(s == ECUIO_ERR_INVALID_ARGUMENT || s == ECUIO_ERR_OUT_OF_MEMORY);
        check(count == 0xFFFFFFFFu && again == 0xFFFFFFFFu);
        check(!words.empty() && words.find('\n') == std::string::npos);
        return;
    }
    check(count == again && count >= 1 && count <= ECUIO_PLUGIN_MAX_VALUES);
    std::vector<ecuio_plugin_value_t> v(count + 1);
    const uint32_t cap = count / 2;
    v[cap].line = 0xABCDu; /* past the cap: left alone */
    uint32_t n = 0;
    check(ecuio_plugin_values(a, bytes.data(), bytes.size(), v.data(), cap,
                              &n) == ECUIO_OK);
    check(n == count && v[cap].line == 0xABCDu);
    check(ecuio_plugin_values(a, bytes.data(), bytes.size(), v.data(), count,
                              &n) == ECUIO_OK);
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_plugin_value_t& e = v[i];
        check(e.variant < variants && e.line >= 1);
        const ecuio_signal_set_t* set = ecuio_plugin_variant_set(a, e.variant);
        check(e.def >= set->defs && e.def < set->defs + set->count);
        check(memchr(e.signal, '\0', sizeof e.signal) != nullptr);
        check(ecuio_plugin_variant_find(a, e.variant, e.signal) == e.def);
        check((e.def->flags & ECUIO_DEF_FLAG_COMPUTED) == 0);
        check(e.def->kind != ECUIO_VALUE_TEXT &&
              e.def->kind != ECUIO_VALUE_HEX);
        for (uint32_t r : e.reserved) {
            check(r == 0);
        }
        for (uint32_t j = 0; j < i; ++j) {
            check(v[j].variant != e.variant || v[j].def != e.def);
        }
    }
}

/* Each declared service once, inside tier 2's ceiling, by sub-function
 * exactly where the ceiling splits; sub_27 gets security access's */
void check_services(const ecuio_plugin_t* a, uint32_t count, bool* sub_27)
{
    ecuio_plugin_service_t sv{};
    sv.struct_size = sizeof sv;
    bool sid_seen[256] = {};
    for (uint32_t i = 0; i < count; ++i) {
        check(ecuio_plugin_service(a, i, &sv) == ECUIO_OK);
        check(sv.struct_size == sizeof sv && sv.flags == 0);
        check(!sid_seen[sv.sid]);
        sid_seen[sv.sid] = true;
        check(sv.sub_count <= ECUIO_PLUGIN_MAX_SUBS);
        check((sv.sub_count > 0) == ecuio_tier2_splits(sv.sid));
        check(sv.sub_count > 0 || ecuio_tier2_allowed(sv.sid, 0));
        for (uint32_t k = 0; k < sizeof sv.subs; ++k) {
            if (k >= sv.sub_count) {
                check(sv.subs[k] == 0);
                continue;
            }
            check(ecuio_tier2_allowed(sv.sid, sv.subs[k]));
            for (uint32_t j = 0; j < k; ++j) {
                check(sv.subs[j] != sv.subs[k]);
            }
            if (sv.sid == 0x27) {
                sub_27[sv.subs[k]] = true;
            }
        }
        check(sv.reserved[0] == 0 && sv.reserved[1] == 0);
    }
    check(ecuio_plugin_service(a, count, &sv) == ECUIO_ERR_INVALID_ARGUMENT);
    if (count > 0) { /* else the index refuses first */
        sv.struct_size = sizeof sv - 1;
        const ecuio_plugin_service_t untouched = sv;
        check(ecuio_plugin_service(a, 0, &sv) == ECUIO_ERR_INVALID_ARGUMENT);
        check(memcmp(&sv, &untouched, sizeof sv) == 0);
    }
}

/* Each security level on one of the group's ECUs, once, inside its
 * bounds, its seed and key requests declared */
void check_levels(const ecuio_plugin_t* a, uint32_t count,
                  const std::vector<uint32_t>& ecus, const bool* sub_27)
{
    ecuio_plugin_security_level_t lv{};
    lv.struct_size = sizeof lv;
    std::vector<ecuio_plugin_security_level_t> levels;
    for (uint32_t i = 0; i < count; ++i) {
        check(ecuio_plugin_security_level(a, i, &lv) == ECUIO_OK);
        check(lv.struct_size == sizeof lv && lv.flags == 0);
        bool listed = false;
        for (const uint32_t known : ecus) {
            listed = listed || known == lv.rx_id;
        }
        check(listed);
        check((lv.level & 1) == 1 && lv.level < 0x7F);
        check(sub_27[lv.level] && sub_27[lv.level + 1]);
        check(lv.seed_bytes >= 1);
        check(lv.key_bytes >= 1 && lv.key_bytes <= ECUIO_PLUGIN_KEY_MAX);
        check(lv.reserved0 == 0);
        check(lv.delay_ms >= 1000 && lv.delay_ms <= 3600000);
        check(lv.key_window_ms >= 50 && lv.key_window_ms <= 4000);
        for (const uint32_t r : lv.reserved) {
            check(r == 0);
        }
        for (const ecuio_plugin_security_level_t& known : levels) {
            check(known.rx_id != lv.rx_id || known.level != lv.level);
        }
        levels.push_back(lv);
    }
    check(ecuio_plugin_security_level(a, count, &lv) ==
          ECUIO_ERR_INVALID_ARGUMENT);
    if (count > 0) { /* else the index refuses first */
        lv.struct_size = sizeof lv - 1;
        const ecuio_plugin_security_level_t untouched = lv;
        check(ecuio_plugin_security_level(a, 0, &lv) ==
              ECUIO_ERR_INVALID_ARGUMENT);
        check(memcmp(&lv, &untouched, sizeof lv) == 0);
    }
}

/* Each ECU the module talks to, once: one of the plugin's, so a channel
 * of every variant's set, with that channel's request ID. Their
 * response IDs, in order */
std::vector<uint32_t> check_ecus(const ecuio_plugin_t* a, uint32_t count,
                                 uint32_t variants)
{
    std::vector<ecuio_plugin_conversation_ecu_t> seen;
    std::vector<uint32_t> ecus;
    for (uint32_t i = 0; i < count; ++i) {
        ecuio_plugin_conversation_ecu_t e{};
        e.struct_size = sizeof e;
        check(ecuio_plugin_conversation_ecu(a, i, &e) == ECUIO_OK);
        check(e.struct_size == sizeof e && e.rx_id != 0 && e.flags == 0);
        const size_t id = strnlen(e.id, sizeof e.id);
        check(id >= 1 && id < sizeof e.id);
        for (const uint32_t r : e.reserved) {
            check(r == 0);
        }
        for (const ecuio_plugin_conversation_ecu_t& known : seen) {
            check(known.rx_id != e.rx_id && strcmp(known.id, e.id) != 0);
        }
        for (uint32_t v = 0; v < variants; ++v) {
            const ecuio_signal_set_t* set =
                ecuio_signal_file_set(ecuio_plugin_variant_file(a, v));
            bool found = false;
            for (uint32_t k = 0; set != nullptr && k < set->channel_count;
                 ++k) {
                found = found || (set->channels[k].rx_id == e.rx_id &&
                                  set->channels[k].tx_id == e.tx_id);
            }
            check(found);
        }
        seen.push_back(e);
        ecus.push_back(e.rx_id);
    }
    ecuio_plugin_conversation_ecu_t past{};
    past.struct_size = sizeof past;
    check(ecuio_plugin_conversation_ecu(a, count, &past) ==
          ECUIO_ERR_INVALID_ARGUMENT);
    return ecus;
}

/* The conversation tier's part, read from both plugins alike: none
 * without the tier; with it, everything inside plugin.h's bounds and
 * tier 2's ceiling, each ECU, service and level once, each level's pair
 * declared, and every getter refusing past its count */
void check_conversation(const ecuio_plugin_t* a, const ecuio_plugin_t* b,
                        uint32_t tiers, uint32_t variants)
{
    ecuio_plugin_conversation_t c{};
    c.struct_size = sizeof c;
    ecuio_plugin_conversation_t d = c;
    const ecuio_status_t s = ecuio_plugin_conversation(a, &c);
    check(ecuio_plugin_conversation(b, &d) == s);
    ecuio_plugin_conversation_ecu_t ecu{};
    ecu.struct_size = sizeof ecu;
    ecuio_plugin_service_t sv{};
    sv.struct_size = sizeof sv;
    ecuio_plugin_security_level_t lv{};
    lv.struct_size = sizeof lv;
    if ((tiers & ECUIO_PLUGIN_TIER_CONVERSATION) == 0) {
        check(s == ECUIO_ERR_INVALID_ARGUMENT);
        check(ecuio_plugin_conversation_ecu(a, 0, &ecu) ==
              ECUIO_ERR_INVALID_ARGUMENT);
        check(ecuio_plugin_service(a, 0, &sv) == ECUIO_ERR_INVALID_ARGUMENT);
        check(ecuio_plugin_security_level(a, 0, &lv) ==
              ECUIO_ERR_INVALID_ARGUMENT);
        return;
    }
    check(s == ECUIO_OK && memcmp(&c, &d, sizeof c) == 0);
    check(c.struct_size == sizeof c && c.reserved[0] == 0 &&
          c.reserved[1] == 0);
    check(c.code_bytes >= 8 && c.code_bytes <= ecuio_wasm::MODULE_MAX_BYTES);
    check(c.budget >= 1 && c.budget <= ECUIO_PLUGIN_BUDGET_MAX);
    check(c.ecu_count >= 1 && c.ecu_count <= ECUIO_MAX_CHANNELS);
    check(c.service_count >= 1 && c.service_count <= ECUIO_PLUGIN_MAX_SERVICES);
    check(c.level_count <= ECUIO_PLUGIN_MAX_LEVELS);

    const std::vector<uint32_t> ecus = check_ecus(a, c.ecu_count, variants);
    bool sub_27[256] = {}; /* security access's sub-functions declared */
    check_services(a, c.service_count, sub_27);
    check_levels(a, c.level_count, ecus, sub_27);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0) {
        return 0;
    }
    const bool folder = (data[0] & 1u) != 0;
    const std::string rest(reinterpret_cast<const char*>(data) + 1, size - 1);
    std::string manifest = rest;
    std::string fixture;
    pieces files;
    if (!folder) {
        const size_t cut = rest.find('\x1E');
        if (cut != std::string::npos) {
            manifest = rest.substr(0, cut);
            fixture = rest.substr(cut + 1);
        }
    } else {
        size_t start = 0;
        bool first = true;
        for (size_t i = 0; i <= rest.size(); ++i) {
            if (i == rest.size() || rest[i] == '\x1E') {
                std::string piece = rest.substr(start, i - start);
                if (first) {
                    manifest = piece;
                    first = false;
                } else {
                    files.files.push_back(piece);
                }
                start = i + 1;
            }
        }
    }

    ecuio_plugin_t* a = nullptr;
    ecuio_plugin_t* b = nullptr;
    std::string wa;
    std::string wb;
    const ecuio_status_t sa =
        load(manifest, folder ? &files : nullptr, &a, &wa);
    const ecuio_status_t sb =
        load(manifest, folder ? &files : nullptr, &b, &wb);
    check(sa == sb);
    check(wa == wb);
    if (sa != ECUIO_OK) {
        check(a == nullptr && b == nullptr);
        check(sa == ECUIO_ERR_INVALID_ARGUMENT ||
              sa == ECUIO_ERR_OUT_OF_MEMORY);
        check(!wa.empty());
        check(wa.find('\n') == std::string::npos);
        check(wa.find('\r') == std::string::npos);
        return 0;
    }
    check(a != nullptr && b != nullptr);
    if (folder && files.next < files.files.size()) {
        fixture = files.files[files.next];
    }

    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    check(ecuio_plugin_info(a, &info) == ECUIO_OK);
    check(info.tiers == ECUIO_PLUGIN_TIER_DATA ||
          info.tiers ==
              (ECUIO_PLUGIN_TIER_DATA | ECUIO_PLUGIN_TIER_CONVERSATION));
    check_conversation(a, b, info.tiers, info.variant_count);
    check(info.variant_count >= 1 &&
          info.variant_count <= ECUIO_PLUGIN_MAX_VARIANTS);
    check(!folder ? info.fixture_count == 0
                  : info.fixture_count <= ECUIO_PLUGIN_MAX_FIXTURES);

    for (uint32_t v = 0; v < info.variant_count; ++v) {
        const ecuio_signal_set_t* set = ecuio_plugin_variant_set(a, v);
        check(set != nullptr);
        check(ecuio_plugin_variant_set(b, v)->count == set->count);
        ecuio_validation_t why{};
        check(ecuio_validate_signal_set(set, &why) == ECUIO_VALIDATE_OK);
        char id[ECUIO_PLUGIN_VARIANT_ID_MAX + 1];
        check(ecuio_plugin_variant(a, v, id, sizeof id) == ECUIO_OK);
        char name[ECUIO_PROFILE_NAME_MAX + 1];
        check(ecuio_plugin_register(a, v, name, sizeof name) == ECUIO_OK);
        const std::string want = std::string("plugin.") + info.id + "." + id;
        check(want == name);
        const ecuio_signal_set_t* found = nullptr;
        check(ecuio_profile_lookup(name, &found) == ECUIO_OK && found == set);
        /* The variant as one signal file: its merged text read back to
         * its own set (an empty one would leave a recording no copy),
         * the same for the same input */
        const ecuio_signal_file_t* file = ecuio_plugin_variant_file(a, v);
        check(file != nullptr && ecuio_signal_file_set(file) == set);
        check(!ecuio_sf::bytes_of(*file).empty());
        check(ecuio_sf::bytes_of(*file) ==
              ecuio_sf::bytes_of(*ecuio_plugin_variant_file(b, v)));
    }

    check_values(a, b, info.variant_count, fixture);

    check(ecuio_plugin_free(a) == ECUIO_OK);
    check(ecuio_plugin_free(b) == ECUIO_OK);
    return 0;
}
