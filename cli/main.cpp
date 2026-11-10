// File/layer: cli/main.cpp | havne-cli, a command-line client over the C ABI (ADR-0031)
// Testing: UNIT: CliGoldenTest, CliUsageTest, CliConnectTest, CliSignalTest,
//          CliReplayTest, CliRecordTest, CliViewRunTest, CliPathTest,
//          CliTransferRunTest, CliExitTest, CliObdbImportTest (ctest);
//          CliTimingTest, CliVinTest, CliSimTest, CliMarkLinesTest,
//          CliViewTest, CliInfoTest, CliSignalFileTest, CliHealthTest,
//          CliPartialTest
//          (test_cli); BENCH
//
// Public headers only: what any program linking core can do.

#include "can_line.h"
#include "clear_run.h"
#include "diagnostics_line.h"
#include "elm327_listener.h"
#include "frames_lines.h"
#include "health_lines.h"
#include "info_lines.h"
#include "log_lines.h"
#include "timing_line.h"
#include "transfer_lines.h"
#include "view_frames.h"
#include "view_lines.h"
#include "vin_line.h"
#include "jsonl.h"
#include "line_ring.h"
#include "mark_lines.h"
#include "partial_file.h"
#include "plugin_lines.h"
#include "recording_signals.h"
#include "refused_signals.h"
#include "scan_line.h"
#include "signal_file_lines.h"
#include "sim_line.h"
#include "sleep_watch.h"

#include "ecuio/core.h"
#include "ecuio/j2534.h"
#include "ecuio/obdb.h"
#include "ecuio/plugin.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/serial.h"
#include "ecuio/signals.h"
#include "ecuio/sim.h"
#include "ecuio/socketcan.h"
#include "ecuio/vin.h"

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h> /* stat: a plugin's folder */
#include <unistd.h>   /* read: the marks typed into stream --record */
#endif

// play writes to a SocketCAN interface: Linux only, as socketcan:// is
#if defined(__linux__) && !defined(__ANDROID__)
#define HAVNE_CLI_PLAY 1
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#define HAVNE_CLI_PLAY 0
#endif

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitConnect = 3;

constexpr int kSchema = 1;
constexpr size_t kRingBytes = size_t{1} << 20;
// A frame line at ECUIO_FRAME_DATA_MAX bytes takes about 12.4 KB; a
// values line with text values can take more, and is cut to fit
constexpr size_t kLineBytes = 32 * 1024;
constexpr auto kPoll = std::chrono::milliseconds(100);
// How long a close may hold up an interrupted --record before the
// recorder closes without it (ADR-0049 section 1)
constexpr auto kRecordingCloseWait = std::chrono::seconds(2);
// How long clear waits for each step: the reports read, each clear's
// outcome, the read after it
constexpr auto kClearWait = std::chrono::seconds(30);

// How often sim writes its counts, when they changed
constexpr uint64_t kSimStatsEveryUs = 10u * 1000 * 1000;

// The ring's size. HAVNE_CLI_RING_BYTES (tests only) makes it small, so
// a golden run fills it: a replay must then wait, not drop. Never below
// one whole line.
size_t ring_bytes()
{
    char env[32] = {};
#ifdef _WIN32
    const DWORD n =
        GetEnvironmentVariableA("HAVNE_CLI_RING_BYTES", env, sizeof env);
    if (n == 0 || n >= sizeof env) {
        return kRingBytes;
    }
#else
    const char* value = std::getenv("HAVNE_CLI_RING_BYTES");
    if (!value || std::strlen(value) >= sizeof env) {
        return kRingBytes;
    }
    std::memcpy(env, value, std::strlen(value));
#endif
    if (!*env) {
        return kRingBytes;
    }
    const unsigned long long v = std::strtoull(env, nullptr, 10);
    const size_t floor = kLineBytes + 256;
    return v < floor        ? floor
           : v > kRingBytes ? kRingBytes
                            : static_cast<size_t>(v);
}

// How long a connect may take. Each of core's waits on the way has a
// bound of its own, the longest the surrogate's handshake and its
// OP_OPEN (8 s each); a stuck spawn held one connect past all of them
// (an antivirus holding a newly built surrogate; ADR-0031's appendix of
// 2026-11-01), so the CLI keeps a deadline of its own. HAVNE_CLI_CONNECT_MS (tests only)
// shortens it.
constexpr unsigned long kConnectMs = 60 * 1000;

// A test knob in milliseconds: the environment's value when it is 100
// or more, else the fallback
unsigned long env_ms(const char* name, unsigned long fallback)
{
    char env[32] = {};
#ifdef _WIN32
    const DWORD n = GetEnvironmentVariableA(name, env, sizeof env);
    if (n == 0 || n >= sizeof env) {
        return fallback;
    }
#else
    const char* value = std::getenv(name);
    if (!value || std::strlen(value) >= sizeof env) {
        return fallback;
    }
    std::memcpy(env, value, std::strlen(value));
#endif
    const unsigned long v = std::strtoul(env, nullptr, 10);
    return v >= 100 ? v : fallback;
}

unsigned long connect_ms()
{
    return env_ms("HAVNE_CLI_CONNECT_MS", kConnectMs);
}

// The log line's period (cli::kLogEveryUs); HAVNE_CLI_LOG_EVERY_MS (tests
// only) shortens it
uint64_t log_every_us()
{
    return env_ms("HAVNE_CLI_LOG_EVERY_MS",
                  static_cast<unsigned long>(cli::kLogEveryUs / 1000)) *
           1000ULL;
}

std::atomic<bool> g_interrupted{false};

#ifdef _WIN32
std::atomic<bool> g_done{false};

BOOL WINAPI on_console_event(DWORD event)
{
    g_interrupted.store(true);
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        return TRUE;
    }
    // A closed console ends the process when this returns: give the
    // main thread its close first (Windows allows about 5 s)
    for (int i = 0; i < 40 && !g_done.load(); ++i) {
        Sleep(100);
    }
    return TRUE;
}
#else
void on_signal(int)
{
    g_interrupted.store(true);
}
#endif

void install_handlers()
{
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, TRUE);
#else
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    // A closed pipe is a failed write (EPIPE), which ends the stream
    signal(SIGPIPE, SIG_IGN);
#endif
}

// Standard output, JSON lines only. A failure is remembered: a closed
// pipe (EPIPE) ends the stream quietly, anything else is an error.
struct Output {
    bool failed = false;
    bool closed = false;
    int error = 0;

    // False after a failure that wasn't a closed pipe, which it reports
    bool ok() const
    {
        if (failed && !closed) {
            std::fprintf(stderr, "havne-cli: writing the output failed: %s\n",
                         std::generic_category().message(error).c_str());
            return false;
        }
        return true;
    }

    void write(const char* p, size_t n)
    {
        if (failed || n == 0) {
            return;
        }
        if (std::fwrite(p, 1, n, stdout) != n) {
            fail();
        }
    }
    void flush()
    {
        if (!failed && std::fflush(stdout) != 0) {
            fail();
        }
    }

private:
    void fail()
    {
        failed = true;
        error = errno;
        closed = errno == EPIPE;
#ifdef _WIN32
        // A pipe whose reader has gone fails WriteFile with ERROR_NO_DATA
        // (Git Bash's head, PowerShell's Select-Object -First), which the
        // CRT reports as EINVAL; ERROR_BROKEN_PIPE is EPIPE already
        const DWORD last = GetLastError();
        closed = closed || last == ERROR_NO_DATA || last == ERROR_BROKEN_PIPE;
#endif
    }
};

// A line built on the main thread, which may be long (a set's defs)
struct BigLine {
    explicit BigLine(size_t cap) : buf(cap), line(buf.data(), buf.size()) {}
    std::vector<char> buf;
    cli::Line line;
};

// The CSV's column map guessed by core (ecuio_import_csv_guess): its whole
// text, asked its size first, then read; a file grown between the two
// calls is read again, a few times at most. A failure's words in *why
ecuio_status_t guess_map(const cli::ImportArgs& im, std::string* text,
                         ecuio_csv_guess_t* guess, std::string* why)
{
    size_t needed = 0;
    ecuio_status_t rc =
        ecuio_import_csv_guess(im.in, im.time_unit, nullptr, 0, &needed, guess);
    for (int tries = 0; rc == ECUIO_OK && tries < 4; ++tries) {
        std::vector<char> buf(needed);
        size_t now = 0;
        rc = ecuio_import_csv_guess(im.in, im.time_unit, buf.data(), buf.size(),
                                    &now, guess);
        if (rc == ECUIO_OK && now <= buf.size()) {
            text->assign(buf.data(), now - 1);
            return ECUIO_OK;
        }
        needed = now;
    }
    if (rc == ECUIO_OK) {
        *why = "the file changed while it was read";
        return ECUIO_ERR_LOAD_FAILED;
    }
    *why = ecuio_last_error_detail();
    return rc;
}

// `import csv <file> --print-map`: the guessed map's JSON alone on stdout
// (core's slice, byte for byte, and a line end), to save and pass with
// --map; no lines. A refusal is stderr's, exit 3; a map whose time has no
// unit is printed with a stderr line saying so, for its unit to be added
int print_guessed_map(const cli::ImportArgs& im)
{
    std::string text;
    std::string why;
    ecuio_csv_guess_t guess{};
    if (guess_map(im, &text, &guess, &why) != ECUIO_OK) {
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        return kExitConnect;
    }
    const std::string map = text.substr(guess.map_offset, guess.map_length);
    if (std::fwrite(map.data(), 1, map.size(), stdout) != map.size() ||
        std::fputc('\n', stdout) == EOF || std::fflush(stdout) != 0) {
        return 1;
    }
    if ((guess.flags & ECUIO_CSV_GUESS_TIME_UNIT_KNOWN) == 0) {
        std::fprintf(stderr, "%s",
                     (guess.flags & ECUIO_CSV_GUESS_TIME_NUMBERS) != 0
                         ? "havne-cli: the map's time has no unit: add one, "
                           "or give --time-unit s, ms or us\n"
                         : "havne-cli: the map's time has no unit: its "
                           "cells need one with the zone (or the date and "
                           "offset)\n");
    }
    return kExitOk;
}

void write_line(Output& out, cli::Line& l)
{
    if (l.finish()) {
        out.write(l.data(), l.size());
    }
}

// A command's last word: its output flushed, then its exit, or 1 when a
// write failed (a closed pipe stays quiet, as ok() says). Every return
// after the output began goes through it, so a write that fails only at
// the flush is never a quiet 0
int ended(Output& out, int code)
{
    out.flush();
    return out.ok() ? code : kExitFailed;
}

// export's cancel (ecuio_export_with), its column check's and import's
// (ecuio_import_with): Ctrl+C or SIGTERM stops it, and core removes what
// it had begun. HAVNE_CLI_EXPORT_HOLD_MS (tests only) holds the run's
// first ask that long, an import's too, naming whose it is on stderr, so
// a test's interrupt lands there
int interrupted_at(bool* asked, const char* where)
{
    if (!*asked) {
        *asked = true;
        const uint64_t hold_ms = env_ms("HAVNE_CLI_EXPORT_HOLD_MS", 0);
        if (hold_ms > 0) {
            std::fprintf(stderr, "havne-cli: holding the %s's first ask\n",
                         where);
            std::fflush(stderr);
        }
        const uint64_t until = ecuio_clock_us() + hold_ms * 1000ULL;
        while (!g_interrupted.load() && ecuio_clock_us() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return g_interrupted.load() ? 1 : 0;
}

int export_interrupted(void* user)
{
    return interrupted_at(static_cast<bool*>(user), "export");
}

int import_interrupted(void* user)
{
    return interrupted_at(static_cast<bool*>(user), "import");
}

// The column check's open (ecuio_open_progress_t) asks as the export does
int open_interrupted(uint64_t, uint64_t, void* user)
{
    return interrupted_at(static_cast<bool*>(user), "column check");
}

// An export, its sizing or an import stopped by an interrupt: the end
// line the other commands write on one, and exit 1, as clear's, since
// nothing was written
int ended_interrupted(Output& out)
{
    char buf[64];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"end\"");
    l.key("reason");
    l.string("interrupted", SIZE_MAX);
    l.ch('}');
    write_line(out, l);
    return ended(out, kExitFailed);
}

// A recording's signal line (view's and info's), an enum's labels on it
void write_signal_line(Output& out, const ecuio_recording_t* r,
                       uint32_t recording, uint32_t index,
                       const ecuio_recording_signal_t& s)
{
    uint32_t count = 0;
    std::vector<ecuio_enum_entry_t> labels;
    if (ecuio_recording_get_labels(r, index, nullptr, 0, &count) == ECUIO_OK &&
        count > 0) {
        labels.resize(count);
        ecuio_recording_get_labels(r, index, labels.data(), count, &count);
    }
    BigLine b(2048 + cli::labels_bytes(count));
    cli::signal_line(b.line, recording, index, s, labels.data(), count);
    write_line(out, b.line);
}

void write_start(Output& out)
{
    char buf[256];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"start\"");
    l.key("schema");
    l.u64(kSchema);
    l.key("core");
    l.string(ecuio_version_string(), SIZE_MAX);
    l.ch('}');
    write_line(out, l);
}

// An error line, with core's detail for the last connect unless given
// one of the CLI's own
void write_error(Output& out, ecuio_status_t status,
                 const char* detail = nullptr);

// ecuio_session_create within connect_ms(): on a thread of its own, the
// detail carried back from it (it's per thread). Past the deadline, the
// error line (after the start line, when the caller hasn't written it
// yet: start_first) and exit 3 at once; the process's end ends the
// surrogate with it (its job object), wherever the connect was held.
ecuio_status_t create_within(Output& out, const char* uri,
                             ecuio_session_t** session, std::string* detail,
                             bool start_first = false)
{
    struct Connect {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        ecuio_status_t status = ECUIO_ERR_TRANSPORT_HARD_FAILURE;
        ecuio_session_t* session = nullptr;
        std::string detail;
        std::string uri;
    };
    auto c = std::make_shared<Connect>();
    c->uri = uri;
    std::thread([c] {
        ecuio_session_t* s = nullptr;
        const ecuio_status_t st = ecuio_session_create(c->uri.c_str(), &s);
        const std::string why = ecuio_last_error_detail();
        std::lock_guard<std::mutex> lock(c->mutex);
        c->status = st;
        c->session = s;
        c->detail = why;
        c->done = true;
        c->cv.notify_all();
    }).detach();
    const unsigned long ms = connect_ms();
    std::unique_lock<std::mutex> lock(c->mutex);
    if (!c->cv.wait_for(lock, std::chrono::milliseconds(ms),
                        [&] { return c->done; })) {
        char why[96];
        std::snprintf(why, sizeof why, "the connect didn't finish within %lu s",
                      (ms + 999) / 1000);
        if (start_first) {
            write_start(out);
        }
        write_error(out, ECUIO_ERR_TRANSPORT_HARD_FAILURE, why);
        out.flush();
        std::fflush(stderr);
        std::_Exit(kExitConnect);
    }
    *session = c->session;
    *detail = c->detail;
    return c->status;
}

void write_error(Output& out, ecuio_status_t status, const char* detail)
{
    if (detail == nullptr) {
        detail = ecuio_last_error_detail();
    }
    BigLine b(1024 + std::strlen(detail) * 6);
    cli::Line& l = b.line;
    l.raw("{\"type\":\"error\"");
    l.key("code");
    l.u64(static_cast<uint32_t>(status));
    l.key("message");
    l.string(ecuio_status_string(status), SIZE_MAX);
    l.key("detail");
    l.string(detail, SIZE_MAX);
    l.ch('}');
    write_line(out, l);
    std::fprintf(stderr, "havne-cli: %s%s%s\n", ecuio_status_string(status),
                 *detail ? ": " : "", detail);
}

const char* backend_name(uint32_t backend)
{
    switch (backend) {
    case ECUIO_BACKEND_MOCK:
        return "mock";
    case ECUIO_BACKEND_J2534_SURROGATE:
        return "j2534";
    case ECUIO_BACKEND_REPLAY:
        return "replay";
    case ECUIO_BACKEND_ELM327:
        return "elm327";
    case ECUIO_BACKEND_SOCKETCAN:
        return "socketcan";
    default:
        return "unknown";
    }
}

void write_session(Output& out, const ecuio_session_info_t& info)
{
    char buf[2048];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"session\"");
    l.key("backend");
    l.string(backend_name(info.backend), SIZE_MAX);
    l.key("profile");
    l.string(info.profile, sizeof info.profile);
    l.key("protocol_id");
    l.u64(info.protocol_id);
    l.key("baud");
    l.u64(info.baud);
    l.key("channels");
    l.ch('[');
    const uint32_t channels =
        info.channel_count < ECUIO_SESSION_INFO_MAX_CHANNELS
            ? info.channel_count
            : ECUIO_SESSION_INFO_MAX_CHANNELS;
    for (uint32_t i = 0; i < channels; ++i) {
        if (i) {
            l.ch(',');
        }
        l.ch('{');
        l.key("rx");
        l.hex_id(info.rx_ids[i]);
        l.key("tx");
        l.hex_id(info.tx_ids[i]);
        l.ch('}');
    }
    l.ch(']');
    if (info.flags & ECUIO_SESSION_INFO_VBATT) {
        l.key("vbatt_mv");
        l.u64(info.vbatt_mv);
    }
    if (info.flags & ECUIO_SESSION_INFO_VERSIONS) {
        l.key("firmware");
        l.string(info.firmware_version, sizeof info.firmware_version);
        l.key("dll_version");
        l.string(info.dll_version, sizeof info.dll_version);
        l.key("api_version");
        l.string(info.api_version, sizeof info.api_version);
    }
    if (info.flags & ECUIO_SESSION_INFO_PENDING_OFF) {
        l.key("pending_off");
        l.raw("true");
    }
    if (info.flags & ECUIO_SESSION_INFO_LISTEN_ONLY) {
        l.key("listen_only");
        l.raw("true");
    }
    if (info.backend == ECUIO_BACKEND_REPLAY) {
        l.key("replay");
        l.ch('{');
        l.key("frames");
        l.u64(info.replay_frames);
        l.key("span_ms");
        l.u64(info.replay_span_ms);
        l.key("speed_milli");
        l.u64(info.replay_speed_milli);
        l.key("loop");
        l.raw((info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP) ? "true" : "false");
        if (info.flags & ECUIO_SESSION_INFO_REPLAY_FAULTS) {
            l.key("faults");
            l.raw("true");
        }
        if (info.replay_rows_torn != 0) {
            l.key("rows_torn");
            l.u64(info.replay_rows_torn);
        }
        l.ch('}');
    }
    l.ch('}');
    write_line(out, l);
}

// One ECU's diagnostics once a read has completed (diagnostics_line.h)
void write_diagnostics(Output& out, const ecuio_channel_diagnostics_t& d)
{
    char buf[8192];
    cli::Line l(buf, sizeof buf);
    cli::diagnostics_line(l, d);
    write_line(out, l);
}

// The latest model year a VIN can carry (ADR-0043): this year plus one,
// the bridge's bound too
uint32_t latest_model_year()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &now) != 0) {
        return 0;
    }
#else
    if (gmtime_r(&now, &utc) == nullptr) {
        return 0;
    }
#endif
    return static_cast<uint32_t>(utc.tm_year + 1900 + 1);
}

// A line for each ECU whose identity has settled since the last call
// (vin_line.h): generations holds each channel's last written
void poll_identity(Output& out, ecuio_session_t* session, uint32_t channels,
                   uint32_t* generations)
{
    for (uint32_t c = 0; c < channels; ++c) {
        ecuio_channel_identity_t id;
        if (ecuio_session_get_channel_identity(session, c, &id) == ECUIO_OK &&
            (id.flags & ECUIO_IDENTITY_COMPLETE) &&
            id.generation != generations[c]) {
            generations[c] = id.generation;
            char buf[4096];
            cli::Line l(buf, sizeof buf);
            cli::identity_line(l, id, latest_model_year());
            write_line(out, l);
        }
    }
}

// A session line per ECU whose session changed and a scan line per ECU
// whose DID scan moved (cli/scan_line.h), every change when final; the
// channels' reports into reports[], for --until-scanned
void poll_scan(Output& out, ecuio_session_t* session, uint32_t channels,
               cli::ScanSeen* seen, cli::SessionSeen* sessions,
               ecuio_channel_support_t* reports, bool final = false)
{
    const uint64_t now = ecuio_clock_us();
    for (uint32_t c = 0; c < channels; ++c) {
        ecuio_channel_support_t& s = reports[c];
        if (ecuio_session_get_channel_support(session, c, &s) != ECUIO_OK) {
            s = ecuio_channel_support_t{};
            continue;
        }
        if (cli::session_due(s, sessions[c])) {
            char buf[512];
            cli::Line l(buf, sizeof buf);
            cli::session_line(l, s);
            write_line(out, l);
        }
        if (cli::scan_due(s, seen[c], now, final)) {
            char buf[512];
            cli::Line l(buf, sizeof buf);
            cli::scan_line(l, s);
            write_line(out, l);
        }
    }
}

