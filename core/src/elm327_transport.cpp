// File/layer: core/src/elm327_transport.cpp | C++17, Qt-free, every
//             platform (ADR-0026)
// Testing: HARNESS — Elm327SessionTest (test_elm327) through the fake
//          ELM327 behind the port opener (tests/fake_elm327.h): the URI
//          and its refusals, the setup and its refusals, the session
//          info and the readings' "not present", the requests (the count
//          digit, header switches, the rotation, the probe, the DID scan,
//          identification), multi-frame replies, link health and its
//          recovery (NO DATA, CAN ERROR, a dropped link, an adapter
//          reset), the pending reply, passive listening (Listening*:
//          AT MA, the channels' messages, the quiet run, the liveness
//          check, BUFFER FULL's overflow flag, the monitor's ends and the
//          stop at detach), the wire log (TheWireLog*), and nothing
//          allocated per frame, with the log's request lines on or off.
//          Elm327PortTest (POSIX): a whole session over a
//          pseudo-terminal through the OS's port. Elm327TcpTest (every
//          platform): a session over a loopback socket, the far side's
//          close and its recovery. Elm327StreamTest (every platform): a
//          session through an app-fed stream (ADR-0028), its failure and
//          reopen, the open's wait and its stop. BENCH: the Win32 port,
//          the VX's WiFi.

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/j2534_ptypes.h" /* ECUIO_PT_PROTOCOL_ISO15765; plain C */
#include "session.h"
#include "elm327_log.h"
#include "elm327_port.h"
#include "elm327_tcp.h"
#include "elm327_stream.h"
#include "elm327_protocol.h"
#include "request_engine.h"
#include "request_setup.h"
#include "link_health.h"
#include "uri_query.h"
#include "error_detail.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <thread>

/* The query keys attach reads, for create's check (internal.h). */
extern "C" const char* const ecuio_elm327_query_keys[] = {
    "baud",  "serial",   "filter",  "txid", "poll",
    "probe", "identify", "profile", "did",  nullptr};

namespace {

/* The opener ecuio_elm327_set_port_opener installed; nullptr is the
 * OS's. */
std::atomic<ecuio_elm327_port_open_t> g_opener{nullptr};

/* The serial rate a wired ELM327 starts at with its rate pin high; the
 * other is 9600 (ADR-0026 §2). */
constexpr uint32_t ELM327_SERIAL_DEFAULT = 38400;
constexpr uint32_t ELM327_SERIAL_MIN = 300;
constexpr uint32_t ELM327_SERIAL_MAX = 4000000;
constexpr unsigned long ELM327_CAN_BAUD_DEFAULT = 500000;

/* No line and no prompt this long: the adapter, or the link to it, has
 * failed (ADR-0026 §6). Counted from the last line, so a pending reply's
 * wait (an STN listening past 7F xx 78) never runs into it. */
constexpr uint32_t ELM327_PROMPT_TIMEOUT_MS = 5000;

/* A reply is read in slices of this, so a close is seen within one. */
constexpr uint32_t ELM327_READ_SLICE_MS = 20;

/* With nothing left to ask the worker listens this long a pass, as the
 * J2534 worker's passive reads do, so a quiet run takes the same time;
 * waiting for a request's floor or wait, at most the slice. A monitor
 * counts each window this long with no message toward the quiet run. */
constexpr uint32_t ELM327_IDLE_READ_MS = 100;
constexpr uint32_t ELM327_WAIT_SLICE_MS = 10;

/* A monitor with no line this long is stopped, to show the adapter is
 * there; its prompt is waited for as long. At detach the stop's prompt
 * is waited for this long, then the port closes either way. */
constexpr uint32_t ELM327_MONITOR_CHECK_MS = 5000;
constexpr uint32_t ELM327_MONITOR_DETACH_MS = 1000;

/* Commands and requests, CR included. */
constexpr size_t ELM327_CMD_MAX = 24;

/* What a refusal's detail quotes of the adapter's answer. */
constexpr size_t ELM327_QUOTE_MAX = 40;

/* How the read to a prompt ended. */
enum class elm327_io {
    prompt,  /* the prompt: the adapter takes the next command */
    silent,  /* no line and no prompt for ELM327_PROMPT_TIMEOUT_MS */
    failed,  /* the port failed */
    stopped, /* the session is closing */
    reset    /* the adapter reset on its own (ADR-0026 §6) */
};

struct ecuio_elm327_transport {
    ecuio_session_t* session = nullptr;
    char port_name[ELM327_PORT_NAME_MAX + 1] = {};
    uint32_t serial_baud = ELM327_SERIAL_DEFAULT;
    unsigned long can_baud = ELM327_CAN_BAUD_DEFAULT;
    bool ext29 = false;  /* the channels' addressing mode */
    char protocol = '6'; /* AT SP's: the mode at the CAN rate */
    /* port_name is a WiFi adapter's address, or an app-fed stream's name
     * (ADR-0028); ?serial= was in the URI */
    bool tcp = false;
    bool stream = false;
    bool serial_given = false;
    /* The opener attach used; the worker reopens through it. A stream
     * opens through the registry instead (open_port). */
    ecuio_elm327_port_open_t opener = nullptr;
    ecuio_elm327_port_t port{};
    bool port_open = false;
    /* The setup has run since the port opened and the adapter last
     * reset; the transmit ID AT SH last set. */
    bool configured = false;
    bool header_set = false;
    uint32_t header_tx = 0;
    uint32_t rx_ids[ECUIO_MAX_CHANNELS] = {};
    uint32_t tx_ids[ECUIO_MAX_CHANNELS] = {};
    uint32_t chan_count = 0;
    /* The requests and their pacing (request_engine.h), built at attach,
     * the worker's alone after; probe and identify as for J2534. */
    ecuio_request_engine engine;
    bool probe = false;
    bool identify = false;
    /* Bytes read and not yet taken, the line under way, and the
     * messages under way (32 KB, so here rather than on a stack). */
    uint8_t rx_buf[256] = {};
    uint32_t rx_len = 0;
    uint32_t rx_pos = 0;
    elm327_reader reader;
    elm327_isotp isotp;
    /* Session time counts from the end of attach, as mock's does; the
     * frame dispatched from here. */
    std::chrono::steady_clock::time_point epoch{};
    /* The last command's send, for its prompt's time in the wire log */
    std::chrono::steady_clock::time_point sent_at{};
    ecuio_frame_t rx_frame{};
    std::thread worker;
    elm327_log log; /* the wire log (elm327_log.h) */
    std::atomic<bool> stop_requested{false};
    /* A request's exchange, or a monitor, is under way: the wire log
     * takes its lines only with frame logging */
    bool in_ask = false;
    /* BUFFER FULL came: the next frame dispatched carries
     * ECUIO_FRAME_FLAG_OVERFLOW */
    bool overflow_pending = false;

