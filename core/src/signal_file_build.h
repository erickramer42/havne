// File/layer: core/src/signal_file_build.h | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest, PluginTest, PluginWrapTest,
//          ConditionFileTest (test_signals); RecorderTest (test_core)
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
#include "least_core.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ecuio_signal_file;

namespace ecuio_sf {

/* Every file core made, by its set (signal_file_readers.cpp): a session
 * finds the file behind its set at create, while the file lives */
void keep_file(const ecuio_signal_file* f);
void forget_file(const ecuio_signal_file* f) noexcept;
const ecuio_signal_file* file_of_set(const ecuio_signal_set_t* set) noexcept;

} // namespace ecuio_sf

struct ecuio_signal_file {
    ecuio_signal_file() { ecuio_sf::keep_file(this); }
    ~ecuio_signal_file() { ecuio_sf::forget_file(this); }
    ecuio_signal_file(const ecuio_signal_file&) = delete;
    ecuio_signal_file& operator=(const ecuio_signal_file&) = delete;

    /* A channel as the "ecus" entry that names it gives it: id "" for a
     * base channel no entry names */
    struct module {
        std::string id;
        std::string name;
        uint32_t part = ECUIO_SIGNAL_PART_NONE;
        std::vector<uint32_t> session_parts; /* those asking it extended */
    };

    /* What ecuio_signal_file_note gives */
    struct note {
        uint32_t kind = 0;
        uint32_t part = ECUIO_SIGNAL_PART_NONE;
        uint32_t other_part = ECUIO_SIGNAL_PART_NONE;
        uint32_t def = UINT32_MAX;
        uint32_t count = 0;
        uint32_t base_def = UINT32_MAX;
        std::string text;
    };

    /* A part of a merged set, as its "parts" entry gives it */
    struct part {
        std::string profile;
        std::string name;
        uint32_t format = 0;
        bool has_plugin = false;
        std::string plugin_id;
        std::string plugin_version;
        std::string plugin_variant;
        ecuio_signal_source_t source{};
        bool has_fingerprint = false; /* a consent can name it */
        uint8_t fingerprint[32] = {};
    };

    std::string name;
    std::string profile;
    std::string extends; /* "obd2" or "" */
    std::vector<ecuio_signal_def_t> defs;
    std::vector<ecuio_channel_decl_t> channels;
    std::vector<module> modules; /* one for each channel */
    std::vector<ecuio_enum_entry_t> enums;
    std::vector<ecuio_computed_t> computed;
    std::vector<std::pair<std::string, uint32_t>> keys; /* key, def index */
    /* A merged set's (signal_file_merged.cpp), empty for any other file:
     * its parts, each def's part (none for a base def) and the keys a
     * signal stands for besides its own */
    std::vector<part> parts;
    bool parts_merged_here = false; /* else their prints are only text */
    std::vector<uint32_t> def_part; /* by def index */
    std::vector<std::pair<std::string, uint32_t>> second_keys; /* def index */
    std::vector<std::pair<uint32_t, ecuio_signal_gauge_t>>
        gauges; /* def index */
    std::vector<std::pair<uint32_t, std::string>> exprs; /* def index */
    uint32_t format = ECUIO_SIGNAL_FILE_FORMAT_OLDEST;
    /* The least core that reads the file's format and what it uses
     * (least_core.h), raised by build at each gate */
    ecuio_least::version least = ecuio_least::kSignalFile;
    ecuio_signal_set_t set{};
    /* The bytes a recording copies as signals.json (ADR-0051 section 4):
     * those ecuio_signal_file_load read, or a plugin variant's files
     * merged and written as one file (plugin_merge.cpp); empty when that text
     * doesn't read back to this set */
    std::string bytes;
    /* A plugin variant's "<id>,<version>,<variant>", a recording's plugin
     * line (ADR-0052 section 4); "" for a file loaded alone */
    std::string plugin;
    /* A variant's plugin's fingerprint, when the plugin names a publisher
     * (a consent can name it) */
    bool consent_fingerprint = false;
    uint8_t fingerprint[32] = {};
    /* Its "source", or a variant's own (ecuio_signal_file_source) */
    ecuio_signal_source_t source{};
    /* Its "needs" as written, and the keys of its signals left out for
     * one: in its text, not in its set */
    std::vector<std::string> needs;
    std::vector<std::string> waiting;
    std::vector<note> notes; /* what its load left for a front end */
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
    /* The most signals of its own the set holds: a data file's, or a
     * set's for several files merged and for a recording's copy */
    size_t max_signals = ECUIO_SIGNAL_FILE_MAX_SIGNALS;
    /* The keys the set's expressions read that another file provides
     * ("needs", read_needs), each with where it is written */
    std::vector<std::pair<std::string, const ecuio_json::Value*>> needs;
    /* Every signal of the list may be gone: the build of what is left
     * once the signals waiting for a need are out */
    bool may_be_empty = false;
};

/* root's "needs" into out: format 3's, a list of keys, each given once
 * (signal_file_format3.cpp) */
void read_needs(
    const ecuio_json::Value& root, uint32_t format,
    std::vector<std::pair<std::string, const ecuio_json::Value*>>* out);

/* A file's "havne_signals", checked first: its first key, a format this
 * core reads. what names the file in the words ("a signal file"). */
uint32_t format_of(const ecuio_json::Value& root, const char* what);

/* The set from in into f, or a refused thrown (std::bad_alloc too) */
void build(const parts& in, ecuio_signal_file& f);

/* A signal file's bytes into *out, read to max_bytes and max_signals:
 * ecuio_signal_file_load's inside, its statuses and detail */
ecuio_status_t load(const char* bytes, size_t length, size_t max_bytes,
                    size_t max_signals, ecuio_signal_file** out);

/* A "source" block's rules */
void read_source(const ecuio_json::Value& source);

/* A "source" read_source took, as ecuio_signal_file_source gives it */
void take_source(const ecuio_json::Value& source, ecuio_signal_source_t& out);

/* Whether a license names Creative Commons' share-alike (signals.h's
 * ECUIO_SOURCE_SHARE_ALIKE rule) */
bool share_alike(const std::string& license);

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

/* what is a view: a temporary std::string bound to a reference parameter
 * trips GCC's -Wdangling-reference where a caller keeps the result */
const std::string& text_of(const ecuio_json::Value& v, size_t lo, size_t hi,
                           std::string_view what);
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

/* A merged set by what its loader made it, though it holds no part */
inline bool is_merged(const ecuio_signal_file& f)
{
    return (f.source.flags & ECUIO_SOURCE_MERGED) != 0;
}

/* The bytes a recording copies, "" for none */
inline const std::string& bytes_of(const ecuio_signal_file& f)
{
    return f.bytes;
}

} // namespace ecuio_sf

#endif /* ECUIO_SIGNAL_FILE_BUILD_H */