// The car's model year, from the first channel's VIN (the vehicle's,
// ADR-0027 section 3) decoded as the identity line decodes it; 0 with
// none
uint32_t session_model_year(const ecuio_session_t* session)
{
    ecuio_channel_identity_t id;
    char vin[ECUIO_VIN_TEXT_LEN];
    ecuio_vin_info_t v;
    if (ecuio_session_get_channel_identity(session, 0, &id) != ECUIO_OK ||
        ecuio_identity_vin(&id, vin, sizeof vin) != ECUIO_OK ||
        vin[0] == '\0' ||
        ecuio_vin_decode(vin, latest_model_year(), &v) != ECUIO_OK) {
        return 0;
    }
    return v.model_year;
}

// A line for each ECU whose diagnostics read has completed since the
// last call: generations holds each channel's last written. After any,
// once no read is still under way, the car's inspection line
// (ADR-0045) over every channel's report
void poll_diagnostics(Output& out, ecuio_session_t* session, uint32_t channels,
                      uint32_t* generations)
{
    std::vector<ecuio_channel_diagnostics_t> reports(channels);
    bool wrote = false;
    for (uint32_t c = 0; c < channels; ++c) {
        ecuio_channel_diagnostics_t& d = reports[c];
        if (ecuio_session_get_channel_diagnostics(session, c, &d) != ECUIO_OK) {
            d = ecuio_channel_diagnostics_t{};
            continue;
        }
        if ((d.flags & ECUIO_DIAG_COMPLETE) && d.generation != generations[c]) {
            generations[c] = d.generation;
            write_diagnostics(out, d);
            wrote = true;
        }
    }
    if (!wrote) {
        return;
    }
    const uint32_t year = session_model_year(session);
    ecuio_inspection_t i;
    if (ecuio_diag_inspect(reports.data(), channels, year, &i) == ECUIO_OK &&
        i.verdict != ECUIO_INSPECTION_READING &&
        i.verdict != ECUIO_INSPECTION_UNKNOWN) {
        char buf[1024];
        cli::Line l(buf, sizeof buf);
        cli::inspection_line(l, i, year);
        write_line(out, l);
    }
}

// Reply timing's snapshots (ADR-0042 section 3): every 10 s of session
// time, and once at the end (final), a timing line for each request whose
// counts changed since the last; rows is sized before the session starts
void poll_timing(Output& out, ecuio_session_t* session,
                 const ecuio_session_info_t& info, cli::TimingSnapshots& snaps,
                 std::vector<ecuio_request_timing_t>& rows, bool final)
{
    ecuio_session_timing_t t;
    uint32_t count = 0;
    if (ecuio_session_get_timing(session, &t, nullptr, 0, &count) != ECUIO_OK ||
        count == 0 || (!final && !snaps.due(t.snapshot_us)) ||
        ecuio_session_get_timing(session, &t, rows.data(),
                                 static_cast<uint32_t>(rows.size()),
                                 &count) != ECUIO_OK) {
        return;
    }
    snaps.taken(t.snapshot_us);
    const uint32_t n =
        count < rows.size() ? count : static_cast<uint32_t>(rows.size());
    for (uint32_t i = 0; i < n; ++i) {
        if (!snaps.changed(i, rows[i])) {
            continue;
        }
        const uint32_t rx = rows[i].channel < ECUIO_SESSION_INFO_MAX_CHANNELS
                                ? info.rx_ids[rows[i].channel]
                                : 0;
        char buf[1024];
        cli::Line l(buf, sizeof buf);
        cli::timing_line(l, t.snapshot_us, rx, rows[i], t.interval_clock);
        write_line(out, l);
    }
}

// The end line's ISO-TP counts, where core reassembles (ADR-0040 section 5)
void write_isotp(cli::Line& l, const ecuio_session_t* session)
{
    ecuio_session_timing_t t;
    uint32_t count = 0;
    if (ecuio_session_get_timing(session, &t, nullptr, 0, &count) != ECUIO_OK ||
        (t.flags & ECUIO_TIMING_ISOTP) == 0) {
        return;
    }
    l.key("isotp");
    l.ch('{');
    l.key("first_frames");
    l.u64(t.isotp_first_frames);
    l.key("completed");
    l.u64(t.isotp_completed);
    l.key("dropped");
    l.u64(t.isotp_dropped);
    l.key("malformed");
    l.u64(t.isotp_malformed);
    l.ch('}');
}

// The signals the session's frames come without (ADR-0039 section 6), as ids
// the signals line names, whenever core's list changes; last holds the
// list last written, read sized to the set
void poll_absent(Output& out, ecuio_session_t* session,
                 std::vector<uint16_t>& read, std::vector<uint16_t>& last)
{
    uint32_t count = 0;
    if (read.empty() ||
        ecuio_session_get_absent_signals(session, read.data(),
                                         static_cast<uint32_t>(read.size()),
                                         &count) != ECUIO_OK) {
        return;
    }
    count = std::min<uint32_t>(count, static_cast<uint32_t>(read.size()));
    if (count == last.size() &&
        std::equal(last.begin(), last.end(), read.begin())) {
        return;
    }
    last.assign(read.begin(), read.begin() + count);
    BigLine b(64 + size_t{count} * 8);
    cli::Line& l = b.line;
    l.raw("{\"type\":\"absent\"");
    l.key("ids");
    l.ch('[');
    for (uint32_t i = 0; i < count; ++i) {
        if (i) {
            l.ch(',');
        }
        l.u64(last[i]);
    }
    l.ch(']');
    l.ch('}');
    write_line(out, l);
}

// A refused line whenever the signals their ECUs refused change, or the
// refusals past a report's list (cli/refused_signals.h, ADR-0051 section
// 3), from the channels' reports poll_scan read; an emptied list writes
// []. With --values only, as the signals they name.
void poll_refused(Output& out, const ecuio_signal_set_t* set,
                  const ecuio_channel_support_t* reports, uint32_t channels,
                  std::vector<cli::RefusedSignal>& last,
                  std::vector<cli::RefusedMore>& last_more)
{
    std::vector<cli::RefusedSignal> now =
        cli::refused_signals(set, reports, channels);
    std::vector<cli::RefusedMore> more =
        cli::refused_unlisted(set, reports, channels);
    const auto same = [](const cli::RefusedSignal& a,
                         const cli::RefusedSignal& b) {
        return a.id == b.id && a.length == b.length && a.nrc == b.nrc &&
               std::equal(a.request, a.request + sizeof a.request, b.request);
    };
    const auto same_more = [](const cli::RefusedMore& a,
                              const cli::RefusedMore& b) {
        return a.rx_id == b.rx_id && a.count == b.count;
    };
    if (now.size() == last.size() &&
        std::equal(now.begin(), now.end(), last.begin(), same) &&
        more.size() == last_more.size() &&
        std::equal(more.begin(), more.end(), last_more.begin(), same_more)) {
        return;
    }
    last = std::move(now);
    last_more = std::move(more);
    BigLine b(cli::refused_line_bytes(last.size(), last_more.size()));
    cli::refused_line(b.line, last, last_more);
    write_line(out, b.line);
}

// The session's signals: each def's id, name, unit, decimals and group,
// then for an enum "enum":true and its "labels" ({"<raw>": label}, as a
// signal file's "values"), for a computed def "computed":true
void write_signals(Output& out, const ecuio_signal_set_t* set)
{
    const uint32_t count = set ? set->count : 0;
    size_t bytes = 256 + size_t{count} * 512;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t labels = 0;
        cli::def_labels(*set, set->defs[i], &labels);
        bytes += cli::labels_bytes(labels);
    }
    BigLine b(bytes);
    cli::Line& l = b.line;
    l.raw("{\"type\":\"signals\"");
    l.key("signals");
    l.ch('[');
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (i) {
            l.ch(',');
        }
        l.ch('{');
        l.key("id");
        l.u64(d.id);
        l.key("name");
        l.string(d.name, sizeof d.name);
        l.key("unit");
        l.string(d.unit, sizeof d.unit);
        l.key("decimals");
        l.u64(d.decimal_places);
        l.key("group");
        const char* group = ecuio_signal_group_name(d.group);
        l.string(group, std::strlen(group));
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            l.key("computed");
            l.raw("true");
        } else if (d.kind == ECUIO_VALUE_ENUM) {
            uint32_t n = 0;
            const ecuio_enum_entry_t* labels = cli::def_labels(*set, d, &n);
            l.key("enum");
            l.raw("true");
            l.key("labels");
            cli::labels_object(l, labels, n);
        }
        l.ch('}');
    }
    l.ch(']');
    l.ch('}');
    write_line(out, l);
}

// A signal file loaded for the process's life: the profile it registers
// keeps its set, and a session's close can outlive the command's return
// (an interrupt while it hangs), so it is never freed
ecuio_signal_file_t* g_signal_file = nullptr;

#ifdef _WIN32
// A UTF-8 path (the arguments arrive as UTF-16 and are converted, core's
// paths are UTF-8) as Windows' own form, for the CLI's own file calls;
// empty when it isn't UTF-8, which then names no file
std::wstring wide(const char* utf8)
{
    return cli::utf8_to_wide(utf8);
}
#endif

// A file's bytes, read whole up to one byte past max, so the reader they
// go to refuses a larger file in its own words. false with why, in words
// of its own and never the path ("the <what> can't be read: ..."):
// strerror isn't thread-safe, and its words differ by platform
bool read_whole(const char* path, size_t max, const char* what,
                std::string* bytes, std::string* why)
{
    bytes->clear();
    FILE* f = nullptr;
#ifdef _WIN32
    int e = _wfopen_s(&f, wide(path).c_str(), L"rb");
#else
    f = std::fopen(path, "rb");
    int e = errno;
#endif
    bool failed = f == nullptr;
    if (f != nullptr) {
        char chunk[64 * 1024];
        size_t n = 0;
        while (bytes->size() <= max &&
               (n = std::fread(chunk, 1, sizeof chunk, f)) > 0) {
            bytes->append(chunk, n);
        }
        failed = std::ferror(f) != 0;
        e = errno;
        std::fclose(f);
        if (bytes->size() > max + 1) {
            bytes->resize(max + 1);
        }
    }
    if (failed) {
        *why = std::string("the ") + what + " can't be read: " +
               (e == ENOENT   ? std::string("there's no such file")
                : e == EACCES ? std::string("permission denied")
                              : "error " + std::to_string(e));
        return false;
    }
    return true;
}

bool is_folder(const char* path);

// Writes a file whole, or not at all: into a partial of the call's own
// beside it (cli/partial_file.h), then renamed over path, so a failed
// write never leaves half a file at the user's path, and never over a
// folder. false with why, never the path
bool write_whole(const char* path, const std::string& bytes, std::string* why)
{
    if (is_folder(path)) {
        *why = "the converted file can't be written: it's a folder";
        return false;
    }
    std::string partial;
    std::string words; /* a Windows error errno doesn't name */
    int e = 0;
    FILE* f = cli::create_partial(path, cli::process_id(), &partial, &e);
    if (f == nullptr) {
        /* nothing made, nothing to remove: a name there is another's */
        *why = std::string("the converted file can't be written: ") +
               (e == EEXIST   ? std::string("every partial name beside it "
                                            "is taken")
                : e == ENOENT ? std::string("there's no such folder")
                : e == EACCES ? std::string("permission denied")
                              : "error " + std::to_string(e));
        return false;
    }
    bool failed =
        std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size() ||
        std::fflush(f) != 0;
    e = errno;
    failed = std::fclose(f) != 0 || failed;
    if (!failed) {
#ifdef _WIN32
        failed = MoveFileExW(wide(partial.c_str()).c_str(), wide(path).c_str(),
                             MOVEFILE_REPLACE_EXISTING |
                                 MOVEFILE_WRITE_THROUGH) == 0;
        if (failed) {
            const DWORD w = GetLastError();
            if (w == ERROR_ACCESS_DENIED) {
                e = EACCES;
            } else {
                words = "Windows error " + std::to_string(w);
            }
        }
#else
        failed = std::rename(partial.c_str(), path) != 0;
        e = errno;
#endif
    }
    if (failed) {
        cli::remove_file(partial);
        *why = std::string("the converted file can't be written: ") +
               (!words.empty() ? words
                : e == ENOENT  ? std::string("there's no such folder")
                : e == EACCES  ? std::string("permission denied")
                               : "error " + std::to_string(e));
        return false;
    }
    return true;
}