    ecuio_elm327_transport() = default;
    ecuio_elm327_transport(const ecuio_elm327_transport&) = delete;
    ecuio_elm327_transport& operator=(const ecuio_elm327_transport&) = delete;
    ~ecuio_elm327_transport() { elm327_log_close(log); }
};

using transport = ecuio_elm327_transport;

int64_t now_us(const transport* t)
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t->epoch)
            .count());
}

/* A command without its CR, for the echo test and a detail. */
void bare_command(const char* cmd, char* out)
{
    size_t n = 0;
    for (; cmd[n] != '\0' && cmd[n] != '\r' && n + 1 < ELM327_CMD_MAX; ++n) {
        out[n] = cmd[n];
    }
    out[n] = '\0';
}

/* src into dst (cap bytes), cut to fit, always terminated. */
void copy_text(char* dst, size_t cap, const char* src)
{
    size_t n = 0;
    for (; src[n] != '\0' && n + 1 < cap; ++n) {
        dst[n] = src[n];
    }
    dst[n] = '\0';
}

/* Whether the exchange under way goes in the wire log: all but a
 * request's, which goes in only with frame logging (elm327_log.h) */
bool logging(const transport* t)
{
    return elm327_log_is_open(t->log) && (!t->in_ask || t->log.requests);
}

/* A line read, as the log shows it: "<" for a command's, "unasked:"
 * when no command was out (sent nullptr), which always goes in */
void log_line(transport* t, const char* sent, const elm327_reader& r)
{
    if (!elm327_log_is_open(t->log) || (sent != nullptr && !logging(t))) {
        return;
    }
    char text[ELM327_LINE_MAX * 4 + 1];
    elm327_log_text(r.line, r.len, text, sizeof(text));
    elm327_log_write(t->log, "%s %s%s", sent == nullptr ? "unasked:" : "<",
                     text,
                     r.overflow ? " (cut: longer than a line holds)" : "");
}

/* A prompt: a command's with its time since the send, to a tenth of a
 * millisecond; one with no command out is dropped, and always logged */
void log_prompt(transport* t, const char* sent)
{
    if (sent == nullptr) {
        elm327_log_write(t->log, "unasked prompt, dropped");
        return;
    }
    if (!logging(t)) {
        return;
    }
    const long long us = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t->sent_at)
            .count());
    elm327_log_write(t->log, "prompt %lld.%lld ms after %s", us / 1000,
                     (us % 1000) / 100, sent);
}

/* Feeds what has been read to the reader, calling on_line for each line
 * that isn't an echo of sent (nullptr: no echo expected), until the
 * prompt (true) or the bytes run out (false). Every line and prompt goes
 * to the wire log, echoes too. */
template <typename OnLine>
bool take_bytes(transport* t, const char* sent, OnLine& on_line, bool* line)
{
    while (t->rx_pos < t->rx_len) {
        const elm327_step s = elm327_take(t->reader, t->rx_buf[t->rx_pos++]);
        if (s.line) {
            *line = true;
            log_line(t, sent, t->reader);
            if (sent == nullptr ||
                !elm327_is_echo(t->reader.line, t->reader.len, sent)) {
                on_line(t->reader);
            }
        }
        if (s.prompt) {
            log_prompt(t, sent);
            return true;
        }
    }
    return false;
}

/* What arrived while no command was out: nothing should have, so a line
 * here is unsolicited. Reads what's there, waiting up to timeout_ms for
 * the first byte; a stray prompt is dropped, so it can never end the
 * next command's reply early. reset when a line says the adapter reset,
 * failed when the port did; otherwise prompt. */
elm327_io drain(transport* t, uint32_t timeout_ms)
{
    bool reset = false;
    auto on_line = [&](const elm327_reader& r) {
        reset = reset ||
                elm327_classify(r, t->ext29, nullptr) == elm327_line::reset;
    };
    bool line = false;
    while (take_bytes(t, nullptr, on_line, &line)) {
    }
    uint32_t wait = timeout_ms;
    for (;;) {
        const int32_t n =
            t->port.ops->read(t->port.impl, t->rx_buf, sizeof(t->rx_buf), wait);
        if (n < 0) {
            elm327_log_write(t->log, "the port failed (a read with no "
                                     "command out)");
            return elm327_io::failed;
        }
        if (n == 0) {
            break;
        }
        t->rx_len = static_cast<uint32_t>(n);
        t->rx_pos = 0;
        while (take_bytes(t, nullptr, on_line, &line)) {
        }
        wait = 0;
    }
    t->reader = elm327_reader{}; /* a partial line goes with them */
    return reset ? elm327_io::reset : elm327_io::prompt;
}

/* Sends cmd (CR included) and reads its reply to the prompt, calling
 * on_line for each line but its echo. watch_reset: an unsolicited reset
 * found before sending ends it (reset) and nothing is sent. */
template <typename OnLine>
elm327_io command(transport* t, const char* cmd, bool watch_reset,
                  OnLine&& on_line)
{
    const elm327_io before = drain(t, 0);
    if (before == elm327_io::failed ||
        (watch_reset && before == elm327_io::reset)) {
        return before;
    }
    char sent[ELM327_CMD_MAX];
    bare_command(cmd, sent);
    if (logging(t)) {
        elm327_log_write(t->log, "> %s", sent);
    }
    t->sent_at = std::chrono::steady_clock::now();
    if (!t->port.ops->write(t->port.impl, reinterpret_cast<const uint8_t*>(cmd),
                            static_cast<uint32_t>(strlen(cmd)))) {
        elm327_log_write(t->log, "the port failed (writing %s)", sent);
        return elm327_io::failed;
    }
    auto last = std::chrono::steady_clock::now();
    for (;;) {
        bool line = false;
        if (take_bytes(t, sent, on_line, &line)) {
            return elm327_io::prompt;
        }
        const auto now = std::chrono::steady_clock::now();
        if (line) {
            last = now;
        }
        if (t->stop_requested.load()) {
            return elm327_io::stopped;
        }
        const auto quiet =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last)
                .count();
        if (quiet >= ELM327_PROMPT_TIMEOUT_MS) {
            elm327_log_write(
                t->log, "no line and no prompt for %u s (%s)",
                static_cast<unsigned>(ELM327_PROMPT_TIMEOUT_MS / 1000), sent);
            return elm327_io::silent;
        }
        const uint32_t left =
            ELM327_PROMPT_TIMEOUT_MS - static_cast<uint32_t>(quiet);
        const int32_t n = t->port.ops->read(
            t->port.impl, t->rx_buf, sizeof(t->rx_buf),
            (left < ELM327_READ_SLICE_MS) ? left : ELM327_READ_SLICE_MS);
        if (n < 0) {
            elm327_log_write(t->log,
                             "the port failed (reading the reply to %s)", sent);
            return elm327_io::failed;
        }
        t->rx_len = static_cast<uint32_t>(n);
        t->rx_pos = 0;
    }
}

