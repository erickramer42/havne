// File/layer: cli/profile_source.cpp | havne-cli, where a command's signals and
//             conditions come from: a signal file, a plugin, a recording
// Testing: UNIT: CliGoldenTest, CliSignalTest, CliReplayTest,
//          CliConditionsRunTest, CliUsageTest (ctest); CliSignalFileTest
//          (test_cli)
//
// Public headers only: what any program linking core can do.

#include "condition_lines.h"
#include "plugin_lines.h"
#include "recording_signals.h"
#include "signal_file_lines.h"

#include "profile_source.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/plugin.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "ecuio/conditions.h"

namespace cmd {

// A signal file loaded for the process's life: the profile it registers
// keeps its set, and a session's close can outlive the command's return
// (an interrupt while it hangs), so it is never freed
ecuio_signal_file_t* g_signal_file = nullptr;

// Reads and loads a signal file: a data file, or with recorded a
// recording's signals.json, a session's whole set, read to a set's
// bounds. On failure, the status and why for an error line, never the
// path: the reason it can't be read, or core's words, which name the line
// and column
ecuio_status_t load_signal_file(const char* path, ecuio_signal_file_t** file,
                                std::string* why, bool recorded)
{
    *file = nullptr;
    std::string bytes;
    if (!read_whole(path,
                    recorded ? ECUIO_SIGNAL_SET_MAX_BYTES
                             : ECUIO_SIGNAL_FILE_MAX_BYTES,
                    "signal file", &bytes, why)) {
        return ECUIO_ERR_LOAD_FAILED;
    }
    const ecuio_status_t st = ecuio_signal_file_load_with(
        bytes.data(), bytes.size(), recorded ? ECUIO_SIGNAL_FILE_LOAD_SET : 0u,
        file);
    if (st != ECUIO_OK) {
        *why = ecuio_last_error_detail();
    }
    return st;
}

// --signals for stream and sim: loads the file, registers its profile
// for the process's life (g_signal_file) and gives the URI to open, the
// file's profile added when it selects none. -1 to go on; else the exit
// code: a file that can't be read or is refused is start and an error
// line (3), a URI that selects another profile a usage error (2)
int use_signal_file(const char* path, const char* uri, std::string* opened)
{
    std::string why;
    ecuio_signal_file_t* file = nullptr;
    ecuio_status_t st = load_signal_file(path, &file, &why);
    if (st == ECUIO_OK) {
        const char* profile = ecuio_signal_file_profile(file);
        if (!cli::uri_for_profile(uri, profile, opened)) {
            std::fprintf(stderr,
                         "havne-cli: the URI's profile= isn't the signal "
                         "file's, %s\n",
                         profile);
            ecuio_signal_file_free(file);
            return kExitUsage;
        }
        st = ecuio_profile_register(profile, ecuio_signal_file_set(file),
                                    nullptr);
        if (st == ECUIO_OK) {
            g_signal_file = file;
            return -1;
        }
        why = ecuio_last_error_detail();
        ecuio_signal_file_free(file);
    }
    Output out;
    write_start(out);
    write_error(out, st, why.c_str());
    out.flush();
    return kExitConnect;
}

// stream replay:// with neither --signals nor --plugin (ADR-0051
// section 4): the recording's own signals.json, when events.log's header
// names one, the reader finds it matching that hash, and the URI selects
// the recorded profile, the copy's own name (rec.<16 hex>) or none. The
// copy is loaded and registered under the profile the URI selects (the
// recorded one when it selects none) for the process's life
// (g_signal_file, so a --record of the replay holds it too), and
// *opened is the URI with that profile. -1 to go on, *opened empty when
// the URI stays as given (a stderr line says why when a copy is there
// but unused); a copy that doesn't load or register is start and an
// error line (3). The match costs the reader's open, one pass over
// signals.csv, before the replay starts.
// What a recording's events.log header says decoded it (its profile and
// signal file lines); empty when it has no events.log
cli::RecordedSignals recorded_signals(const std::string& folder)
{
    cli::RecordedSignals recorded;
    std::string head;
    std::string why;
    if (!read_whole((folder + "/events.log").c_str(), 4u << 20, "events.log",
                    &head, &why)) {
        return recorded;
    }
    size_t at = 0;
    while (at < head.size()) {
        size_t end = head.find('\n', at);
        if (end == std::string::npos) {
            end = head.size();
        }
        if (!recorded.take(head.substr(at, end - at))) {
            break;
        }
        at = end + 1;
    }
    return recorded;
}

int use_recording_signals(const char* uri, std::string* opened)
{
    opened->clear();
    std::string folder;
    if (!cli::replay_folder(uri, &folder)) {
        return -1;
    }
    std::string why;
    const cli::RecordedSignals recorded = recorded_signals(folder);
    if (!recorded.names_file()) {
        return -1;
    }
    std::string selected;
    const bool selects = cli::uri_profile(uri, &selected);
    const std::string own = cli::own_profile(recorded.sha256);
    if (selects && selected != recorded.profile && selected != own) {
        return -1;
    }
    const std::string profile = selects ? selected : recorded.profile;
    const auto unused = [&](const char* how) {
        std::fprintf(stderr,
                     "havne-cli: the recording's signals.json %s; decoding "
                     "with %s as loaded\n",
                     how, selects ? selected.c_str() : "the replay's raw");
        return -1;
    };
    ecuio_recording_t* r = nullptr;
    if (ecuio_recording_open(folder.c_str(), nullptr, nullptr, &r) !=
        ECUIO_OK) {
        return unused("can't be checked: the recording doesn't open");
    }
    ecuio_recording_signals_t sig{};
    const ecuio_status_t got = ecuio_recording_get_signals(r, &sig);
    ecuio_recording_close(r);
    if (got != ECUIO_OK || (sig.flags & ECUIO_RECORDING_SIGNALS_PRESENT) == 0) {
        return unused("is missing");
    }
    if ((sig.flags & ECUIO_RECORDING_SIGNALS_MATCH) == 0) {
        return unused("doesn't match its hash");
    }
    ecuio_signal_file_t* file = nullptr;
    ecuio_status_t st =
        load_signal_file((folder + "/signals.json").c_str(), &file, &why, true);
    if (st == ECUIO_OK) {
        st = ecuio_profile_register(profile.c_str(),
                                    ecuio_signal_file_set(file), nullptr);
        if (st == ECUIO_OK) {
            g_signal_file = file;
            *opened =
                selects ? std::string(uri) : cli::with_profile(uri, profile);
            return -1;
        }
        why = ecuio_last_error_detail();
        ecuio_signal_file_free(file);
    }
    Output out;
    write_start(out);
    write_error(out, st, why.c_str());
    out.flush();
    return kExitConnect;
}

// stream replay:// with no --conditions (ADR-0063 section 8): the
// recording's own conditions.json, when events.log's header names one and
// the reader finds it matching that hash, loaded to be attached as a
// --conditions file is; its key: references bind through the replay's
// signal file, the recording's own when that was taken. Else nullptr, with
// a stderr line when the header names a copy that can't be taken, and the
// replay goes on judging none. The match costs the reader's open, a pass
// over signals.csv, as the signal file's does.
ecuio_condition_set_t* recording_conditions(const char* uri)
{
    std::string folder;
    if (!cli::replay_folder(uri, &folder)) {
        return nullptr;
    }
    if (recorded_signals(folder).conditions_sha256.empty()) {
        return nullptr;
    }
    const auto unused = [](const std::string& how) -> ecuio_condition_set_t* {
        std::fprintf(stderr,
                     "havne-cli: the recording's conditions.json %s; judging "
                     "no conditions\n",
                     how.c_str());
        return nullptr;
    };
    ecuio_recording_t* r = nullptr;
    if (ecuio_recording_open(folder.c_str(), nullptr, nullptr, &r) !=
        ECUIO_OK) {
        return unused("can't be checked: the recording doesn't open");
    }
    ecuio_recording_conditions_t cs{};
    const ecuio_status_t got = ecuio_recording_get_conditions(r, &cs);
    ecuio_recording_close(r);
    if (got != ECUIO_OK ||
        (cs.flags & ECUIO_RECORDING_CONDITIONS_PRESENT) == 0) {
        return unused("is missing");
    }
    if ((cs.flags & ECUIO_RECORDING_CONDITIONS_MATCH) == 0) {
        return unused("doesn't match its hash");
    }
    std::string bytes;
    std::string why;
    if (!read_whole((folder + "/conditions.json").c_str(),
                    ECUIO_CONDITION_FILE_MAX_BYTES, "condition file", &bytes,
                    &why)) {
        return unused("doesn't read: " + why);
    }
    ecuio_condition_set_t* set = nullptr;
    if (ecuio_condition_set_load(bytes.data(), bytes.size(), &set) !=
        ECUIO_OK) {
        return unused(std::string("doesn't load: ") +
                      ecuio_last_error_detail());
    }
    return set;
}

// A plugin loaded for the process's life (stream --plugin, sim --plugin):
// its profiles keep their sets, and a session's close can outlive the
// command's return, so it is never freed, as g_signal_file isn't
ecuio_plugin_t* g_plugin = nullptr;

namespace {

uint32_t g_plugin_variant = 0; // the variant the URI's profile= selects

} // namespace

// The signal file decoding the session: the --signals file or a
// recording's copy, the --plugin variant's files merged into one, or NULL
const ecuio_signal_file_t* session_signal_file()
{
    return g_signal_file != nullptr ? g_signal_file
           : g_plugin != nullptr
               ? ecuio_plugin_variant_file(g_plugin, g_plugin_variant)
               : nullptr;
}

// --conditions' file, read and loaded before the connect, so one the
// loader refuses never opens the adapter: nullptr with the status and the
// words for an error line
ecuio_condition_set_t*
load_conditions_file(const char* path, ecuio_status_t* st, std::string* why)
{
    std::string bytes;
    ecuio_condition_set_t* set = nullptr;
    *st = ECUIO_ERR_LOAD_FAILED;
    if (read_whole(path, ECUIO_CONDITION_FILE_MAX_BYTES, "condition file",
                   &bytes, why)) {
        *st = ecuio_condition_set_load(bytes.data(), bytes.size(), &set);
        *why = ecuio_last_error_detail();
    }
    return *st == ECUIO_OK ? set : nullptr;
}

namespace {

void write_conditions_line(Output& out, const ShownConditions& s)
{
    const uint32_t count = static_cast<uint32_t>(s.infos.size());
    BigLine b(cli::conditions_line_bytes(s.name.c_str(), count));
    cli::conditions_line(b.line, s.name.c_str(), s.source,
                         count > 0 ? s.states[0].generation : 1, s.infos.data(),
                         s.states.data(), count);
    write_line(out, b.line);
}

} // namespace

// The set attached before a recording begins (a recording holds one set
// for its whole length; stream's and log's) and the conditions line
// saying what the session made of each. The set is freed either way;
// false after an error line.
bool attach_conditions(Output& out, ecuio_session_t* session,
                       ecuio_condition_set_t* set, const char* source,
                       ShownConditions* shown)
{
    ecuio_status_t st =
        ecuio_session_set_conditions(session, set, session_signal_file());
    const uint32_t count = ecuio_condition_set_count(set);
    shown->name = ecuio_condition_set_name(set);
    shown->source = source;
    shown->keys.resize(count);
    shown->link_terms.assign(count, 0);
    shown->infos.resize(count);
    shown->states.resize(count);
    shown->now.resize(count);
    uint32_t held = 0;
    if (st == ECUIO_OK) {
        st = ecuio_session_get_condition_states(session, shown->states.data(),
                                                count, &held);
    }
    if (st != ECUIO_OK || held != count) {
        write_error(out, st != ECUIO_OK ? st : ECUIO_ERR_INVALID_ARGUMENT);
        ecuio_condition_set_free(set);
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        ecuio_condition_set_get(set, i, &shown->infos[i]);
        shown->keys[i] = shown->infos[i].key;
        for (uint32_t k = 0; k < shown->infos[i].term_count && k < 8; ++k) {
            ecuio_condition_term_t t{};
            if (ecuio_condition_set_get_term(set, i, k, &t) == ECUIO_OK &&
                t.kind == ECUIO_TERM_LINK) {
                shown->link_terms[i] =
                    static_cast<uint8_t>(shown->link_terms[i] | 1u << k);
            }
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        shown->infos[i].key = shown->keys[i].c_str();
        shown->infos[i].name = "";
    }
    ecuio_condition_set_free(set);
    write_conditions_line(out, *shown);
    return true;
}

// The conditions line again when a condition's availability has changed
// since the line last written: a signal the session reports, once it
// runs, it doesn't get (ECUIO_CONDITION_WHY_NOT_RECEIVED), or its first
// sample after all. The attach's other reasons never change.
void poll_conditions(Output& out, ecuio_session_t* session, ShownConditions* s)
{
    const uint32_t count = static_cast<uint32_t>(s->states.size());
    uint32_t n = 0;
    if (count == 0 ||
        ecuio_session_get_condition_states(session, s->now.data(), count, &n) !=
            ECUIO_OK ||
        n != count) {
        return;
    }
    if (cli::availability_changed(s->states.data(), s->now.data(), count)) {
        s->states.swap(s->now);
        write_conditions_line(out, *s);
    }
}

namespace {

// A folder package's files for ecuio_plugin_load, read beside its
// manifest; a file that can't be read is refused by core at the path the
// manifest names it by
struct PackageFolder {
    std::string root;
    std::string bytes;

    static int read(void* user, const char* path, const char** bytes,
                    size_t* length)
    {
        auto* self = static_cast<PackageFolder*>(user);
        std::string why;
        if (!read_whole((self->root + "/" + path).c_str(),
                        ECUIO_SIGNAL_FILE_MAX_BYTES, "file", &self->bytes,
                        &why)) {
            return 1;
        }
        *bytes = self->bytes.data();
        *length = self->bytes.size();
        return 0;
    }
};

} // namespace

// Reads and loads a package: a folder holding manifest.json, or a packed
// plugin's file. On failure the status and why for an error line, never
// the path: the reason it can't be read, or core's words, which name the
// file in the package, the line and the column
ecuio_status_t load_package(const char* path, ecuio_plugin_t** out,
                            std::string* why)
{
    *out = nullptr;
    std::string bytes;
    ecuio_status_t st = ECUIO_OK;
    if (is_folder(path)) {
        PackageFolder folder;
        folder.root = path;
        if (!read_whole((folder.root + "/manifest.json").c_str(),
                        ECUIO_PLUGIN_MANIFEST_MAX, "plugin's manifest.json",
                        &bytes, why)) {
            return ECUIO_ERR_LOAD_FAILED;
        }
        st = ecuio_plugin_load(bytes.data(), bytes.size(), &PackageFolder::read,
                               &folder, out);
    } else {
        if (!read_whole(path, ECUIO_PLUGIN_PACKAGE_MAX, "plugin", &bytes,
                        why)) {
            return ECUIO_ERR_LOAD_FAILED;
        }
        st = ecuio_plugin_load(bytes.data(), bytes.size(), nullptr, nullptr,
                               out);
    }
    if (st != ECUIO_OK) {
        *why = ecuio_last_error_detail();
    }
    return st;
}

ecuio_plugin_info_t plugin_info(const ecuio_plugin_t* p)
{
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    ecuio_plugin_info(p, &info);
    return info;
}

std::string variant_of(const ecuio_plugin_t* p, uint32_t index)
{
    char id[ECUIO_PLUGIN_VARIANT_ID_MAX + 1] = {};
    ecuio_plugin_variant(p, index, id, sizeof id);
    return id;
}

// Registers every variant's profile: ECUIO_OK, or the first refusal's
// status with why
ecuio_status_t register_variants(ecuio_plugin_t* p, std::string* why)
{
    const ecuio_plugin_info_t info = plugin_info(p);
    for (uint32_t v = 0; v < info.variant_count; ++v) {
        const ecuio_status_t st = ecuio_plugin_register(p, v, nullptr, 0);
        if (st != ECUIO_OK) {
            *why = ecuio_last_error_detail();
            return st;
        }
    }
    return ECUIO_OK;
}

// --plugin for stream and sim: loads the package and registers its
// variants for the process's life (g_plugin); the URI must select one of
// them by profile=, never a guess (ADR-0052 section 5). -1 to go on; else
// the exit code: a package that can't be read or is refused is start and
// an error line (3), a URI that selects none of its variants a usage
// error (2)
int use_plugin(const char* path, const char* uri)
{
    std::string why;
    ecuio_plugin_t* p = nullptr;
    ecuio_status_t st = load_package(path, &p, &why);
    if (st == ECUIO_OK) {
        const ecuio_plugin_info_t info = plugin_info(p);
        std::string selected;
        const bool given = cli::uri_profile(uri, &selected);
        std::string names;
        bool known = false;
        for (uint32_t v = 0; v < info.variant_count; ++v) {
            const std::string name =
                cli::plugin_profile(info.id, variant_of(p, v).c_str());
            names += (v > 0 ? ", " : "") + name;
            if (given && name == selected) {
                known = true;
                g_plugin_variant = v;
            }
        }
        if (!known) {
            std::fprintf(stderr,
                         "havne-cli: --plugin needs the URI's profile= to "
                         "name one of its variants: %s\n",
                         names.c_str());
            ecuio_plugin_free(p);
            return kExitUsage;
        }
        st = register_variants(p, &why);
        if (st == ECUIO_OK) {
            g_plugin = p;
            return -1;
        }
        ecuio_plugin_free(p);
    }
    Output out;
    write_start(out);
    write_error(out, st, why.c_str());
    out.flush();
    return kExitConnect;
}

// stream's and log's profile source: a plugin's variants registered, a
// signal file's profile put in the URI, or a replay's own signals.json
// (opened holds the URI then; *uri points into it). An exit code (0 or
// more) when the command ends here, -1 to go on.
int take_profile_source(const char* signals, const char* plugin,
                        const char** uri, std::string* opened)
{
    if (plugin != nullptr) {
        const int rc = use_plugin(plugin, *uri);
        if (rc >= 0) {
            return rc;
        }
    }
    if (signals != nullptr) {
        const int rc = use_signal_file(signals, *uri, opened);
        if (rc >= 0) {
            return rc;
        }
        *uri = opened->c_str();
    } else if (plugin == nullptr) {
        const int rc = use_recording_signals(*uri, opened);
        if (rc >= 0) {
            return rc;
        }
        if (!opened->empty()) {
            *uri = opened->c_str();
        }
    }
    return -1;
}

} // namespace cmd
