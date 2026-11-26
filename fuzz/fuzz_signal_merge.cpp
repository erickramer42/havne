// File/layer: fuzz/fuzz_signal_merge.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: signal files merged into one set (ADR-0065 section 9,
//          ecuio_signal_file_merge)
//
// The first byte's low two bits are the base flags (obd2 whole, its
// modules alone; both is a refusal); the rest, cut at
// each 0x1E byte, is up to four signal files. Those that load at a set's
// bounds are the parts, in their order. fuzz/README.md lists what each
// input must hold to.

#include <ecuio/core.h>
#include <ecuio/profile.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>

#include "fuzz_same_tables.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <string>
#include <vector>

namespace {

const size_t kMaxParts = 4;
const std::string kOwnFault = "the merged set doesn't read back";

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

struct merged {
    ecuio_signal_file_t* file = nullptr;
    ecuio_status_t status = ECUIO_OK;
    std::string words;
};

merged merge(const std::vector<const ecuio_signal_file_t*>& parts,
             uint32_t flags)
{
    ecuio_merge_options_t options{};
    options.struct_size = sizeof options;
    options.flags = flags;
    merged m;
    m.file = reinterpret_cast<ecuio_signal_file_t*>(1);
    m.status = ecuio_signal_file_merge(
        parts.data(), static_cast<uint32_t>(parts.size()), &options, &m.file);
    m.words = ecuio_last_error_detail();
    return m;
}

std::string text_of(const ecuio_signal_file_t* f)
{
    size_t length = 0;
    const char* text = ecuio_signal_file_text(f, &length);
    return std::string(text, length);
}

bool extends_obd2(const ecuio_signal_file_t* f)
{
    return strcmp(ecuio_signal_file_extends(f), "obd2") == 0;
}

std::vector<std::string> replaced_of(const ecuio_signal_file_t* f,
                                     const ecuio_signal_def_t* def)
{
    uint32_t count = 0;
    check(ecuio_signal_file_replaced_keys(f, def, nullptr, 0, &count) ==
          ECUIO_OK);
    std::vector<ecuio_signal_key_t> keys(count);
    uint32_t again = 0;
    check(ecuio_signal_file_replaced_keys(f, def, keys.data(), count, &again) ==
              ECUIO_OK &&
          again == count);
    std::vector<std::string> out;
    for (const ecuio_signal_key_t& k : keys) {
        out.emplace_back(k.text);
    }
    return out;
}

/* Each def's part and keys: a base def has neither, a part's def has its
 * part and a key, and every key, own or replaced, finds its def and is
 * one signal's. obd2's modules alone may hold no def. */
void check_keys(const ecuio_signal_file_t* f, uint32_t parts, bool base,
                bool modules)
{
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f);
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    const uint32_t first = base ? obd2->count : 0;
    check(set->count > first || modules);
    if (base) {
        check(memcmp(set->defs, obd2->defs, obd2->count * sizeof *set->defs) ==
              0);
    }
    std::set<std::string> seen;
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t* def = &set->defs[i];
        const uint32_t part = ecuio_signal_file_def_part(f, def);
        const std::string key = ecuio_signal_file_key(f, def);
        if (i < first) {
            check(part == ECUIO_SIGNAL_PART_NONE && key.empty());
            check(replaced_of(f, def).empty());
            continue;
        }
        check(part < parts && !key.empty());
        check(ecuio_signal_file_find(f, key.c_str()) == def);
        check(seen.insert(key).second);
        for (const std::string& k : replaced_of(f, def)) {
            check(ecuio_signal_file_find(f, k.c_str()) == def);
            check(seen.insert(k).second);
        }
    }
    /* The keys a value is kept under are one each too */
    std::vector<ecuio_signal_key_t> kept(set->count);
    uint32_t count = 0;
    check(ecuio_signal_set_keys(set, f, kept.data(), set->count, &count) ==
              ECUIO_OK &&
          count == set->count);
    std::set<std::string> under;
    for (const ecuio_signal_key_t& k : kept) {
        check(under.insert(k.text).second);
    }
}

void check_notes(const ecuio_signal_file_t* f, uint32_t parts)
{
    const uint32_t defs = ecuio_signal_file_set(f)->count;
    for (uint32_t i = 0; i < ecuio_signal_file_note_count(f); ++i) {
        ecuio_signal_note_t n;
        memset(&n, 0xA5, sizeof n);
        n.struct_size = sizeof n;
        check(ecuio_signal_file_note(f, i, &n) == ECUIO_OK);
        check(n.kind >= ECUIO_NOTE_REPLACED_BY_ADDRESS &&
              n.kind <= ECUIO_NOTE_SIGNAL_LEFT_OUT);
        check(n.part < parts);
        check(n.other_part < parts || n.other_part == ECUIO_SIGNAL_PART_NONE);
        check(n.def < defs || n.def == UINT32_MAX);
        /* Beside obd2's def, which is no part's; any other kind names none */
        if (n.kind == ECUIO_NOTE_BESIDE_BASE) {
            check(n.base_def < defs && n.def < defs && n.def != n.base_def);
            check(ecuio_signal_file_def_part(
                      f, &ecuio_signal_file_set(f)->defs[n.base_def]) ==
                  ECUIO_SIGNAL_PART_NONE);
        } else {
            check(n.base_def == UINT32_MAX);
        }
        const void* end = memchr(n.text, '\0', sizeof n.text);
        check(end != nullptr && n.text[0] != '\0');
        for (const char* c = n.text; *c != '\0'; ++c) {
            check(static_cast<unsigned char>(*c) >= 0x20 && *c != 0x7F);
        }
    }
}