/* A line for a detail: one line, cut to ELM327_QUOTE_MAX. */
void quote(const elm327_reader& r, char* out)
{
    size_t n = 0;
    for (; n < r.len && n < ELM327_QUOTE_MAX; ++n) {
        out[n] = r.line[n];
    }
    out[n] = '\0';
    ecuio_text_one_line(out);
}

/* How a setup command went. */
struct setup_answer {
    elm327_io io = elm327_io::prompt;
    bool ok = false;                       /* an OK line came */
    char first[ELM327_QUOTE_MAX + 1] = {}; /* else the first line */
};

/* A contract command (ADR-0026 §3): its answer must be OK. */
setup_answer contract(transport* t, const char* cmd)
{
    setup_answer a;
    a.io = command(t, cmd, false, [&](const elm327_reader& r) {
        if (elm327_classify(r, t->ext29, nullptr) == elm327_line::ok) {
            a.ok = true;
        } else if (a.first[0] == '\0') {
            quote(r, a.first);
        }
    });
    a.ok = a.ok && a.io == elm327_io::prompt;
    return a;
}

/* A setup command's failure, as attach reports it: the port's
 * (LOAD_FAILED), silence or a wrong answer (VENDOR_REFUSED, naming the
 * command and what came back). The words go in the wire log too. */
ecuio_status_t refuse(transport* t, const char* cmd, const setup_answer& a,
                      const char* wanted)
{
    const ecuio_status_t st = (a.io == elm327_io::failed)
                                  ? ECUIO_ERR_LOAD_FAILED
                                  : ECUIO_ERR_VENDOR_REFUSED;
    char sent[ELM327_CMD_MAX];
    bare_command(cmd, sent);
    if (a.io == elm327_io::failed) {
        ecuio_detail_set("%s: the port failed during the adapter's setup",
                         t->port_name);
    } else if (a.io == elm327_io::silent) {
        ecuio_detail_set(
            "the adapter on %s didn't answer %s within %u s", t->port_name,
            sent, static_cast<unsigned>(ELM327_PROMPT_TIMEOUT_MS / 1000));
    } else if (a.first[0] == '\0') {
        ecuio_detail_set("the adapter on %s answered %s with nothing, not %s",
                         t->port_name, sent, wanted);
    } else {
        ecuio_detail_set("the adapter on %s answered %s with '%s', not %s",
                         t->port_name, sent, a.first, wanted);
    }
    elm327_log_write(t->log, "the setup failed: %s", ecuio_last_error_detail());
    return st;
}

/* AT SH (and AT CP for 29-bit) for transmit ID tx, unless it's the one
 * set: an OK answer, or the first that wasn't, with its command in
 * failed_cmd (ELM327_CMD_MAX bytes). */
setup_answer set_header(transport* t, uint32_t tx, char* failed_cmd)
{
    setup_answer a;
    a.ok = true;
    if (t->header_set && t->header_tx == tx) {
        return a;
    }
    char cmds[2][24];
    const size_t n = elm327_format_header(tx, t->ext29, cmds);
    for (size_t i = 0; i < n; ++i) {
        a = contract(t, cmds[i]);
        if (!a.ok) {
            t->header_set = false;
            copy_text(failed_cmd, ELM327_CMD_MAX, cmds[i]);
            return a;
        }
    }
    t->header_set = true;
    t->header_tx = tx;
    return a;
}

/* AT Z or AT I: ok when a line is an ELM327 version, copied to version
 * (when not NULL). With plain_ok, an OK line sets it instead of being
 * the answer quoted. */
setup_answer identify(transport* t, const char* cmd, char* version,
                      size_t version_cap, bool* plain_ok)
{
    setup_answer a;
    a.io = command(t, cmd, false, [&](const elm327_reader& r) {
        if (elm327_is_version(r.line, r.len)) {
            a.ok = true;
            if (version != nullptr) {
                copy_text(version, version_cap, r.line);
            }
        } else if (plain_ok != nullptr &&
                   elm327_classify(r, t->ext29, nullptr) == elm327_line::ok) {
            *plain_ok = true;
        } else if (a.first[0] == '\0') {
            quote(r, a.first);
        }
    });
    a.ok = a.ok && a.io == elm327_io::prompt;
    return a;
}

/* The setup's contract commands (ADR-0026 §3), each answered before the
 * next: a known state, then everything the framing rests on. Any
 * answer but OK refuses, so an adapter never runs half-configured. The
 * reset is sent twice at most: an adapter still monitoring the bus takes
 * the first character as its stop and answers the rest '?'. A reset
 * answered OK alone (an OBDX) leaves the version to AT I, which must
 * give one. */
