// File/layer: cli/profile_source.h | havne-cli, where a command's signals and
//             conditions come from: a signal file, a plugin, a recording
// Testing: UNIT: CliGoldenTest, CliSignalTest, CliReplayTest,
//          CliConditionsRunTest (ctest)
//
// Public headers only: what any program linking core can do.

#ifndef HAVNE_CLI_PROFILE_SOURCE_H
#define HAVNE_CLI_PROFILE_SOURCE_H

#include "recording_signals.h"

#include "common.h"

#include "ecuio/core.h"
#include "ecuio/plugin.h"
#include "ecuio/signals.h"
#include "ecuio/conditions.h"

namespace cmd {

extern ecuio_signal_file_t* g_signal_file;

ecuio_status_t load_signal_file(const char* path, ecuio_signal_file_t** file,
                                std::string* why, bool recorded = false);
int use_signal_file(const char* path, const char* uri, std::string* opened);
cli::RecordedSignals recorded_signals(const std::string& folder);
int use_recording_signals(const char* uri, std::string* opened);
ecuio_condition_set_t* recording_conditions(const char* uri);

ecuio_status_t load_package(const char* path, ecuio_plugin_t** out,
                            std::string* why);
ecuio_plugin_info_t plugin_info(const ecuio_plugin_t* p);
std::string variant_of(const ecuio_plugin_t* p, uint32_t index);

extern ecuio_plugin_t* g_plugin;

const ecuio_signal_file_t* session_signal_file();
ecuio_condition_set_t*
load_conditions_file(const char* path, ecuio_status_t* st, std::string* why);

// The set a conditions line shows, kept past the set's free (each info's
// key points into keys), so poll_conditions can write the line again
struct ShownConditions {
    std::string name;
    const char* source = "file";
    std::vector<std::string> keys;
    std::vector<uint8_t> link_terms; // a bit for each link term
    std::vector<ecuio_condition_info_t> infos;
    std::vector<ecuio_condition_state_t> states; // as last written
    std::vector<ecuio_condition_state_t> now;    // poll_conditions' read
};

bool attach_conditions(Output& out, ecuio_session_t* session,
                       ecuio_condition_set_t* set, const char* source,
                       ShownConditions* shown);
void poll_conditions(Output& out, ecuio_session_t* session, ShownConditions* s);
ecuio_status_t register_variants(ecuio_plugin_t* p, std::string* why);
int use_plugin(const char* path, const char* uri);
int take_profile_source(const char* signals, const char* plugin,
                        const char** uri, std::string* opened);

} // namespace cmd

#endif // HAVNE_CLI_PROFILE_SOURCE_H