// Reads and loads a signal file. On failure, the status and why for an
// error line, never the path: the reason it can't be read, or core's
// words, which name the line and column
ecuio_status_t load_signal_file(const char* path, ecuio_signal_file_t** file,
                                std::string* why)
{
    *file = nullptr;
    std::string bytes;
    if (!read_whole(path, ECUIO_SIGNAL_FILE_MAX_BYTES, "signal file", &bytes,
                    why)) {
        return ECUIO_ERR_LOAD_FAILED;
    }
    const ecuio_status_t st =
        ecuio_signal_file_load(bytes.data(), bytes.size(), file);
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
        load_signal_file((folder + "/signals.json").c_str(), &file, &why);
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

// signals check's lines for a loaded file: signal_file, a signal_def line
// for each of the file's own signals, then a unit line for each unit that
// never converts
void write_check_lines(Output& out, const ecuio_signal_file_t* file)
{
    const ecuio_signal_set_t& set = *ecuio_signal_file_set(file);
    {
        BigLine b(1024 + size_t{set.channel_count} * 64);
        cli::signal_file_line(b.line, ecuio_signal_file_name(file),
                              ecuio_signal_file_profile(file), set);
        write_line(out, b.line);
    }
    for (uint32_t i = 0; i < set.count; ++i) {
        if (cli::own_signal(set.defs[i])) {
            BigLine b(cli::def_line_bytes(set, set.defs[i]));
            cli::def_line(b.line, set, set.defs[i]);
            write_line(out, b.line);
        }
    }
    for (uint32_t i = 0; i < set.count; ++i) {
        if (cli::unit_kept(set.defs[i])) {
            char buf[512];
            cli::Line l(buf, sizeof buf);
            cli::unit_line(l, set.defs[i]);
            write_line(out, l);
        }
    }
}

// signals check <file> (ADR-0051 section 3): what the loader read, or why
// it refused the file. start, signal_file, a signal_def line for each of
// the file's own signals, then a unit line for each unit that never
// converts; refused, start and an error line, exit 3. No end line (a
// list, as view's)
int run_signals_check(const char* path)
{
    Output out;
    write_start(out);
    std::string why;
    ecuio_signal_file_t* file = nullptr;
    const ecuio_status_t st = load_signal_file(path, &file, &why);
    if (st != ECUIO_OK) {
        write_error(out, st, why.c_str());
        out.flush();
        return kExitConnect;
    }
    write_check_lines(out, file);
    ecuio_signal_file_free(file);
    out.flush();
    return ended(out, kExitOk);
}

// A plugin loaded for the process's life (stream --plugin, sim --plugin):
// its profiles keep their sets, and a session's close can outlive the
// command's return, so it is never freed, as g_signal_file isn't
ecuio_plugin_t* g_plugin = nullptr;
uint32_t g_plugin_variant = 0; // the variant the URI's profile= selects

// Whether path names a folder (a plugin's); otherwise it's read as a file
bool is_folder(const char* path)
{
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st{};
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

// Whether path names a file, not a folder
bool is_file(const char* path)
{
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st{};
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

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

// plugins list <pkg>... (ADR-0052): start, then a plugin line for each
// package, or a refused line saying why and going on; exit 3 when any was
// refused. No end line (a list)
int run_plugins_list(const std::vector<const char*>& paths)
{
    Output out;
    write_start(out);
    bool refused = false;
    for (size_t i = 0; i < paths.size(); ++i) {
        std::string why;
        ecuio_plugin_t* p = nullptr;
        const ecuio_status_t st = load_package(paths[i], &p, &why);
        const auto index = static_cast<uint32_t>(i);
        BigLine b(4096 + why.size() * 6);
        if (st != ECUIO_OK) {
            refused = true;
            cli::refused_line(b.line, "refused", nullptr, index, st,
                              why.c_str());
        } else {
            cli::plugin_line(b.line, index, p, plugin_info(p));
            ecuio_plugin_free(p);
        }
        write_line(out, b.line);
    }
    out.flush();
    if (!out.ok()) {
        return kExitFailed;
    }
    return refused ? kExitConnect : kExitOk;
}

// plugins match <pkg> --vin <VIN> (ADR-0052 section 5): the variants the
// VIN's manufacturer code and model year fall in, a proposal only. The
// VIN is taken as vin takes it, and never written. start, plugin, match;
// a package refused is start and an error line, exit 3
int run_plugins_match(const char* path, const char* typed)
{
    std::string vin;
    for (const char* c = typed; *c != '\0'; ++c) {
        if (*c != ' ') {
            vin += (*c >= 'a' && *c <= 'z') ? static_cast<char>(*c - 'a' + 'A')
                                            : *c;
        }
    }
    ecuio_vin_info_t decoded{};
    if (ecuio_vin_decode(vin.c_str(), latest_model_year(), &decoded) !=
        ECUIO_OK) {
        std::fprintf(stderr,
                     "havne-cli: not a VIN: 49 CFR 565 takes 17 digits and "
                     "capital letters, never I, O or Q\n");
        return kExitUsage;
    }
    Output out;
    write_start(out);
    std::string why;
    ecuio_plugin_t* p = nullptr;
    const ecuio_status_t st = load_package(path, &p, &why);
    if (st != ECUIO_OK) {
        write_error(out, st, why.c_str());
        out.flush();
        return kExitConnect;
    }
    const ecuio_plugin_info_t info = plugin_info(p);
    uint32_t indices[ECUIO_PLUGIN_MAX_VARIANTS] = {};
    uint32_t count = 0;
    ecuio_plugin_match(p, &decoded, indices, ECUIO_PLUGIN_MAX_VARIANTS, &count);
    {
        BigLine b(4096);
        cli::plugin_line(b.line, 0, p, info);
        write_line(out, b.line);
    }
    {
        BigLine b(4096);
        cli::match_line(b.line, p, info, decoded, indices,
                        std::min<uint32_t>(count, ECUIO_PLUGIN_MAX_VARIANTS));
        write_line(out, b.line);
    }
    ecuio_plugin_free(p);
    out.flush();
    return ended(out, kExitOk);
}

// A fixture entry and what its run found
struct FixtureRun {
    std::string file;
    ecuio_plugin_value_t value{};
    cli::FixtureResult result = cli::FixtureResult::not_run;
    bool have = false;
    double got = 0;
    std::string why;
};

// The newest decoded value of each signal a round's fixtures name,
// filled on the worker without a lock or a wait: the ids are fixed
// before start, and each slot is an atomic the main thread reads
struct FixtureValues {
    explicit FixtureValues(const std::vector<uint16_t>& signal_ids)
        : ids(signal_ids), values(new std::atomic<double>[signal_ids.size()]),
          have(new std::atomic<bool>[signal_ids.size()])
    {
        for (size_t k = 0; k < ids.size(); ++k) {
            values[k].store(0.0);
            have[k].store(false);
        }
    }

    static void on_values(const ecuio_value_batch_t* batch, void* user)
    {
        auto* self = static_cast<FixtureValues*>(user);
        for (uint32_t i = 0; i < batch->count; ++i) {
            const ecuio_value_t& v = batch->values[i];
            for (size_t k = 0; k < self->ids.size(); ++k) {
                if (self->ids[k] == v.signal_id) {
                    self->values[k].store(v.value);
                    self->have[k].store(true);
                }
            }
        }
    }

    bool all() const
    {
        for (size_t k = 0; k < ids.size(); ++k) {
            if (!have[k].load()) {
                return false;
            }
        }
        return true;
    }

    const std::vector<uint16_t> ids;
    std::unique_ptr<std::atomic<double>[]> values;
    std::unique_ptr<std::atomic<bool>[]> have;
};

// How long a variant's fixtures wait for their values: the probe's walk
// and the rotation over the base set's PIDs share the bus with them
constexpr int kFixtureWaitMs = 20000;

// Runs one round of a variant's fixtures, each signal at most once in it,
// against the simulated ECU on interface on: the sim answers the
// variant's DIDs with the fixtures' values, a socketcan:// session
// decodes them through the variant's own profile, and each must come back
// within half its field's step. ECUIO_OK, or the status and why of a sim
// or session that wouldn't start
ecuio_status_t run_fixture_round(const std::string& profile, const char* on,
                                 const std::vector<FixtureRun*>& mine,
                                 std::string* why)
{
    const std::string sim_uri = "mock://check?profile=" + profile;
    ecuio_sim_t* sim = nullptr;
    ecuio_status_t st = ecuio_sim_create(sim_uri.c_str(), on, &sim);
    if (st != ECUIO_OK) {
        *why = ecuio_last_error_detail();
        return st;
    }
    std::vector<FixtureRun*> asked;
    std::vector<uint16_t> ids;
    for (FixtureRun* r : mine) {
        if (ecuio_sim_set_value(sim, r->value.def->id, r->value.value) !=
            ECUIO_OK) {
            r->result = cli::FixtureResult::failed;
            r->why = "the simulated ECU can't answer it: it answers a "
                     "Service 22 DID's field, and only a value the field "
                     "holds";
            continue;
        }
        asked.push_back(r);
        ids.push_back(r->value.def->id);
    }
    FixtureValues got(ids);
    if (!asked.empty()) {
        const std::string uri = std::string("socketcan://") + on +
                                "?profile=" + profile +
                                "&identify=0&diagnose=0";
        ecuio_session_t* s = nullptr;
        st = ecuio_session_create(uri.c_str(), &s);
        if (st == ECUIO_OK) {
            st = ecuio_register_values_callback(s, &FixtureValues::on_values,
                                                &got);
        }
        if (st == ECUIO_OK) {
            st = ecuio_session_start(s);
        }
        if (st != ECUIO_OK) {
            *why = ecuio_last_error_detail();
            ecuio_session_destroy(s);
            ecuio_sim_destroy(sim);
            return st;
        }
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(kFixtureWaitMs);
        while (!got.all() && !g_interrupted.load() &&
               std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ecuio_session_close(s);
        ecuio_session_destroy(s);
    }
    ecuio_sim_destroy(sim);
    for (size_t k = 0; k < asked.size(); ++k) {
        FixtureRun& r = *asked[k];
        r.have = got.have[k].load();
        r.got = got.values[k].load();
        if (!r.have) {
            r.result = cli::FixtureResult::failed;
            r.why = g_interrupted.load()
                        ? "interrupted before its value came"
                        : "no value came within " +
                              std::to_string(kFixtureWaitMs / 1000) + " s";
        } else if (cli::fixture_passes(*r.value.def, r.value.value, r.got)) {
            r.result = cli::FixtureResult::passed;
        } else {
            r.result = cli::FixtureResult::failed;
            r.why = "the decoded value is past half the field's step";
        }
    }
    return ECUIO_OK;
}

// Runs variant index's fixtures (the runs whose value.variant is index)
// in rounds, a signal given in two fixture files being in a later round
// than its first: the simulated ECU answers one value per signal at a time
ecuio_status_t run_variant_fixtures(const std::string& profile, uint32_t index,
                                    const char* on,
                                    std::vector<FixtureRun>& runs,
                                    std::string* why)
{
    std::vector<FixtureRun*> left;
    for (FixtureRun& r : runs) {
        if (r.value.variant == index) {
            left.push_back(&r);
        }
    }
    while (!left.empty()) {
        std::vector<FixtureRun*> round;
        std::vector<FixtureRun*> later;
        for (FixtureRun* r : left) {
            const bool taken =
                std::any_of(round.begin(), round.end(), [r](FixtureRun* o) {
                    return o->value.def == r->value.def;
                });
            (taken ? later : round).push_back(r);
        }
        const ecuio_status_t st = run_fixture_round(profile, on, round, why);
        if (st != ECUIO_OK) {
            return st;
        }
        left.swap(later);
    }
    return ECUIO_OK;
}

// plugins check <pkg> [--on <vcan>] (ADR-0052 section 8): loads the
// package as a session would, then reads its value fixtures. start,
// plugin, for each variant a variant line and its own signals'
// signal_def lines, a fixture_refused line for each fixture file that
// can't be read or that core refused, a fixture line for each entry (with
// --on, run through the simulated ECU on that interface, Linux's vcan;
// else not run), then checked. A package refused is start and an error
// line, exit 3, as is a sim or session that won't start; a fixture failed
// or a file refused, exit 1. No end line
int run_plugins_check(const char* path, const char* on)
{
    Output out;
    write_start(out);
    std::string why;
    ecuio_plugin_t* p = nullptr;
    ecuio_status_t st = load_package(path, &p, &why);
    if (st != ECUIO_OK) {
        write_error(out, st, why.c_str());
        out.flush();
        return kExitConnect;
    }
    const ecuio_plugin_info_t info = plugin_info(p);
    {
        BigLine b(4096);
        cli::plugin_line(b.line, 0, p, info);
        write_line(out, b.line);
    }
    for (uint32_t v = 0; v < info.variant_count; ++v) {
        const ecuio_signal_set_t& set = *ecuio_plugin_variant_set(p, v);
        const std::string id = variant_of(p, v);
        {
            BigLine b(1024 + size_t{set.channel_count} * 64);
            cli::variant_line(b.line, id.c_str(),
                              cli::plugin_profile(info.id, id.c_str()), set);
            write_line(out, b.line);
        }
        for (uint32_t i = 0; i < set.count; ++i) {
            if (cli::own_signal(set.defs[i])) {
                BigLine b(cli::def_line_bytes(set, set.defs[i]));
                cli::def_line(b.line, set, set.defs[i]);
                write_line(out, b.line);
            }
        }
    }

    cli::FixtureCounts counts;
    std::vector<FixtureRun> runs;
    for (uint32_t f = 0; f < info.fixture_count; ++f) {
        char name[ECUIO_PLUGIN_PATH_MAX + 1] = {};
        ecuio_plugin_fixture(p, f, name, sizeof name);
        std::string bytes;
        st = read_whole((std::string(path) + "/" + name).c_str(),
                        ECUIO_SIGNAL_FILE_MAX_BYTES, "fixture file", &bytes,
                        &why)
                 ? ECUIO_OK
                 : ECUIO_ERR_LOAD_FAILED;
        uint32_t count = 0;
        if (st == ECUIO_OK) {
            st = ecuio_plugin_values(p, bytes.data(), bytes.size(), nullptr, 0,
                                     &count);
            why = ecuio_last_error_detail();
        }
        std::vector<ecuio_plugin_value_t> values(count);
        if (st == ECUIO_OK) {
            st = ecuio_plugin_values(p, bytes.data(), bytes.size(),
                                     values.data(), count, &count);
            why = ecuio_last_error_detail();
        }
        if (st != ECUIO_OK) {
            ++counts.files_refused;
            BigLine b(1024 + why.size() * 6);
            cli::refused_line(b.line, "fixture_refused", name, 0, st,
                              why.c_str());
            write_line(out, b.line);
            continue;
        }
        for (const ecuio_plugin_value_t& v : values) {
            FixtureRun r;
            r.file = name;
            r.value = v;
            r.why = "no --on interface: the simulated ECU answers on one";
            runs.push_back(r);
        }
    }

    if (on != nullptr && !runs.empty()) {
        st = register_variants(p, &why);
        for (uint32_t v = 0; st == ECUIO_OK && v < info.variant_count; ++v) {
            const std::string profile =
                cli::plugin_profile(info.id, variant_of(p, v).c_str());
            st = run_variant_fixtures(profile, v, on, runs, &why);
        }
        if (st != ECUIO_OK) {
            write_error(out, st, why.c_str());
            out.flush();
            ecuio_plugin_free(p);
            return kExitConnect;
        }
    }
    for (const FixtureRun& r : runs) {
        switch (r.result) {
        case cli::FixtureResult::passed:
            ++counts.passed;
            break;
        case cli::FixtureResult::failed:
            ++counts.failed;
            break;
        default:
            ++counts.not_run;
        }
        const std::string variant = variant_of(p, r.value.variant);
        BigLine b(1024 + r.why.size() * 6);
        cli::fixture_line(
            b.line, r.file.c_str(), variant.c_str(), r.value, r.result, r.have,
            r.got,
            r.result == cli::FixtureResult::passed ? nullptr : r.why.c_str());
        write_line(out, b.line);
    }
    {
        BigLine b(512 + (on != nullptr ? std::strlen(on) * 6 : 0));
        cli::checked_line(b.line, counts, on, info.fixture_count);
        write_line(out, b.line);
    }
    ecuio_plugin_free(p);
    out.flush();
    if (!out.ok()) {
        return kExitFailed;
    }
    return counts.failed > 0 || counts.files_refused > 0 ? kExitFailed
                                                         : kExitOk;
}

struct ImportArgs {
    const char* file = nullptr; // the OBDb set
    const char* out = nullptr;  // --out: the signal file written
    ecuio_obdb_options_t options{};
};

// What an import's note callback shares: the output and each kind's count
struct ImportNotes {
    Output* out = nullptr;
    uint64_t counts[4] = {};
};

int write_import_note(const ecuio_obdb_note_t* n, void* user)
{
    auto* notes = static_cast<ImportNotes*>(user);
    if (n->what < 4) {
        ++notes->counts[n->what];
    }
    BigLine b(cli::note_line_bytes(*n));
    cli::import_note_line(b.line, *n);
    write_line(*notes->out, b.line);
    return 0;
}

// signals import obdb <file> --out <file> (ADR-0051 section 7, obdb.h):
// start, an import_note line for each of core's notes, the converted file
// written to --out whole or not at all, imported with the counts, then
// signals check's lines for the file written. A set that can't be read or
// isn't OBDb's: start and an error line, exit 3; an --out that can't be
// written: its error line, exit 1. No end line
int run_signals_import(const ImportArgs& args)
{
    Output out;
    write_start(out);
    std::string bytes;
    std::string why;
    if (!read_whole(args.file, ECUIO_OBDB_MAX_BYTES, "OBDb file", &bytes,
                    &why)) {
        write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
        out.flush();
        return kExitConnect;
    }
    ImportNotes notes;
    notes.out = &out;
    char* json = nullptr;
    size_t length = 0;
    const ecuio_status_t st =
        ecuio_obdb_import(bytes.data(), bytes.size(), &args.options,
                          write_import_note, &notes, &json, &length);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        return kExitConnect;
    }
    const std::string text(json, length);
    ecuio_obdb_free(json);
    if (!write_whole(args.out, text, &why)) {
        write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
        out.flush();
        return kExitFailed;
    }
    {
        char buf[256];
        cli::Line l(buf, sizeof buf);
        cli::imported_line(l, notes.counts[ECUIO_OBDB_NOTE_CONVERTED],
                           notes.counts[ECUIO_OBDB_NOTE_CHANGED],
                           notes.counts[ECUIO_OBDB_NOTE_SKIPPED]);
        write_line(out, l);
    }
    ecuio_signal_file_t* file = nullptr;
    if (ecuio_signal_file_load(text.data(), text.size(), &file) == ECUIO_OK) {
        write_check_lines(out, file);
        ecuio_signal_file_free(file);
    }
    out.flush();
    return ended(out, kExitOk);
}

// What the callbacks share with the main thread
struct Stream {
    cli::LineRing ring;
    bool wait = false;                // a replay: wait for room, never drop
    uint64_t limit = 0;               // --count; 0 = none
    std::atomic<uint64_t> covered{0}; // frames written or dropped, up to limit
    // The worker's own: frames seen, and whether the last one was
    // written (its values follow it, or not)
    uint64_t seen = 0;
    bool last_written = false;
};

void on_frame(const ecuio_frame_t* f, void* user)
{
    auto* s = static_cast<Stream*>(user);
    const uint64_t n = ++s->seen;
    if (s->limit && n > s->limit) {
        s->last_written = false;
        return;
    }
    char buf[kLineBytes];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"frame\"");
    l.key("t_us");
    l.u64(f->timestamp_us);
    l.key("id");
    l.hex_id(f->arbitration_id);
    l.key("ch");
    l.u64(f->channel_id);
    l.key("flags");
    l.u64(f->flags);
    l.key("data");
    l.hex_bytes(f->data, f->data_length < ECUIO_FRAME_DATA_MAX
                             ? f->data_length
                             : ECUIO_FRAME_DATA_MAX);
    l.ch('}');
    s->last_written = l.finish() && s->ring.push(cli::LineKind::Frame, l.data(),
                                                 l.size(), s->wait);
    s->covered.store(n, std::memory_order_release);
}

void on_values(const ecuio_value_batch_t* batch, void* user)
{
    auto* s = static_cast<Stream*>(user);
    if (!s->last_written) {
        return;
    }
    char buf[kLineBytes];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"values\"");
    l.key("t_us");
    l.u64(batch->timestamp_us);
    l.key("id");
    l.hex_id(batch->arbitration_id);
    l.key("values");
    l.ch('[');
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        if (i) {
            l.ch(',');
        }
        l.ch('{');
        l.key("id");
        l.u64(v.signal_id);
        if (v.kind == ECUIO_VALUE_ENUM) {
            // The raw number, and its label when the set has one
            // (ADR-0051): the label is at most ECUIO_ENUM_LABEL_MAX bytes
            l.key("value");
            l.number(v.value);
            if (v.text) {
                l.key("label");
                l.string(v.text, v.text_len, ECUIO_ENUM_LABEL_MAX);
            }
        } else if (v.text) {
            l.key("text");
            // Room for ,"cut":true}]}\n after a cut text
            const size_t limit = l.room() > 32 ? l.room() - 32 : 0;
            if (l.string(v.text, v.text_len, limit) < v.text_len) {
                l.key("cut");
                l.raw("true");
            }
        } else {
            l.key("value");
            l.number(v.value);
        }
        l.ch('}');
    }
    l.raw("]}");
    if (l.finish()) {
        s->ring.push(cli::LineKind::Values, l.data(), l.size(), s->wait);
    }
}

void usage(FILE* to)
{
    std::fprintf(
        to,
        "usage: havne-cli stream <uri> [--values] [--count N] [--seconds S]\n"
        "                         [--until-scanned] [--record <folder> "
        "[--label <name>]]\n"
        "                         [--signals <file> | --plugin <pkg>]\n"
        "       havne-cli log <uri> <folder> [--signal <name>]... "
        "[--min-sweep <ms>]\n"
        "                      [--label <name>] [--seconds S]\n"
        "                      [--signals <file> | --plugin <pkg>]\n"
        "       havne-cli play <uri> --to <vcan> [--count N] [--seconds S]\n"
        "       havne-cli clear <uri> --yes [--engine-running [--moving]]\n"
        "       havne-cli sim <uri> --on <vcan> [--desk-bus] "
        "[--seconds S]\n"
        "                      [--signals <file> | --plugin <pkg>]\n"
        "       havne-cli sim <uri> --elm327 tcp:[<address>:]<port> "
        "[--seconds S]\n"
        "                      [--signals <file> | --plugin <pkg>]\n"
        "       havne-cli view <recording>... [--signal <name>]... [--from S]\n"
        "                      [--to S] [--columns N] [--at S]\n"
        "                      [--align-on <what>] [--hysteresis H]\n"
        "                      [--frames [--profile <name>]\n"
        "                      [--signals <file> | --plugin <pkg>]]\n"
        "       havne-cli info <recording> [--gap S] [--rate-drop X] "
        "[--settle S]\n"
        "       havne-cli frames <recording> [--from S] [--to S]\n"
        "       havne-cli export <recording> <out> --format folder|csv|pcapng\n"
        "                      [--from S] [--to S] [--raw]\n"
        "                      [--layout wide|long] [--rows sweep|<ms>]\n"
        "                      [--units metric|imperial] [--signal <name>]...\n"
        "       havne-cli export <recording> --format csv --estimate\n"
        "                      [--from S] [--to S] [--rows sweep|<ms>]\n"
        "                      [--units metric|imperial] [--signal <name>]...\n"
        "       havne-cli import candump <log> <folder> [--interface <name>]\n"
        "       havne-cli import csv <file> <folder> [--map <map.json> |\n"
        "                      --time-unit s|ms|us]\n"
        "       havne-cli import csv <file> --print-map [--time-unit s|ms|us]\n"
        "       havne-cli health <uri> <out> --format json|text "
        "[--units metric|imperial]\n"
        "                      [--seconds S] [--signals <file> | --plugin "
        "<pkg>]\n"
        "                      [--baseline <recording>]\n"
        "       havne-cli health <recording> <out> --format json|text\n"
        "                      [--units metric|imperial] [--from S] [--to S]\n"
        "                      [--baseline <recording>]\n"
        "       havne-cli signals check <file>\n"
        "       havne-cli signals import obdb <file> --out <file> [--year Y]\n"
        "                      [--name N] [--url U] [--commit C] [--alone]\n"
        "       havne-cli plugins list <pkg>...\n"
        "       havne-cli plugins check <pkg> [--on <vcan>]\n"
        "       havne-cli plugins match <pkg> --vin <VIN>\n"
        "       havne-cli devices\n"
        "       havne-cli vin <VIN>\n"
        "       havne-cli dtc <code>\n"
        "       havne-cli --version | --help\n"
        "\n"
        "stream   opens the session <uri> names (mock://, replay://, "
        "elm327://,\n"
        "         j2534:// on Windows, socketcan:// on Linux) and writes its\n"
        "         frames as JSON lines;\n"
        "         --values adds the values its profile decodes. Ends at "
        "Ctrl+C,\n"
        "         after N frames, after S seconds, or at a replay's last "
        "frame;\n"
        "         --until-scanned also once uds-generic's did= ranges are "
        "asked.\n"
        "         --record writes the session to a folder of its own in "
        "<folder>\n"
        "         (frames.csv, signals.csv, events.log), named for when it\n"
        "         started and the --label; each line typed meanwhile marks "
        "it.\n"
        "         --signals loads a signal file (stream and sim alike): its\n"
        "         profile, user.<name>, is the URI's when it names none.\n"
        "         A replay:// with neither reads with its recording's own\n"
        "         signals.json when its hash still matches and the URI\n"
        "         selects the recorded profile or none.\n"
        "         --plugin loads a plugin (a folder or a packed file) and\n"
        "         registers its variants as plugin.<id>.<variant>; the URI's\n"
        "         profile= names the one to read.\n"
        "log      records the session <uri> names to a folder of its own in\n"
        "         <folder>, as stream --record does, asking only the signals\n"
        "         --signal names (every one with none) so each is read as\n"
        "         often as the bus allows; export then takes those columns\n"
        "         by itself. Writes a log line every 10 s with the\n"
        "         recording's size, no frames or values. Runs until Ctrl+C,\n"
        "         S seconds, a failed recording or a replay's last frame; a\n"
        "         lost link is logged and waited out. --signals and --plugin\n"
        "         as stream's.\n"
        "play     (Linux) writes a session's frames to a virtual CAN "
        "interface\n"
        "         as they arrive: mock://demo drives the simulated car on it.\n"
        "sim      (Linux) answers OBD-II requests on a virtual CAN interface\n"
        "         as an engine ECU: the simulated car's, "
        "mock://sim[?values=drive|\n"
        "         canned][&modules=2][&vin=<VIN>][&faults=silent:<n>,...], or "
        "a\n"
        "         recording's, replay://<absolute path>[?loop=1][&from=S]\n"
        "         [&to=S][&vin=<VIN>][&faults=...]; from and to in view's\n"
        "         seconds. modules=2 adds a second module on 0x7E1/0x7E9\n"
        "         with Service 22 DIDs D100, D101 and D102. faults: silent,\n"
        "         late, pending (7F xx 78, then the reply), bad_sequence,\n"
        "         stalled, each every <n>th reply.\n"
        "         --desk-bus answers on a real CAN controller too (up, not\n"
        "         listen-only), only on a desk bus with no car. Ends at\n"
        "         Ctrl+C, after S seconds, or once another node sends on\n"
        "         its reply ID (0x7E8, or 0x7E9 with modules=2).\n"
        "         --elm327 serves the same car, on every platform, as an\n"
        "         ELM327 adapter on a TCP port, as a WiFi adapter does: on\n"
        "         127.0.0.1 unless an address is named (one the LAN can\n"
        "         reach is your choice), port 0 for one the system picks.\n"
        "         elm327://127.0.0.1:<port> connects Havne to it; so do\n"
        "         python-OBD and other ELM327 apps. Ends at Ctrl+C or\n"
        "         after S seconds.\n"
        "clear    clears each ECU's trouble codes, freeze frame and\n"
        "         readiness through core's gate: every ECU's report is "
        "written\n"
        "         first, then each ECU is cleared at that report, by its own\n"
        "         ID, only at 0 rpm unless --engine-running gives your word\n"
        "         (then only at 0 km/h unless --moving does too), and read\n"
        "         again. --yes is your word that the codes may go.\n"
        "view     reads recordings offline (the folders stream --record and\n"
        "         the app write): what each holds, then each signal's lowest,\n"
        "         highest and count in N columns (100) from --from to --to\n"
        "         seconds; the plotted signals unless --signal names them,\n"
        "         and with --at the values in force then. --align-on puts\n"
        "         several on one axis, its zero at rise:<signal>:<level>\n"
        "         (reaching it after being --hysteresis below),\n"
        "         fall:<signal>:<level>, marker:<label>, utc (their\n"
        "         wall-clock starts), or engine-start (the rpm signal\n"
        "         reaching 500 after being below 100): view <run>\n"
        "         <baseline> --align-on engine-start overlays a run on a\n"
        "         vehicle's baseline.\n"
        "         --frames reads the values from frames.csv through a\n"
        "         profile's decode: --profile's, a --signals file's or a\n"
        "         --plugin variant's, else the recording's own signals.json\n"
        "         or the profile it was recorded under, so a raw recording\n"
        "         has values too.\n"
        "info     checks a recording's frames offline: what it holds, then\n"
        "         each gap (no frame for over --gap seconds, 1), ID whose\n"
        "         interval passed --rate-drop times its median (10), ID first\n"
        "         seen over --settle seconds in (2), overflow, broken ISO-TP\n"
        "         sequence, link change and sleep, then a summary.\n"
        "frames   writes a recording's frames from --from to --to seconds\n"
        "         (both included) as it recorded them, a frame line each\n"
        "         with its host_us, nothing decoded, then an end line.\n"
        "export   writes a recording from --from to --to seconds to <out>\n"
        "         for another tool: pcapng, its frames for Wireshark, a\n"
        "         message an adapter delivered whole as the frames its ECU\n"
        "         sent, each marked made up; csv, its values one row a\n"
        "         sample (time_s,signal,unit,value) after a comment line;\n"
        "         folder, a copy of the recording to send (name <out> by\n"
        "         its time, such as 20261106-142233: the source's name can\n"
        "         hold the vehicle's label), the caller's notes left out,\n"
        "         markers' labels kept as typed. Scrubbed unless --raw:\n"
        "         identification replies and text signals left out, then\n"
        "         each file checked for the car's identity; a check that\n"
        "         finds any writes nothing. --estimate writes no file: the\n"
        "         wide CSV's rows and bytes, exact, at --rows' interval, or\n"
        "         for a row a sweep, every 100 ms and every 250 ms.\n"
        "health   writes a car's health as one scrubbed file, JSON or a\n"
        "         text for a person: the vehicle from its VIN (model year,\n"
        "         make, check digit; never the VIN), the inspection's\n"
        "         verdict, each ECU's readiness, trouble codes with their\n"
        "         titles, freeze frame, values since the codes were "
        "cleared\n"
        "         and identification by name only, then the profile's\n"
        "         ranked values. From a session it reads only (Services 01,\n"
        "         02, 03, 07, 09 and 0A, never a clear), parked or driving,\n"
        "         until every report it asks is in and the values have\n"
        "         come, or --seconds (30); from a recording, its replies\n"
        "         and values from --from to --to seconds. --baseline names\n"
        "         the vehicle's baseline recording, by its start and length,\n"
        "         never its folder. The file is checked for the car's\n"
        "         identity before it's written.\n"
        "import   reads a candump -L log (one bus: --interface names it\n"
        "         when the log has several) into a recording's folder that\n"
        "         view --frames, info and replay:// read. import csv reads\n"
        "         a logger's CSV by its column map (--map: the time column\n"
        "         and each signal's column, name and unit) into a\n"
        "         recording's folder that view and info read; with no map\n"
        "         it guesses one from the header (--time-unit for a time of\n"
        "         plain numbers whose header names no unit), and\n"
        "         --print-map prints that guess alone, to edit and pass.\n"
        "signals  check reads a signal file as a session would: what it\n"
        "         holds, each unit that never converts, or why it's "
        "refused.\n"
        "         import obdb converts an OBDb signal set to a signal file at\n"
        "         --out, saying field by field what differs and what's left\n"
        "         out; --year takes the commands for that model year, --name,\n"
        "         --url and --commit go in the file, and --alone leaves\n"
        "         obd2 out of it.\n"
        "plugins  list says what each package holds, or why it's refused.\n"
        "         check reads one as a session would, then its value\n"
        "         fixtures: with --on (Linux), each answered by the\n"
        "         simulated ECU on that interface and decoded back.\n"
        "         match lists the variants a VIN's manufacturer code and\n"
        "         model year fall in: the choice stays yours.\n"
        "devices  lists the serial ports, J2534 adapters and CAN interfaces a\n"
        "         URI can name.\n"
        "vin      decodes a VIN on this machine: its model year, make and\n"
        "         check digit (49 CFR 565, NHTSA vPIC's manufacturer codes).\n"
        "dtc      names a trouble code (P0301): SAE J2012's title for a\n"
        "         generic powertrain or network code.\n"
        "\n"
        "Exit: 0 the stream ended, 1 the CLI or its recording failed, 2 "
        "usage,\n"
        "      3 the connect, a recording's open or its check failed, or a\n"
        "        signal file or OBDb set was refused.\n");
}