ecuio_status_t run_setup(transport* t, char* version, size_t version_cap)
{
    t->configured = false;
    t->header_set = false;
    t->reader = elm327_reader{};
    setup_answer reset;
    bool reset_ok = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        reset_ok = false;
        reset = identify(t, "ATZ\r", version, version_cap, &reset_ok);
        if (reset.io != elm327_io::prompt || reset.ok || reset_ok) {
            break;
        }
    }
    if (reset.io == elm327_io::prompt && !reset.ok && reset_ok) {
        const setup_answer id =
            identify(t, "ATI\r", version, version_cap, nullptr);
        if (!id.ok) {
            return refuse(t, "ATI\r", id, "an ELM327 version");
        }
    } else if (!reset.ok) {
        return refuse(t, "ATZ\r", reset, "an ELM327 version");
    }

    char sp[ELM327_CMD_MAX];
    snprintf(sp, sizeof(sp), "ATSP%c\r", t->protocol);
    const char* const steps[] = {
        "ATE0\r", /* no echo, no linefeeds, no spaces: fewer bytes */
        "ATL0\r",   "ATS0\r",
        "ATH1\r",   /* the CAN ID and the PCI byte on every line */
        "ATD0\r",   /* no data length digit after the ID (an STN's PP 29) */
        "ATCAF1\r", /* the adapter adds the PCI bytes when sending */
        "ATCFC1\r", /* and sends the flow control after a First Frame */
        sp,         /* ISO 15765-4 at the mode and rate, never a search */
        "ATAT1\r",  /* adaptive timing */
        "ATST19\r", /* the first reply's timeout, the engine's 100 ms */
    };
    for (const char* cmd : steps) {
        const setup_answer a = contract(t, cmd);
        if (!a.ok) {
            return refuse(t, cmd, a, "OK");
        }
    }
    char header_cmd[ELM327_CMD_MAX] = {};
    const setup_answer header = set_header(t, t->tx_ids[0], header_cmd);
    if (!header.ok) {
        return refuse(t, header_cmd, header, "OK");
    }
    char filter[2][24];
    const size_t n = elm327_format_receive_filter(t->rx_ids, t->chan_count,
                                                  t->ext29, filter);
    for (size_t i = 0; i < n; ++i) {
        const setup_answer a = contract(t, filter[i]);
        if (!a.ok) {
            return refuse(t, filter[i], a, "OK");
        }
    }
    t->configured = true;
    return ECUIO_OK;
}

/* A reading's first line that matches (ADR-0026 §3): anything else,
 * '?' included, means "not present", and nothing fails. */
template <typename Match>
bool reading(transport* t, const char* cmd, char* out, size_t cap,
             Match&& match)
{
    bool found = false;
    const elm327_io io = command(t, cmd, false, [&](const elm327_reader& r) {
        if (!found && match(r)) {
            copy_text(out, cap, r.line);
            found = true;
        }
    });
    return io == elm327_io::prompt && found;
}

/* The session info (ADR-0019, ADR-0026 §7): what the session runs on,
 * then the readings. A reading the adapter refuses leaves its flag
 * clear. */
void describe_session(transport* t, const char* reset_version,
                      ecuio_session_info_t* info)
{
    *info = ecuio_session_info_t{};
    info->backend = ECUIO_BACKEND_ELM327;
    info->protocol_id = static_cast<uint32_t>(ECUIO_PT_PROTOCOL_ISO15765);
    info->baud = static_cast<uint32_t>(t->can_baud);
    info->channel_count = t->chan_count;
    for (uint32_t i = 0; i < t->chan_count; ++i) {
        info->rx_ids[i] = t->rx_ids[i];
        info->tx_ids[i] = t->tx_ids[i];
    }

    char version[ELM327_LINE_MAX + 1] = {};
    if (!reading(t, "ATI\r", version, sizeof(version),
                 [](const elm327_reader& r) {
                     return elm327_is_version(r.line, r.len);
                 })) {
        copy_text(version, sizeof(version), reset_version);
    }
    char stn[ELM327_LINE_MAX + 1] = {};
    const bool is_stn =
        reading(t, "STI\r", stn, sizeof(stn), [](const elm327_reader& r) {
            return elm327_is_stn(r.line, r.len);
        });
    char* fw = info->firmware_version;
    copy_text(fw, sizeof(info->firmware_version), version);
    if (is_stn) {
        const size_t used = strlen(fw);
        copy_text(fw + used, sizeof(info->firmware_version) - used, ", ");
        copy_text(fw + strlen(fw), sizeof(info->firmware_version) - strlen(fw),
                  stn);
    }
    ecuio_text_one_line(info->firmware_version);
    if (info->firmware_version[0] != '\0') {
        info->flags |= ECUIO_SESSION_INFO_VERSIONS;
    }

    char volts[ELM327_LINE_MAX + 1] = {};
    uint32_t mv = 0;
    if (reading(t, is_stn ? "STVR\r" : "ATRV\r", volts, sizeof(volts),
                [&](const elm327_reader& r) {
                    return elm327_parse_millivolts(r.line, r.len, &mv);
                })) {
        info->vbatt_mv = mv;
        info->flags |= ECUIO_SESSION_INFO_VBATT;
    }

    if (is_stn) {
        /* Parameter 2A on with bit 2 clear: no 7F xx 78 processing */
        char line[ELM327_LINE_MAX + 1] = {};
        uint8_t value = 0;
        bool on = false;
        if (reading(t, "ATPPS\r", line, sizeof(line),
                    [&](const elm327_reader& r) {
                        return elm327_pps_value(r.line, r.len, 0x2A, &value,
                                                &on);
                    }) &&
            on && (value & 0x04) == 0) {
            info->flags |= ECUIO_SESSION_INFO_PENDING_OFF;
        }
    }
}

/* What the lines of a request's reply said. */
struct reply_seen {
    bool matched = false; /* a whole message from a channel's ECU */
    bool no_bus = false;  /* CAN ERROR and kin: it never reached a node */
    bool refused = false; /* '?': the adapter didn't take the request */
    bool reset = false;   /* the adapter reset */
};

/* The wire log's lines of a request's exchange or a monitor go in only
 * with frame logging (logging()) */
struct exchange_scope {
    transport* t;
    explicit exchange_scope(transport* x) : t(x) { t->in_ask = true; }
    ~exchange_scope() { t->in_ask = false; }
    exchange_scope(const exchange_scope&) = delete;
    exchange_scope& operator=(const exchange_scope&) = delete;
};

/* BUFFER FULL: the adapter dropped lines it couldn't send in time. The
 * next frame dispatched says so (ADR-0026's appendix "passive
 * listening"); the link's state is unchanged. */
void note_overflow(transport* t)
{
    t->overflow_pending = true;
    elm327_log_write(t->log, "BUFFER FULL: frames were lost");
}

/* A frame line toward its message; a whole one from a channel's ECU is
 * dispatched, and handed to the engine when to_engine (a monitor's
 * messages answer no request of ours). One from another ID got past the
 * receive filter's mask and is dropped, as J2534's are. */
