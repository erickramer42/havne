// File/layer: core/src/signal_file_build.h | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest, PluginTest, ConditionFileTest (test_signals);
//          RecorderTest (test_core)
//
// The signal-file loader's inside (ADR-0051), shared with the plugin
// loader (ADR-0052): a set built from parts, so a signal file and a
// plugin's variant (several files' signals, the manifest's ECUs) go
// through the same rules and words. A refusal is thrown as refused,
// carrying the value it names, so a plugin can say which file holds it.

#ifndef ECUIO_SIGNAL_FILE_BUILD_H
#define ECUIO_SIGNAL_FILE_BUILD_H

#include "ecuio/signals.h"
#include "json.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

struct ecuio_signal_file {
    std::string name;
    std::string profile;
    std::string extends; /* "obd2" or "" */
    std::vector<ecuio_signal_def_t> defs;
    std::vector<ecuio_channel_decl_t> channels;
    std::vector<ecuio_enum_entry_t> enums;
    std::vector<ecuio_computed_t> computed;
    std::vector<std::pair<std::string, uint32_t>> keys; /* key, def index */
    std::vector<std::pair<uint32_t, ecuio_signal_gauge_t>>
        gauges; /* def index */
    std::vector<std::pair<uint32_t, std::string>> exprs; /* def index */
    uint32_t format = ECUIO_SIGNAL_FILE_FORMAT_OLDEST;
    ecuio_signal_set_t set{};
    /* The bytes a recording copies as signals.json (ADR-0051 section 4):
     * those ecuio_signal_file_load read, or a plugin variant's files
     * merged and written as one file (plugin.cpp); empty when that text
     * doesn't read back to this set */
    std::string bytes;
    /* A plugin variant's "<id>,<version>,<variant>", a recording's plugin
     * line (ADR-0052 section 4); "" for a file loaded alone */
    std::string plugin;
};

namespace ecuio_sf {

/* A refusal: where in the file, why, and the value it names (never
 * NULL: the plugin loader finds the file holding it) */
struct refused {
    uint32_t line;
    uint32_t column;
    std::string words;
    const ecuio_json::Value* at;
};

/* What a set is built from */
struct parts {
    const ecuio_json::Value* at =
        nullptr; /* refusals that name nothing smaller */
    std::string name;
    std::string profile;
    const ecuio_json::Value* extends = nullptr;
    const ecuio_json::Value* ecus = nullptr; /* an array, or NULL */
    std::vector<const ecuio_json::Value*> signals;
    /* The list's own refusals; with own_list, the "signals" array itself,
     * whose items follow signals */
    const ecuio_json::Value* signals_at = nullptr;
    bool own_list = false;
    /* The set's format: the file's, or the newest of a variant's files;
     * each of signals compiles at its own file's (formats, one each),
     * the own list's at format */
    uint32_t format = ECUIO_SIGNAL_FILE_FORMAT_OLDEST;
    std::vector<uint32_t> formats;
};

/* A file's "havne_signals", checked first: its first key, a format this
 * core reads. what names the file in the words ("a signal file"). */
uint32_t format_of(const ecuio_json::Value& root, const char* what);

/* The set from in into f, or a refused thrown (std::bad_alloc too) */
void build(const parts& in, ecuio_signal_file& f);

/* A "source" block's rules */
void read_source(const ecuio_json::Value& source);

/* An "ecus" entry's addresses, the entry one a load took: its rx, and
 * its tx as given or the standard's for that rx */
void ecu_addresses(const ecuio_json::Value& ecu, uint32_t* rx, uint32_t* tx);

/* The loader's rules for any JSON it reads, each throwing refused */
[[noreturn]] void refuse(const ecuio_json::Value& at, std::string words);
[[noreturn]] void refuse(const ecuio_json::Member& at, std::string words);
std::string q(std::string_view text);
void expect(const ecuio_json::Value& v, ecuio_json::Type t,
            const std::string& what);
void only_keys(const ecuio_json::Value& obj,
               std::initializer_list<const char*> allowed,
               const std::string& context);
/* "user." and name's letters and digits in lower case, every run of
 * other characters one '-'; "" when there are none */
std::string profile_of(const std::string& name);

const std::string& text_of(const ecuio_json::Value& v, size_t lo, size_t hi,
                           const std::string& what);
int64_t whole_of(const ecuio_json::Value& v, int64_t lo, int64_t hi,
                 const std::string& what);
double finite_of(const ecuio_json::Value& v, const std::string& what);
/* s into a zeroed buffer of cap bytes, cut to cap - 1 */
void copy_text(char* dst, size_t cap, const std::string& s);
/* A key: 1 to 31 of [a-z0-9_] */
bool key_ok(const std::string& k);
/* Refuses text a spreadsheet would run as a formula (ADR-0049 section 8) */
void no_formula(const ecuio_json::Value& v, const std::string& what);
/* The index in base (the obd2 set) of the def an obd2 key names
 * ("coolant", "pid_0b", "pid_14_1"), or -1 */
long obd2_def(const ecuio_signal_set_t* base, const std::string& key);

/* The bytes a recording copies, "" for none */
inline const std::string& bytes_of(const ecuio_signal_file& f)
{
    return f.bytes;
}

} // namespace ecuio_sf

#endif /* ECUIO_SIGNAL_FILE_BUILD_H */
