// File/layer: core/src/elm327_transport.h | internal, C++17, Qt-free,
//             every platform (ADR-0026)
// Testing: HARNESS: Elm327SessionTest, Elm327TalkTest, Elm327TurnsTest
//          (test_elm327)
//
// elm327://'s transport as its sources share it: the struct, the
// constants, and the I/O every command goes through (take_bytes,
// reply_to, command). elm327_transport.cpp's header names the parts.

#ifndef ECUIO_ELM327_TRANSPORT_H
#define ECUIO_ELM327_TRANSPORT_H

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "access_cut.h"
#include "session.h"
#include "elm327_log.h"
#include "elm327_port.h"
#include "elm327_protocol.h"
#include "request_engine.h"
#include "link_health.h"
#include "host_clock.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>

namespace ecuio_elm327 {

/* The serial rate a wired ELM327 starts at with its rate pin high; the
 * other is 9600 (ADR-0026 section 2). */
constexpr uint32_t ELM327_SERIAL_DEFAULT = 38400;
constexpr uint32_t ELM327_SERIAL_MIN = 300;
constexpr uint32_t ELM327_SERIAL_MAX = 4000000;
constexpr unsigned long ELM327_CAN_BAUD_DEFAULT = 500000;

/* No line and no prompt this long: the adapter, or the link to it, has
 * failed (ADR-0026 section 6). Counted from the last line, so a pending reply's
 * wait (an STN listening past 7F xx 78) never runs into it. */
constexpr uint32_t ELM327_PROMPT_TIMEOUT_MS = 5000;

/* A reply's wait that ends with the machine's slept count up this much
 * since its command was typed says the sleep in the wire log. The baseline
 * is the send, not the last line: a sleep mid-reply followed by 5 s awake
 * is worded as the sleep too, and the reply is lost either way. */
constexpr uint64_t ELM327_SLEEP_SAID_US = 1000000;

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

/* At the close (ADR-0060) each reply is read to its prompt within this
 * of its send, and the whole close ends within ELM327_CLOSE_MS. */
constexpr uint32_t ELM327_LEAVE_MS = 1000;
constexpr uint32_t ELM327_CLOSE_MS = 2000;

/* A wait in the words the log and the detail print: "5 s", or "300 ms"
 * for a test's shortened one */
struct wait_words {
    char text[24];
};

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
    reset    /* the adapter reset on its own (ADR-0026 section 6) */
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
    /* The engine's reply timing (ADR-0042): an ELM327's frames are
     * stamped as their lines are read, so the intervals are the host's */
    std::unique_ptr<ecuio_timing_table> timing;
    bool probe = false;
    bool identify = false;
    /* The Service 01 PIDs asked with the response-count digit
     * (elm327_one_frame_pids_for); one whose reply spans frames after
     * all leaves it */
    elm327_one_frame_pids one_frame;
    /* The DIDs asked with no digit: one whose reply needed more than one
     * line leaves the digit (elm327_response_count) */
    elm327_long_dids long_dids;
    /* Per channel, the ECU still sending a cut reply: nothing is asked of
     * it before hold_until_us; the DID asked again after it (cut_open),
     * the hold it had, and how often it was held again for NO DATA */
    struct cut_hold {
        int64_t hold_until_us = 0;
        int64_t hold_us = 0;
        uint16_t did = 0;
        bool open = false;
        uint8_t tries = 0;
    };
    cut_hold cut[ECUIO_MAX_CHANNELS];
    /* Bytes read and not yet taken, the line under way, and the
     * messages under way (too big for a stack, isotp.h, so here). */
    uint8_t rx_buf[256] = {};
    uint32_t rx_len = 0;
    uint32_t rx_pos = 0;
    elm327_reader reader;
    isotp_rx isotp;
    /* Session time counts from the end of attach, as mock's does; the
     * frame dispatched from here. */
    host_clock::time_point epoch{};
    /* The last command's send, for its prompt's time in the wire log */
    std::chrono::steady_clock::time_point sent_at{};
    /* The slept count (slept_us) when it was typed */
    uint64_t slept_at = 0;
    ecuio_frame_t rx_frame{};
    std::thread worker;
    elm327_log log; /* the wire log (elm327_log.h) */
    std::atomic<bool> stop_requested{false};
    /* The close's 10 01 is under way (leave_sessions): a command waits
     * for its prompt whatever the stop, until close_by at the latest */
    bool leaving = false;
    std::chrono::steady_clock::time_point close_by{};
    /* The stop ended owed_cmd before its prompt: its reply still comes */
    bool prompt_owed = false;
    char owed_cmd[ELM327_CMD_MAX] = {};
    /* A request's exchange, or a monitor, is under way: the wire log
     * takes its lines only with frame logging */
    bool in_ask = false;
    /* The command out is a security access's: its key and every line of
     * its reply are cut from the wire log (access_cut.h) */
    bool cutting = false;
    char cut_sent[ELM327_CMD_MAX + ECUIO_ACCESS_CUT_TEXT + 4] = {};
    /* BUFFER FULL came: the next frame dispatched carries
     * ECUIO_FRAME_FLAG_OVERFLOW */
    bool overflow_pending = false;