bool parse_count(const char* text, uint64_t& out);

// --seconds of stream, play and sim: a whole number up to the shared cap
bool parse_run_seconds(const char* text, uint64_t& out)
{
    return parse_count(text, out) && out <= cli::kMaxRunSeconds;
}

bool parse_count(const char* text, uint64_t& out)
{
    if (!text || !*text) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (errno || *end || v == 0 || *text == '-' || *text == '+') {
        return false;
    }
    out = v;
    return true;
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
};

// The markers typed into stream --record (ADR-0049 section 4): a thread
// of its own reads standard input through the OS, never stdio, whose
// lock a read waiting on a console would hold through exit's flush, and
// queues each line; the main loop marks them, so the recorder has one
// caller. Detached, since a console's read never ends: exit takes it.
struct MarkQueue {
    std::mutex m;
    std::vector<std::string> lines;
};

void read_marks(std::shared_ptr<MarkQueue> q)
{
    cli::MarkLines split;
    const auto push = [&q](const std::string& line) {
        std::lock_guard<std::mutex> g(q->m);
        q->lines.push_back(line);
    };
    char buf[1024];
    for (;;) {
        size_t n = 0;
#ifdef _WIN32
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        if (in == nullptr || in == INVALID_HANDLE_VALUE) {
            break;
        }
        DWORD mode = 0;
        if (GetConsoleMode(in, &mode)) {
            // A console types UTF-16; ReadFile would give its code page's
            // bytes, not UTF-8
            wchar_t w[256];
            DWORD got = 0;
            if (!ReadConsoleW(in, w, 256, &got, nullptr) || got == 0) {
                break;
            }
            const int len =
                WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(got), buf,
                                    sizeof buf, nullptr, nullptr);
            if (len <= 0) {
                continue;
            }
            n = static_cast<size_t>(len);
        } else {
            DWORD got = 0;
            if (!ReadFile(in, buf, sizeof buf, &got, nullptr) || got == 0) {
                break;
            }
            n = got;
        }
#else
        const ssize_t got = ::read(0, buf, sizeof buf);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        n = static_cast<size_t>(got);
#endif
        split.feed(buf, n, push);
    }
    split.finish(push);
}

// A recording line: started with its folder, failed with core's reason,
// closed with whether every file ended whole
void write_recording(Output& out, const char* state, const char* key,
                     const char* text)
{
    char buf[8192];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"recording\"");
    l.key("state");
    l.string(state, SIZE_MAX);
    if (key != nullptr) {
        l.key(key);
        l.string(text, 4096);
    }
    l.ch('}');
    write_line(out, l);
}

// stream --record and log: a recorder opened under root (with the label)
// and attached before the session starts; false (and the recording line
// says why) when it can't be
bool open_recording(Output& out, ecuio_session_t* session, const char* root,
                    const char* label, ecuio_recorder_t** recorder)
{
    const std::string caller =
        std::string("havne-cli ") + ecuio_version_string();
    const char* lines[] = {caller.c_str()};
    ecuio_record_options_t o;
    std::memset(&o, 0, sizeof o);
    o.root_utf8 = root;
    o.label_utf8 = label;
    o.header_lines = lines;
    o.header_line_count = 1;
    ecuio_status_t st = ecuio_recorder_open(&o, recorder);
    if (st != ECUIO_OK) {
        const char* detail = ecuio_last_error_detail();
        write_recording(out, "failed", "reason",
                        *detail ? detail : ecuio_status_string(st));
        return false;
    }
    // The signal file decoding the session goes in as signals.json: the
    // --signals file, or the --plugin variant's files merged into one
    const ecuio_signal_file_t* file =
        g_signal_file != nullptr ? g_signal_file
        : g_plugin != nullptr
            ? ecuio_plugin_variant_file(g_plugin, g_plugin_variant)
            : nullptr;
    if (file != nullptr) {
        st = ecuio_recorder_signal_file(*recorder, file);
    }
    if (st == ECUIO_OK) {
        st = ecuio_session_record(session, *recorder);
    }
    if (st != ECUIO_OK) {
        ecuio_recorder_close(*recorder, "the session couldn't be recorded");
        *recorder = nullptr;
        write_recording(out, "failed", "reason", ecuio_status_string(st));
        return false;
    }
    char folder[4096];
    uint32_t len = 0;
    ecuio_recorder_get_folder(*recorder, folder, sizeof folder, &len);
    write_recording(out, "started", "folder", folder);
    return true;
}

// Each drain: the lines typed since the last are markers, each a mark
// line; a recording that failed says so once
void poll_recording(Output& out, ecuio_recorder_t* recorder, MarkQueue* marks,
                    bool* failed)
{
    if (recorder == nullptr) {
        return;
    }
    std::vector<std::string> typed;
    if (marks != nullptr) {
        std::lock_guard<std::mutex> g(marks->m);
        typed.swap(marks->lines);
    }
    for (const std::string& label : typed) {
        if (ecuio_recorder_mark(recorder, label.c_str()) != ECUIO_OK) {
            continue;
        }
        char buf[8192];
        cli::Line l(buf, sizeof buf);
        l.raw("{\"type\":\"mark\"");
        l.key("label");
        l.string(label.c_str(), 4096);
        l.ch('}');
        write_line(out, l);
    }
    ecuio_recorder_state_t st;
    if (!*failed && ecuio_recorder_get_state(recorder, &st) == ECUIO_OK &&
        st.state == ECUIO_RECORDING_FAILED) {
        *failed = true;
        write_recording(out, "failed", "reason", st.reason);
    }
}

// The recorder closed, after its session or, when that close hangs past
// an interrupt, while still attached (ADR-0049 section 1): either way the
// files end whole, with the summary
void close_recording(Output& out, ecuio_recorder_t** recorder, const char* why,
                     bool* failed)
{
    if (*recorder == nullptr) {
        return;
    }
    const bool whole = ecuio_recorder_close(*recorder, why) == ECUIO_OK;
    *recorder = nullptr;
    *failed = *failed || !whole;
    char buf[64];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"recording\"");
    l.key("state");
    l.string("closed", SIZE_MAX);
    l.key("whole");
    l.raw(whole ? "true" : "false");
    l.ch('}');
    write_line(out, l);
}

// A health line on each change of the session's error state (the code or
// its episode count); the state read is left in last_error and episodes
void poll_health(Output& out, ecuio_session_t* session, int32_t* last_error,
                 uint32_t* episodes)
{
    int32_t now_error = ECUIO_OK;
    uint32_t now_episodes = 0;
    ecuio_session_get_error_state(session, &now_error, &now_episodes);
    if (now_error == *last_error && now_episodes == *episodes) {
        return;
    }
    *last_error = now_error;
    *episodes = now_episodes;
    char buf[256];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"health\"");
    l.key("code");
    l.u64(static_cast<uint32_t>(now_error));
    l.key("message");
    l.string(ecuio_status_string(static_cast<ecuio_status_t>(now_error)),
             SIZE_MAX);
    l.key("episodes");
    l.u64(now_episodes);
    l.ch('}');
    write_line(out, l);
}

// A replay that doesn't loop has ended when every row is read and every
// row its faults didn't take (ADR-0041) has been written: frames lost to
// ?faults= never arrive, so the rows alone would wait forever
bool replay_done(const ecuio_session_t* session,
                 const ecuio_session_info_t& info, uint64_t covered)
{
    ecuio_replay_faults_t f;
    if (ecuio_session_get_replay_faults(session, &f) != ECUIO_OK) {
        return covered >= info.replay_frames;
    }
    return f.frames >= info.replay_frames &&
           covered + f.dropped + f.overflowed >= info.replay_frames;
}

// The end line's faults: what a replay's ?faults= did (ADR-0041)
void write_faults(cli::Line& l, const ecuio_session_t* session)
{
    ecuio_replay_faults_t f;
    if (ecuio_session_get_replay_faults(session, &f) != ECUIO_OK) {
        return;
    }
    l.key("faults");
    l.ch('{');
    l.key("frames");
    l.u64(f.frames);
    l.key("dropped");
    l.u64(f.dropped);
    l.key("overflowed");
    l.u64(f.overflowed);
    l.key("corrupted");
    l.u64(f.corrupted);
    l.key("truncated");
    l.u64(f.truncated);
    l.key("stalled");
    l.u64(f.stalled);
    l.ch('}');
}

