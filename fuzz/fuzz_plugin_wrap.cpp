// File/layer: fuzz/fuzz_plugin_wrap.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: making and changing a packed plugin (ADR-0064,
//          ecuio_plugin_wrap, ecuio_plugin_put_variant,
//          ecuio_plugin_drop_variant)
//
// The first byte holds the switches below; the rest, cut at its first
// 0x1E byte, is a signal file (or with kPacked a packed plugin) and a
// second signal file. The first is wrapped, the second put into the
// result, a variant dropped from that. fuzz/README.md lists what each
// input must hold to.

#include <ecuio/core.h>
#include <ecuio/plugin.h>
#include <ecuio/signals.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

const uint8_t kId = 0x01;      /* the wrap names its plugin */
const uint8_t kVariant = 0x02; /* the wrap names its variant */
const uint8_t kWmi = 0x04;     /* the wrap has a match */
const uint8_t kYears = 0x08;   /* and the match has years */
const uint8_t kPutSlug = 0x10; /* the put names no variant */
const uint8_t kPutBase = 0x20; /* the put names "base", else "other" */
const uint8_t kPacked = 0x40;  /* the first piece is a packed plugin */

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

struct answer {
    ecuio_status_t status = ECUIO_OK;
    std::string text;
    std::string words;
};

answer taken(ecuio_status_t status, char* out, size_t length)
{
    answer a;
    a.status = status;
    a.words = ecuio_last_error_detail();
    check(a.words.find('\n') == std::string::npos &&
          a.words.find('\r') == std::string::npos);
    if (status == ECUIO_OK) {
        check(out != nullptr && strlen(out) == length);
        a.text.assign(out, length);
        /* Line ends are the writer's own, whatever the inputs had */
        check(a.text.find('\r') == std::string::npos);
    } else {
        check(status == ECUIO_ERR_INVALID_ARGUMENT ||
              status == ECUIO_ERR_OUT_OF_MEMORY);
        check(out == nullptr && length == 0 && !a.words.empty());
    }
    ecuio_plugin_text_free(out);
    return a;
}

answer wrap(const std::string& file, const ecuio_plugin_wrap_options_t* o)
{
    char* out = reinterpret_cast<char*>(1);
    size_t length = 1;
    const ecuio_status_t s =
        ecuio_plugin_wrap(file.data(), file.size(), o, &out, &length);
    return taken(s, out, length);
}

answer put(const std::string& packed, const char* variant,
           const std::string& file)
{
    char* out = reinterpret_cast<char*>(1);
    size_t length = 1;
    const ecuio_status_t s =
        ecuio_plugin_put_variant(packed.data(), packed.size(), variant,
                                 file.data(), file.size(), &out, &length);
    return taken(s, out, length);
}

answer drop(const std::string& packed, const char* variant)
{
    char* out = reinterpret_cast<char*>(1);
    size_t length = 1;
    const ecuio_status_t s = ecuio_plugin_drop_variant(
        packed.data(), packed.size(), variant, &out, &length);
    return taken(s, out, length);
}

/* The same call again gives the same answer */
void check_same(const answer& a, const answer& b)
{
    check(a.status == b.status && a.words == b.words && a.text == b.text);
}

/* What a variant decodes with, but its channels: a put or a drop may
 * add or take an ECU another variant never reads */
struct tables {
    std::string id;
    std::string defs;
    std::string enums;
    std::string computed;

    bool operator==(const tables& o) const
    {
        return defs == o.defs && enums == o.enums && computed == o.computed;
    }
};

tables tables_of(const ecuio_signal_set_t* set)
{
    auto bytes = [](const void* p, size_t n) {
        return n == 0 ? std::string()
                      : std::string(static_cast<const char*>(p), n);
    };
    tables t;
    t.defs = bytes(set->defs, sizeof *set->defs * set->count);
    t.enums = bytes(set->enums, sizeof *set->enums * set->enum_count);
    t.computed =
        bytes(set->computed, sizeof *set->computed * set->computed_count);
    return t;
}

struct plugin_view {
    std::string id;
    std::string name;
    std::string version;
    std::string lineage;
    uint16_t needs[3] = {};
    std::vector<tables> variants;

    const tables* find(const std::string& variant) const
    {
        for (const tables& t : variants) {
            if (t.id == variant) {
                return &t;
            }
        }
        return nullptr;
    }
};

