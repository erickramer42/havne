// File/layer: cli/commands.h | havne-cli, each command's arguments and its run, and
//             what one command's file shows another
// Testing: UNIT: CliGoldenTest, CliUsageTest (ctest)
//
// Public headers only: what any program linking core can do.

#ifndef HAVNE_CLI_COMMANDS_H
#define HAVNE_CLI_COMMANDS_H

#include "condition_lines.h"
#include "frames_lines.h"
#include "health_lines.h"
#include "log_lines.h"
#include "transfer_lines.h"
#include "view_lines.h"
#include "plugin_lines.h"
#include "elm327_lines.h"

#include "common.h"

#include "ecuio/core.h"
#include "ecuio/obdb.h"
#include "ecuio/recording.h"
#include "ecuio/socketcan.h"

namespace cmd {

// usage.cpp
void usage(FILE* to);

// cmd_stream.cpp
struct StreamArgs {
    const char* uri = nullptr;
    bool values = false;
    uint64_t count = 0;
    uint64_t seconds = 0;
    bool until_scanned = false;
    const char* record = nullptr; // --record's folder
    const char* label = nullptr;
    const char* signals = nullptr; // --signals' file
    const char* plugin = nullptr;  // --plugin's package
    const char* conditions = nullptr; // --conditions' file
    bool pick_ranked = false;         // --pick ranked
};

int run_stream(const StreamArgs& args);
int cmd_stream(int argc, char** argv);

// cmd_log.cpp
int run_log(const cli::LogArgs& args);
int cmd_log(int argc, char** argv);

// cmd_view.cpp
void write_signal_line(Output& out, const ecuio_recording_t* r,
                       uint32_t recording, uint32_t index,
                       const ecuio_recording_signal_t& s);
void write_condition_lines(Output& out, const ecuio_recording_t* r,
                           uint32_t recording,
                           const ecuio_recording_info_t& info);

struct ViewArgs {
    std::vector<const char*> folders;
    std::vector<std::string> signals; // --signal, in order; none: the ranked
    bool has_from = false;
    bool has_to = false;
    bool has_at = false;
    int64_t from_us = 0;
    int64_t to_us = 0;
    int64_t at_us = 0;
    std::vector<cli::Span> spans; // --span, in order
    // --stretches: the rules (--stretch-level's, in km/h), and --role's
    // names, each a role and a signal every recording holds as a number
    bool stretches = false;
    ecuio_stretch_rules_t rules = cli::stretch_rules(0);
    std::vector<std::pair<uint32_t, std::string>> roles;
    uint32_t columns = 100;
    const char* align_text = nullptr;
    cli::AlignOn align;
    double hysteresis = 0;
    bool has_hysteresis = false;
    // --frames: the values from frames.csv through a profile's decode
    bool frames = false;
    std::string profile;                // --profile; "" for the default
    const char* signals_path = nullptr; // --signals
    const char* plugin_path = nullptr;  // --plugin
};

struct RecordingCloser {
    void operator()(ecuio_recording_t* r) const { ecuio_recording_close(r); }
};

int run_view(ViewArgs& args);

// cmd_view_args.cpp
int cmd_view(int argc, char** argv);

// cmd_info.cpp
struct InfoArgs {
    const char* folder = nullptr;
    ecuio_check_options_t options{};
};

int run_info(const InfoArgs& args);
int cmd_info(int argc, char** argv);

// cmd_plugins.cpp
int run_plugins_list(const std::vector<const char*>& paths);
int run_plugins_match(const char* path, const char* typed);
int run_plugins_check(const char* path, const char* on);

// A packed plugin's text one of plugin.h's calls gave (freed here), or
// its refusal: the status and core's words. An OK's words are its notes,
// which ECUs took another id: read right after the call
struct MadeText {
    ecuio_status_t status = ECUIO_OK;
    std::string text;
    std::string words;
};

MadeText made_text(ecuio_status_t status, char* text, size_t length);
bool write_made_file(Output& out, const char* path, const MadeText& m);
void write_made_lines(Output& out, const MadeText& m, cli::Written action,
                      const std::string& variant);

// plugins wrap, put and drop (ADR-0064 Q9): what each reads and names
struct PluginMakeArgs {
    const char* package = nullptr; // put, drop: the packed plugin read
    const char* file = nullptr;    // wrap, put: the signal file read
    const char* out = nullptr;     // --out: the packed plugin written
    const char* id = nullptr;      // wrap's --id
    const char* variant = nullptr; // --variant
};

int run_plugins_wrap(const PluginMakeArgs& args);
int run_plugins_put(const PluginMakeArgs& args);
int run_plugins_drop(const PluginMakeArgs& args);
bool parse_plugin_make(const std::string& word, int first, int argc,
                       char** argv, PluginMakeArgs* args, std::string* why);

// cmd_signals.cpp
int run_signals_check(const char* path);

struct ImportArgs {
    const char* file = nullptr; // the OBDb set
    const char* out = nullptr;  // --out: the signal file, or plugin, written
    ecuio_obdb_options_t options{};
    bool plugin = false;        // --plugin: the converted file wrapped
    std::string wmi;            // --wmi: the match's codes, upper case
    uint32_t years[2] = {0, 0}; // --years: the match's model years
};

int run_signals_import(const ImportArgs& args);
int run_signals_expr(int argc, char** argv);
int cmd_signals(int argc, char** argv);

// cmd_check.cpp
int run_conditions_check(const cli::ConditionsCheckArgs& args);
int run_layout_check(const char* path, const std::string& form);
int cmd_conditions(int argc, char** argv);
int cmd_layout(int argc, char** argv);

// cmd_transfer.cpp
ecuio_status_t guess_map(const cli::ImportArgs& im, std::string* text,
                         ecuio_csv_guess_t* guess, std::string* why);
int print_guessed_map(const cli::ImportArgs& im);
bool missing_columns(const cli::ExportArgs& ex, bool* asked,
                     std::vector<const char*>* missing);
int run_frames(const cli::FramesArgs& args);
int cmd_frames(int argc, char** argv);
int cmd_export(int argc, char** argv);
int cmd_import(int argc, char** argv);

// cmd_health.cpp
int run_health_recording(cli::HealthArgs& a);
int run_health_session(cli::HealthArgs& a, const char* uri);
int cmd_health(int argc, char** argv);

// cmd_sim.cpp
struct SimArgs {
    const char* uri = nullptr;
    const char* on = nullptr;
    uint64_t seconds = 0;
    bool desk_bus = false; /* --desk-bus: a real controller too */
    const char* signals = nullptr; /* --signals' file */
    const char* plugin = nullptr;  /* --plugin's package */
    const char* elm327 = nullptr;  /* --elm327's tcp:[<address>:]<port> */
    cli::Elm327Address elm327_at;
};

int run_sim_elm327(const SimArgs& args);
int run_sim(const SimArgs& args);
int cmd_sim(int argc, char** argv);

// cmd_play.cpp
#if HAVNE_CLI_PLAY
struct PlayArgs {
    const char* uri = nullptr;
    const char* to = nullptr;
    uint64_t count = 0;
    uint64_t seconds = 0;
};

int run_play(const PlayArgs& args);
#endif
int cmd_play(int argc, char** argv);

// cmd_small.cpp
std::vector<ecuio_can_interface_t> can_interfaces();
int run_clear_command(const char* uri, uint32_t flags);
int run_vin(const char* typed);
int run_dtc(const char* typed);
int run_devices();
int cmd_clear(int argc, char** argv);

} // namespace cmd

#endif // HAVNE_CLI_COMMANDS_H