int run_stream(const StreamArgs& args)
{
    Output out;
    auto stream = std::unique_ptr<Stream>(new (std::nothrow) Stream);
    if (!stream || !stream->ring.init(ring_bytes())) {
        std::fprintf(stderr, "havne-cli: out of memory for the output ring\n");
        return kExitFailed;
    }
    std::vector<char> drained;
    try {
        drained.resize(stream->ring.capacity());
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "havne-cli: out of memory for the output ring\n");
        return kExitFailed;
    }
    stream->limit = args.count;

    write_start(out);
    ecuio_session_t* session = nullptr;
    std::string detail;
    ecuio_status_t st = create_within(out, args.uri, &session, &detail);
    if (st != ECUIO_OK) {
        write_error(out, st, detail.c_str());
        out.flush();
        return kExitConnect;
    }
    ecuio_session_info_t info;
    std::memset(&info, 0, sizeof info);
    ecuio_session_get_info(session, &info);
    const bool replay = info.backend == ECUIO_BACKEND_REPLAY;
    stream->wait = replay;
    const bool replay_ends =
        replay && !(info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP);
    write_session(out, info);
    ecuio_recorder_t* recorder = nullptr;
    bool recording_failed = false;
    if (args.record != nullptr &&
        !open_recording(out, session, args.record, args.label, &recorder)) {
        out.flush();
        ecuio_session_destroy(session);
        return kExitFailed;
    }
    // A read that completed at create (the mock's) comes first; the rest
    // as they complete
    const uint32_t channels =
        info.channel_count < ECUIO_SESSION_INFO_MAX_CHANNELS
            ? info.channel_count
            : ECUIO_SESSION_INFO_MAX_CHANNELS;
    uint32_t diag_generations[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    uint32_t identity_generations[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    poll_identity(out, session, channels, identity_generations);
    poll_diagnostics(out, session, channels, diag_generations);
    cli::ScanSeen scan_seen[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    cli::SessionSeen session_seen[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    ecuio_channel_support_t scan_reports[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    poll_scan(out, session, channels, scan_seen, session_seen, scan_reports);
    // Judged from the values core decodes, so only with --values
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    std::vector<uint16_t> absent_read(
        (args.values && set != nullptr && set->defs != nullptr) ? set->count
                                                                : 0u);
    std::vector<uint16_t> absent_last;
    std::vector<cli::RefusedSignal> refused_last;
    std::vector<cli::RefusedMore> refused_more_last;
    cli::TimingSnapshots timing_snaps;
    std::vector<ecuio_request_timing_t> timing_rows(ECUIO_PROFILE_MAX_REQUESTS);
    if (args.values) {
        write_signals(out, set);
    }
    out.flush();

    ecuio_register_frame_callback(session, on_frame, stream.get());
    if (args.values) {
        ecuio_register_values_callback(session, on_values, stream.get());
    }
    install_handlers();
    // --until-scanned with no did= range to scan has nothing to wait for:
    // it ends before it starts
    const bool nothing_to_scan =
        args.until_scanned && !cli::scans_any(scan_reports, channels);
    st = nothing_to_scan ? ECUIO_OK : ecuio_session_start(session);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        ecuio_session_close(session);
        ecuio_session_destroy(session);
        close_recording(out, &recorder, "the session didn't start",
                        &recording_failed);
        out.flush();
        return kExitConnect;
    }
    std::shared_ptr<MarkQueue> marks;
    if (recorder != nullptr) {
        try {
            marks = std::make_shared<MarkQueue>();
            std::thread(read_marks, marks).detach();
        } catch (const std::exception&) {
            marks.reset(); // no marks, the recording goes on
        }
    }

    // Session time's clock (ADR-0033): --seconds counts a sleep too
    const uint64_t started = ecuio_clock_us();
    cli::SleepWatch sleep_watch(ecuio_clock_slept_us());
    int32_t last_error = ECUIO_OK;
    uint32_t episodes = 0;
    const char* reason = nothing_to_scan ? "nothing-to-scan" : nullptr;
    while (!reason) {
        const size_t n = stream->ring.drain(drained.data(), kPoll);
        out.write(drained.data(), n);
        out.flush();
        if (out.failed) {
            break;
        }
        const uint64_t covered =
            stream->covered.load(std::memory_order_acquire);
        if (g_interrupted.load()) {
            reason = "interrupted";
        } else if (args.count && covered >= args.count) {
            reason = "count";
        } else if (replay_ends && replay_done(session, info, covered)) {
            reason = "replay-end";
        } else if (args.seconds &&
                   ecuio_clock_us() - started >= args.seconds * 1000000u) {
            reason = "seconds";
        } else if (args.until_scanned &&
                   cli::scans_complete(scan_reports, channels)) {
            reason = "scanned";
        }

        if (const uint64_t slept = sleep_watch.poll(ecuio_clock_slept_us())) {
            char buf[64];
            cli::Line l(buf, sizeof buf);
            l.raw("{\"type\":\"slept\"");
            l.key("us");
            l.u64(slept);
            l.ch('}');
            write_line(out, l);
        }

        poll_identity(out, session, channels, identity_generations);
        poll_diagnostics(out, session, channels, diag_generations);
        poll_scan(out, session, channels, scan_seen, session_seen,
                  scan_reports);
        poll_absent(out, session, absent_read, absent_last);
        if (args.values) {
            poll_refused(out, set, scan_reports, channels, refused_last,
                         refused_more_last);
        }
        poll_timing(out, session, info, timing_snaps, timing_rows, false);
        poll_recording(out, recorder, marks.get(), &recording_failed);

        poll_health(out, session, &last_error, &episodes);
        // A replay whose file changed has stopped, short of its last
        // frame: the health line above says so, then the stream ends
        if (!reason && replay && last_error == ECUIO_ERR_RECORDING_CHANGED) {
            reason = "recording-changed";
        }
    }

    // The loop left on a failed output (a closed pipe, a full stdout):
    // nothing more is written, but the recording's close needs its why
    if (reason == nullptr) {
        reason = "output-failed";
    }
    // Close joins the worker, whose last callbacks may still be pushing
    // (a replay waiting for room): keep draining until it returns. With
    // the output gone, nothing drains, so the waits end instead.
    if (out.failed) {
        stream->ring.stop();
    }
    std::atomic<bool> closed{false};
    std::thread closer([&] {
        ecuio_session_close(session);
        closed.store(true);
        stream->ring.wake();
    });
    const auto closing = std::chrono::steady_clock::now();
    while (!closed.load()) {
        const size_t n = stream->ring.drain(drained.data(), kPoll);
        out.write(drained.data(), n);
        if (out.failed) {
            stream->ring.stop();
        }
        // Interrupted and the close still waits (an adapter's teardown):
        // the recording ends whole now, the session going on unrecorded
        if (recorder != nullptr && g_interrupted.load() &&
            std::chrono::steady_clock::now() - closing >= kRecordingCloseWait) {
            close_recording(out, &recorder,
                            "interrupted: the session's close didn't finish",
                            &recording_failed);
        }
    }
    closer.join();
    out.write(drained.data(),
              stream->ring.drain(drained.data(), std::chrono::milliseconds(0)));

    int code = kExitOk;
    poll_recording(out, recorder, marks.get(), &recording_failed);
    char buf[512];
    cli::Line l(buf, sizeof buf);
    if (!out.failed) {
        poll_scan(out, session, channels, scan_seen, session_seen, scan_reports,
                  true);
        poll_timing(out, session, info, timing_snaps, timing_rows, true);
        uint64_t covered = stream->covered.load();
        if (args.count && covered > args.count) {
            covered = args.count;
        }
        l.raw("{\"type\":\"end\"");
        l.key("reason");
        l.string(reason, SIZE_MAX);
        l.key("frames");
        l.u64(covered);
        l.key("dropped_frames");
        l.u64(stream->ring.dropped_frames());
        l.key("dropped_values");
        l.u64(stream->ring.dropped_values());
        if (info.flags & ECUIO_SESSION_INFO_REPLAY_FAULTS) {
            write_faults(l, session);
        }
        write_isotp(l, session);
        l.ch('}');
    }
    // The recording's last note is the end line; it closes after its
    // session, as the app's does
    if (recorder != nullptr && l.size() > 0) {
        const std::string end(l.data(), l.size());
        ecuio_recorder_note(recorder, end.c_str());
    }
    ecuio_session_destroy(session);
    const std::string why = std::string("havne-cli: ") + reason;
    close_recording(out, &recorder, why.c_str(), &recording_failed);
    if (!out.failed) {
        write_line(out, l);
        out.flush();
    }
    if (!out.ok() || recording_failed) {
        code = kExitFailed;
    }
#ifdef _WIN32
    g_done.store(true);
#endif
    return code;
}

// The named columns the recording has no values for, by the reader's
// signal list (ecuio_recording_open lists the signals with a sample; a
// pick the recording never answered isn't among them). False when an
// interrupt stopped the open; a recording the reader refuses has none
bool missing_columns(const cli::ExportArgs& ex, bool* asked,
                     std::vector<const char*>* missing)
{
    missing->clear();
    ecuio_recording_t* r = nullptr;
    const ecuio_status_t opened =
        ecuio_recording_open(ex.folder, open_interrupted, asked, &r);
    if (opened == ECUIO_ERR_CANCELED) {
        return false;
    }
    if (opened != ECUIO_OK || r == nullptr) {
        return true;
    }
    std::vector<std::string> recorded;
    ecuio_recording_info_t info;
    std::memset(&info, 0, sizeof info);
    if (ecuio_recording_get_info(r, &info) == ECUIO_OK) {
        for (uint32_t i = 0; i < info.signal_count; ++i) {
            ecuio_recording_signal_t s;
            std::memset(&s, 0, sizeof s);
            if (ecuio_recording_get_signal(r, i, &s) == ECUIO_OK) {
                recorded.emplace_back(s.name, strnlen(s.name, sizeof s.name));
            }
        }
    }
    ecuio_recording_close(r);
    *missing = cli::missing_names(ex.signals, recorded);
    return true;
}

// The machine's CAN interfaces (ecuio/socketcan.h); none off Linux
std::vector<ecuio_can_interface_t> can_interfaces()
{
    std::vector<ecuio_can_interface_t> list;
    uint32_t count = 0;
    if (ecuio_can_interfaces(nullptr, 0, &count) == ECUIO_OK && count) {
        list.resize(count);
        if (ecuio_can_interfaces(list.data(), count, &count) != ECUIO_OK) {
            count = 0;
        }
        list.resize(count < list.size() ? count : list.size());
    }
    return list;
}

// The recording's size so far: the recorder's files' bytes summed (its
// writer thread adds to them meanwhile; a file not yet made counts 0)
uint64_t folder_bytes(const std::string& folder)
{
    static const char* const kFiles[] = {"frames.csv",  "signals.csv",
                                         "events.log",  "markers.csv",
                                         "latency.csv", "signals.json"};
    uint64_t total = 0;
    for (const char* name : kFiles) {
        const std::string path = folder + "/" + name;
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(wide(path.c_str()).c_str(),
                                 GetFileExInfoStandard, &a)) {
            total +=
                (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
        }
#else
        struct stat st{};
        if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
            total += static_cast<uint64_t>(st.st_size);
        }
#endif
    }
    return total;
}

// log's frame callback: the frames seen, for a replay's end (stream
// counts them as it writes them; log writes none)
struct LogFrames {
    std::atomic<uint64_t> covered{0};
};

void on_log_frame(const ecuio_frame_t*, void* user)
{
    static_cast<LogFrames*>(user)->covered.fetch_add(1,
                                                     std::memory_order_release);
}

// What the log and end lines say: session time since start in whole
// seconds, the folder's bytes, the rows signals.csv holds
cli::LogReadout log_readout(uint64_t started, const std::string& folder,
                            ecuio_recorder_t* recorder)
{
    cli::LogReadout r;
    r.seconds = (ecuio_clock_us() - started) / 1000000u;
    r.bytes = folder_bytes(folder);
    ecuio_recorder_state_t st;
    if (recorder != nullptr &&
        ecuio_recorder_get_state(recorder, &st) == ECUIO_OK) {
        r.values = st.value_rows;
    }
    return r;
}

// The analysis log from the command line (ADR-0056 section 7): the
// session, the focus on the signals named (core asks those alone, as
// often as the bus allows), a recorder attached before start so the
// folder's header carries the focus, and a log line each period of
// session time with the recording's size; no frame or values lines, the
// folder is the output. The names are checked against the session's set
// before any line is written, so a typo is usage: exit 2, nothing on
// stdout. Ends at Ctrl+C, --seconds, a failed recording (exit 1) or a
// replay's last frame; a lost link is logged in health lines and waited
// out. Exit codes as stream's.
int run_log(const cli::LogArgs& args)
{
    Output out;
    ecuio_session_t* session = nullptr;
    std::string detail;
    ecuio_status_t st =
        create_within(out, args.uri, &session, &detail, /*start_first=*/true);
    if (st != ECUIO_OK) {
        write_start(out);
        write_error(out, st, detail.c_str());
        out.flush();
        return kExitConnect;
    }
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    std::vector<uint16_t> ids;
    std::string why;
    if (!cli::resolve_focus(set, args.names, ids, &why)) {
        ecuio_session_destroy(session);
        std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
        return kExitUsage;
    }
    write_start(out);
    ecuio_session_info_t info;
    std::memset(&info, 0, sizeof info);
    ecuio_session_get_info(session, &info);
    const bool replay_ends = info.backend == ECUIO_BACKEND_REPLAY &&
                             !(info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP);
    write_session(out, info);
    // The focus before the recorder attaches: the header's focus line is
    // the focus at the attach
    st = ecuio_session_set_focus_with(
        session, ids.empty() ? nullptr : ids.data(),
        static_cast<uint32_t>(ids.size()), args.min_sweep_ms, 0);
    if (st != ECUIO_OK) {
        write_error(out, st, ecuio_last_error_detail());
        out.flush();
        ecuio_session_destroy(session);
        return kExitConnect;
    }
    {
        BigLine b(cli::focus_line_bytes(ids.size()));
        cli::focus_line(b.line, set, ids, args.min_sweep_ms);
        write_line(out, b.line);
    }
    ecuio_recorder_t* recorder = nullptr;
    bool recording_failed = false;
    if (!open_recording(out, session, args.folder, args.label, &recorder)) {
        out.flush();
        ecuio_session_destroy(session);
        return kExitFailed;
    }
    // The folder, which the log and end lines name: a path core can't give
    // whole is an error, never an empty or cut folder in the lines
    std::string folder;
    {
        char buf[4096];
        uint32_t len = 0;
        st = ecuio_recorder_get_folder(recorder, buf, sizeof buf, &len);
        if (st != ECUIO_OK || len >= sizeof buf) {
            write_error(out, st != ECUIO_OK ? st : ECUIO_ERR_INVALID_ARGUMENT,
                        st != ECUIO_OK
                            ? "the recording's folder couldn't be read"
                            : "the recording's folder path is longer than "
                              "4095 bytes");
            out.flush();
            ecuio_session_destroy(session);
            close_recording(out, &recorder,
                            "havne-cli: the folder couldn't be read",
                            &recording_failed);
            out.flush();
            return kExitFailed;
        }
        folder.assign(buf, len);
    }
    out.flush();

    LogFrames frames;
    ecuio_register_frame_callback(session, on_log_frame, &frames);
    install_handlers();
    st = ecuio_session_start(session);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        ecuio_session_close(session);
        ecuio_session_destroy(session);
        close_recording(out, &recorder, "the session didn't start",
                        &recording_failed);
        out.flush();
        return kExitConnect;
    }

    // Session time's clock (ADR-0033): --seconds and the log line's
    // period count a sleep too
    const uint64_t started = ecuio_clock_us();
    cli::SleepWatch sleep_watch(ecuio_clock_slept_us());
    cli::LogTicker ticker(started, log_every_us());
    int32_t last_error = ECUIO_OK;
    uint32_t episodes = 0;
    const char* reason = nullptr;
    while (!reason) {
        std::this_thread::sleep_for(kPoll);
        const uint64_t now = ecuio_clock_us();
        if (const uint64_t slept = sleep_watch.poll(ecuio_clock_slept_us())) {
            char buf[64];
            cli::Line l(buf, sizeof buf);
            l.raw("{\"type\":\"slept\"");
            l.key("us");
            l.u64(slept);
            l.ch('}');
            write_line(out, l);
        }
        poll_recording(out, recorder, nullptr, &recording_failed);
        poll_health(out, session, &last_error, &episodes);
        reason = cli::log_end_reason(
            g_interrupted.load(), recording_failed,
            replay_ends &&
                replay_done(session, info,
                            frames.covered.load(std::memory_order_acquire)),
            args.seconds != 0 && now - started >= args.seconds * 1000000u);
        if (ticker.due(now)) {
            char buf[256];
            cli::Line l(buf, sizeof buf);
            cli::log_line(l, log_readout(started, folder, recorder));
            write_line(out, l);
        }
        out.flush();
        if (out.failed) {
            break;
        }
    }
    // The loop left on a failed output (a closed pipe, a full stdout):
    // nothing more is written, but the recording's close needs its why
    if (reason == nullptr) {
        reason = "output-failed";
    }

    // The close on a thread of its own, as stream's: interrupted and the
    // close still waiting (an adapter's teardown), the recording ends
    // whole now, the session going on unrecorded
    std::atomic<bool> closed{false};
    std::thread closer([&] {
        ecuio_session_close(session);
        closed.store(true);
    });
    const auto closing = std::chrono::steady_clock::now();
    while (!closed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (recorder != nullptr && g_interrupted.load() &&
            std::chrono::steady_clock::now() - closing >= kRecordingCloseWait) {
            close_recording(out, &recorder,
                            "interrupted: the session's close didn't finish",
                            &recording_failed);
        }
    }
    closer.join();

    int code = kExitOk;
    poll_recording(out, recorder, nullptr, &recording_failed);
    const cli::LogReadout last = log_readout(started, folder, recorder);
    BigLine end(cli::log_end_line_bytes(folder.c_str()));
    if (!out.failed) {
        cli::log_end_line(end.line, reason, folder.c_str(), last);
    }
    // The recording's last note is the end line, without the folder (its
    // path holds the account name, and a recording gets shared); it closes
    // after its session, as the app's does
    if (recorder != nullptr && end.line.size() > 0) {
        char noted_buf[256];
        cli::Line noted(noted_buf, sizeof noted_buf);
        cli::log_end_line(noted, reason, nullptr, last);
        const std::string text(noted.data(), noted.size());
        ecuio_recorder_note(recorder, text.c_str());
    }
    ecuio_session_destroy(session);
    const std::string why_closed = std::string("havne-cli: ") + reason;
    close_recording(out, &recorder, why_closed.c_str(), &recording_failed);
    if (!out.failed) {
        write_line(out, end.line);
        out.flush();
    }
    if (!out.ok() || recording_failed) {
        code = kExitFailed;
    }
#ifdef _WIN32
    g_done.store(true);
#endif
    return code;
}

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

// Clearing trouble codes (ADR-0038's appendix of 2026-11-06): start,
// session, every ECU's diagnostics line before any clear is asked, a
// clear line per ECU, the read after, end. Exit 0 when every ECU answered
// 44, 1 otherwise (each clear line says why).
int run_clear_command(const char* uri, uint32_t flags)
{
    Output out;
    write_start(out);
    ecuio_session_t* session = nullptr;
    std::string detail;
    ecuio_status_t st = create_within(out, uri, &session, &detail);
    if (st != ECUIO_OK) {
        write_error(out, st, detail.c_str());
        out.flush();
        return kExitConnect;
    }
    ecuio_session_info_t info;
    std::memset(&info, 0, sizeof info);
    ecuio_session_get_info(session, &info);
    write_session(out, info);
    out.flush();
    install_handlers();
    st = ecuio_session_start(session);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        ecuio_session_destroy(session);
        return kExitConnect;
    }
    const uint32_t channels =
        info.channel_count < ECUIO_SESSION_INFO_MAX_CHANNELS
            ? info.channel_count
            : ECUIO_SESSION_INFO_MAX_CHANNELS;
    cli::ClearIo io;
    io.write = [&out](cli::Line& l) { write_line(out, l); };
    io.flush = [&out] {
        out.flush();
        return !out.failed;
    };
    io.stop = [] { return g_interrupted.load(); };
    const cli::ClearResult r =
        cli::run_clear(session, channels, flags, io, kClearWait);
    ecuio_session_destroy(session);
    if (out.failed) {
        return ended(out, kExitOk);
    }
    char buf[256];
    cli::Line l(buf, sizeof buf);
    cli::clear_end_line(l, r);
    write_line(out, l);
    out.flush();
    if (!out.ok()) {
        return kExitFailed;
    }
    return std::strcmp(r.reason, "cleared") == 0 ? kExitOk : kExitFailed;
}

// ---- health (ADR-0054's appendix "the health report as built") ----

// Microseconds since 1970 UTC: the report's time, the CLI's to give
uint64_t utc_now_us()
{
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(since).count());
}

// A live report's values: each def's newest and when it came
// (ecuio_clock_us, 0 none), a slot for every def ID so the worker's
// callback allocates nothing
struct HealthValues {
    std::mutex mutex;
    std::vector<double> value;
    std::vector<uint64_t> at_us;
};

void on_health_values(const ecuio_value_batch_t* batch, void* user_data)
{
    auto* h = static_cast<HealthValues*>(user_data);
    const uint64_t now = ecuio_clock_us();
    std::lock_guard<std::mutex> lock(h->mutex);
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        if (v.kind == ECUIO_VALUE_TEXT || v.kind == ECUIO_VALUE_HEX) {
            continue;
        }
        h->value[v.signal_id] = v.value;
        h->at_us[v.signal_id] = now != 0 ? now : 1;
    }
}

// The report's line, or a failed check's class then the error line
int write_health_result(Output& out, const cli::HealthArgs& a,
                        ecuio_status_t rc, const ecuio_health_report_t& r,
                        bool settled)
{
    BigLine b(cli::health_line_bytes(a));
    cli::Line& l = b.line;
    if (rc != ECUIO_OK) {
        if (r.failed_class != 0) {
            cli::health_failed_line(l, r);
            write_line(out, l);
        }
        write_error(out, rc);
        out.flush();
        return ended(out, kExitConnect);
    }
    cli::health_line(l, a, r, settled);
    write_line(out, l);
    out.flush();
    return ended(out, kExitOk);
}

// A recording's report: core reads the folder, the CLI gives the time
int run_health_recording(cli::HealthArgs& a)
{
    Output out;
    write_start(out);
    a.options.utc_us = utc_now_us();
    a.options.latest_model_year = latest_model_year();
    ecuio_health_report_t r{};
    const ecuio_status_t rc = ecuio_health_export(&a.options, &r);
    return write_health_result(out, a, rc, r, true);
}

// A live session's report: start, session, then once every asked report
// has settled (cli::reports_settled) and no shown signal has gained its
// first value for kHealthQuietUs, or at --seconds or Ctrl+C, the report
// with what's held, each value aged from its arrival. Reads only: core's
// diagnostics and identification ask 01, 02, 03, 07, 09 and 0A, never 04.
int run_health_session(cli::HealthArgs& a, const char* uri)
{
    Output out;
    write_start(out);
    auto values =
        std::unique_ptr<HealthValues>(new (std::nothrow) HealthValues);
    try {
        if (!values) {
            throw std::bad_alloc();
        }
        values->value.assign(65536, 0.0);
        values->at_us.assign(65536, 0);
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "havne-cli: out of memory for the values\n");
        return kExitFailed;
    }
    ecuio_session_t* session = nullptr;
    std::string detail;
    ecuio_status_t st = create_within(out, uri, &session, &detail);
    if (st != ECUIO_OK) {
        write_error(out, st, detail.c_str());
        out.flush();
        return kExitConnect;
    }
    ecuio_session_info_t info;
    std::memset(&info, 0, sizeof info);
    ecuio_session_get_info(session, &info);
    write_session(out, info);
    out.flush();
    ecuio_register_values_callback(session, on_health_values, values.get());
    install_handlers();
    st = ecuio_session_start(session);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        ecuio_session_destroy(session);
        return kExitConnect;
    }
    const uint32_t channels =
        info.channel_count < ECUIO_SESSION_INFO_MAX_CHANNELS
            ? info.channel_count
            : ECUIO_SESSION_INFO_MAX_CHANNELS;
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    const auto settled_now = [&]() {
        bool all = true;
        for (uint32_t c = 0; c < channels; ++c) {
            ecuio_channel_diagnostics_t d{};
            ecuio_channel_identity_t id{};
            (void)ecuio_session_get_channel_diagnostics(session, c, &d);
            (void)ecuio_session_get_channel_identity(session, c, &id);
            all = all && cli::reports_settled(d, id);
        }
        return all;
    };
    const auto shown_with_value = [&]() {
        uint32_t n = 0;
        std::lock_guard<std::mutex> lock(values->mutex);
        for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
            if (cli::shown_def(set->defs[i]) &&
                values->at_us[set->defs[i].id] != 0) {
                ++n;
            }
        }
        return n;
    };
    // The shown defs still owed a first value: PIDs the bitmaps list
    const auto owed_now = [&]() {
        ecuio_channel_support_t supports[ECUIO_SESSION_INFO_MAX_CHANNELS]{};
        for (uint32_t c = 0; c < channels; ++c) {
            (void)ecuio_session_get_channel_support(session, c, &supports[c]);
        }
        uint32_t n = 0;
        std::lock_guard<std::mutex> lock(values->mutex);
        for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if (cli::awaited(d, values->at_us[d.id] != 0, supports, channels)) {
                ++n;
            }
        }
        return n;
    };
    const uint64_t deadline = ecuio_clock_us() + a.seconds * 1000000u;
    cli::ValuesQuiet quiet;
    for (;;) {
        const uint64_t now = ecuio_clock_us();
        const bool settled_here = settled_now();
        const bool quiet_here =
            settled_here &&
            quiet.quiet(shown_with_value(), now, cli::kHealthQuietUs);
        if (cli::health_wait_over(settled_here, quiet_here,
                                  quiet_here ? owed_now() : 0, now, deadline) ||
            g_interrupted.load()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const bool settled = settled_now();
    std::vector<ecuio_health_value_t> held;
    {
        const uint64_t now = ecuio_clock_us();
        std::lock_guard<std::mutex> lock(values->mutex);
        for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
            const uint16_t id = set->defs[i].id;
            if (values->at_us[id] != 0) {
                ecuio_health_value_t v{};
                v.def_id = id;
                v.value = values->value[id];
                v.age_us =
                    now > values->at_us[id] ? now - values->at_us[id] : 0;
                held.push_back(v);
            }
        }
    }
    a.options.session = session;
    a.options.values = held.empty() ? nullptr : held.data();
    a.options.value_count = static_cast<uint32_t>(held.size());
    a.options.utc_us = utc_now_us();
    a.options.latest_model_year = latest_model_year();
    ecuio_health_report_t r{};
    const ecuio_status_t rc = ecuio_health_export(&a.options, &r);
    ecuio_session_destroy(session);
    return write_health_result(out, a, rc, r, settled);
}

