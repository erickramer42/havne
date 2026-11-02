// File/layer: cli/main.cpp | havne-cli, a command-line client over the C ABI (ADR-0031)
// Testing: HARNESS: golden runs of the program (CliGoldenTest, against
//          tests/golden/cli/: the replay fixture, mock:// with its
//          diagnostics line, the refusals, --until-scanned with nothing
//          to scan),
//          its usage errors (CliUsageTest), the connect's deadline
//          (CliConnectTest, Windows, the fake DLL) and, on POSIX, SIGINT and
//          SIGTERM (CliSignalTest), the absent line (CliReplayTest,
//          tests/cli_absent.py); the formatting, the ring and the
//          sleep rule UNIT (tests/test_cli.cpp); a real sleep BENCH
//          (BENCH prep #27);
//          the timing lines UNIT (CliTimingTest) and by hand against the fake
//          DLL (docs/testing.md); the vin command in the golden run and
//          CliUsageTest, the vin and identity lines UNIT (CliVinTest),
//          stream's identity line by hand against the fake DLL; the dtc
//          command in the golden run and CliUsageTest; the sim
//          command's lines UNIT (CliSimTest), its usage errors
//          (CliUsageTest), on vcan0 by hand (ADR-0046); main's
//          catch by reading and clang-tidy
//          (bugprone-exception-escape)
//
// Public headers only: what any program linking core can do.

#include "diagnostics_line.h"
#include "timing_line.h"
#include "vin_line.h"
#include "jsonl.h"
#include "line_ring.h"
#include "scan_line.h"
#include "sim_line.h"
#include "sleep_watch.h"

#include "ecuio/core.h"
#include "ecuio/j2534.h"
#include "ecuio/profile.h"
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