    ecuio_elm327_transport() = default;
    ecuio_elm327_transport(const ecuio_elm327_transport&) = delete;
    ecuio_elm327_transport& operator=(const ecuio_elm327_transport&) = delete;
    ~ecuio_elm327_transport() { elm327_log_close(log); }
};

using transport = ecuio_elm327_transport;

/* elm327_transport.cpp */
uint32_t prompt_timeout_ms();
uint64_t slept_us();
uint32_t monitor_check_ms();
wait_words words_of(uint32_t ms);
void close_port(transport* t);
bool reconnect(transport* t);
bool backoff_sleep(transport* t, uint32_t ms);

/* elm327_worker.cpp */
void bare_command(const char* cmd, char* out);
void copy_text(char* dst, size_t cap, const char* src);
bool logging(const transport* t);
void log_line(transport* t, const char* sent, const elm327_reader& r);
void log_prompt(transport* t, const char* sent);
void cut_command(transport* t, const char* sent);
elm327_io drain(transport* t, uint32_t timeout_ms);

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

/* Logs the machine's sleep since sent was typed, when there was one, with
 * what became of the reply; false when there was none. */
inline bool say_sleep(transport* t, const char* sent, const char* then)
{
    const uint64_t slept = slept_us() - t->slept_at;
    if (slept < ELM327_SLEEP_SAID_US) {
        return false;
    }
    const unsigned long long tenths = slept / 100000;
    elm327_log_write(t->log, "the machine slept %llu.%llu s with %s out: %s",
                     tenths / 10, tenths % 10, sent, then);
    return true;
}

/* Reads the reply to sent (sent without its CR) to the prompt, calling
 * on_line for each line but its echo; a stop first leaves it owed. */
template <typename OnLine>
elm327_io reply_to(transport* t, const char* sent, OnLine& on_line)
{
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
        if (t->stop_requested.load() && !t->leaving) {
            t->prompt_owed = true;
            copy_text(t->owed_cmd, sizeof(t->owed_cmd), sent);
            return elm327_io::stopped;
        }
        uint32_t left = 0;
        if (t->leaving) {
            /* At the close a reply's wait runs from its send, lines or none */
            const uint32_t ms = (prompt_timeout_ms() < ELM327_LEAVE_MS)
                                    ? prompt_timeout_ms()
                                    : ELM327_LEAVE_MS;
            auto by = t->sent_at + std::chrono::milliseconds(ms);
            by = (by < t->close_by) ? by : t->close_by;
            if (now >= by) {
                elm327_log_write(t->log, "no prompt for %s by the close's wait",
                                 sent);
                return elm327_io::silent;
            }
            left = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(by - now)
                    .count() +
                1);
        } else {
            const auto quiet =
                std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                      last)
                    .count();
            const uint32_t timeout = prompt_timeout_ms();
            if (quiet >= timeout) {
                if (!say_sleep(t, sent, "its reply is lost")) {
                    elm327_log_write(t->log,
                                     "no line and no prompt for %s (%s)",
                                     words_of(timeout).text, sent);
                }
                return elm327_io::silent;
            }
            left = timeout - static_cast<uint32_t>(quiet);
        }
        const int32_t n = t->port.ops->read(
            t->port.impl, t->rx_buf, sizeof(t->rx_buf),
            (left < ELM327_READ_SLICE_MS) ? left : ELM327_READ_SLICE_MS);
        if (n < 0) {
            if (!say_sleep(t, sent, "the port failed on the wake")) {
                elm327_log_write(
                    t->log, "the port failed (reading the reply to %s)", sent);
            }
            return elm327_io::failed;
        }
        t->rx_len = static_cast<uint32_t>(n);
        t->rx_pos = 0;
    }
}