// The simulated ECU as an ELM327 adapter on a TCP port (ADR-0055), on
// every platform, until Ctrl+C or --seconds: start, elm327, a client
// line as each host connects and leaves, a sim_stats line every 10 s
// while the counts change, end with them
int run_sim_elm327(const SimArgs& args)
{
    install_handlers();
    Output out;
    write_start(out);
    ecuio_sim_t* sim = nullptr;
    const ecuio_status_t st = ecuio_sim_create_unbound(args.uri, 0, &sim);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        return kExitConnect;
    }
    const auto created = std::chrono::steady_clock::now();
    cli::Elm327Server server;
    std::string why;
    if (!server.open(args.elm327_at, &why)) {
        ecuio_sim_destroy(sim);
        write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
        out.flush();
        return kExitConnect;
    }
    {
        char buf[1024];
        cli::Line l(buf, sizeof buf);
        cli::elm327_line(
            l, args.uri,
            cli::address_text(server.address().ip, server.address().port));
        write_line(out, l);
    }
    out.flush();
    cli::Elm327Emulator emu(sim);
    const uint64_t started = ecuio_clock_us();
    uint64_t next_stats = started + kSimStatsEveryUs;
    ecuio_sim_stats_t last{};
    ecuio_sim_stats_t s{};
    const char* reason = nullptr;
    server.serve(
        emu,
        [created] {
            return static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - created)
                    .count());
        },
        [&] {
            const uint64_t now = ecuio_clock_us();
            if (g_interrupted.load()) {
                reason = "interrupted";
            } else if (args.seconds &&
                       now - started >= args.seconds * 1000000u) {
                reason = "seconds";
            } else if (now >= next_stats) {
                next_stats = now + kSimStatsEveryUs;
                (void)ecuio_sim_get_stats(sim, &s);
                if (std::memcmp(&s, &last, sizeof s) != 0) {
                    last = s;
                    char buf[1024];
                    cli::Line l(buf, sizeof buf);
                    cli::sim_stats_line(l, s);
                    write_line(out, l);
                    out.flush();
                }
            }
            return reason != nullptr || out.failed;
        },
        [&](const char* event, const std::string& peer) {
            char buf[256];
            cli::Line l(buf, sizeof buf);
            cli::client_line(l, event, peer);
            write_line(out, l);
            out.flush();
        });
    (void)ecuio_sim_get_stats(sim, &s);
    ecuio_sim_destroy(sim);
    if (out.failed) {
        return ended(out, kExitOk);
    }
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::sim_end_line(l, reason != nullptr ? reason : "failed", s);
    write_line(out, l);
    out.flush();
    if (reason == nullptr) {
        // The listener's wait failed: the end line says "failed", stderr why
        std::fprintf(stderr, "havne-cli: %s\n",
                     server.failure().empty() ? "the listener stopped"
                                              : server.failure().c_str());
        (void)out.ok();
        return kExitFailed;
    }
    return ended(out, kExitOk);
}

// The simulated ECU (ADR-0046) answering on a virtual bus, or a desk
// bus's controller with --desk-bus, until Ctrl+C, --seconds, the bus
// failing or another node sending on 0x7E8: start, sim, a sim_stats
// line every 10 s
// while the counts change, end with them
int run_sim(const SimArgs& args)
{
    install_handlers();
    Output out;
    write_start(out);
    ecuio_sim_t* sim = nullptr;
    const ecuio_status_t st = ecuio_sim_create_with(
        args.uri, args.on, args.desk_bus ? ECUIO_SIM_ALLOW_DESK_BUS : 0u, &sim);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        return kExitConnect;
    }
    {
        char buf[1024];
        cli::Line l(buf, sizeof buf);
        cli::sim_line(l, args.uri, args.on, args.desk_bus);
        write_line(out, l);
    }
    out.flush();
    const uint64_t started = ecuio_clock_us();
    uint64_t next_stats = started + kSimStatsEveryUs;
    ecuio_sim_stats_t last{};
    ecuio_sim_stats_t s{};
    const char* reason = nullptr;
    while (!reason) {
        std::this_thread::sleep_for(kPoll);
        (void)ecuio_sim_get_stats(sim, &s);
        const uint64_t now = ecuio_clock_us();
        if (g_interrupted.load()) {
            reason = "interrupted";
        } else if (s.flags & ECUIO_SIM_FAILED) {
            reason = "bus-failed";
        } else if (s.flags & ECUIO_SIM_YIELDED) {
            reason = "yielded";
        } else if (args.seconds && now - started >= args.seconds * 1000000u) {
            reason = "seconds";
        } else if (now >= next_stats) {
            next_stats = now + kSimStatsEveryUs;
            if (std::memcmp(&s, &last, sizeof s) != 0) {
                last = s;
                char buf[1024];
                cli::Line l(buf, sizeof buf);
                cli::sim_stats_line(l, s);
                write_line(out, l);
                out.flush();
            }
        }
        if (out.failed) {
            break; /* a closed pipe ends it quietly */
        }
    }
    ecuio_sim_destroy(sim);
    if (out.failed) {
        return ended(out, kExitOk);
    }
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::sim_end_line(l, reason, s);
    write_line(out, l);
    out.flush();
    if (std::strcmp(reason, "bus-failed") == 0) {
        std::fprintf(stderr,
                     "havne-cli: the simulated ECU's bus failed (%s "
                     "went down or away)\n",
                     args.on);
        return kExitFailed;
    }
    if (std::strcmp(reason, "yielded") == 0) {
        std::fprintf(stderr,
                     "havne-cli: another node sent on 0x7E8 on %s, so "
                     "an ECU answers there: the simulated ECU went "
                     "silent\n",
                     args.on);
        return kExitFailed;
    }
    return ended(out, kExitOk);
}

// A typed VIN's decode (ADR-0043), offline: spaces dropped and upper
// case, as Settings' lookup takes it; not a VIN is a usage error
int run_vin(const char* typed)
{
    std::string vin;
    for (const char* p = typed; *p != '\0'; ++p) {
        if (*p != ' ') {
            vin += (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - 'a' + 'A')
                                            : *p;
        }
    }
    ecuio_vin_info_t info;
    if (ecuio_vin_decode(vin.c_str(), 0, &info) != ECUIO_OK) {
        std::fprintf(stderr,
                     "havne-cli: not a VIN: 49 CFR 565 takes 17 digits and "
                     "capital letters, never I, O or Q\n");
        return kExitUsage;
    }
    Output out;
    write_start(out);
    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::vin_line(l, vin.c_str(), latest_model_year());
    write_line(out, l);
    return ended(out, kExitOk);
}

// A typed trouble code's title (ADR-0044), as Diagnostics' lookup takes
// it: spaces dropped and upper case; not a code is a usage error
int run_dtc(const char* typed)
{
    std::string text;
    for (const char* p = typed; *p != '\0'; ++p) {
        if (*p != ' ') {
            text += (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - 'a' + 'A')
                                             : *p;
        }
    }
    uint16_t code = 0;
    if (ecuio_dtc_parse(text.c_str(), &code) != ECUIO_OK) {
        std::fprintf(stderr, "havne-cli: not a trouble code: a letter P, C, B "
                             "or U, then four hex digits, the first 0-3\n");
        return kExitUsage;
    }
    Output out;
    write_start(out);
    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::dtc_line(l, code);
    write_line(out, l);
    return ended(out, kExitOk);
}

struct ViewArgs {
    std::vector<const char*> folders;
    std::vector<std::string> signals; // --signal, in order; none: the ranked
    bool has_from = false;
    bool has_to = false;
    bool has_at = false;
    int64_t from_us = 0;
    int64_t to_us = 0;
    int64_t at_us = 0;
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

struct InfoArgs {
    const char* folder = nullptr;
    ecuio_check_options_t options{};
};

// The check's findings, kept until it ends: an error line then stands
// alone, as a failed open's does
struct Findings {
    std::vector<ecuio_check_finding_t> list;
    bool out_of_memory = false;
};

int keep_finding(const ecuio_check_finding_t* f, void* user)
{
    auto* found = static_cast<Findings*>(user);
    try {
        found->list.push_back(*f);
    } catch (const std::bad_alloc&) {
        found->out_of_memory = true;
        return 1;
    }
    return 0;
}

struct RecordingCloser {
    void operator()(ecuio_recording_t* r) const { ecuio_recording_close(r); }
};

// One recording opened for view: what it holds, the signals it shows,
// and where the axis puts it
struct Viewed {
    std::unique_ptr<ecuio_recording_t, RecordingCloser> r;
    ecuio_recording_info_t info{};
    ecuio_recording_signals_t signal_file{};
    std::vector<ecuio_recording_signal_t> signals;
    std::vector<uint32_t> shown;
    bool aligned = true;
    uint64_t align_host_us = ECUIO_RECORDING_NONE;
    int64_t offset_us = 0;
    std::string decoded; // --frames: the profile, or "signals.json"