unsigned long connect_ms()
{
    char env[32] = {};
#ifdef _WIN32
    const DWORD n =
        GetEnvironmentVariableA("HAVNE_CLI_CONNECT_MS", env, sizeof env);
    if (n == 0 || n >= sizeof env) {
        return kConnectMs;
    }
#else
    const char* value = std::getenv("HAVNE_CLI_CONNECT_MS");
    if (!value || std::strlen(value) >= sizeof env) {
        return kConnectMs;
    }
    std::memcpy(env, value, std::strlen(value));
#endif
    const unsigned long v = std::strtoul(env, nullptr, 10);
    return v >= 100 ? v : kConnectMs;
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

void write_line(Output& out, cli::Line& l)
{
    if (l.finish()) {
        out.write(l.data(), l.size());
    }
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
// error line and exit 3 at once; the process's end ends the surrogate
// with it (its job object), wherever the connect was held.
ecuio_status_t create_within(Output& out, const char* uri,
                             ecuio_session_t** session, std::string* detail)
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

// A scan line per ECU whose DID scan moved (cli/scan_line.h), every
// change when final; the channels' reports into reports[], for
// --until-scanned
void poll_scan(Output& out, ecuio_session_t* session, uint32_t channels,
               cli::ScanSeen* seen, ecuio_channel_support_t* reports,
               bool final = false)
{
    const uint64_t now = ecuio_clock_us();
    for (uint32_t c = 0; c < channels; ++c) {
        ecuio_channel_support_t& s = reports[c];
        if (ecuio_session_get_channel_support(session, c, &s) != ECUIO_OK) {
            s = ecuio_channel_support_t{};
            continue;
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

void write_signals(Output& out, const ecuio_signal_set_t* set)
{
    const uint32_t count = set ? set->count : 0;
    BigLine b(256 + size_t{count} * 512);
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
        l.ch('}');
    }
    l.ch(']');
    l.ch('}');
    write_line(out, l);
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
        if (v.text) {
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
        "                         [--until-scanned]\n"
        "       havne-cli play <uri> --to <vcan> [--count N] [--seconds S]\n"
        "       havne-cli sim <uri> --on <vcan> [--seconds S]\n"
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
        "play     (Linux) writes a session's frames to a virtual CAN "
        "interface\n"
        "         as they arrive: mock://demo drives the simulated car on it.\n"
        "sim      (Linux) answers OBD-II requests on a virtual CAN interface\n"
        "         as an engine ECU: the simulated car's, "
        "mock://sim[?values=drive|\n"
        "         canned][&vin=<VIN>][&faults=silent:<n>,...], or a "
        "recording's,\n"
        "         replay://<absolute path>[?loop=1][&vin=<VIN>][&faults=...].\n"
        "         Ends at Ctrl+C or after S seconds.\n"
        "devices  lists the serial ports, J2534 adapters and CAN interfaces a\n"
        "         URI can name.\n"
        "vin      decodes a VIN on this machine: its model year, make and\n"
        "         check digit (49 CFR 565, NHTSA vPIC's manufacturer codes).\n"
        "dtc      names a trouble code (P0301): SAE J2012's title for a\n"
        "         generic powertrain or network code.\n"
        "\n"
        "Exit: 0 the stream ended, 1 the CLI failed, 2 usage, 3 the connect "
        "failed.\n");
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

struct StreamArgs {
    const char* uri = nullptr;
    bool values = false;
    uint64_t count = 0;
    uint64_t seconds = 0;
    bool until_scanned = false;
};

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
    ecuio_channel_support_t scan_reports[ECUIO_SESSION_INFO_MAX_CHANNELS] = {};
    poll_scan(out, session, channels, scan_seen, scan_reports);
    // Judged from the values core decodes, so only with --values
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    std::vector<uint16_t> absent_read(
        (args.values && set != nullptr && set->defs != nullptr) ? set->count
                                                                : 0u);
    std::vector<uint16_t> absent_last;
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
        return kExitConnect;
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
        poll_scan(out, session, channels, scan_seen, scan_reports);
        poll_absent(out, session, absent_read, absent_last);
        poll_timing(out, session, info, timing_snaps, timing_rows, false);

        int32_t now_error = ECUIO_OK;
        uint32_t now_episodes = 0;
        ecuio_session_get_error_state(session, &now_error, &now_episodes);
        if (now_error != last_error || now_episodes != episodes) {
            last_error = now_error;
            episodes = now_episodes;
            char buf[256];
            cli::Line l(buf, sizeof buf);
            l.raw("{\"type\":\"health\"");
            l.key("code");
            l.u64(static_cast<uint32_t>(now_error));
            l.key("message");
            l.string(
                ecuio_status_string(static_cast<ecuio_status_t>(now_error)),
                SIZE_MAX);
            l.key("episodes");
            l.u64(now_episodes);
            l.ch('}');
            write_line(out, l);
        }
        // A replay whose file changed has stopped, short of its last
        // frame: the health line above says so, then the stream ends
        if (!reason && replay && now_error == ECUIO_ERR_RECORDING_CHANGED) {
            reason = "recording-changed";
        }
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
    while (!closed.load()) {
        const size_t n = stream->ring.drain(drained.data(), kPoll);
        out.write(drained.data(), n);
        if (out.failed) {
            stream->ring.stop();
        }
    }
    closer.join();
    out.write(drained.data(),
              stream->ring.drain(drained.data(), std::chrono::milliseconds(0)));

    int code = kExitOk;
    if (!out.failed) {
        poll_scan(out, session, channels, scan_seen, scan_reports, true);
        poll_timing(out, session, info, timing_snaps, timing_rows, true);
        uint64_t covered = stream->covered.load();
        if (args.count && covered > args.count) {
            covered = args.count;
        }
        char buf[512];
        cli::Line l(buf, sizeof buf);
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
        write_line(out, l);
        out.flush();
    }
    if (!out.ok()) {
        code = kExitFailed;
    }
    ecuio_session_destroy(session);
#ifdef _WIN32
    g_done.store(true);
#endif
    return code;
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

struct SimArgs {
    const char* uri = nullptr;
    const char* on = nullptr;
    uint64_t seconds = 0;
};

// The simulated ECU (ADR-0046) answering on a virtual bus until Ctrl+C,
// --seconds or the bus failing: start, sim, a sim_stats line every 10 s
// while the counts change, end with them
int run_sim(const SimArgs& args)
{
    install_handlers();
    Output out;
    write_start(out);
    ecuio_sim_t* sim = nullptr;
    const ecuio_status_t st = ecuio_sim_create(args.uri, args.on, &sim);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        return kExitConnect;
    }
    {
        char buf[1024];
        cli::Line l(buf, sizeof buf);
        cli::sim_line(l, args.uri, args.on);
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
        return out.ok() ? kExitOk : kExitFailed;
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
    return out.ok() ? kExitOk : kExitFailed;
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
    return out.ok() ? kExitOk : kExitFailed;
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
    return out.ok() ? kExitOk : kExitFailed;
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
        l.raw("{\"type\":\"can\"");
        l.key("name");
        l.string(c.name, sizeof c.name);
        l.key("kind");
        l.string(c.kind, sizeof c.kind);
        l.key("up");
        l.raw((c.flags & ECUIO_CAN_UP) ? "true" : "false");
        l.key("listen_only");
        l.raw((c.flags & ECUIO_CAN_LISTEN_ONLY) ? "true" : "false");
        l.key("fd");
        l.raw((c.flags & ECUIO_CAN_FD) ? "true" : "false");
        l.key("bitrate");
        l.u64(c.bitrate);
        l.key("uri");
        const std::string uri = std::string("socketcan://") + c.name;
        l.string(uri.c_str(), SIZE_MAX);
        l.ch('}');
        write_line(out, l);
    }
    out.flush();
    return out.ok() ? kExitOk : kExitFailed;
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
            cf.flags =
                (f->flags & ECUIO_FRAME_FLAG_BITRATE_SWITCH) ? CANFD_BRS : 0;
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
    return out.ok() ? kExitOk : kExitFailed;
}
#endif

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
    if (command == "play") {
#if HAVNE_CLI_PLAY
        PlayArgs play;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--to" && i + 1 < argc) {
                play.to = argv[++i];
            } else if ((a == "--count" || a == "--seconds") && i + 1 < argc) {
                if (!parse_count(argv[++i], a == "--count" ? play.count
                                                           : play.seconds)) {
                    std::fprintf(stderr,
                                 "havne-cli: %s takes a whole number above 0\n",
                                 a.c_str());
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
    if (command == "sim") {
        SimArgs sim;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--on" && i + 1 < argc) {
                sim.on = argv[++i];
            } else if (a == "--seconds" && i + 1 < argc) {
                if (!parse_count(argv[++i], sim.seconds)) {
                    std::fprintf(
                        stderr,
                        "havne-cli: --seconds takes a whole number above 0\n");
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
        if (!sim.uri || !sim.on) {
            std::fprintf(stderr,
                         "havne-cli: sim needs a URI and --on <interface>\n");
            usage(stderr);
            return kExitUsage;
        }
        return run_sim(sim);
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
            if (!parse_count(argv[++i], args.seconds)) {
                std::fprintf(
                    stderr,
                    "havne-cli: --seconds takes a whole number above 0\n");
                return kExitUsage;
            }
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
    return run_stream(args);
}

} // namespace

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
