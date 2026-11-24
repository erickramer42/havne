// File/layer: cli/session_lines.h | havne-cli, what the commands with a session
//             share: the connect, the session's lines, the recording's
// Testing: UNIT: CliGoldenTest, CliConnectTest, CliReplayTest,
//          CliRecordTest, CliLogRunTest, CliRefusedRunTest (ctest)
//
// Public headers only: what any program linking core can do.

#ifndef HAVNE_CLI_SESSION_LINES_H
#define HAVNE_CLI_SESSION_LINES_H

#include "timing_line.h"
#include "jsonl.h"
#include "refused_signals.h"
#include "scan_line.h"

#include "common.h"

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

namespace cmd {

// How long a close may hold up an interrupted --record before the
// recorder closes without it (ADR-0049 section 1)
constexpr auto kRecordingCloseWait = std::chrono::seconds(2);

ecuio_status_t create_within(Output& out, const char* uri,
                             ecuio_session_t** session, std::string* detail,
                             bool start_first = false);
// create_within, then the record need_consent found applied between create
// and start (cmd_consent.cpp; none: as create_within); a refused apply
// destroys the session, *detail its words
ecuio_status_t create_consented(Output& out, const char* uri,
                                ecuio_session_t** session, std::string* detail,
                                bool start_first = false);
// The consent a run applied (create_consented): the session line's
// "consent" key, absent when none was
struct SessionConsent {
    bool applied = false;
    uint8_t fingerprint[32] = {};
    uint64_t given_utc = 0;
};
extern SessionConsent g_session_consent;

void write_session(Output& out, const ecuio_session_t* session,
                   const ecuio_session_info_t& info);
/* The session's modules, every one: what the per-channel arrays
 * (ECUIO_MAX_CHANNELS each) and the reports' loops run to */
inline uint32_t module_count(const ecuio_session_info_t& info)
{
    return info.module_count < ECUIO_MAX_CHANNELS ? info.module_count
                                                  : ECUIO_MAX_CHANNELS;
}
uint32_t latest_model_year();
void poll_identity(Output& out, ecuio_session_t* session, uint32_t channels,
                   uint32_t* generations, char (&vehicle)[ECUIO_VIN_TEXT_LEN]);
void poll_scan(Output& out, ecuio_session_t* session, uint32_t channels,
               cli::ScanSeen* seen, cli::SessionSeen* sessions,
               ecuio_channel_support_t* reports, bool final = false);
void poll_sessions(Output& out, ecuio_session_t* session, uint32_t channels,
                   cli::SessionSeen* seen);
void poll_diagnostics(Output& out, ecuio_session_t* session, uint32_t channels,
                      uint32_t* generations);
void poll_timing(Output& out, ecuio_session_t* session,
                 cli::TimingSnapshots& snaps,
                 std::vector<ecuio_request_timing_t>& rows, bool final);
void write_isotp(cli::Line& l, const ecuio_session_t* session);
void poll_absent(Output& out, ecuio_session_t* session,
                 std::vector<uint16_t>& read, std::vector<uint16_t>& last);
void poll_refused(Output& out, const ecuio_signal_set_t* set,
                  const ecuio_channel_support_t* reports, uint32_t channels,
                  std::vector<cli::RefusedSignal>& last,
                  std::vector<cli::RefusedMore>& last_more);
void write_signals(Output& out, const ecuio_signal_set_t* set,
                   const ecuio_signal_file_t* file);

// The markers typed into stream --record (ADR-0049 section 4): a thread
// of its own reads standard input through the OS, never stdio, whose
// lock a read waiting on a console would hold through exit's flush, and
// queues each line; the main loop marks them, so the recorder has one
// caller. Detached, since a console's read never ends: exit takes it.
struct MarkQueue {
    std::mutex m;
    std::vector<std::string> lines;
};

bool open_recording(Output& out, ecuio_session_t* session, const char* root,
                    const char* label, ecuio_recorder_t** recorder,
                    bool armed = false);
void poll_recording(Output& out, ecuio_recorder_t* recorder, MarkQueue* marks,
                    bool* failed);
void close_recording(Output& out, ecuio_recorder_t** recorder, const char* why,
                     bool* failed, bool armed = false);
void poll_health(Output& out, ecuio_session_t* session, int32_t* last_error,
                 uint32_t* episodes);
bool session_asks(ecuio_session_t* session);

// Whether a session is asked only what is picked: no focus asks nothing
bool session_pick_only(ecuio_session_t* session);

// Whether a session reads a vehicle (an adapter or a CAN interface): only
// such a session holds the host awake, and only its session line says so
bool session_live(const ecuio_session_info_t& info);

// stream's and log's session line, after their hold on the host's idle
// sleep (ADR-0033's appendix "the host kept awake while a session
// reads"): a live session holds unless --allow-sleep
void write_held_session(Output& out, ecuio_session_t* session,
                        const ecuio_session_info_t& info, bool allow_sleep);
bool replay_done(const ecuio_session_t* session,
                 const ecuio_session_info_t& info, uint64_t covered);

} // namespace cmd

#endif // HAVNE_CLI_SESSION_LINES_H