/* What an OK merge holds to */
void check_merged(const ecuio_signal_file_t* f,
                  const std::vector<const ecuio_signal_file_t*>& parts,
                  bool base, bool modules)
{
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f);
    const std::string profile = ecuio_signal_file_profile(f);
    const auto count = static_cast<uint32_t>(parts.size());
    check(ecuio_signal_file_format(f) == ECUIO_SIGNAL_FILE_FORMAT);
    check(ecuio_signal_file_part_count(f) == count);
    check(profile.compare(0, 7, "merged.") == 0 && profile.size() == 23);
    check(ecuio_validate_signal_set(set, nullptr) == ECUIO_VALIDATE_OK);
    /* obd2's modules come first, whole or alone; alone, no obd2 def */
    const ecuio_signal_set_t* obd2 = ecuio_obd2_signal_set();
    check(!(base || modules) ||
          (set->channel_count >= obd2->channel_count &&
           memcmp(set->channels, obd2->channels,
                  obd2->channel_count * sizeof *set->channels) == 0));
    check(!modules || ecuio_signal_file_extends(f)[0] == '\0');
    check_keys(f, count, base, modules);
    check_notes(f, count);

    /* The text reads back to the same set, parts and keys */
    const std::string text = text_of(f);
    ecuio_signal_file_t* back = nullptr;
    check(ecuio_signal_file_load_with(text.data(), text.size(),
                                      ECUIO_SIGNAL_FILE_LOAD_SET,
                                      &back) == ECUIO_OK);
    check(same_tables(set, ecuio_signal_file_set(back)));
    check(profile == ecuio_signal_file_profile(back));
    check(ecuio_signal_file_part_count(back) == count);
    check(ecuio_signal_file_note_count(back) == 0);
    check_keys(back, count, base, modules);
    ecuio_signal_file_free(back);

    /* It registers under its profile, as a session needs */
    const ecuio_status_t held =
        ecuio_profile_register(profile.c_str(), set, nullptr);
    check(held == ECUIO_OK || held == ECUIO_ERR_OUT_OF_MEMORY);
    if (held == ECUIO_OK) {
        check(ecuio_profile_unregister(profile.c_str()) == ECUIO_OK);
    }
}

void check_merge(const std::vector<const ecuio_signal_file_t*>& parts,
                 uint32_t flags)
{
    /* The set extends obd2 when it is asked to or a part does; obd2's
     * modules alone are asked for and no part extends it */
    bool with_base = (flags & ECUIO_MERGE_BASE_OBD2) != 0;
    for (const ecuio_signal_file_t* p : parts) {
        with_base = with_base || extends_obd2(p);
    }
    const bool modules = (flags & ECUIO_MERGE_BASE_MODULES) != 0 && !with_base;
    const merged m = merge(parts, flags);
    const uint32_t both = ECUIO_MERGE_BASE_OBD2 | ECUIO_MERGE_BASE_MODULES;
    check((flags & both) != both || m.status == ECUIO_ERR_INVALID_ARGUMENT);
    if (m.status != ECUIO_OK) {
        check(m.file == nullptr);
        check(m.status == ECUIO_ERR_OUT_OF_MEMORY ||
              (m.status == ECUIO_ERR_INVALID_ARGUMENT && !m.words.empty()));
        /* The composer's own fault, never an input's */
        check(m.words.compare(0, kOwnFault.size(), kOwnFault) != 0);
        return;
    }
    check(m.file != nullptr && m.words.empty());
    check_merged(m.file, parts, with_base, modules);
    /* The same parts give the same bytes */
    const merged again = merge(parts, flags);
    check(again.status == ECUIO_OK || again.status == ECUIO_ERR_OUT_OF_MEMORY);
    if (again.status == ECUIO_OK) {
        check(text_of(again.file) == text_of(m.file));
        ecuio_signal_file_free(again.file);
    }
    /* A format 3 part merged alone is its own set */
    if (parts.size() == 1 && ecuio_signal_file_format(parts[0]) == 3 &&
        with_base == extends_obd2(parts[0]) && !modules) {
        check(same_tables(ecuio_signal_file_set(m.file),
                          ecuio_signal_file_set(parts[0])));
    }
    ecuio_signal_file_free(m.file);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 2) {
        return 0;
    }
    const uint32_t flags =
        data[0] & (ECUIO_MERGE_BASE_OBD2 | ECUIO_MERGE_BASE_MODULES);
    std::vector<ecuio_signal_file_t*> loaded;
    const char* at = reinterpret_cast<const char*>(data) + 1;
    const char* const end = reinterpret_cast<const char*>(data) + size;
    for (size_t piece = 0; piece < kMaxParts && at <= end; ++piece) {
        const void* cut = memchr(at, 0x1E, static_cast<size_t>(end - at));
        const char* stop = cut != nullptr && piece + 1 < kMaxParts
                               ? static_cast<const char*>(cut)
                               : end;
        ecuio_signal_file_t* f = nullptr;
        if (ecuio_signal_file_load_with(at, static_cast<size_t>(stop - at),
                                        ECUIO_SIGNAL_FILE_LOAD_SET,
                                        &f) == ECUIO_OK) {
            loaded.push_back(f);
        }
        if (stop == end) {
            break; /* end + 1 is no pointer to form */
        }
        at = stop + 1;
    }
    const std::vector<const ecuio_signal_file_t*> parts(loaded.begin(),
                                                        loaded.end());
    if (!parts.empty() || flags == ECUIO_MERGE_BASE_MODULES) {
        check_merge(parts, flags);
    }
    for (ecuio_signal_file_t* f : loaded) {
        ecuio_signal_file_free(f);
    }
    return 0;
}