void take_frame(transport* t, const elm327_can_line& f, reply_seen& seen,
                bool to_engine)
{
    elm327_message m;
    if (elm327_isotp_take(t->isotp, f, &m) != elm327_isotp_result::complete) {
        return;
    }
    for (uint32_t c = 0; c < t->chan_count; ++c) {
        if (t->rx_ids[c] != m.id) {
            continue;
        }
        ecuio_frame_t& frame = t->rx_frame;
        const int64_t at = now_us(t);
        frame.timestamp_us = static_cast<uint64_t>(at);
        frame.arbitration_id = m.id;
        frame.flags = t->ext29 ? ECUIO_FRAME_FLAG_EXTENDED : 0u;
        if (t->overflow_pending) {
            frame.flags |= ECUIO_FRAME_FLAG_OVERFLOW;
            t->overflow_pending = false;
        }
        frame.reserved = 0;
        /* ISO-TP's longest message, 4095 bytes, fits the frame whole */
        memcpy(frame.data, m.data, m.len);
        frame.data_length = m.len;
        ecuio_session_dispatch_callbacks(t->session, &frame);
        if (to_engine) {
            (void)ecuio_engine_on_frame(&t->engine, c, frame.data,
                                        frame.data_length, at, nullptr);
        }
        seen.matched = true;
        return;
    }
}

/* Asks request r of channel c (ADR-0026 §3): its header if another
 * channel's is set, then the request with the response-count digit when
 * one single-frame reply is certain, then the reply to its prompt. */
elm327_io ask(transport* t, uint32_t c, int32_t r, reply_seen& seen)
{
    /* The header switch, the request and its reply go in the wire log
     * only with frame logging */
    const exchange_scope scope(t);
    ecuio_request_engine& e = t->engine;
    /* Until the request goes out, an early return leaves it unsent: a
     * one-shot stays due */
    const auto unsent = [&](elm327_io io) {
        ecuio_engine_sent(&e, c, r, false, now_us(t));
        return io;
    };
    const elm327_io before = drain(t, 0);
    if (before != elm327_io::prompt) {
        return unsent(before);
    }
    char header_cmd[ELM327_CMD_MAX];
    const setup_answer header = set_header(t, t->tx_ids[c], header_cmd);
    if (!header.ok) {
        /* refused mid-session: the setup runs again */
        return unsent((header.io == elm327_io::prompt) ? elm327_io::reset
                                                       : header.io);
    }
    const ecuio_request& q = ecuio_engine_request_of(&e, c, r);
    char cmd[ELM327_CMD_MAX];
    if (elm327_format_request(q.bytes, q.len,
                              elm327_response_count(q.bytes[0], t->tx_ids[c]),
                              cmd, sizeof(cmd)) == 0) {
        return unsent(elm327_io::prompt); /* never: 2 or 3 bytes */
    }
    ecuio_engine_sent(&e, c, r, true, now_us(t));
    const elm327_io io = command(t, cmd, true, [&](const elm327_reader& line) {
        elm327_can_line f;
        switch (elm327_classify(line, t->ext29, &f)) {
        case elm327_line::frame:
            take_frame(t, f, seen, true);
            break;
        case elm327_line::no_bus:
            seen.no_bus = true;
            break;
        case elm327_line::refused:
            seen.refused = true;
            break;
        case elm327_line::reset:
            seen.reset = true;
            break;
        default:
            /* NO DATA leaves the engine's wait to judge the request;
                 * the read errors end this read without a reply; OK, a
                 * search, an alert or anything else is ignored */
            break;
        }
    });
    if (io == elm327_io::prompt) {
        (void)elm327_isotp_end_reply(t->isotp);
    }
    return io;
}

/* ---- Passive listening (ADR-0026's appendix "passive listening") ----
 * A session with nothing to ask monitors with AT MA, which keeps the
 * receive filter the setup set, so it hears the channels' reply IDs on
 * every adapter. The monitor is silent: no ACK, no flow control. */

/* How a monitor ended. */
struct monitor_end {
    /* prompt: the monitor ended and the adapter takes commands; else
     * what ended the read (silent, failed, stopped, reset) */
    elm327_io io = elm327_io::prompt;
    bool at_once = false; /* AT MA again without a wait: a check or
                           * BUFFER FULL ended it */
    bool heard = false;   /* a channel's message arrived */
};

/* Stops the monitor with one space and reads to the prompt within
 * timeout_ms, its lines still taken. A space because both chips ignore
 * one at the prompt: a stop that crosses a monitor already ended can't
 * become a command, where a CR alone would repeat AT MA. */
template <typename OnLine>
elm327_io stop_monitor(transport* t, uint32_t timeout_ms, OnLine& on_line)
{
    if (logging(t)) {
        elm327_log_write(t->log, "> (a space: the monitor's stop)");
    }
    t->sent_at = std::chrono::steady_clock::now();
    const uint8_t stop = ' ';
    if (!t->port.ops->write(t->port.impl, &stop, 1)) {
        elm327_log_write(t->log,
                         "the port failed (writing the monitor's stop)");
        return elm327_io::failed;
    }
    const auto deadline = t->sent_at + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        bool line = false;
        if (take_bytes(t, "ATMA", on_line, &line)) {
            return elm327_io::prompt;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return elm327_io::silent;
        }
        const auto left = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                  now)
                .count());
        const int32_t n = t->port.ops->read(
            t->port.impl, t->rx_buf, sizeof(t->rx_buf),
            (left < ELM327_READ_SLICE_MS) ? left + 1 : ELM327_READ_SLICE_MS);
        if (n < 0) {
            elm327_log_write(t->log,
                             "the port failed (reading to the monitor's stop)");
            return elm327_io::failed;
        }
        t->rx_len = static_cast<uint32_t>(n);
        t->rx_pos = 0;
    }
}

/* One monitor: AT MA, then its lines with no prompt expected. A
 * channel's message is dispatched and matched; each 100 ms with none
 * counts toward the quiet run. It ends on:
 *   the prompt         BUFFER FULL ended it (AT MA again at once), or
 *                      something else did (again on the backoff's
 *                      schedule, counted as quiet)
 *   5 s with no line   the check: stopped, and STOPPED and the prompt
 *                      within 5 s show the adapter is there (AT MA
 *                      again); no prompt is silent, a hard failure,
 *                      since a quiet bus and a hung adapter look the
 *                      same until the adapter is asked
 *   the session closes stopped, the prompt waited for at most 1 s
 *   a reset line, or the port failing */