    // The index of a signal by name, or UINT32_MAX
    uint32_t find(const std::string& name) const
    {
        for (uint32_t k = 0; k < signals.size(); ++k) {
            if (name ==
                std::string(signals[k].name,
                            strnlen(signals[k].name, sizeof signals[k].name))) {
                return k;
            }
        }
        return UINT32_MAX;
    }
};

// view's open of one recording: its signals.csv, or with --frames its
// frames.csv through the set cli/view_frames.h's rules pick, *decoded
// naming it. A failure's why for the error line, never a path.
ecuio_status_t open_viewed(const ViewArgs& args, const char* folder,
                           ecuio_recording_t** r, std::string* decoded,
                           std::string* why)
{
    if (!args.frames) {
        const ecuio_status_t st =
            ecuio_recording_open(folder, nullptr, nullptr, r);
        *why = ecuio_last_error_detail();
        return st;
    }
    // A folder with no frames.csv (one that isn't there, too) is refused
    // in the reader's words before a profile is asked for, as the app's
    // viewer refuses it
    if (!is_file((std::string(folder) + "/frames.csv").c_str())) {
        *why = "the folder has no frames.csv";
        return ECUIO_ERR_LOAD_FAILED;
    }
    const cli::RecordedSignals recorded = recorded_signals(folder);
    bool own = false;
    if (args.profile.empty() && recorded.names_file()) {
        ecuio_recording_t* plain = nullptr;
        ecuio_recording_signals_t sig{};
        if (ecuio_recording_open(folder, nullptr, nullptr, &plain) ==
                ECUIO_OK &&
            ecuio_recording_get_signals(plain, &sig) == ECUIO_OK) {
            own = (sig.flags & ECUIO_RECORDING_SIGNALS_MATCH) != 0;
        }
        ecuio_recording_close(plain);
    }
    const cli::FramesSource source =
        cli::frames_source(args.profile, recorded, own);
    ecuio_signal_file_t* file = nullptr;
    const ecuio_signal_set_t* set = nullptr;
    if (source.own_file) {
        const ecuio_status_t st = load_signal_file(
            (std::string(folder) + "/signals.json").c_str(), &file, why);
        if (st != ECUIO_OK) {
            return st;
        }
        set = ecuio_signal_file_set(file);
        *decoded = "signals.json";
    } else if (source.profile.empty()) {
        *why = source.why;
        return ECUIO_ERR_UNKNOWN_PROFILE;
    } else {
        const bool registered =
            ecuio_profile_lookup(source.profile.c_str(), &set) == ECUIO_OK;
        *why = cli::profile_refusal(source.profile, registered,
                                    set != nullptr ? set->count : 0);
        if (!why->empty()) {
            return ECUIO_ERR_UNKNOWN_PROFILE;
        }
        *decoded = source.profile;
    }
    const ecuio_status_t st =
        ecuio_recording_open_frames(folder, set, nullptr, nullptr, r);
    *why = ecuio_last_error_detail();
    ecuio_signal_file_free(file); /* the reader keeps its own copy */
    return st;
}

// view --frames' --signals and --plugin: the file's profile, or each
// variant's, registered for the process's life, and the --profile to
// decode with when none was given. -1 to go on; else the exit code: a
// file that can't be read or is refused is start and an error line (3),
// a --profile that isn't the file's or a variant's a usage error (2)
int use_view_signals(Output& out, ViewArgs& args)
{
    std::string why;
    ecuio_status_t st = ECUIO_OK;
    if (args.signals_path != nullptr) {
        ecuio_signal_file_t* file = nullptr;
        st = load_signal_file(args.signals_path, &file, &why);
        if (st == ECUIO_OK) {
            const char* profile = ecuio_signal_file_profile(file);
            if (!args.profile.empty() && args.profile != profile) {
                std::fprintf(stderr,
                             "havne-cli: --profile isn't the signal file's, "
                             "%s\n",
                             profile);
                ecuio_signal_file_free(file);
                return kExitUsage;
            }
            st = ecuio_profile_register(profile, ecuio_signal_file_set(file),
                                        nullptr);
            if (st == ECUIO_OK) {
                g_signal_file = file;
                args.profile = profile;
                return -1;
            }
            why = ecuio_last_error_detail();
            ecuio_signal_file_free(file);
        }
    } else if (args.plugin_path != nullptr) {
        ecuio_plugin_t* p = nullptr;
        st = load_package(args.plugin_path, &p, &why);
        if (st == ECUIO_OK) {
            const ecuio_plugin_info_t info = plugin_info(p);
            std::string names;
            bool known = false;
            for (uint32_t v = 0; v < info.variant_count; ++v) {
                const std::string name =
                    cli::plugin_profile(info.id, variant_of(p, v).c_str());
                names += (v == 0 ? "" : ", ") + name;
                known = known || name == args.profile;
            }
            if (args.profile.empty() && info.variant_count == 1) {
                args.profile = names;
                known = true;
            }
            if (!known) {
                std::fprintf(stderr,
                             "havne-cli: --profile names the variant to "
                             "decode with: %s\n",
                             names.c_str());
                ecuio_plugin_free(p);
                return kExitUsage;
            }
            st = register_variants(p, &why);
            if (st == ECUIO_OK) {
                g_plugin = p;
                return -1;
            }
            (void)ecuio_plugin_free(p);
        }
    } else {
        return -1;
    }
    write_start(out);
    write_error(out, st, why.c_str());
    out.flush();
    return kExitConnect;
}

// A recording read offline (ADR-0049 section 5): every recording opens,
// then the names are checked (a name in none is a usage error, before any
// output), then the lines. A failed open or query is an error line, exit 3.
int run_view(ViewArgs& args)
{
    Output out;
    const int used = use_view_signals(out, args);
    if (used >= 0) {
        return used;
    }
    std::vector<Viewed> views(args.folders.size());
    for (size_t i = 0; i < args.folders.size(); ++i) {
        ecuio_recording_t* r = nullptr;
        std::string decoded;
        std::string why;
        const ecuio_status_t rc =
            open_viewed(args, args.folders[i], &r, &decoded, &why);
        if (rc != ECUIO_OK) {
            write_start(out);
            write_error(out, rc, why.c_str());
            return ended(out, kExitConnect);
        }
        Viewed& v = views[i];
        v.r.reset(r);
        v.decoded = decoded;
        ecuio_recording_get_info(r, &v.info);
        ecuio_recording_get_signals(r, &v.signal_file);
        v.signals.resize(v.info.signal_count);
        for (uint32_t k = 0; k < v.info.signal_count; ++k) {
            ecuio_recording_get_signal(r, k, &v.signals[k]);
        }
    }
    auto in_any = [&](const std::string& name) {
        return std::any_of(views.begin(), views.end(), [&](const Viewed& v) {
            return v.find(name) != UINT32_MAX;
        });
    };
    for (const std::string& name : args.signals) {
        if (!in_any(name)) {
            std::fprintf(stderr, "havne-cli: no recording has a signal '%s'\n",
                         name.c_str());
            return kExitUsage;
        }
    }
    const bool threshold = args.align.kind == cli::AlignOn::rise ||
                           args.align.kind == cli::AlignOn::fall;
    if (threshold) {
        if (!in_any(args.align.signal)) {
            std::fprintf(stderr, "havne-cli: no recording has a signal '%s'\n",
                         args.align.signal.c_str());
            return kExitUsage;
        }
        for (const Viewed& v : views) {
            const uint32_t k = v.find(args.align.signal);
            if (k != UINT32_MAX &&
                (v.signals[k].flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0) {
                std::fprintf(stderr,
                             "havne-cli: '%s' is text: it crosses no level\n",
                             args.align.signal.c_str());
                return kExitUsage;
            }
        }
    }

    // The signals each shows: those named, in order, or its ranked ones by
    // rank
    for (Viewed& v : views) {
        if (!args.signals.empty()) {
            for (const std::string& name : args.signals) {
                const uint32_t k = v.find(name);
                if (k != UINT32_MAX) {
                    v.shown.push_back(k);
                }
            }
        } else {
            for (uint32_t k = 0; k < v.signals.size(); ++k) {
                if (v.signals[k].plot_rank != 0) {
                    v.shown.push_back(k);
                }
            }
            std::stable_sort(
                v.shown.begin(), v.shown.end(), [&](uint32_t a, uint32_t b) {
                    return v.signals[a].plot_rank < v.signals[b].plot_rank;
                });
        }
    }

    // Where each recording's zero lands: the event at 0, or the UTC
    // starts' differences from the earliest
    uint64_t utc_min = UINT64_MAX;
    for (const Viewed& v : views) {
        if (v.info.utc_start_us != 0) {
            utc_min = (std::min)(utc_min, v.info.utc_start_us);
        }
    }
    for (Viewed& v : views) {
        switch (args.align.kind) {
        case cli::AlignOn::none:
            break;
        case cli::AlignOn::utc:
            v.aligned = v.info.utc_start_us != 0;
            if (v.aligned) {
                v.offset_us =
                    static_cast<int64_t>(v.info.utc_start_us - utc_min);
            }
            break;
        default: {
            ecuio_recording_find_t what{};
            if (threshold) {
                what.kind = ECUIO_RECORDING_FIND_THRESHOLD;
                what.signal = v.find(args.align.signal);
                what.direction = args.align.kind == cli::AlignOn::rise
                                     ? ECUIO_RECORDING_FIND_RISING
                                     : ECUIO_RECORDING_FIND_FALLING;
                what.level = args.align.level;
                what.hysteresis = args.hysteresis;
            } else if (args.align.kind == cli::AlignOn::engine_start) {
                what.kind = ECUIO_RECORDING_FIND_ENGINE_START;
            } else {
                what.kind = ECUIO_RECORDING_FIND_MARKER;
                what.label_utf8 = args.align.label.c_str();
            }
            uint64_t at = ECUIO_RECORDING_NONE;
            if (!threshold || what.signal != UINT32_MAX) {
                const ecuio_status_t rc =
                    ecuio_recording_find(v.r.get(), &what, 0, &at);
                if (rc != ECUIO_OK) {
                    write_start(out);
                    write_error(out, rc,
                                "the recording changed while it was read");
                    return ended(out, kExitConnect);
                }
            }
            v.aligned = at != ECUIO_RECORDING_NONE;
            v.align_host_us = at;
            v.offset_us = v.aligned ? -static_cast<int64_t>(at) : 0;
            break;
        }
        }
    }

    // The axis: given, or from the first recording's start to the last's
    // end on it
    int64_t from = INT64_MAX;
    int64_t to = INT64_MIN;
    for (const Viewed& v : views) {
        if (v.aligned) {
            from = (std::min)(from, v.offset_us);
            to = (std::max)(to, v.offset_us +
                                    static_cast<int64_t>(v.info.duration_us));
        }
    }
    if (from == INT64_MAX) {
        from = 0;
        to = 0;
    }
    if (to <= from) {
        to = from + 1000000; /* nothing recorded: a second */
    }
    if (args.has_from) {
        from = args.from_us;
    }
    if (args.has_to) {
        to = args.to_us;
    }
    cli::Axis axis;
    if (!cli::make_axis(from, to, args.columns, axis)) {
        std::fprintf(stderr, "havne-cli: --from must come before --to, here "
                             "on the recordings' axis\n");
        return kExitUsage;
    }

    write_start(out);
    char buf[2048];
    for (uint32_t i = 0; i < views.size(); ++i) {
        const Viewed& v = views[i];
        cli::Line l(buf, sizeof buf);
        cli::opened_line(l, i, v.info, v.signal_file,
                         v.decoded.empty() ? nullptr : v.decoded.c_str());
        write_line(out, l);
        for (uint32_t k = 0; k < v.signals.size(); ++k) {
            write_signal_line(out, v.r.get(), i, k, v.signals[k]);
        }
        for (uint32_t k = 0; k < v.info.marker_count; ++k) {
            ecuio_recording_marker_t m{};
            ecuio_recording_get_marker(v.r.get(), k, &m);
            BigLine b(256 + static_cast<size_t>(m.label_length) * 6);
            cli::marker_line(b.line, i, k, m);
            write_line(out, b.line);
        }
    }
    if (args.align.kind != cli::AlignOn::none) {
        for (uint32_t i = 0; i < views.size(); ++i) {
            const Viewed& v = views[i];
            BigLine b(512 + std::strlen(args.align_text) * 6);
            cli::align_line(b.line, i, args.align_text, v.aligned,
                            v.align_host_us, v.offset_us);
            write_line(out, b.line);
        }
    }
    {
        cli::Line l(buf, sizeof buf);
        cli::axis_line(l, axis);
        write_line(out, l);
    }
    std::vector<ecuio_recording_column_t> cols;
    for (uint32_t i = 0; i < views.size(); ++i) {
        const Viewed& v = views[i];
        if (!v.aligned || v.shown.empty()) {
            continue;
        }
        cli::Window w;
        const bool any = cli::window_of(axis, v.offset_us, w);
        const size_t signals = v.shown.size();
        if (any) {
            /* Signal by signal, w.count columns each: the partial column's
             * own query first, then the rest's */
            cols.assign(signals * w.count, {});
            const uint32_t partial = w.partial ? 1 : 0;
            const uint32_t rest = w.count - partial;
            std::vector<ecuio_recording_column_t> got(signals *
                                                      (std::max)(rest, 1u));
            ecuio_status_t rc = ECUIO_OK;
            if (partial != 0) {
                rc = ecuio_recording_query(v.r.get(), v.shown.data(),
                                           static_cast<uint32_t>(signals), 0,
                                           w.partial_t1_us, got.data(), 1);
                for (size_t n = 0; rc == ECUIO_OK && n < signals; ++n) {
                    cols[n * w.count] = got[n];
                }
            }
            if (rc == ECUIO_OK && rest != 0) {
                rc = ecuio_recording_query(v.r.get(), v.shown.data(),
                                           static_cast<uint32_t>(signals),
                                           w.t0_us, w.t1_us, got.data(), rest);
                for (size_t n = 0; rc == ECUIO_OK && n < signals; ++n) {
                    std::copy(
                        got.begin() + static_cast<std::ptrdiff_t>(n * rest),
                        got.begin() +
                            static_cast<std::ptrdiff_t>((n + 1) * rest),
                        cols.begin() +
                            static_cast<std::ptrdiff_t>(n * w.count + partial));
                }
            }
            if (rc != ECUIO_OK) {
                write_error(out, rc, "the recording changed while it was read");
                return ended(out, kExitConnect);
            }
        }
        for (size_t n = 0; n < v.shown.size(); ++n) {
            BigLine b(cli::columns_line_bytes(axis.columns));
            cli::columns_line(b.line, i, v.signals[v.shown[n]], w, axis.columns,
                              any ? cols.data() + n * w.count : nullptr);
            write_line(out, b.line);
        }
    }
    if (args.has_at) {
        std::vector<char> text(32768);
        for (uint32_t i = 0; i < views.size(); ++i) {
            const Viewed& v = views[i];
            if (!v.aligned) {
                continue;
            }
            const int64_t t = args.at_us - v.offset_us;
            BigLine b(256 + v.shown.size() * (128 + text.size() * 6));
            cli::at_begin(b.line, i, args.at_us);
            for (size_t n = 0; n < v.shown.size(); ++n) {
                uint64_t host = ECUIO_RECORDING_NONE;
                text[0] = '\0';
                if (t >= 0) {
                    const ecuio_status_t rc = ecuio_recording_text_at(
                        v.r.get(), v.shown[n], static_cast<uint64_t>(t),
                        text.data(), static_cast<uint32_t>(text.size()), &host);
                    if (rc != ECUIO_OK) {
                        write_error(out, rc,
                                    "the recording changed while it was read");
                        return ended(out, kExitConnect);
                    }
                }
                cli::at_value(b.line, n == 0, v.signals[v.shown[n]], host,
                              text.data());
            }
            cli::at_end(b.line);
            write_line(out, b.line);
        }
    }
    return ended(out, kExitOk);
}

int run_devices()
{
    Output out;
    write_start(out);
    uint32_t count = 0;
    std::vector<ecuio_serial_port_t> ports;
    if (ecuio_serial_ports(nullptr, 0, &count) == ECUIO_OK && count) {
        ports.resize(count);
        ecuio_serial_ports(ports.data(), count, &count);
        if (count < ports.size()) {
            ports.resize(count);
        }
    } else {
        count = 0;
    }
    for (const ecuio_serial_port_t& p : ports) {
        char buf[1024];
        cli::Line l(buf, sizeof buf);
        l.raw("{\"type\":\"port\"");
        l.key("name");
        l.string(p.name, sizeof p.name);
        l.key("device");
        l.string(p.device, sizeof p.device);
        l.key("uri");
        const std::string uri = std::string("elm327://") + p.name;
        l.string(uri.c_str(), SIZE_MAX);
        l.ch('}');
        write_line(out, l);
    }
    ecuio_j2534_device_t* devices = nullptr;
    uint32_t device_count = 0;
    if (ecuio_j2534_discover_devices(&devices, &device_count) == ECUIO_OK) {
        for (uint32_t i = 0; i < device_count; ++i) {
            const ecuio_j2534_device_t& d = devices[i];
            char buf[2048];
            cli::Line l(buf, sizeof buf);
            l.raw("{\"type\":\"j2534\"");
            l.key("name");
            l.string(d.vendor_name, sizeof d.vendor_name);
            l.key("dll");
            l.string(d.dll_path, sizeof d.dll_path);
            l.key("arch");
            l.string(d.reserved[0] == ECUIO_DLL_ARCH_X86   ? "x86"
                     : d.reserved[0] == ECUIO_DLL_ARCH_X64 ? "x64"
                                                           : "unknown",
                     SIZE_MAX);
            l.key("uri");
            const std::string uri = std::string("j2534://") + d.vendor_name;
            l.string(uri.c_str(), SIZE_MAX);
            l.ch('}');
            write_line(out, l);
        }
    }
    ecuio_j2534_free_devices(devices);
    for (const ecuio_can_interface_t& c : can_interfaces()) {
        char buf[512];
        cli::Line l(buf, sizeof buf);
        cli::can_line(l, c);
        write_line(out, l);
    }
    out.flush();
    return ended(out, kExitOk);
}

#if HAVNE_CLI_PLAY
// play (ADR-0032 section 8): a session's frames written to a virtual CAN
// interface as they arrive, so the mock's car (or a recording) drives on
// a bus a socketcan:// session, or any SocketCAN tool, can listen to
struct Player {
    int fd = -1;
    bool fd_frames = false;   // the interface takes CAN FD (MTU >= 72)
    uint64_t limit = 0;       // --count; 0 = none
    uint64_t seen = 0;        // the worker's own
    std::atomic<uint64_t> covered{0};
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> skipped{0};
};

// The CAN FD length that holds n bytes: 0 to 8, then 12, 16, 20, 24,
// 32, 48, 64, the rest zero padding as an FD controller sends it; 0 for
// more than a frame holds
size_t fd_length(size_t n)
{
    static const size_t lengths[] = {12, 16, 20, 24, 32, 48, 64};
    if (n <= 8) {
        return n;
    }
    for (size_t len : lengths) {
        if (n <= len) {
            return len;
        }
    }
    return 0;
}

// On the worker: one non-blocking write, never a wait
void on_play(const ecuio_frame_t* f, void* user)
{
    auto* p = static_cast<Player*>(user);
    const uint64_t n = ++p->seen;
    if (p->limit && n > p->limit) {
        return;
    }
    const size_t len = f->data_length;
    const bool fd = (f->flags & ECUIO_FRAME_FLAG_FD) != 0 || len > 8;
    canid_t id = f->arbitration_id;
    if (f->flags & ECUIO_FRAME_FLAG_EXTENDED) {
        id = (id & CAN_EFF_MASK) | CAN_EFF_FLAG;
    } else {
        id &= CAN_SFF_MASK;
    }
    bool ok = false;
    // An error report isn't a frame to send, and a message longer than one
    // frame needs ISO-TP framing, which play doesn't do
    if (!(f->flags & ECUIO_FRAME_FLAG_ERROR) && (len <= 8 || fd_length(len) != 0) &&
        (!fd || p->fd_frames)) {
        if (fd) {
            struct canfd_frame cf;
            std::memset(&cf, 0, sizeof cf);
            cf.can_id = id;
            cf.len = static_cast<uint8_t>(fd_length(len));
            cf.flags = static_cast<uint8_t>(
                ((f->flags & ECUIO_FRAME_FLAG_BITRATE_SWITCH) ? CANFD_BRS : 0) |
                ((f->flags & ECUIO_FRAME_FLAG_FD_ESI) ? CANFD_ESI : 0));
            std::memcpy(cf.data, f->data, len);
            ok = write(p->fd, &cf, sizeof cf) == static_cast<ssize_t>(sizeof cf);
        } else {
            struct can_frame cf;
            std::memset(&cf, 0, sizeof cf);
            cf.can_id =
                id | ((f->flags & ECUIO_FRAME_FLAG_REMOTE) ? CAN_RTR_FLAG : 0);
            cf.len = static_cast<uint8_t>(len);
            std::memcpy(cf.data, f->data, len);
            ok = write(p->fd, &cf, sizeof cf) == static_cast<ssize_t>(sizeof cf);
        }
    }
    (ok ? p->sent : p->skipped).fetch_add(1);
    p->covered.store(n, std::memory_order_release);
}

struct PlayArgs {
    const char* uri = nullptr;
    const char* to = nullptr;
    uint64_t count = 0;
    uint64_t seconds = 0;
};

int run_play(const PlayArgs& args)
{
    Output out;
    write_start(out);

    // Only a vcan interface, up: a frame from a simulation or a
    // recording never goes onto a real bus
    const std::vector<ecuio_can_interface_t> list = can_interfaces();
    const ecuio_can_interface_t* target = nullptr;
    for (const ecuio_can_interface_t& c : list) {
        if (std::strncmp(c.name, args.to, sizeof c.name) == 0) {
            target = &c;
        }
    }
    char why[200];
    why[0] = '\0';
    if (target == nullptr) {
        std::snprintf(why, sizeof why, "no CAN interface is named %s", args.to);
    } else if (std::strcmp(target->kind, "vcan") != 0) {
        std::snprintf(why, sizeof why,
                      "play writes only to a virtual CAN interface (vcan), "
                      "and %s is a %s",
                      args.to, target->kind);
    } else if (!(target->flags & ECUIO_CAN_UP)) {
        std::snprintf(why, sizeof why, "%s is down: sudo ip link set %s up",
                      args.to, args.to);
    }
    if (why[0] != '\0') {
        write_error(out, ECUIO_ERR_LOAD_FAILED, why);
        out.flush();
        return kExitConnect;
    }

    Player player;
    player.limit = args.count;
    player.fd_frames = (target->flags & ECUIO_CAN_FD) != 0;
    player.fd = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
    const int on = 1;
    struct sockaddr_can addr;
    std::memset(&addr, 0, sizeof addr);
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(if_nametoindex(args.to));
    if (player.fd < 0 ||
        (player.fd_frames && setsockopt(player.fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES,
                                        &on, sizeof on) != 0) ||
        bind(player.fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
        const std::string reason = std::generic_category().message(errno);
        std::snprintf(why, sizeof why, "%s: %s", args.to, reason.c_str());
        write_error(out, ECUIO_ERR_LOAD_FAILED, why);
        out.flush();
        if (player.fd >= 0) {
            close(player.fd);
        }
        return kExitConnect;
    }
    if (!player.fd_frames) {
        std::fprintf(stderr,
                     "havne-cli: %s takes classic frames only (MTU 16), so "
                     "frames over 8 bytes are skipped: sudo ip link set %s "
                     "down, then sudo ip link set %s mtu 72 up\n",
                     args.to, args.to, args.to);
    }

    ecuio_session_t* session = nullptr;
    std::string detail;
    ecuio_status_t st = create_within(out, args.uri, &session, &detail);
    if (st != ECUIO_OK) {
        write_error(out, st, detail.c_str());
        out.flush();
        close(player.fd);
        return kExitConnect;
    }
    ecuio_session_info_t info;
    std::memset(&info, 0, sizeof info);
    ecuio_session_get_info(session, &info);
    const bool replay = info.backend == ECUIO_BACKEND_REPLAY;
    const bool replay_ends =
        replay && !(info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP);
    write_session(out, info);
    {
        char buf[256];
        cli::Line l(buf, sizeof buf);
        l.raw("{\"type\":\"play\"");
        l.key("to");
        l.string(args.to, SIZE_MAX);
        l.key("fd");
        l.raw(player.fd_frames ? "true" : "false");
        l.ch('}');
        write_line(out, l);
    }
    out.flush();

    ecuio_register_frame_callback(session, on_play, &player);
    install_handlers();
    st = ecuio_session_start(session);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        ecuio_session_close(session);
        ecuio_session_destroy(session);
        close(player.fd);
        return kExitConnect;
    }

    const uint64_t started = ecuio_clock_us();
    const char* reason = nullptr;
    while (!reason) {
        std::this_thread::sleep_for(kPoll);
        const uint64_t covered = player.covered.load(std::memory_order_acquire);
        if (g_interrupted.load()) {
            reason = "interrupted";
        } else if (args.count && covered >= args.count) {
            reason = "count";
        } else if (replay_ends && replay_done(session, info, covered)) {
            reason = "replay-end";
        } else if (args.seconds &&
                   ecuio_clock_us() - started >= args.seconds * 1000000u) {
            reason = "seconds";
        }
        int32_t error = ECUIO_OK;
        uint32_t episodes = 0;
        ecuio_session_get_error_state(session, &error, &episodes);
        if (!reason && replay && error == ECUIO_ERR_RECORDING_CHANGED) {
            reason = "recording-changed"; /* stopped short of its end */
        }
    }
    ecuio_session_close(session);
    ecuio_session_destroy(session);
    close(player.fd);

    uint64_t covered = player.covered.load();
    if (args.count && covered > args.count) {
        covered = args.count;
    }
    char buf[256];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"end\"");
    l.key("reason");
    l.string(reason, SIZE_MAX);
    l.key("frames");
    l.u64(covered);
    l.key("sent");
    l.u64(player.sent.load());
    l.key("skipped");
    l.u64(player.skipped.load());
    l.ch('}');
    write_line(out, l);
    out.flush();
    return ended(out, kExitOk);
}
#endif

// A recording's frames read back as it recorded them (ADR-0049's
// appendix "frames read back"): a frame line each, written from core's
// callback on this thread, then the end line. Ctrl+C or a failed write
// stops the read; a refused row is an error line, exit 3, the rows
// before it already out.
struct FramesOut {
    Output* out = nullptr;
    char buf[kLineBytes];
};

int on_frame_row(void* user, const ecuio_frame_t* f, uint64_t host_us)
{
    auto* w = static_cast<FramesOut*>(user);
    cli::Line l(w->buf, sizeof w->buf);
    cli::frame_row_line(l, *f, host_us);
    write_line(*w->out, l);
    return w->out->failed || g_interrupted.load() ? 1 : 0;
}

int run_frames(const cli::FramesArgs& args)
{
    Output out;
    write_start(out);
    install_handlers();
    auto w = std::make_unique<FramesOut>();
    w->out = &out;
    ecuio_frames_read_t summary{};
    const ecuio_status_t rc = ecuio_recording_read_frames(
        args.folder, args.from_us, args.to_us, on_frame_row, w.get(), &summary);
    if (out.failed) {
        return ended(out, kExitOk); /* a closed pipe stays quiet */
    }
    if (rc != ECUIO_OK && rc != ECUIO_ERR_CANCELED) {
        write_error(out, rc);
        return ended(out, kExitConnect);
    }
    char buf[256];
    cli::Line l(buf, sizeof buf);
    cli::frames_end_line(l, rc == ECUIO_OK ? "read" : "interrupted", summary);
    write_line(out, l);
    return ended(out, kExitOk);
}

// A recording checked offline (ADR-0049 section 6): the frames check runs
// whatever the reader makes of the folder (one with frames.csv alone
// still has its frames checked), then the lines: what the reader found,
// each finding, the summary. A failed check is an error line, exit 3.
int run_info(const InfoArgs& args)
{
    Output out;
    std::unique_ptr<ecuio_recording_t, RecordingCloser> r;
    ecuio_recording_t* opened = nullptr;
    const ecuio_status_t open_rc =
        ecuio_recording_open(args.folder, nullptr, nullptr, &opened);
    r.reset(opened);
    const std::string open_detail = ecuio_last_error_detail();

    Findings found;
    ecuio_check_summary_t summary{};
    const ecuio_status_t rc = ecuio_recording_check(
        args.folder, &args.options, nullptr, keep_finding, &found, &summary);
    if (rc != ECUIO_OK) {
        write_start(out);
        if (found.out_of_memory) {
            write_error(out, ECUIO_ERR_OUT_OF_MEMORY,
                        "the findings didn't fit in memory");
        } else {
            write_error(out, rc);
        }
        return ended(out, kExitConnect);
    }

    write_start(out);
    char buf[2048];
    if (open_rc == ECUIO_OK) {
        ecuio_recording_info_t info{};
        ecuio_recording_get_info(r.get(), &info);
        ecuio_recording_signals_t signal_file{};
        ecuio_recording_get_signals(r.get(), &signal_file);
        cli::Line l(buf, sizeof buf);
        cli::opened_line(l, 0, info, signal_file);
        write_line(out, l);
        for (uint32_t k = 0; k < info.signal_count; ++k) {
            ecuio_recording_signal_t s{};
            ecuio_recording_get_signal(r.get(), k, &s);
            write_signal_line(out, r.get(), 0, k, s);
        }
        for (uint32_t k = 0; k < info.marker_count; ++k) {
            ecuio_recording_marker_t m{};
            ecuio_recording_get_marker(r.get(), k, &m);
            BigLine b(256 + static_cast<size_t>(m.label_length) * 6);
            cli::marker_line(b.line, 0, k, m);
            write_line(out, b.line);
        }
    } else {
        BigLine b(1024 + open_detail.size() * 6);
        cli::open_failed_line(b.line, 0, open_rc, open_detail.c_str());
        write_line(out, b.line);
    }
    for (const ecuio_check_finding_t& f : found.list) {
        cli::Line l(buf, sizeof buf);
        cli::finding_line(l, f);
        write_line(out, l);
    }
    cli::Line l(buf, sizeof buf);
    cli::summary_line(l, summary, args.options);
    write_line(out, l);
    return ended(out, kExitOk);
}

int run(int argc, char** argv)
{
#ifdef _WIN32
    // JSON lines end in \n on every platform
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) {
        usage(stderr);
        return kExitUsage;
    }
    const std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        usage(stdout);
        return kExitOk;
    }
    if (command == "--version") {
        std::printf("havne-cli %s\n", ecuio_version_string());
        return kExitOk;
    }
    if (command == "devices" && argc == 2) {
        return run_devices();
    }
    if (command == "vin" && argc == 3) {
        return run_vin(argv[2]);
    }
    if (command == "dtc" && argc == 3) {
        return run_dtc(argv[2]);
    }
    if (command == "view") {
        ViewArgs view;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            const bool has_value = i + 1 < argc;
            if (a == "--signal" && has_value) {
                view.signals.emplace_back(argv[++i]);
            } else if ((a == "--from" || a == "--to" || a == "--at") &&
                       has_value) {
                int64_t us = 0;
                if (!cli::parse_seconds(argv[++i], us)) {
                    std::fprintf(stderr, "havne-cli: %s takes seconds\n",
                                 a.c_str());
                    return kExitUsage;
                }
                if (a == "--from") {
                    view.from_us = us;
                    view.has_from = true;
                } else if (a == "--to") {
                    view.to_us = us;
                    view.has_to = true;
                } else {
                    view.at_us = us;
                    view.has_at = true;
                }
            } else if (a == "--columns" && has_value) {
                uint64_t n = 0;
                if (!parse_count(argv[++i], n) || n > 10000) {
                    std::fprintf(stderr,
                                 "havne-cli: --columns takes 1 to 10000\n");
                    return kExitUsage;
                }
                view.columns = static_cast<uint32_t>(n);
            } else if (a == "--align-on" && has_value) {
                view.align_text = argv[++i];
                if (!cli::parse_align_on(view.align_text, view.align)) {
                    std::fprintf(stderr,
                                 "havne-cli: --align-on takes "
                                 "rise:<signal>:<level>, "
                                 "fall:<signal>:<level>, marker:<label>, "
                                 "utc or engine-start\n");
                    return kExitUsage;
                }
            } else if (a == "--hysteresis" && has_value) {
                if (!cli::parse_decimal(argv[++i], view.hysteresis) ||
                    view.hysteresis < 0) {
                    std::fprintf(stderr,
                                 "havne-cli: --hysteresis takes 0 or more\n");
                    return kExitUsage;
                }
                view.has_hysteresis = true;
            } else if (a == "--frames") {
                view.frames = true;
            } else if (a == "--profile" && has_value) {
                view.profile = argv[++i];
            } else if (a == "--signals" && has_value) {
                view.signals_path = argv[++i];
            } else if (a == "--plugin" && has_value) {
                view.plugin_path = argv[++i];
            } else if (a.rfind("--", 0) == 0) {
                std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
                usage(stderr);
                return kExitUsage;
            } else {
                view.folders.push_back(argv[i]);
            }
        }
        if (view.folders.empty()) {
            std::fprintf(stderr,
                         "havne-cli: view needs a recording's folder\n");
            usage(stderr);
            return kExitUsage;
        }
        if (view.has_hysteresis && view.align.kind != cli::AlignOn::rise &&
            view.align.kind != cli::AlignOn::fall) {
            std::fprintf(stderr, "havne-cli: --hysteresis goes with "
                                 "--align-on rise: or fall:\n");
            return kExitUsage;
        }
        if (view.has_from && view.has_to && view.from_us >= view.to_us) {
            std::fprintf(stderr, "havne-cli: --from must come before --to\n");
            return kExitUsage;
        }
        if (!view.frames &&
            (!view.profile.empty() || view.signals_path != nullptr ||
             view.plugin_path != nullptr)) {
            std::fprintf(stderr, "havne-cli: --profile, --signals and "
                                 "--plugin go with --frames\n");
            return kExitUsage;
        }
        if (view.signals_path != nullptr && view.plugin_path != nullptr) {
            std::fprintf(stderr, "havne-cli: --signals or --plugin, not "
                                 "both\n");
            return kExitUsage;
        }
        return run_view(view);
    }
    if (command == "info") {
        InfoArgs info;
        info.options.gap_us = ECUIO_CHECK_DEFAULT_GAP_US;
        info.options.rate_drop_x100 = ECUIO_CHECK_DEFAULT_RATE_DROP_X100;
        info.options.settle_us = ECUIO_CHECK_DEFAULT_SETTLE_US;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            const bool has_value = i + 1 < argc;
            if ((a == "--gap" || a == "--settle") && has_value) {
                int64_t us = 0;
                if (!cli::parse_seconds(argv[++i], us) || us <= 0) {
                    std::fprintf(stderr,
                                 "havne-cli: %s takes seconds, more than 0\n",
                                 a.c_str());
                    return kExitUsage;
                }
                (a == "--gap" ? info.options.gap_us : info.options.settle_us) =
                    static_cast<uint64_t>(us);
            } else if (a == "--rate-drop" && has_value) {
                if (!cli::parse_rate_drop(argv[++i],
                                          info.options.rate_drop_x100)) {
                    std::fprintf(stderr, "havne-cli: --rate-drop takes a "
                                         "multiple of the median, more than "
                                         "1\n");
                    return kExitUsage;
                }
            } else if (a.rfind("--", 0) == 0) {
                std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
                usage(stderr);
                return kExitUsage;
            } else if (info.folder != nullptr) {
                std::fprintf(stderr, "havne-cli: info takes one recording\n");
                return kExitUsage;
            } else {
                info.folder = argv[i];
            }
        }
        if (info.folder == nullptr) {
            std::fprintf(stderr,
                         "havne-cli: info needs a recording's folder\n");
            usage(stderr);
            return kExitUsage;
        }
        return run_info(info);
    }
    if (command == "health") {
        cli::HealthArgs h;
        std::string why;
        if (!cli::parse_health(argc, argv, h, &why)) {
            std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
            usage(stderr);
            return kExitUsage;
        }
        if (!h.live) {
            return run_health_recording(h);
        }
        const char* uri = h.source;
        std::string opened;
        if (h.plugin != nullptr) {
            const int rc = use_plugin(h.plugin, uri);
            if (rc >= 0) {
                return rc;
            }
        } else if (h.signals != nullptr) {
            const int rc = use_signal_file(h.signals, uri, &opened);
            if (rc >= 0) {
                return rc;
            }
            uri = opened.c_str();
        } else {
            const int rc = use_recording_signals(uri, &opened);
            if (rc >= 0) {
                return rc;
            }
            if (!opened.empty()) {
                uri = opened.c_str();
            }
        }
        return run_health_session(h, uri);
    }
    if (command == "frames") {
        cli::FramesArgs fr;
        std::string why;
        if (!cli::parse_frames(argc, argv, fr, &why)) {
            std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
            usage(stderr);
            return kExitUsage;
        }
        return run_frames(fr);
    }
    if (command == "export") {
        cli::ExportArgs ex;
        std::string why;
        if (!cli::parse_export(argc, argv, ex, &why)) {
            std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
            usage(stderr);
            return kExitUsage;
        }
        // A named column the recording has no values for stays empty
        // (a focus pick that never answered is a column of a log): each is
        // said on stderr once the export is done, so a typo isn't silent,
        // and counted. The check runs first, so an interrupt during it
        // leaves nothing written
        std::vector<const char*> missing;
        const auto say_missing = [&missing] {
            for (const char* name : missing) {
                std::fprintf(stderr,
                             "havne-cli: the recording has no values for "
                             "'%s': its column is empty\n",
                             name);
            }
            return missing.size();
        };
        Output out;
        install_handlers();
        write_start(out);
        bool asked = false;
        if (ex.options.format == ECUIO_EXPORT_CSV &&
            ex.options.layout == ECUIO_CSV_WIDE && !ex.signals.empty() &&
            !missing_columns(ex, &asked, &missing)) {
            return ended_interrupted(out);
        }
        if (ex.estimate) {
            const std::vector<uint32_t> rows = cli::estimate_intervals(ex);
            std::vector<ecuio_export_estimate_t> sized(rows.size());
            const ecuio_status_t sizing = ecuio_export_estimate_with(
                &ex.options, rows.data(), static_cast<uint32_t>(rows.size()),
                export_interrupted, &asked, sized.data());
            if (sizing == ECUIO_ERR_CANCELED) {
                return ended_interrupted(out);
            }
            if (sizing != ECUIO_OK) {
                write_error(out, sizing);
                return ended(out, kExitConnect);
            }
            BigLine sizes(1024);
            cli::estimate_line(sizes.line, rows.data(), sized.data(),
                               sized.size(), say_missing());
            write_line(out, sizes.line);
            return ended(out, kExitOk);
        }
        ecuio_export_report_t report{};
        const ecuio_status_t rc =
            ecuio_export_with(&ex.options, export_interrupted, &asked, &report);
        if (rc == ECUIO_ERR_CANCELED) {
            return ended_interrupted(out);
        }
        BigLine b(cli::exported_line_bytes(ex));
        cli::Line& l = b.line;
        if (rc != ECUIO_OK) {
            if (report.failed_class != 0) {
                cli::export_failed_line(l, report);
                write_line(out, l);
            }
            write_error(out, rc);
            return ended(out, kExitConnect);
        }
        cli::exported_line(l, ex, report, say_missing());
        write_line(out, l);
        return ended(out, kExitOk);
    }
    if (command == "import") {
        cli::ImportArgs im;
        std::string why;
        if (!cli::parse_import(argc, argv, im, &why)) {
            std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
            usage(stderr);
            return kExitUsage;
        }
        if (im.print_map) {
            return print_guessed_map(im);
        }
        Output out;
        // Before the guess, so a Ctrl+C during it stops the import at its
        // first ask
        install_handlers();
        write_start(out);
        // A CSV's column map: core takes its text, up to the JSON reader's
        // 1 MiB, and refuses a larger one in its own words
        std::string map;
        if (im.options.format == ECUIO_IMPORT_CSV && im.map_path == nullptr) {
            // None given: guessed from the file, the guess written first,
            // so its notes come before an import's words
            std::string text;
            ecuio_csv_guess_t guess{};
            const ecuio_status_t grc = guess_map(im, &text, &guess, &why);
            if (grc != ECUIO_OK) {
                write_error(out, grc, why.c_str());
                return ended(out, kExitConnect);
            }
            {
                BigLine g(cli::guessed_line_bytes(text));
                cli::guessed_line(g.line, text);
                write_line(out, g.line);
            }
            if ((guess.flags & ECUIO_CSV_GUESS_TIME_UNIT_KNOWN) == 0) {
                // --time-unit gives a unit only to plain numbers
                write_error(out, ECUIO_ERR_LOAD_FAILED,
                            (guess.flags & ECUIO_CSV_GUESS_TIME_NUMBERS) != 0
                                ? "the guessed map's time has no unit: give "
                                  "--time-unit s, ms or us, or a map with "
                                  "--map"
                                : "the guessed map's time has no unit a "
                                  "guess can give: its cells need a map "
                                  "with the zone (or the date and offset), "
                                  "given with --map");
                return ended(out, kExitConnect);
            }
            map = text.substr(guess.map_offset, guess.map_length);
            im.options.column_map_utf8 = map.c_str();
        } else if (im.map_path != nullptr) {
            if (!read_whole(im.map_path, size_t{1} << 20, "column map", &map,
                            &why)) {
                write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
                return ended(out, kExitConnect);
            }
            im.options.column_map_utf8 = map.c_str();
        }
        ecuio_import_report_t report{};
        bool asked = false;
        const ecuio_status_t rc =
            ecuio_import_with(&im.options, import_interrupted, &asked, &report);
        if (rc == ECUIO_ERR_CANCELED) {
            return ended_interrupted(out);
        }
        if (rc != ECUIO_OK) {
            write_error(out, rc);
            return ended(out, kExitConnect);
        }
        BigLine b(cli::imported_line_bytes(im));
        cli::Line& l = b.line;
        cli::imported_line(l, im, report);
        write_line(out, l);
        return ended(out, kExitOk);
    }
    if (command == "play") {
#if HAVNE_CLI_PLAY
        PlayArgs play;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--to" && i + 1 < argc) {
                play.to = argv[++i];
            } else if ((a == "--count" || a == "--seconds") && i + 1 < argc) {
                const bool is_count = a == "--count";
                if (!(is_count ? parse_count(argv[++i], play.count)
                               : parse_run_seconds(argv[++i], play.seconds))) {
                    std::fprintf(stderr, "havne-cli: %s\n",
                                 is_count
                                     ? "--count takes a whole number above 0"
                                     : cli::run_seconds_why());
                    return kExitUsage;
                }
            } else if (a.rfind("--", 0) == 0 || play.uri) {
                std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
                usage(stderr);
                return kExitUsage;
            } else {
                play.uri = argv[i];
            }
        }
        if (!play.uri || !play.to) {
            std::fprintf(stderr,
                         "havne-cli: play needs a URI and --to <interface>\n");
            usage(stderr);
            return kExitUsage;
        }
        return run_play(play);
