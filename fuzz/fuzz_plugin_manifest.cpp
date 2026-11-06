// File/layer: fuzz/fuzz_plugin_manifest.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: the plugin loader (ADR-0052, ecuio_plugin_load) and its
//          value fixtures (ecuio_plugin_values)
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
        check(!words.empty() && words.find('
') == std::string::npos);
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
        const size_t cut = rest.find('');
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
    check(info.tiers == ECUIO_PLUGIN_TIER_DATA);
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
    }

    check_values(a, b, info.variant_count, fixture);

    check(ecuio_plugin_free(a) == ECUIO_OK);
    check(ecuio_plugin_free(b) == ECUIO_OK);
    return 0;
}
