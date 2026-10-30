// File/layer: cli/main.cpp | havne-cli, a command-line client over the C ABI (ADR-0031)
// Testing: HARNESS: golden runs of the program (CliGoldenTest, against
//          tests/golden/cli/: the replay fixture, mock:// with its
//          diagnostics line, the refusals),
//          its usage errors (CliUsageTest) and, on POSIX, SIGINT and
//          SIGTERM (CliSignalTest); the formatting, the ring and the
//          sleep rule UNIT (tests/test_cli.cpp); a real sleep BENCH
//          (BENCH prep #27)
//
// Public headers only: what any program linking core can do.

#include "diagnostics_line.h"
#include "jsonl.h"
#include "line_ring.h"
#include "sleep_watch.h"

#include "ecuio/core.h"
#include "ecuio/j2534.h"
#include "ecuio/profile.h"
#include "ecuio/serial.h"
#include "ecuio/signals.h"
#include "ecuio/socketcan.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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
                 const char* detail = nullptr)
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

// A line for each ECU whose diagnostics read has completed since the
// last call: generations holds each channel's last written
void poll_diagnostics(Output& out, ecuio_session_t* session, uint32_t channels,
                      uint32_t* generations)
{
    for (uint32_t c = 0; c < channels; ++c) {
        ecuio_channel_diagnostics_t d;
        if (ecuio_session_get_channel_diagnostics(session, c, &d) == ECUIO_OK &&
            (d.flags & ECUIO_DIAG_COMPLETE) && d.generation != generations[c]) {
            generations[c] = d.generation;
            write_diagnostics(out, d);
        }
    }
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
        "       havne-cli play <uri> --to <vcan> [--count N] [--seconds S]\n"
        "       havne-cli devices\n"
        "       havne-cli --version | --help\n"
        "\n"
        "stream   opens the session <uri> names (mock://, replay://, "
        "elm327://,\n"
        "         j2534:// on Windows, socketcan:// on Linux) and writes its\n"
        "         frames as JSON lines;\n"
        "         --values adds the values its profile decodes. Ends at "
        "Ctrl+C,\n"
        "         after N frames, after S seconds, or at a replay's last "
        "frame.\n"
        "play     (Linux) writes a session's frames to a virtual CAN "
        "interface\n"
        "         as they arrive: mock://demo drives the simulated car on it.\n"
        "devices  lists the serial ports, J2534 adapters and CAN interfaces a\n"
        "         URI can name.\n"
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
};

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
    ecuio_status_t st = ecuio_session_create(args.uri, &session);
    if (st != ECUIO_OK) {
        write_error(out, st);
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
    poll_diagnostics(out, session, channels, diag_generations);
    if (args.values) {
        write_signals(out, ecuio_session_signal_set(session));
    }
    out.flush();

    ecuio_register_frame_callback(session, on_frame, stream.get());
    if (args.values) {
        ecuio_register_values_callback(session, on_values, stream.get());
    }
    install_handlers();
    st = ecuio_session_start(session);
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
    const char* reason = nullptr;
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
        } else if (replay_ends && covered >= info.replay_frames) {
            reason = "replay-end";
        } else if (args.seconds &&
                   ecuio_clock_us() - started >= args.seconds * 1000000u) {
            reason = "seconds";
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

        poll_diagnostics(out, session, channels, diag_generations);

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
        uint64_t covered = stream->covered.load();
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
        l.key("dropped_frames");
        l.u64(stream->ring.dropped_frames());
        l.key("dropped_values");
        l.u64(stream->ring.dropped_values());
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
// play (ADR-0032 §8): a session's frames written to a virtual CAN
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
    ecuio_status_t st = ecuio_session_create(args.uri, &session);
    if (st != ECUIO_OK) {
        write_error(out, st);
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
        } else if (replay_ends && covered >= info.replay_frames) {
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

} // namespace

int main(int argc, char** argv)
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
    if (command != "stream") {
        usage(stderr);
        return kExitUsage;
    }
    StreamArgs args;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--values") {
            args.values = true;
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