#else
        std::fprintf(stderr, "havne-cli: play needs Linux, where a CAN "
                             "interface is a network device\n");
        return kExitUsage;
#endif
    }
    if (command == "clear") {
        const char* uri = nullptr;
        bool yes = false;
        bool running = false;
        bool moving = false;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--yes") {
                yes = true;
            } else if (a == "--engine-running") {
                running = true;
            } else if (a == "--moving") {
                moving = true;
            } else if (a.rfind("--", 0) == 0 || uri != nullptr) {
                std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
                usage(stderr);
                return kExitUsage;
            } else {
                uri = argv[i];
            }
        }
        if (uri == nullptr) {
            std::fprintf(stderr, "havne-cli: clear needs a URI\n");
            usage(stderr);
            return kExitUsage;
        }
        if (!yes) {
            std::fprintf(stderr,
                         "havne-cli: clear erases the ECUs' trouble codes, "
                         "freeze frame and readiness: it needs --yes\n");
            return kExitUsage;
        }
        if (moving && !running) {
            std::fprintf(stderr, "havne-cli: --moving is a word on top of "
                                 "--engine-running's\n");
            return kExitUsage;
        }
        return run_clear_command(uri,
                                 (running ? ECUIO_CLEAR_ALLOW_RUNNING : 0u) |
                                     (moving ? ECUIO_CLEAR_ALLOW_MOVING : 0u));
    }
    if (command == "sim") {
        SimArgs sim;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--on" && i + 1 < argc) {
                sim.on = argv[++i];
            } else if (a == "--elm327" && i + 1 < argc) {
                sim.elm327 = argv[++i];
                if (!cli::parse_elm327_address(sim.elm327, &sim.elm327_at)) {
                    std::fprintf(stderr,
                                 "havne-cli: --elm327 takes tcp:<port> or "
                                 "tcp:<a.b.c.d>:<port>, a port 0-65535\n");
                    return kExitUsage;
                }
            } else if (a == "--desk-bus") {
                sim.desk_bus = true;
            } else if (a == "--signals" && i + 1 < argc) {
                sim.signals = argv[++i];
            } else if (a == "--plugin" && i + 1 < argc) {
                sim.plugin = argv[++i];
            } else if (a == "--seconds" && i + 1 < argc) {
                if (!parse_run_seconds(argv[++i], sim.seconds)) {
                    std::fprintf(stderr, "havne-cli: %s\n",
                                 cli::run_seconds_why());
                    return kExitUsage;
                }
            } else if (a.rfind("--", 0) == 0 || sim.uri) {
                std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
                usage(stderr);
                return kExitUsage;
            } else {
                sim.uri = argv[i];
            }
        }
        if (!sim.uri || (sim.on == nullptr) == (sim.elm327 == nullptr)) {
            std::fprintf(stderr, "havne-cli: sim needs a URI and either --on "
                                 "<interface> or --elm327 tcp:<port>\n");
            usage(stderr);
            return kExitUsage;
        }
        if (sim.elm327 != nullptr && sim.desk_bus) {
            std::fprintf(stderr, "havne-cli: --desk-bus is for --on: the "
                                 "ELM327 emulator is on no bus\n");
            usage(stderr);
            return kExitUsage;
        }
        if (sim.signals != nullptr && sim.plugin != nullptr) {
            std::fprintf(stderr, "havne-cli: --signals and --plugin each "
                                 "give the profile: one at a time\n");
            usage(stderr);
            return kExitUsage;
        }
        if (sim.plugin != nullptr) {
            const int rc = use_plugin(sim.plugin, sim.uri);
            if (rc >= 0) {
                return rc;
            }
        }
        std::string opened;
        if (sim.signals != nullptr) {
            const int rc = use_signal_file(sim.signals, sim.uri, &opened);
            if (rc >= 0) {
                return rc;
            }
            sim.uri = opened.c_str();
        }
        return sim.elm327 != nullptr ? run_sim_elm327(sim) : run_sim(sim);
    }
    if (command == "plugins") {
        const std::string word = argc > 2 ? argv[2] : "";
        if (word == "list" && argc > 3) {
            std::vector<const char*> paths;
            for (int i = 3; i < argc; ++i) {
                if (argv[i][0] == '-' && argv[i][1] == '-') {
                    std::fprintf(stderr, "havne-cli: unexpected '%s'\n",
                                 argv[i]);
                    usage(stderr);
                    return kExitUsage;
                }
                paths.push_back(argv[i]);
            }
            return run_plugins_list(paths);
        }
        const bool package =
            argc > 3 && std::string(argv[3]).rfind("--", 0) != 0;
        if (word == "check" && package && (argc == 4 || argc == 6)) {
            const char* on = nullptr;
            if (argc == 6) {
                if (std::string(argv[4]) != "--on" || *argv[5] == '\0') {
                    std::fprintf(stderr, "havne-cli: plugins check takes "
                                         "<pkg> [--on <vcan>]\n");
                    usage(stderr);
                    return kExitUsage;
                }
                on = argv[5];
            }
            return run_plugins_check(argv[3], on);
        }
        if (word == "match" && package && argc == 6 &&
            std::string(argv[4]) == "--vin") {
            return run_plugins_match(argv[3], argv[5]);
        }
        std::fprintf(stderr, "havne-cli: plugins takes list <pkg>..., check "
                             "<pkg> [--on <vcan>], or match <pkg> --vin "
                             "<VIN>\n");
        usage(stderr);
        return kExitUsage;
    }
    if (command == "signals") {
        const std::string word = argc > 2 ? argv[2] : "";
        if (word == "check" && argc == 4) {
            return run_signals_check(argv[3]);
        }
        if (word == "import" && argc > 4 && std::string(argv[3]) == "obdb") {
            ImportArgs im;
            im.file = argv[4];
            for (int i = 5; i < argc; ++i) {
                const std::string a = argv[i];
                const bool has_value = i + 1 < argc;
                if (a == "--out" && has_value) {
                    im.out = argv[++i];
                } else if (a == "--year" && has_value) {
                    uint64_t year = 0;
                    if (!parse_count(argv[++i], year) || year < 1980 ||
                        year > 2100) {
                        std::fprintf(stderr, "havne-cli: --year takes a model "
                                             "year, 1980 to 2100\n");
                        return kExitUsage;
                    }
                    im.options.model_year = static_cast<uint32_t>(year);
                } else if (a == "--name" && has_value) {
                    im.options.name = argv[++i];
                } else if (a == "--url" && has_value) {
                    im.options.url = argv[++i];
                } else if (a == "--commit" && has_value) {
                    im.options.commit = argv[++i];
                } else if (a == "--alone") {
                    im.options.flags |= ECUIO_OBDB_ALONE;
                } else {
                    std::fprintf(stderr, "havne-cli: unexpected '%s'\n",
                                 argv[i]);
                    usage(stderr);
                    return kExitUsage;
                }
            }
            if (im.out == nullptr || *im.out == '\0') {
                std::fprintf(
                    stderr,
                    "havne-cli: signals import obdb needs --out <file>\n");
                usage(stderr);
                return kExitUsage;
            }
            return run_signals_import(im);
        }
        std::fprintf(stderr, "havne-cli: signals takes check <file>, or import "
                             "obdb <file> --out <file>\n");
        usage(stderr);
        return kExitUsage;
    }
    if (command == "log") {
        cli::LogArgs la;
        std::string why;
        if (!cli::parse_log(argc, argv, la, &why)) {
            std::fprintf(stderr, "havne-cli: %s\n", why.c_str());
            usage(stderr);
            return kExitUsage;
        }
        std::string opened;
        const int rc =
            take_profile_source(la.signals, la.plugin, &la.uri, &opened);
        if (rc >= 0) {
            return rc;
        }
        return run_log(la);
    }
    if (command != "stream") {
        usage(stderr);
        return kExitUsage;
    }
    StreamArgs args;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--values") {
            args.values = true;
        } else if (a == "--until-scanned") {
            args.until_scanned = true;
        } else if (a == "--count" && i + 1 < argc) {
            if (!parse_count(argv[++i], args.count)) {
                std::fprintf(
                    stderr,
                    "havne-cli: --count takes a whole number above 0\n");
                return kExitUsage;
            }
        } else if (a == "--seconds" && i + 1 < argc) {
            if (!parse_run_seconds(argv[++i], args.seconds)) {
                std::fprintf(stderr, "havne-cli: %s\n", cli::run_seconds_why());
                return kExitUsage;
            }
        } else if (a == "--record" && i + 1 < argc) {
            args.record = argv[++i];
            if (*args.record == '\0') {
                std::fprintf(stderr, "havne-cli: --record takes a folder\n");
                return kExitUsage;
            }
        } else if (a == "--label" && i + 1 < argc) {
            args.label = argv[++i];
        } else if (a == "--signals" && i + 1 < argc) {
            args.signals = argv[++i];
        } else if (a == "--plugin" && i + 1 < argc) {
            args.plugin = argv[++i];
        } else if (a.rfind("--", 0) == 0 || args.uri) {
            std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
            usage(stderr);
            return kExitUsage;
        } else {
            args.uri = argv[i];
        }
    }
    if (!args.uri) {
        std::fprintf(stderr, "havne-cli: stream needs a URI\n");
        usage(stderr);
        return kExitUsage;
    }
    if (args.label && !args.record) {
        std::fprintf(stderr, "havne-cli: --label names a --record's folder\n");
        usage(stderr);
        return kExitUsage;
    }
    if (args.signals != nullptr && args.plugin != nullptr) {
        std::fprintf(stderr, "havne-cli: --signals and --plugin each give "
                             "the profile: one at a time\n");
        usage(stderr);
        return kExitUsage;
    }
    std::string opened;
    const int rc =
        take_profile_source(args.signals, args.plugin, &args.uri, &opened);
    if (rc >= 0) {
        return rc;
    }
    return run_stream(args);
}

} // namespace

#ifdef _WIN32
// Windows hands the arguments over as UTF-16 here; main's char** would be
// the ANSI code page's, where a path with a character outside it (an
// accented user name's folder in another code page, Greek, CJK) comes as
// '?'. Each is UTF-8 from here on, as core's paths are.
int wmain(int argc, wchar_t** wargv)
{
    try {
        std::vector<std::string> args(static_cast<size_t>(argc));
        std::vector<char*> argv(static_cast<size_t>(argc) + 1, nullptr);
        for (int i = 0; i < argc; ++i) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr,
                                              0, nullptr, nullptr);
            if (n > 0) {
                args[i].resize(static_cast<size_t>(n));
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &args[i][0], n,
                                    nullptr, nullptr);
                args[i].resize(static_cast<size_t>(n - 1));
            }
            argv[i] = &args[i][0];
        }
        return run(argc, argv.data());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "havne-cli: %s\n", e.what());
        return kExitFailed;
    }
}
#else
int main(int argc, char** argv)
{
    // What throws (an allocation, a thread that won't start) ends as the
    // ring's out of memory does: a CLI failure with its reason, not
    // std::terminate
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "havne-cli: %s\n", e.what());
        return kExitFailed;
    }
}
#endif