/* Sends cmd (CR included) and reads its reply to the prompt, calling
 * on_line for each line but its echo. watch_reset: an unsolicited reset
 * found before sending ends it (reset) and nothing is sent. */
template <typename OnLine>
elm327_io command(transport* t, const char* cmd, bool watch_reset,
                  OnLine&& on_line)
{
    t->prompt_owed = false;
    const elm327_io before = drain(t, 0);
    if (before == elm327_io::failed ||
        (watch_reset && before == elm327_io::reset)) {
        return before;
    }
    char sent[ELM327_CMD_MAX];
    bare_command(cmd, sent);
    cut_command(t, sent);
    if (logging(t)) {
        elm327_log_write(t->log, "> %s", t->cutting ? t->cut_sent : sent);
    }
    t->sent_at = std::chrono::steady_clock::now();
    t->slept_at = slept_us();
    if (!t->port.ops->write(t->port.impl, reinterpret_cast<const uint8_t*>(cmd),
                            static_cast<uint32_t>(strlen(cmd)))) {
        elm327_log_write(t->log, "the port failed (writing %s)", sent);
        return elm327_io::failed;
    }
    return reply_to(t, sent, on_line);
}

/* How a setup command went. */
struct setup_answer {
    elm327_io io = elm327_io::prompt;
    bool ok = false;                       /* an OK line came */
    char first[ELM327_QUOTE_MAX + 1] = {}; /* else the first line */
};

/* What the lines of a request's reply said. */
struct reply_seen {
    bool matched = false; /* a whole message from a channel's ECU */
    bool no_bus = false;  /* CAN ERROR and kin: it never reached a node */
    bool refused = false; /* '?': the adapter didn't take the request */
    bool reset = false;   /* the adapter reset */
    bool pending = false; /* a channel's ECU answered 7F xx 78: busy */
    bool no_data = false; /* NO DATA: the adapter's timer ran out */
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

/* How a monitor ended. */
struct monitor_end {
    /* prompt: the monitor ended and the adapter takes commands; else
     * what ended the read (silent, failed, stopped, reset) */
    elm327_io io = elm327_io::prompt;
    bool at_once = false; /* AT MA again without a wait: a check or
                           * BUFFER FULL ended it */
    bool heard = false;   /* a channel's message arrived */
};

/* elm327_setup.cpp */
setup_answer contract(transport* t, const char* cmd);
bool asks_nothing(const transport* t);
ecuio_status_t run_setup(transport* t, char* version, size_t version_cap);
void describe_session(transport* t, const char* reset_version,
                      ecuio_session_info_t* info);

/* elm327_worker.cpp */
setup_answer set_header(transport* t, uint32_t tx, char* failed_cmd);
void take_frame(transport* t, const elm327_can_line& f, reply_seen& seen,
                bool to_engine);
void elm327_worker(transport* t);

/* elm327_listen.cpp */
monitor_end listen(transport* t, link_health& h);

/* elm327_close.cpp */
void leave_sessions(transport* t);

} // namespace ecuio_elm327

#endif /* ECUIO_ELM327_TRANSPORT_H */