monitor_end listen(transport* t, link_health& h)
{
    monitor_end end;
    end.io = drain(t, 0);
    if (end.io != elm327_io::prompt) {
        return end;
    }
    const exchange_scope scope(t);
    elm327_log_write(t->log, "the monitor starts (AT MA)");
    if (logging(t)) {
        elm327_log_write(t->log, "> ATMA");
    }
    static const char kMonitor[] = "ATMA\r";
    t->sent_at = std::chrono::steady_clock::now();
    if (!t->port.ops->write(t->port.impl,
                            reinterpret_cast<const uint8_t*>(kMonitor),
                            static_cast<uint32_t>(sizeof(kMonitor) - 1))) {
        elm327_log_write(t->log, "the port failed (writing ATMA)");
        end.io = elm327_io::failed;
        return end;
    }
    bool full = false;
    bool reset = false;
    bool heard_now = false; /* a message in this 100 ms window */
    auto on_line = [&](const elm327_reader& r) {
        elm327_can_line f;
        switch (elm327_classify(r, t->ext29, &f)) {
        case elm327_line::frame: {
            reply_seen seen;
            take_frame(t, f, seen, false);
            if (seen.matched) {
                heard_now = true;
                end.heard = true;
                link_on_read(h, false, true, false);
            }
            break;
        }
        case elm327_line::buffer_full:
            note_overflow(t);
            full = true;
            break;
        case elm327_line::reset:
            reset = true;
            break;
        default:
            /* a flow control's FC:, <DATA ERROR, STOPPED, CAN ERROR:
             * not frames; a prompt after them ends the monitor */
            break;
        }
    };
    auto last_line = t->sent_at;
    auto window = t->sent_at;
    for (;;) {
        bool line = false;
        const bool prompt = take_bytes(t, "ATMA", on_line, &line);
        if (reset) {
            end.io = elm327_io::reset;
            break;
        }
        if (prompt) {
            end.at_once = full;
            elm327_log_write(t->log,
                             full ? "the monitor ended at BUFFER FULL: AT MA "
                                    "again"
                                  : "the monitor ended unasked: AT MA again "
                                    "after a wait");
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (line) {
            last_line = now;
        }
        if (t->stop_requested.load()) {
            (void)stop_monitor(t, ELM327_MONITOR_DETACH_MS, on_line);
            elm327_log_write(t->log, "the monitor stopped: the session "
                                     "closes");
            end.io = elm327_io::stopped;
            break;
        }
        if (now - window >= std::chrono::milliseconds(ELM327_IDLE_READ_MS)) {
            if (!heard_now) {
                link_on_read(h, false, false, true);
            }
            heard_now = false;
            window = now;
        }
        if (now - last_line >=
            std::chrono::milliseconds(ELM327_MONITOR_CHECK_MS)) {
            elm327_log_write(
                t->log,
                "no line for %u s: the monitor stops to "
                "check the adapter",
                static_cast<unsigned>(ELM327_MONITOR_CHECK_MS / 1000));
            end.io = stop_monitor(t, ELM327_MONITOR_CHECK_MS, on_line);
            if (reset) {
                end.io = elm327_io::reset;
            } else if (end.io == elm327_io::prompt) {
                end.at_once = true;
                elm327_log_write(t->log, "the adapter answered the stop: AT MA "
                                         "again");
            } else if (end.io == elm327_io::silent) {
                elm327_log_write(
                    t->log, "no prompt within %u s of the stop",
                    static_cast<unsigned>(ELM327_MONITOR_CHECK_MS / 1000));
            }
            break;
        }
        const int32_t n = t->port.ops->read(
            t->port.impl, t->rx_buf, sizeof(t->rx_buf), ELM327_READ_SLICE_MS);
        if (n < 0) {
            elm327_log_write(t->log, "the port failed (reading the monitor)");
            end.io = elm327_io::failed;
            break;
        }
        t->rx_len = static_cast<uint32_t>(n);
        t->rx_pos = 0;
    }
    /* a message under way when the monitor ended never finishes */
    (void)elm327_isotp_end_reply(t->isotp);
    return end;
}

/* Opens the port attach named: an app-fed stream through the registry,
 * its wait ended by a close; anything else through the opener. */
ecuio_status_t open_port(transport* t, char* why, uint32_t why_cap)
{
    if (t->stream) {
        return elm327_stream_open(t->port_name, &t->stop_requested, &t->port,
                                  why, why_cap);
    }
    return t->opener(t->port_name, t->serial_baud, &t->port, why, why_cap);
}

/* Closes the port; the worker reopens it on the backoff's schedule. */
void close_port(transport* t)
{
    if (t->port_open) {
        t->port.ops->close(t->port.impl);
        t->port_open = false;
        elm327_log_write(t->log, "%s closed", t->port_name);
    }
    t->configured = false;
    t->rx_len = t->rx_pos = 0;
}

/* Opens the port if it's closed and runs the setup's contract commands:
 * true when the adapter is ready for requests. */
bool reconnect(transport* t)
{
    if (!t->port_open) {
        char why[128];
        if (open_port(t, why, sizeof(why)) != ECUIO_OK) {
            elm327_log_write(t->log, "reopening %s failed: %s", t->port_name,
                             why);
            return false;
        }
        t->port_open = true;
        t->rx_len = t->rx_pos = 0;
        elm327_log_write(t->log, "%s reopened", t->port_name);
    }
    return run_setup(t, nullptr, 0) == ECUIO_OK;
}

/* Backoff-aware sleep, abortable within 20 ms of a stop request.
 * Returns true if stop was requested. */
bool backoff_sleep(transport* t, uint32_t ms)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!t->stop_requested.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        auto chunk = deadline - now;
        if (chunk > std::chrono::milliseconds(20)) {
            chunk = std::chrono::milliseconds(20);
        }
        std::this_thread::sleep_for(chunk);
    }
    return true;
}

/* The worker (ADR-0026 §3): the request engine driven as the J2534
 * worker drives it, with one command outstanding for the whole adapter,
 * since an ELM327 takes the next command only after its prompt. Each
 * pass ends the waits that ran out, asks the first channel in turn with
 * a request due, and reads its reply to the prompt. With nothing due it
 * listens for what the adapter says unasked (a reset): until the next
 * request's floor or wait, or, with nothing left to ask, for the idle
 * read's 100 ms, which counts toward quiet as J2534's passive reads do.
 *
 * Link health (ADR-0026 §6, link_health.h): a reply with a channel's
 * message is OK; NO DATA leaves the engine's wait to time the request
 * out; CAN ERROR and its kin, or '?', is a write that failed; a port
 * that fails, or no prompt within 5 s, is a hard failure, after which
 * the port is reopened and the setup run on the backoff's schedule, so a
 * Bluetooth link that drops and returns recovers without a reconnect. An
 * adapter that resets on its own gets its setup again, and is a hard
 * failure only until that succeeds.
 *
 * A session with nothing to ask at all listens instead (listen(), AT MA):
 * a monitor that ends unasked starts again on the backoff's schedule,
 * reset by a message heard. */
