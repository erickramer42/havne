// File/layer: fuzz/fuzz_plugin_manifest.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: the plugin loader (ADR-0052, ecuio_plugin_load)
//
// The first byte picks the form: even, a packed package (the rest is the
// file); odd, a folder, the rest cut at 0x1E bytes into manifest.json
// and the files the loader asks for in turn. fuzz/README.md lists what
// each input must hold to.

#include <ecuio/core.h>
#include <ecuio/plugin.h>
#include <ecuio/profile.h>
#include <ecuio/signals.h>

#include "signal_file_build.h" /* core/src: a variant's merged text */

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

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0) {
        return 0;
    }
    const bool folder = (data[0] & 1u) != 0;
    const std::string rest(reinterpret_cast<const char*>(data) + 1, size - 1);
    std::string manifest = rest;
    pieces files;
    if (folder) {
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
        /* The variant as one signal file: its merged text read back to
         * its own set (an empty one would leave a recording no copy),
         * the same for the same input */
        const ecuio_signal_file_t* file = ecuio_plugin_variant_file(a, v);
        check(file != nullptr && ecuio_signal_file_set(file) == set);
        check(!ecuio_sf::bytes_of(*file).empty());
        check(ecuio_sf::bytes_of(*file) ==
              ecuio_sf::bytes_of(*ecuio_plugin_variant_file(b, v)));
    }

    check(ecuio_plugin_free(a) == ECUIO_OK);
    check(ecuio_plugin_free(b) == ECUIO_OK);
    return 0;
}