/* A text one of the calls gave: it loads, or the harness stops */
plugin_view view_of(const std::string& packed)
{
    ecuio_plugin_t* p = nullptr;
    check(ecuio_plugin_load(packed.data(), packed.size(), nullptr, nullptr,
                            &p) == ECUIO_OK);
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    check(ecuio_plugin_info(p, &info) == ECUIO_OK);
    plugin_view v;
    v.id = info.id;
    v.name = info.name;
    v.version = info.version;
    char lineage[ECUIO_PLUGIN_LINEAGE_LEN + 1];
    check(ecuio_plugin_lineage(p, lineage, sizeof lineage) == ECUIO_OK);
    v.lineage = lineage;
    check(ecuio_plugin_requires_core(p, &v.needs[0], &v.needs[1],
                                     &v.needs[2]) == ECUIO_OK);
    for (uint32_t i = 0; i < info.variant_count; ++i) {
        char id[ECUIO_PLUGIN_VARIANT_ID_MAX + 1];
        check(ecuio_plugin_variant(p, i, id, sizeof id) == ECUIO_OK);
        tables t = tables_of(ecuio_plugin_variant_set(p, i));
        t.id = id;
        v.variants.push_back(t);
        /* Each variant's text reads back as a set as large, and names a
         * source */
        const ecuio_signal_file_t* f = ecuio_plugin_variant_file(p, i);
        size_t length = 0;
        const char* text = ecuio_signal_file_text(f, &length);
        ecuio_signal_file_t* back = nullptr;
        check(ecuio_signal_file_load(text, length, &back) == ECUIO_OK);
        check(ecuio_signal_file_set(back)->count ==
              ecuio_plugin_variant_set(p, i)->count);
        ecuio_signal_file_free(back);
        ecuio_signal_source_t source{};
        source.struct_size = sizeof source;
        check(ecuio_signal_file_source(f, &source) == ECUIO_OK &&
              source.name[0] != '\0');
    }
    check(ecuio_plugin_free(p) == ECUIO_OK);
    return v;
}

void check_requires_this_core(const plugin_view& v)
{
    uint32_t core[3] = {};
    check(ecuio_version(&core[0], &core[1], &core[2]) == ECUIO_OK);
    check(v.needs[0] == core[0] && v.needs[1] == core[1] &&
          v.needs[2] == core[2]);
}

/* Whether core version a is before b */
bool older(const uint16_t a[3], const uint16_t b[3])
{
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) {
            return a[i] < b[i];
        }
    }
    return false;
}