void elm327_worker(transport* t)
{
    ecuio_request_engine& e = t->engine;
    link_health h(t->session->last_error, t->session->error_count);
    uint32_t published[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_identity[ECUIO_MAX_CHANNELS] = {};
    uint32_t next_chan = 0;
    const bool passive = e.count == 0 && e.scan_count == 0;
    uint32_t relisten_ms = 0;
    if (t->probe) {
        ecuio_engine_probe(&e, now_us(t));
    }

    while (!t->stop_requested.load()) {
        const int64_t now = now_us(t);
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            if (ecuio_engine_expire(&e, c, now)) {
                link_on_request_timeout(h);
            }
        }

        if (!t->configured) {
            const bool ready = reconnect(t);
            if (t->stop_requested.load()) {
                break;
            }
            if (!ready) {
                /* The adapter answered wrongly or not at all: start over
                 * from the port */
                close_port(t);
            }
            /* The adapter answering its setup ends the backoff */
            link_on_read(h, !ready, false, false);
        } else {
            uint32_t c = 0;
            int32_t r = ECUIO_ENGINE_NONE;
            for (uint32_t k = 0; k < e.chan_count && r == ECUIO_ENGINE_NONE;
                 ++k) {
                c = (next_chan + k) % e.chan_count;
                r = ecuio_engine_due(&e, c, now);
            }
            elm327_io io = elm327_io::prompt;
            if (passive) {
                const monitor_end m = listen(t, h);
                io = m.io;
                if (m.heard) {
                    relisten_ms = 0;
                }
                if (io == elm327_io::prompt && !m.at_once) {
                    link_on_read(h, false, false, true);
                    relisten_ms = (relisten_ms == 0)
                                      ? LINK_BACKOFF_BASE_MS
                                      : link_backoff_next(relisten_ms);
                    if (backoff_sleep(t, relisten_ms)) {
                        break;
                    }
                }
            } else if (r != ECUIO_ENGINE_NONE) {
                next_chan = (c + 1) % e.chan_count;
                reply_seen seen;
                io = ask(t, c, r, seen);
                if (io == elm327_io::prompt) {
                    if (seen.reset) {
                        io = elm327_io::reset;
                    } else if (seen.no_bus || seen.refused) {
                        /* never reached the bus: not sent */
                        ecuio_engine_sent(&e, c, r, false, now_us(t));
                        link_on_write(h, false);
                    } else {
                        link_on_write(h, true);
                        link_on_read(h, false, seen.matched, false);
                    }
                }
            } else {
                const int64_t next_us = ecuio_engine_next_event_us(&e, now);
                const bool asking = next_us != INT64_MAX;
                uint32_t wait = ELM327_IDLE_READ_MS;
                if (asking) {
                    const int64_t ms = (next_us + 999) / 1000;
                    wait = (ms < 1) ? 1u
                           : (ms > ELM327_WAIT_SLICE_MS)
                               ? ELM327_WAIT_SLICE_MS
                               : static_cast<uint32_t>(ms);
                }
                io = drain(t, wait);
                if (io == elm327_io::prompt && !asking) {
                    link_on_read(h, false, false, true);
                }
            }
            if (io == elm327_io::stopped) {
                break;
            }
            if (io == elm327_io::failed || io == elm327_io::silent) {
                close_port(t);
                link_on_read(h, true, false, false);
            } else if (io == elm327_io::reset) {
                /* setup again next pass; hard only if that fails */
                elm327_log_write(t->log, "the adapter reset: the setup runs "
                                         "again");
                t->configured = false;
            }
        }

        if (h.recovered) {
            h.recovered = false;
            if (t->probe) {
                ecuio_engine_probe(&e, now_us(t));
            }
            /* the scan's and identification's unanswered requests */
            ecuio_engine_rescan(&e);
        }
        for (uint32_t c = 0; c < t->chan_count; ++c) {
            ecuio_publish_support(t->session, e, c, t->rx_ids[c], published[c]);
            ecuio_publish_identity(t->session, e, c, t->rx_ids[c],
                                   published_identity[c]);
        }

        if (h.backoff_ms > 0) {
            if (backoff_sleep(t, h.backoff_ms)) {
                break;
            }
            h.backoff_ms = link_backoff_next(h.backoff_ms);
        }
    }
}

