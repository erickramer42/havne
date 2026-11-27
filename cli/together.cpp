#include "default_pick.h"
#include "plugin_lines.h"
#include "signal_file_lines.h"
#include "together.h"

#include "profile_source.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/plugin.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"

#include <cstring>

namespace cmd {

ecuio_signal_file_t* g_together = nullptr;

namespace {

const char kObd2[] = "obd2";
const char kModules[] = "obd2-modules";

std::vector<std::string> g_named;
std::vector<ecuio_plugin_t*> g_parts_plugins;
std::vector<ecuio_signal_file_t*> g_parts_files;

void drop_pair(int& argc, char** argv, int at)
{
    for (int k = at; k + 2 < argc; ++k) {
        argv[k] = argv[k + 2];
    }
    argc -= 2;
}

bool split_profiles(const char* list, std::vector<std::string>* out)
{
    std::string item;
    for (const char* p = list;; ++p) {
        if (*p == ',' || *p == '\0') {
            if (item.empty()) {
                return false;
            }
            out->push_back(item);
            item.clear();
            if (*p == '\0') {
                return true;
            }
        } else {
            item += *p;
        }
    }
}

int usage_line(const char* words)
{
    std::fprintf(stderr, "havne-cli: %s\n", words);
    return kExitUsage;
}

} // namespace

bool take_together(int& argc, char** argv, int first, Together* t)
{
    for (int i = first; i < argc;) {
        if (std::strcmp(argv[i], "--together") != 0) {
            ++i;
            continue;
        }
        if (t->given || i + 1 >= argc ||
            !split_profiles(argv[i + 1], &t->profiles)) {
            usage_line("--together takes one list of profiles, comma "
                       "separated: obd2 or obd2-modules, a --plugin's "
                       "variants, --signals files' profiles");
            return false;
        }
        t->given = true;
        drop_pair(argc, argv, i);
    }
    if (!t->given) {
        return true;
    }
    for (int i = first; i < argc;) {
        const bool plugin = std::strcmp(argv[i], "--plugin") == 0;
        if (!plugin && std::strcmp(argv[i], "--signals") != 0) {
            ++i;
            continue;
        }
        if (i + 1 >= argc || argv[i + 1][0] == '\0') {
            usage_line(plugin ? "--plugin takes a package"
                              : "--signals takes a signal file");
            return false;
        }
        (plugin ? t->plugins : t->signals).push_back(argv[i + 1]);
        drop_pair(argc, argv, i);
    }
    return true;
}

namespace {

int refused(Output& out, ecuio_status_t st, const std::string& why)
{
    write_start(out);
    write_error(out, st, why.c_str());
    return ended(out, kExitConnect);
}

int load_parts(Output& out, const Together& t)
{
    std::string why;
    for (const char* path : t.plugins) {
        ecuio_plugin_t* p = nullptr;
        const ecuio_status_t st = load_package(path, &p, &why);
        if (st != ECUIO_OK) {
            return refused(out, st, why);
        }
        g_parts_plugins.push_back(p);
    }
    for (const char* path : t.signals) {
        ecuio_signal_file_t* f = nullptr;
        const ecuio_status_t st = load_signal_file(path, &f, &why);
        if (st != ECUIO_OK) {
            return refused(out, st, why);
        }
        g_parts_files.push_back(f);
    }
    return -1;
}

const ecuio_signal_file_t* part_named(const std::string& profile,
                                      std::string* given)
{
    const ecuio_signal_file_t* found = nullptr;
    for (ecuio_plugin_t* p : g_parts_plugins) {
        const ecuio_plugin_info_t info = plugin_info(p);
        for (uint32_t v = 0; v < info.variant_count; ++v) {
            const std::string name =
                cli::plugin_profile(info.id, variant_of(p, v).c_str());
            *given += ", " + name;
            if (found == nullptr && name == profile) {
                found = ecuio_plugin_variant_file(p, v);
            }
        }
    }
    for (const ecuio_signal_file_t* f : g_parts_files) {
        const char* name = ecuio_signal_file_profile(f);
        *given += std::string(", ") + name;
        if (found == nullptr && profile == name) {
            found = f;
        }
    }
    return found;
}

int pick_parts(const Together& t,
               std::vector<const ecuio_signal_file_t*>* parts, uint32_t* flags)
{
    *flags = 0;
    for (const std::string& name : t.profiles) {
        const uint32_t base = name == kObd2      ? ECUIO_MERGE_BASE_OBD2
                              : name == kModules ? ECUIO_MERGE_BASE_MODULES
                                                 : 0u;
        if (base != 0) {
            if (*flags != 0) {
                return usage_line("--together takes obd2 or obd2-modules "
                                  "once, not both");
            }
            *flags = base;
            continue;
        }
        std::string given = std::string(kObd2) + ", " + kModules;
        const ecuio_signal_file_t* f = part_named(name, &given);
        if (f == nullptr) {
            std::fprintf(stderr,
                         "havne-cli: --together names '%s', which isn't "
                         "among what was given: %s\n",
                         name.c_str(), given.c_str());
            return kExitUsage;
        }
        parts->push_back(f);
    }
    return -1;
}

void free_parts()
{
    for (ecuio_plugin_t* p : g_parts_plugins) {
        ecuio_plugin_free(p);
    }
    for (ecuio_signal_file_t* f : g_parts_files) {
        ecuio_signal_file_free(f);
    }
    g_parts_plugins.clear();
    g_parts_files.clear();
}

int merge_parts(Output& out, const Together& t, TogetherSet* set)
{
    const int loaded = load_parts(out, t);
    if (loaded >= 0) {
        return loaded;
    }
    std::vector<const ecuio_signal_file_t*> parts;
    uint32_t flags = 0;
    const int picked = pick_parts(t, &parts, &flags);
    if (picked >= 0) {
        return picked;
    }
    g_named = t.profiles;
    if (parts.empty() && flags == ECUIO_MERGE_BASE_OBD2) {
        set->profile = kObd2;
        return -1;
    }
    ecuio_merge_options_t options{};
    options.struct_size = sizeof options;
    options.flags = flags;
    ecuio_signal_file_t* merged = nullptr;
    const ecuio_status_t st = ecuio_signal_file_merge(
        parts.data(), static_cast<uint32_t>(parts.size()), &options, &merged);
    if (st != ECUIO_OK) {
        return refused(out, st, ecuio_last_error_detail());
    }
    set->merged = merged;
    set->profile = ecuio_signal_file_profile(merged);
    set->flags = flags;
    return -1;
}

} // namespace

int merge_together(Output& out, const Together& t, TogetherSet* set)
{
    const int rc = merge_parts(out, t, set);
    free_parts();
    return rc;
}

int use_together(const Together& t, const char* uri, std::string* opened)
{
    Output out;
    TogetherSet set;
    const int rc = merge_together(out, t, &set);
    if (rc >= 0) {
        return rc;
    }
    if (!cli::uri_for_profile(uri, set.profile.c_str(), opened)) {
        std::fprintf(stderr,
                     "havne-cli: the URI's profile= isn't --together's, %s\n",
                     set.profile.c_str());
        ecuio_signal_file_free(set.merged);
        return kExitUsage;
    }
    if (set.merged == nullptr) {
        return -1;
    }
    const ecuio_status_t st = ecuio_profile_register(
        set.profile.c_str(), ecuio_signal_file_set(set.merged), nullptr);
    if (st != ECUIO_OK) {
        const std::string why = ecuio_last_error_detail();
        ecuio_signal_file_free(set.merged);
        return refused(out, st, why);
    }
    g_together = set.merged;
    return -1;
}

int take_source(const Together& t, const char* signals, const char* plugin,
                const char** uri, std::string* opened)
{
    if (!t.given) {
        return take_profile_source(signals, plugin, uri, opened);
    }
    const int rc = use_together(t, *uri, opened);
    if (rc < 0) {
        *uri = opened->c_str();
    }
    return rc;
}

const char* note_kind_word(uint32_t kind)
{
    switch (kind) {
    case ECUIO_NOTE_REPLACED_BY_ADDRESS:
        return "replaced_by_address";
    case ECUIO_NOTE_REPLACED_BY_KEY:
        return "replaced_by_key";
    case ECUIO_NOTE_KEY_TAKEN:
        return "key_taken";
    case ECUIO_NOTE_BESIDE_BASE:
        return "beside_base";
    case ECUIO_NOTE_MODULE_RENAMED:
        return "module_renamed";
    case ECUIO_NOTE_MODULE_LEFT_OUT:
        return "module_left_out";
    case ECUIO_NOTE_PART_LEFT_OUT:
        return "part_left_out";
    case ECUIO_NOTE_SIGNAL_LEFT_OUT:
        return "signal_left_out";
    default:
        return "other";
    }
}

std::vector<uint16_t> together_left_out()
{
    return cli::beside_base(g_together);
}

size_t together_keys_bytes()
{
    size_t n = 64 + ECUIO_SIGNAL_KEY_MAX;
    for (const std::string& name : g_named) {
        n += cli::Line::string_bytes(name.c_str()) + 1;
    }
    return n;
}

void together_keys(cli::Line& l)
{
    if (g_named.empty()) {
        return;
    }
    l.key("together");
    l.ch('[');
    for (size_t i = 0; i < g_named.size(); ++i) {
        if (i != 0) {
            l.ch(',');
        }
        l.string(g_named[i].c_str(), SIZE_MAX);
    }
    l.ch(']');
    if (g_together != nullptr) {
        l.key("merged");
        l.string(ecuio_signal_file_profile(g_together), SIZE_MAX);
    }
}

void write_note_lines(Output& out, const ecuio_signal_file_t* file)
{
    const uint32_t count = ecuio_signal_file_note_count(file);
    for (uint32_t i = 0; i < count; ++i) {
        ecuio_signal_note_t n{};
        n.struct_size = sizeof n;
        if (ecuio_signal_file_note(file, i, &n) != ECUIO_OK) {
            continue;
        }
        BigLine b(64 + cli::Line::string_bytes(n.text));
        b.line.raw("{\"type\":\"note\"");
        b.line.key("kind");
        b.line.string(note_kind_word(n.kind), SIZE_MAX);
        b.line.key("text");
        b.line.string(n.text, sizeof n.text);
        b.line.ch('}');
        write_line(out, b.line);
    }
}

void write_together_notes(Output& out)
{
    if (g_together != nullptr) {
        write_note_lines(out, g_together);
    }
}

} // namespace cmd