/* "X.Y.Z" with the minor one higher and the patch 0 */
std::string raised(const std::string& version)
{
    const size_t a = version.find('.');
    const size_t b = version.find('.', a + 1);
    const unsigned long minor =
        strtoul(version.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
    return version.substr(0, a + 1) + std::to_string(minor + 1) + ".0";
}

/* The put into packed (a plugin that loads) and the drops after it */
void put_and_drop(const std::string& packed, uint8_t switches,
                  const std::string& file)
{
    const plugin_view before = view_of(packed);
    const char* named = (switches & kPutSlug) != 0   ? nullptr
                        : (switches & kPutBase) != 0 ? "base"
                                                     : "other";
    const answer added = put(packed, named, file);
    check_same(added, put(packed, named, file));

    ecuio_signal_file_t* alone = nullptr;
    const ecuio_status_t alone_s =
        ecuio_signal_file_load(file.data(), file.size(), &alone);
    const std::string alone_words = ecuio_last_error_detail();
    std::string after = packed;
    if (added.status == ECUIO_OK) {
        check(alone_s == ECUIO_OK);
        const plugin_view now = view_of(added.text);
        check(now.id == before.id && now.name == before.name &&
              now.lineage == before.lineage);
        check(now.version == raised(before.version));
        check_requires_this_core(now);
        /* One variant is the file's, decoding as the file alone does;
         * every other is as it was, in its place */
        const tables want = tables_of(ecuio_signal_file_set(alone));
        size_t changed = 0;
        size_t place = now.variants.size();
        for (size_t i = 0; i < now.variants.size(); ++i) {
            const tables* old = before.find(now.variants[i].id);
            if (named != nullptr
                    ? now.variants[i].id == named
                    : (old == nullptr || !(*old == now.variants[i]))) {
                ++changed;
                place = i;
            } else {
                check(old != nullptr && *old == now.variants[i]);
            }
        }
        check(changed <= 1);
        if (changed == 1) {
            check(now.variants[place] == want);
            const bool replaced =
                before.find(now.variants[place].id) != nullptr;
            check(now.variants.size() ==
                  before.variants.size() + (replaced ? 0 : 1));
            check(replaced || place + 1 == now.variants.size());
        } else {
            /* No name given and the file's slug is a variant that
             * decoded as the file already */
            check(named == nullptr &&
                  now.variants.size() == before.variants.size());
        }
        after = added.text;
    } else if (alone_s != ECUIO_OK) {
        /* The file's own refusal, in its loader's words */
        check(added.status == alone_s && added.words == alone_words);
    }
    ecuio_signal_file_free(alone);

    /* Each variant dropped from what the put left: refused only as the
     * last, and the others decode as they did */
    const plugin_view whole = view_of(after);
    for (const tables& gone : whole.variants) {
        const answer less = drop(after, gone.id.c_str());
        check_same(less, drop(after, gone.id.c_str()));
        if (whole.variants.size() == 1) {
            check(less.status == ECUIO_ERR_INVALID_ARGUMENT);
            continue;
        }
        if (less.status != ECUIO_OK) {
            /* Only a version with no minor left stops a drop */
            check(less.words.find("no minor left") != std::string::npos);
            continue;
        }
        const plugin_view rest = view_of(less.text);
        check(rest.variants.size() + 1 == whole.variants.size());
        check(rest.id == whole.id && rest.lineage == whole.lineage);
        check(rest.version == raised(whole.version));
        /* Raised where the plugin named less than the result needs */
        check(!older(rest.needs, whole.needs));
        check(rest.find(gone.id) == nullptr);
        for (const tables& t : rest.variants) {
            const tables* old = whole.find(t.id);
            check(old != nullptr && *old == t);
        }
    }
    check(drop(after, "no-such-variant-here").status ==
          ECUIO_ERR_INVALID_ARGUMENT);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0) {
        return 0;
    }
    const uint8_t switches = data[0];
    const std::string rest(reinterpret_cast<const char*>(data) + 1, size - 1);
    const size_t cut = rest.find('\x1E');
    const std::string first = rest.substr(0, cut);
    const std::string second =
        cut == std::string::npos ? std::string() : rest.substr(cut + 1);

    if ((switches & kPacked) != 0) {
        ecuio_plugin_t* p = nullptr;
        if (ecuio_plugin_load(first.data(), first.size(), nullptr, nullptr,
                              &p) != ECUIO_OK) {
            /* Refused in the plugin loader's words, placed as its input */
            const std::string why = ecuio_last_error_detail();
            const answer a = put(first, "other", second);
            check(a.status != ECUIO_OK && a.words == "the plugin, " + why);
            const answer d = drop(first, "base");
            check(d.status != ECUIO_OK && d.words == why);
            return 0;
        }
        check(ecuio_plugin_free(p) == ECUIO_OK);
        put_and_drop(first, switches, second);
        return 0;
    }

    ecuio_plugin_wrap_options_t o{};
    o.struct_size = sizeof o;
    o.id = (switches & kId) != 0 ? "fuzz-id" : nullptr;
    o.variant = (switches & kVariant) != 0 ? "v2" : nullptr;
    if ((switches & kWmi) != 0) {
        o.wmi = "ZZZ,ZZ9ABC";
        if ((switches & kYears) != 0) {
            o.years[0] = 2015;
            o.years[1] = 2030;
        }
    }
    const answer made = wrap(first, &o);
    check_same(made, wrap(first, &o));

    ecuio_signal_file_t* alone = nullptr;
    const ecuio_status_t alone_s =
        ecuio_signal_file_load(first.data(), first.size(), &alone);
    const std::string alone_words = ecuio_last_error_detail();
    if (made.status != ECUIO_OK) {
        if (alone_s != ECUIO_OK) {
            check(made.status == alone_s && made.words == alone_words);
        } else {
            /* A file that loads wraps, but for one too large or too
             * deep once it is written out inside a package: only the
             * words of those two bounds pass, so a variant that reads
             * back as another set stops the run */
            check(made.status == ECUIO_ERR_OUT_OF_MEMORY ||
                  made.words.find("past the limit of") != std::string::npos ||
                  made.words.find("nested deeper than") != std::string::npos);
        }
        ecuio_signal_file_free(alone);
        return 0;
    }
    check(alone_s == ECUIO_OK);
    const plugin_view v = view_of(made.text);
    check(v.variants.size() == 1);
    check(v.variants[0].id == ((switches & kVariant) != 0 ? "v2" : "base"));
    check(v.name == ecuio_signal_file_name(alone));
    check(v.version == "1.0.0" && v.lineage.size() == ECUIO_PLUGIN_LINEAGE_LEN);
    check((switches & kId) == 0 || v.id == "fuzz-id");
    check_requires_this_core(v);
    /* The variant is the file: every table, the channels too */
    {
        ecuio_plugin_t* p = nullptr;
        check(ecuio_plugin_load(made.text.data(), made.text.size(), nullptr,
                                nullptr, &p) == ECUIO_OK);
        const ecuio_signal_set_t* a = ecuio_signal_file_set(alone);
        const ecuio_signal_set_t* b = ecuio_plugin_variant_set(p, 0);
        check(tables_of(a) == tables_of(b));
        check(a->channel_count == b->channel_count &&
              (a->channel_count == 0 ||
               memcmp(a->channels, b->channels,
                      sizeof *a->channels * a->channel_count) == 0));
        for (uint32_t i = 0; i < a->count; ++i) {
            const char* key = ecuio_signal_file_key(alone, &a->defs[i]);
            if (key[0] != '\0') {
                check(ecuio_plugin_variant_find(p, 0, key) == &b->defs[i]);
            }
        }
        check(ecuio_plugin_free(p) == ECUIO_OK);
    }
    ecuio_signal_file_free(alone);

    put_and_drop(made.text, switches, second);
    return 0;
}