ecuio_status_t elm327_start(ecuio_session_t* session, void* impl)
{
    (void)session;
    auto* t = static_cast<transport*>(impl);
    if (t->worker.joinable()) {
        return ECUIO_OK; /* internal.h: a second start is OK */
    }
    t->stop_requested.store(false);
    try {
        t->worker = std::thread(elm327_worker, t);
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

/* Joins the worker, then closes the port. The adapter keeps the setup
 * until its next reset; nothing it holds was written (ADR-0026 §5). */
void elm327_detach(ecuio_session_t* session, void* impl)
{
    (void)session;
    auto* t = static_cast<transport*>(impl);
    if (t->worker.joinable()) {
        t->stop_requested.store(true);
        t->worker.join();
    }
    elm327_log_write(t->log, "the session closes");
    close_port(t);
    delete t;
}

const ecuio_transport_ops_t kElm327Ops = {elm327_start, elm327_detach};

/* The query's values (ecuio_elm327_query_keys), strict as J2534's: a bad
 * value is BAD_URI at create, before the port opens.
 *   baud    the CAN rate: 500000 (the default) or 250000, ISO 15765-4's
 *           two, which the ELM327 has protocols for
 *   serial  the serial port's rate for a wired adapter, default 38400;
 *           a Bluetooth port ignores it, a TCP address refuses it
 *   filter, txid, poll, probe, identify: the request keys
 *           (ecuio_request_param), as for j2534://
 *   profile, did: core's, read by create */
ecuio_status_t parse_query(const ecuio_uri_query* query, transport* t,
                           request_params* req)
{
    for (size_t i = 0; i < query->count; ++i) {
        const ecuio_uri::param& q = query->params[i];
        unsigned long v = 0;
        const char* wanted = nullptr;
        if (q.is("baud")) {
            if (!ecuio_parse_number(q.val, q.val_len, &v) ||
                (v != 500000 && v != 250000)) {
                wanted = "500000 or 250000";
            } else {
                t->can_baud = v;
            }
        } else if (q.is("serial")) {
            if (!ecuio_parse_number(q.val, q.val_len, &v) ||
                v < ELM327_SERIAL_MIN || v > ELM327_SERIAL_MAX) {
                wanted = "a serial rate from 300 to 4000000";
            } else {
                t->serial_baud = static_cast<uint32_t>(v);
                t->serial_given = true;
            }
        } else {
            (void)ecuio_request_param(q, req, &wanted);
        }
        if (wanted != nullptr) {
            ecuio_detail_bad_value(q, wanted);
            return ECUIO_ERR_BAD_URI;
        }
    }
    return ECUIO_OK;
}

ecuio_status_t elm327_attach(ecuio_session_t* session, const char* uri,
                             const ecuio_uri_query* query)
{
    if (session->transport != nullptr) {
        return ECUIO_OK; /* internal.h: idempotent */
    }
    std::unique_ptr<transport> t(new (std::nothrow) transport());
    if (t == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }

    /* The port: the text before the query, judged before any value */
    const char* name = uri + strlen("elm327://");
    const char* q = strchr(name, '?');
    const size_t name_len =
        (q != nullptr) ? static_cast<size_t>(q - name) : strlen(name);
    if (name_len == 0) {
        ecuio_detail_set("elm327:// needs a port: %s, or %s",
                         elm327_port_name_example(), elm327_tcp_example());
        return ECUIO_ERR_BAD_URI;
    }
    t->tcp = elm327_tcp_address(name, name_len, nullptr, nullptr);
    t->stream = !t->tcp && elm327_stream_name(name, name_len);
    if (!t->tcp && !t->stream && !elm327_port_name_ok(name, name_len)) {
        ecuio_detail_set("the port must be %s, or %s",
                         elm327_port_name_example(), elm327_tcp_example());
        return ECUIO_ERR_BAD_URI;
    }
    memcpy(t->port_name, name, name_len);
    t->port_name[name_len] = '\0';

    request_params req;
    ecuio_status_t st = parse_query(query, t.get(), &req);
    if (st != ECUIO_OK) {
        return st;
    }
    /* A socket has no serial rate; a run sheet that carries one over from
     * a USB session is told so rather than ignored */
    if ((t->tcp || t->stream) && t->serial_given) {
        ecuio_detail_set("?serial= is a serial port's rate, and %s is %s, "
                         "which has none",
                         t->port_name,
                         t->tcp ? "a TCP address" : "an app's stream");
        return ECUIO_ERR_BAD_URI;
    }

    /* Channels, their addressing mode, and the requests, before the port
     * opens. One mode for the session: AT SP takes one */
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    t->chan_count = ecuio_resolve_channels(set, req, t->rx_ids, t->tx_ids);
    uint32_t wide = 0;
    for (uint32_t i = 0; i < t->chan_count; ++i) {
        if (ecuio_channel_is_29bit(t->rx_ids[i], t->tx_ids[i])) {
            ++wide;
        }
    }
    if (wide != 0 && wide != t->chan_count) {
        ecuio_detail_set("the channels mix 11-bit and 29-bit IDs, which "
                         "one connection can't carry");
        return ECUIO_ERR_BAD_URI;
    }
    t->ext29 = wide != 0;
    t->protocol = elm327_protocol_for(t->ext29, t->can_baud);
    st = ecuio_build_requests(session, set, req, t->rx_ids, t->chan_count,
                              &t->engine, &t->probe, &t->identify);
    if (st != ECUIO_OK) {
        return st;
    }
    /* A session that asks nothing listens on its channels (AT MA keeps
     * the receive filter); with none, the whole bus would be raw frames
     * a serial link can't carry, a step of its own */
    if (t->engine.count == 0 && t->engine.scan_count == 0 &&
        t->chan_count == 0) {
        ecuio_detail_set("elm327:// listens on its channels' IDs, and this "
                         "session has none: add ?filter=, or use a profile "
                         "that declares a channel");
        return ECUIO_ERR_NOT_IMPLEMENTED;
    }

    /* The port, then the adapter's setup and readings */
    ecuio_elm327_port_open_t opener = g_opener.load();
    t->opener = (opener != nullptr) ? opener : elm327_port_open_os;
    char why[128] = "";
    st = open_port(t.get(), why, sizeof(why));
    if (st != ECUIO_OK) {
        ecuio_detail_set("%s: %s", t->port_name, why);
        return st;
    }
    t->port_open = true;
    elm327_log_open(t->log);
    if (elm327_log_is_open(t->log)) {
        char text[512];
        elm327_log_text(uri, strlen(uri), text, sizeof(text));
        elm327_log_write(t->log, "QtTune ELM327 wire log (ADR-0026), core %s",
                         ecuio_version_string());
        elm327_log_write(t->log, "uri %s", text);
        char link[48];
        if (t->tcp) {
            snprintf(link, sizeof(link), "over TCP");
        } else if (t->stream) {
            snprintf(link, sizeof(link), "(app stream)");
        } else {
            snprintf(link, sizeof(link), "at %u bit/s",
                     static_cast<unsigned>(t->serial_baud));
        }
        elm327_log_write(
            t->log, "%s %s; CAN at %lu bit/s, protocol %c; %s", t->port_name,
            link, t->can_baud, t->protocol,
            t->log.requests
                ? "every request and reply (ECUIO_SUR_LOG_FRAMES=1)"
                : "the setup, the readings and the link's events; the "
                  "requests with ECUIO_SUR_LOG_FRAMES=1");
    }
    char version[ELM327_LINE_MAX + 1] = {};
    st = run_setup(t.get(), version, sizeof(version));
    if (st != ECUIO_OK) {
        close_port(t.get());
        return st;
    }

    auto* wrap = new (std::nothrow) ecuio_transport_t();
    if (wrap == nullptr) {
        close_port(t.get());
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    wrap->ops = &kElm327Ops;
    t->session = session;
    describe_session(t.get(), version, &session->info);
    ecuio_seed_reports(session, t->engine, t->rx_ids, t->chan_count);
    t->epoch = std::chrono::steady_clock::now(); /* as mock: end of attach */
    wrap->impl = t.release();
    session->transport = wrap;
    return ECUIO_OK;
}

} // namespace

extern "C" ecuio_status_t
ecuio_elm327_transport_attach(ecuio_session_t* session, const char* uri,
                              const ecuio_uri_query* query)
{
    return elm327_attach(session, uri, query);
}

extern "C" void ecuio_elm327_set_port_opener(ecuio_elm327_port_open_t open)
{
    g_opener.store(open);
}
