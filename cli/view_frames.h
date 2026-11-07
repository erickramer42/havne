// File/layer: cli/view_frames.h | havne-cli, view --frames (ADR-0049's appendix of 2026-11-06)
// Testing: UNIT: CliViewFramesTest (test_cli)
//
// view --frames reads a recording's values from its frames.csv through a
// profile's decode (ecuio_recording_open_frames) in place of its
// signals.csv: a raw recording then has values, and a signal file written
// since decodes an older recording. These are the rules for which set
// decodes it: the --profile given (a --signals file's or a --plugin
// variant's, once registered), else the recording's own signals.json when
// it still matches the hash events.log's header names, else the profile
// the header names when it has signals of its own.

#ifndef HAVNE_CLI_VIEW_FRAMES_H
#define HAVNE_CLI_VIEW_FRAMES_H

#include "recording_signals.h"

#include <cstdint>
#include <string>

namespace cli {

// What decodes a recording's frames: its own signals.json, a profile by
// name, or neither, with why (a recording that names none to use)
struct FramesSource {
    bool own_file = false;
    std::string profile;
    std::string why;
};

// given: --profile's name ("" with none); own_matches: the folder's
// signals.json has the hash the header names
inline FramesSource frames_source(const std::string& given,
                                  const RecordedSignals& recorded,
                                  bool own_matches)
{
    FramesSource s;
    if (!given.empty()) {
        s.profile = given;
    } else if (own_matches && recorded.names_file()) {
        s.own_file = true;
    } else if (recorded.profile.empty()) {
        s.why = "its events.log names no profile: name one with --profile";
    } else if (recorded.profile == "raw") {
        s.why = "it was recorded under raw, which decodes nothing: name a "
                "profile with --profile";
    } else if (recorded.profile == "uds-generic") {
        s.why = "uds-generic's signals come from its session's did= list: "
                "give a --signals file";
    } else {
        s.profile = recorded.profile;
    }
    return s;
}

// Why a profile can't decode frames, or "" when it can: one not
// registered (a user. or plugin. profile needs its file loaded), or one
// with no signals of its own (raw; uds-generic, whose sessions build
// theirs). defs is the set's count: never "signals", which Qt's macro
// takes, since the app includes this too
inline std::string profile_refusal(const std::string& name, bool registered,
                                   uint32_t defs)
{
    if (!registered) {
        return "no profile '" + name +
               "' is loaded: give its --signals file or --plugin";
    }
    if (defs == 0) {
        return "the profile '" + name + "' has no signals to decode with";
    }
    return "";
}

} // namespace cli

#endif /* HAVNE_CLI_VIEW_FRAMES_H */
