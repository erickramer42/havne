// File/layer: cli/cmd_small.cpp | havne-cli, clear (ADR-0038), vin, dtc and devices
// Testing: UNIT: CliGoldenTest, CliClearRunTest, CliUsageTest (ctest);
//          CliClearTest, CliVinTest, CliDtcTest, CliCanTest (test_cli)
//
// Public headers only: what any program linking core can do.

#include "can_line.h"
#include "clear_run.h"
#include "diagnostics_line.h"
#include "vin_line.h"
#include "jsonl.h"

#include "commands.h"
#include "session_lines.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/j2534.h"
#include "ecuio/serial.h"
#include "ecuio/socketcan.h"
#include "ecuio/vin.h"

namespace cmd {

namespace {

// How long clear waits for each step: the reports read, each clear's
// outcome, the read after it
constexpr auto kClearWait = std::chrono::seconds(30);

} // namespace

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

} // namespace cmd
