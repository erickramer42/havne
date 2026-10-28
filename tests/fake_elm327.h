// File/layer: tests/fake_elm327.h | test-only, C++17, Qt-free, no
//             platform headers; header-only
// Testing: the HARNESS tier's ELM327 (ADR-0026 §8). Elm327SessionTest
//          puts it behind the transport's port opener (internal.h);
//          Elm327PortTest (POSIX) puts it on the far side of a
//          pseudo-terminal
//
// An ELM327 as its datasheet and the OBDLink manual describe it, bytes
// in and bytes out: the AT settings it keeps (echo, linefeeds, spaces,
// headers, the DLC digit, the protocol, the header and the receive
// filter), its replies ended by a blank line and the '>' prompt, an
// STN's ST commands, and requests answered by the fake J2534 DLL's ECU
// (tests/fake_ecu.h) as the CAN frames an ISO 15765 ECU sends, one line
// each. It records the commands it was sent, so a test can pin the
// setup and show what was never sent. Nothing allocates: the transport's
// worker calls it, and its per-frame allocation is counted.
//
// Knobs (fake_elm327::knobs), set under the port's lock (with()):
//   plain        a plain ELM327: STI, STVR and every ST command answer '?'
//   version      what AT Z and AT I answer ("ELM327 v1.4b")
//   reset_ok     AT Z and AT WS answer OK, and only AT I the version, as
//                the OBDX Pro VX does
//   refuse       one command, without its CR ("ATH1"), answered '?'
//   no_data      every request answered NO DATA
//   can_error    every request answered CAN ERROR (no node to ACK)
//   silent       requests get nothing at all, not even the prompt
//   pending      each request's reply follows 7F <service> 78 in the same
//                read, as an STN prints and listens past it;
//                pending_count of them, pending_gap_ms apart (an ECU
//                repeats it while it works)
//   reply_delay_ms  each request's reply and prompt this long after it
//   unsupported_nrc fake_ecu::answer's (-1 the ECU's codes, 0 silence)
//   four_digit_ids  11-bit IDs printed as two bytes (07E8)
//   pp2a, pp2a_on   AT PPS's parameter 2A
//   dropped      the link is down: the port's reads and writes fail, and
//                opening it says "no such port"
//   hung         while monitoring, a stop gets nothing back and the
//                monitor prints nothing more: no STOPPED, no prompt
//   full_ends_monitor  BUFFER FULL ends the monitor with a prompt, as a
//                plain ELM327's does
//
// AT MA monitors (ADR-0026's appendix "passive listening"): the
// messages bus() puts on the bus print, one ISO-TP frame a line, while
// they pass the receive filter; any byte from the host stops it,
// discarded, with STOPPED and the prompt. buffer_full() and
// end_monitor() are the monitor's other ends.

#ifndef ECUIO_TESTS_FAKE_ELM327_H
#define ECUIO_TESTS_FAKE_ELM327_H

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "fake_ecu.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace fake_elm327 {

struct knobs {
    bool plain = false;
    const char* version = "ELM327 v1.4b";
    bool reset_ok = false;
    const char* refuse = nullptr;
    bool no_data = false;
    bool can_error = false;
    bool silent = false;
    bool pending = false;
    uint32_t pending_count = 1;
    uint32_t pending_gap_ms = 0;
    uint32_t reply_delay_ms = 0;
    long unsupported_nrc = -1;
    bool four_digit_ids = false;
    uint8_t pp2a = 0x04;
    bool pp2a_on = false;
    bool dropped = false;
    bool hung = false;
    bool full_ends_monitor = false;
};

/* The fake's STN firmware string: an STN chip's shape, a version no chip
 * has. */
constexpr char kStnVersion[] = "STN2255 v9.9.9";
constexpr uint32_t kVbattMv = 13800;

inline int64_t steady_us()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

class adapter
{
public:
    knobs k;

    /* What it has been sent since clear(): every command and request,
     * without its CR, spaces dropped and in upper case (the first
     * kRecordDepth), and counts. */
    static constexpr uint32_t kRecordDepth = 96;
    char recorded[kRecordDepth][24] = {};
    uint32_t commands = 0;
    uint32_t requests = 0;
    uint32_t resets = 0;   /* AT Z */
    uint32_t bare_crs = 0; /* a CR alone, which repeats a command */
    uint32_t stops = 0;    /* bytes that stopped a monitor */
    uint8_t last_stop = 0; /* the last of them */

    /* Back to power-on: settings, output and the record. */
    void clear()
    {
        settings_defaults();
        protocol_ = '0';
        head_ = tail_ = 0;
        in_len_ = 0;
        commands = requests = resets = bare_crs = stops = 0;
        last_stop = 0;
        k = knobs{};
    }

    bool monitoring() const { return monitoring_; }

    /* A message on the bus at now_us, from CAN ID id: printed while
     * monitoring, if the receive filter passes it. */
    void bus(uint32_t id, const uint8_t* m, size_t len, int64_t now_us)
    {
        if (monitoring_ && !k.hung && passes(id)) {
            message(id, m, len, now_us);
        }
    }

    /* A line printed as it is while monitoring: a flow control's FC:, a
     * frame with <DATA ERROR beside it. */
    void monitor_line(const char* text, int64_t now_us)
    {
        if (monitoring_ && !k.hung) {
            text_line(text, now_us);
        }
    }

    /* The adapter's buffer overflowed while monitoring: BUFFER FULL, and
     * with full_ends_monitor the prompt after it. */
    void buffer_full(int64_t now_us)
    {
        if (!monitoring_ || k.hung) {
            return;
        }
        text_line("BUFFER FULL", now_us);
        if (k.full_ends_monitor) {
            monitoring_ = false;
            end_line(now_us);
            prompt(now_us);
        }
    }

    /* The monitor ends with no stop sent: word (CAN ERROR, STOPPED, or
     * nullptr for the prompt alone), then the prompt. */
    void end_monitor(const char* word, int64_t now_us)
    {
        if (!monitoring_) {
            return;
        }
        monitoring_ = false;
        if (word != nullptr) {
            text_line(word, now_us);
        }
        end_line(now_us);
        prompt(now_us);
    }

    /* The command index i (0-based) as recorded, or "" past the end. */
    const char* command(uint32_t i) const
    {
        return (i < commands && i < kRecordDepth) ? recorded[i] : "";
    }

    /* How many recorded commands start with prefix. */
    uint32_t count(const char* prefix) const
    {
        uint32_t n = 0;
        const size_t len = strlen(prefix);
        for (uint32_t i = 0; i < commands && i < kRecordDepth; ++i) {
            n += strncmp(recorded[i], prefix, len) == 0 ? 1 : 0;
        }
        return n;
    }

    /* Bytes from the host, at now_us. */
    void from_host(const uint8_t* data, size_t n, int64_t now_us)
    {
        for (size_t i = 0; i < n; ++i) {
            const char c = static_cast<char>(data[i]);
            if (monitoring_) {
                /* Any byte stops the monitor and is discarded */
                ++stops;
                last_stop = data[i];
                if (!k.hung) {
                    monitoring_ = false;
                    reply("STOPPED", now_us);
                }
                continue;
            }
            if (echo_ && c != '\r') {
                put(&data[i], 1, now_us);
            }
            if (c == '\r') {
                if (echo_) {
                    end_line(now_us);
                }
                in_[in_len_] = '\0';
                run(now_us);
                in_len_ = 0;
            } else if (c != '\n' && in_len_ + 1 < sizeof(in_)) {
                in_[in_len_++] = c;
            }
        }
    }

    /* Bytes for the host that are due by now_us, up to cap. */
    size_t to_host(uint8_t* out, size_t cap, int64_t now_us)
    {
        size_t n = 0;
        while (n < cap && head_ != tail_ && due_[head_] <= now_us) {
            out[n++] = out_[head_];
            head_ = (head_ + 1) % kOutDepth;
        }
        return n;
    }

    /* When the next byte for the host is due; INT64_MAX when none is
     * queued. */
    int64_t next_due_us() const
    {
        return (head_ != tail_) ? due_[head_] : INT64_MAX;
    }

    /* Drops what's queued for the host (a link that dropped loses it). */
    void drop_output() { head_ = tail_ = 0; }

    /* A reset nobody asked for (a supply dip): the settings go, and the
     * version banner arrives unsolicited. */
    void spontaneous_reset(int64_t now_us)
    {
        settings_defaults();
        end_line(now_us);
        end_line(now_us);
        text_line(k.version, now_us);
        end_line(now_us);
        prompt(now_us);
    }

private:
    static constexpr size_t kOutDepth = 1u << 16;
    uint8_t out_[kOutDepth] = {};
    int64_t due_[kOutDepth] = {};
    size_t head_ = 0;
    size_t tail_ = 0;
    char in_[64] = {};
    size_t in_len_ = 0;
    uint8_t answer_[fake_ecu::kReplyMax] = {};

    bool echo_ = true;
    bool linefeeds_ = false;
    bool spaces_ = true;
    bool headers_ = false;
    bool dlc_ = false;
    char protocol_ = '0';
    uint32_t sh_ = 0x7DF; /* AT SH: 11-bit, or a 29-bit ID's low 24 */
    uint8_t cp_ = 0x18;   /* AT CP: a 29-bit ID's top five bits */
    bool cra_set_ = false;
    uint32_t cra_ = 0;
    bool cfm_set_ = false;
    uint32_t cf_ = 0;
    uint32_t cm_ = 0;
    bool monitoring_ = false;

    void settings_defaults()
    {
        monitoring_ = false;
        echo_ = true;
        linefeeds_ = false;
        spaces_ = true;
        headers_ = false;
        dlc_ = false;
        sh_ = 0x7DF;
        cp_ = 0x18;
        cra_set_ = cfm_set_ = false;
    }

    bool ext29() const { return protocol_ == '7' || protocol_ == '9'; }

    /* Whether the receive filter passes CAN ID id */
    bool passes(uint32_t id) const
    {
        return !cra_set_ ? (!cfm_set_ || (id & cm_) == (cf_ & cm_))
                         : id == cra_;
    }

    void put(const void* bytes, size_t n, int64_t at)
    {
        const uint8_t* b = static_cast<const uint8_t*>(bytes);
        for (size_t i = 0; i < n; ++i) {
            const size_t next = (tail_ + 1) % kOutDepth;
            if (next == head_) {
                return; /* full: the rest is lost, as BUFFER FULL's is */
            }
            out_[tail_] = b[i];
            due_[tail_] = at;
            tail_ = next;
        }
    }

    void end_line(int64_t at)
    {
        put("\r", 1, at);
        if (linefeeds_) {
            put("\n", 1, at);
        }
    }

    void text_line(const char* text, int64_t at)
    {
        put(text, strlen(text), at);
        end_line(at);
    }

    void prompt(int64_t at) { put(">", 1, at); }

    /* A reply: its lines, then a blank line and the prompt. */
    void reply(const char* text, int64_t at)
    {
        text_line(text, at);
        end_line(at);
        prompt(at);
    }

    static int hex(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    static bool all_hex(const char* s, size_t n)
    {
        for (size_t i = 0; i < n; ++i) {
            if (hex(s[i]) < 0) {
                return false;
            }
        }
        return n > 0;
    }

    /* The hex value of s (n digits, at most 8), or -1 when it isn't
     * hex. */
    static long long hex_value(const char* s, size_t n)
    {
        if (n == 0) {
            return -1;
        }
        long long v = 0;
        for (size_t i = 0; i < n; ++i) {
            const int d = hex(s[i]);
            if (d < 0) {
                return -1;
            }
            v = (v << 4) | d;
        }
        return v;
    }

    void record(const char* cmd)
    {
        if (commands < kRecordDepth) {
            snprintf(recorded[commands], sizeof(recorded[commands]), "%s", cmd);
        }
        ++commands;
    }

    void run(int64_t now)
    {
        char cmd[64];
        size_t n = 0;
        for (size_t i = 0; i < in_len_ && n + 1 < sizeof(cmd); ++i) {
            char c = in_[i];
            if (c == ' ') {
                continue;
            }
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
            cmd[n++] = c;
        }
        cmd[n] = '\0';
        if (n == 0) {
            ++bare_crs;
            prompt(now);
            return;
        }
        record(cmd);
        if (k.refuse != nullptr && strcmp(cmd, k.refuse) == 0) {
            reply("?", now);
            return;
        }
        if (strncmp(cmd, "AT", 2) == 0) {
            at_command(cmd + 2, n - 2, now);
        } else if (strncmp(cmd, "ST", 2) == 0) {
            st_command(cmd + 2, now);
        } else if (all_hex(cmd, n)) {
            request(cmd, n, now);
        } else {
            reply("?", now);
        }
    }

    void at_command(const char* c, size_t n, int64_t now)
    {
        const auto is = [&](const char* name) { return strcmp(c, name) == 0; };
        const auto flag = [&](const char* name, bool* v) {
            const size_t len = strlen(name);
            if (n == len + 1 && strncmp(c, name, len) == 0 &&
                (c[len] == '0' || c[len] == '1')) {
                *v = c[len] == '1';
                return true;
            }
            return false;
        };
        bool ignored = false;
        if (is("Z") || is("WS")) {
            ++resets;
            settings_defaults();
            if (k.reset_ok) {
                reply("OK", now);
                return;
            }
            end_line(now);
            end_line(now);
            reply(k.version, now);
            return;
        }
        if (is("MA")) {
            monitoring_ = true; /* no answer until it's stopped */
        } else if (is("I")) {
            reply(k.version, now);
        } else if (is("RV")) {
            char v[16];
            snprintf(v, sizeof(v), "%u.%uV", kVbattMv / 1000,
                     (kVbattMv % 1000) / 100);
            reply(v, now);
        } else if (is("PPS")) {
            char line[64];
            snprintf(line, sizeof(line),
                     "28:FF F  29:FF F  2A:%02X %c  2B:02 F", k.pp2a,
                     k.pp2a_on ? 'N' : 'F');
            text_line("00:FF F  01:FF F  02:FF F  03:32 F", now);
            reply(line, now);
        } else if (is("DPN")) {
            char p[2] = {protocol_, '\0'};
            reply(p, now);
        } else if (is("D")) {
            settings_defaults();
            reply("OK", now);
        } else if (flag("E", &echo_) || flag("L", &linefeeds_) ||
                   flag("S", &spaces_) || flag("H", &headers_) ||
                   flag("D", &dlc_) || flag("CAF", &ignored) ||
                   flag("CFC", &ignored)) {
            reply("OK", now);
        } else if (n == 3 && strncmp(c, "AT", 2) == 0 && c[2] >= '0' &&
                   c[2] <= '2') {
            reply("OK", now);
        } else if (n == 4 && strncmp(c, "ST", 2) == 0 &&
                   hex_value(c + 2, 2) >= 0) {
            reply("OK", now);
        } else if (n == 3 && strncmp(c, "SP", 2) == 0 && hex(c[2]) >= 0) {
            protocol_ = c[2];
            reply("OK", now);
        } else if (strncmp(c, "SH", 2) == 0 && (n == 5 || n == 8) &&
                   hex_value(c + 2, n - 2) >= 0) {
            sh_ = static_cast<uint32_t>(hex_value(c + 2, n - 2));
            reply("OK", now);
        } else if (n == 4 && strncmp(c, "CP", 2) == 0 &&
                   hex_value(c + 2, 2) >= 0) {
            cp_ = static_cast<uint8_t>(hex_value(c + 2, 2) & 0x1F);
            reply("OK", now);
        } else if (strncmp(c, "CRA", 3) == 0 && (n == 6 || n == 11) &&
                   hex_value(c + 3, n - 3) >= 0) {
            cra_ = static_cast<uint32_t>(hex_value(c + 3, n - 3));
            cra_set_ = true;
            cfm_set_ = false;
            reply("OK", now);
        } else if ((strncmp(c, "CF", 2) == 0 || strncmp(c, "CM", 2) == 0) &&
                   (n == 5 || n == 10) && hex_value(c + 2, n - 2) >= 0) {
            const uint32_t v = static_cast<uint32_t>(hex_value(c + 2, n - 2));
            (c[1] == 'F' ? cf_ : cm_) = v;
            cfm_set_ = true;
            cra_set_ = false;
            reply("OK", now);
        } else {
            reply("?", now);
        }
    }

    void st_command(const char* c, int64_t now)
    {
        if (k.plain) {
            reply("?", now);
        } else if (strcmp(c, "I") == 0) {
            reply(kStnVersion, now);
        } else if (strcmp(c, "VR") == 0) {
            char v[16];
            snprintf(v, sizeof(v), "%u.%02u", kVbattMv / 1000,
                     (kVbattMv % 1000) / 10);
            reply(v, now);
        } else {
            reply("?", now);
        }
    }

    /* One CAN frame's line: the ID as the settings print it, then the
     * eight data bytes. */
    void frame_line(uint32_t id, const uint8_t* d, int64_t at)
    {
        char line[64];
        size_t n = 0;
        if (headers_) {
            n += static_cast<size_t>(snprintf(
                line + n, sizeof(line) - n,
                ext29() ? "%08X" : (k.four_digit_ids ? "%04X" : "%03X"), id));
            if (spaces_) {
                line[n++] = ' ';
            }
            if (dlc_) {
                line[n++] = '8';
                if (spaces_) {
                    line[n++] = ' ';
                }
            }
        }
        for (int i = 0; i < 8; ++i) {
            n += static_cast<size_t>(
                snprintf(line + n, sizeof(line) - n, "%02X", d[i]));
            if (spaces_ && i < 7) {
                line[n++] = ' ';
            }
        }
        line[n] = '\0';
        text_line(line, at);
    }

    /* A message as an ISO 15765-2 ECU sends it: a Single Frame, or a
     * First Frame and Consecutive Frames, padded to eight bytes. With
     * headers off the adapter strips the PCI bytes (the datasheet's
     * multi-line form for a long message). */
    void message(uint32_t id, const uint8_t* m, size_t len, int64_t at)
    {
        uint8_t f[8];
        if (!headers_) {
            char line[64];
            if (len <= 7) {
                size_t n = 0;
                for (size_t i = 0; i < len; ++i) {
                    n += static_cast<size_t>(
                        snprintf(line + n, sizeof(line) - n, "%02X", m[i]));
                }
                text_line(line, at);
                return;
            }
            snprintf(line, sizeof(line), "%03X", static_cast<unsigned>(len));
            text_line(line, at);
            size_t i = 0;
            for (unsigned idx = 0; i < len; ++idx) {
                size_t n = static_cast<size_t>(
                    snprintf(line, sizeof(line), "%X:", idx & 0xF));
                const size_t take = (idx == 0) ? 6 : 7;
                for (size_t b = 0; b < take && i < len; ++b, ++i) {
                    n += static_cast<size_t>(
                        snprintf(line + n, sizeof(line) - n, "%02X", m[i]));
                }
                text_line(line, at);
            }
            return;
        }
        if (len <= 7) {
            memset(f, 0xAA, sizeof(f));
            f[0] = static_cast<uint8_t>(len);
            memcpy(f + 1, m, len);
            frame_line(id, f, at);
            return;
        }
        f[0] = static_cast<uint8_t>(0x10 | ((len >> 8) & 0x0F));
        f[1] = static_cast<uint8_t>(len & 0xFF);
        memcpy(f + 2, m, 6);
        frame_line(id, f, at);
        size_t i = 6;
        for (uint8_t seq = 1; i < len;
             seq = static_cast<uint8_t>((seq + 1) & 0x0F)) {
            memset(f, 0xAA, sizeof(f));
            f[0] = static_cast<uint8_t>(0x20 | seq);
            const size_t take = (len - i < 7) ? len - i : 7;
            memcpy(f + 1, m + i, take);
            i += take;
            frame_line(id, f, at);
        }
    }

    void request(const char* cmd, size_t n, int64_t now)
    {
        ++requests;
        /* An odd digit at the end is the response count */
        const size_t digits = (n % 2 == 1) ? n - 1 : n;
        uint8_t req[8] = {};
        const size_t len = digits / 2;
        if (len == 0 || len > sizeof(req)) {
            reply("?", now);
            return;
        }
        for (size_t i = 0; i < len; ++i) {
            const long long v = hex_value(cmd + 2 * i, 2);
            if (v < 0) {
                reply("?", now);
                return;
            }
            req[i] = static_cast<uint8_t>(v);
        }
        if (k.silent) {
            return;
        }
        const int64_t at = now + int64_t{k.reply_delay_ms} * 1000;
        if (k.can_error) {
            reply("CAN ERROR", at);
            return;
        }
        const uint32_t tx = ext29() ? (uint32_t{cp_} << 24) | (sh_ & 0xFFFFFFu)
                                    : (sh_ & 0x7FFu);
        const uint32_t rx = fake_ecu::reply_id(tx);
        size_t answer_len = 0;
        if (k.no_data || rx == 0 || !passes(rx) ||
            !fake_ecu::answer(req, len, k.unsupported_nrc, answer_,
                              &answer_len) ||
            answer_len > 4095) {
            reply("NO DATA", at);
            return;
        }
        if (k.pending) {
            const uint8_t wait[3] = {0x7F, req[0], 0x78};
            for (uint32_t i = 0; i < k.pending_count; ++i) {
                message(rx, wait, sizeof(wait),
                        now + int64_t{i} * k.pending_gap_ms * 1000);
            }
        }
        message(rx, answer_, answer_len, at);
        end_line(at);
        prompt(at);
    }
};

/* The fake behind the transport's port opener: one adapter, its lock,
 * and what the opener saw. The worker blocks in read() until a byte is
 * due or its timeout ends. */
struct port_state {
    std::mutex lock;
    std::condition_variable changed;
    adapter a;
    uint32_t opens = 0;
    bool open = false;
    char last_name[128] = {};
    uint32_t last_serial = 0;
};

inline port_state& state()
{
    static port_state s;
    return s;
}

/* Runs fn(adapter&) under the port's lock, and wakes a waiting read. */
template <typename Fn> void with(Fn&& fn)
{
    port_state& s = state();
    {
        std::lock_guard<std::mutex> g(s.lock);
        fn(s.a);
    }
    s.changed.notify_all();
}

inline bool port_write(void*, const uint8_t* data, uint32_t len)
{
    port_state& s = state();
    {
        std::lock_guard<std::mutex> g(s.lock);
        if (s.a.k.dropped || !s.open) {
            return false;
        }
        s.a.from_host(data, len, steady_us());
    }
    s.changed.notify_all();
    return true;
}

inline int32_t port_read(void*, uint8_t* buf, uint32_t cap, uint32_t timeout_ms)
{
    port_state& s = state();
    std::unique_lock<std::mutex> g(s.lock);
    const int64_t deadline = steady_us() + int64_t{timeout_ms} * 1000;
    for (;;) {
        if (s.a.k.dropped || !s.open) {
            return -1;
        }
        const int64_t now = steady_us();
        const size_t n = s.a.to_host(buf, cap, now);
        if (n > 0) {
            return static_cast<int32_t>(n);
        }
        if (now >= deadline) {
            return 0;
        }
        const int64_t next = s.a.next_due_us();
        const int64_t until = (next < deadline) ? next : deadline;
        s.changed.wait_for(g, std::chrono::microseconds(until - now));
    }
}

inline void port_close(void*)
{
    port_state& s = state();
    std::lock_guard<std::mutex> g(s.lock);
    s.open = false;
}

const ecuio_elm327_port_ops_t kPortOps = {port_write, port_read, port_close};

/* ecuio_elm327_port_open_t: the fake, unless its link is down. */
inline ecuio_status_t port_open(const char* name, uint32_t serial_baud,
                                ecuio_elm327_port_t* out, char* why,
                                uint32_t why_cap)
{
    port_state& s = state();
    std::lock_guard<std::mutex> g(s.lock);
    snprintf(s.last_name, sizeof(s.last_name), "%s", name);
    s.last_serial = serial_baud;
    if (s.a.k.dropped) {
        snprintf(why, why_cap, "no such port");
        return ECUIO_ERR_LOAD_FAILED;
    }
    ++s.opens;
    s.open = true;
    s.a.drop_output();
    out->ops = &kPortOps;
    out->impl = &s;
    return ECUIO_OK;
}

/* Puts the fake behind the opener for a scope, cleared, with the OS's
 * ports back after it. */
struct installed {
    installed()
    {
        with([](adapter& a) { a.clear(); });
        {
            std::lock_guard<std::mutex> g(state().lock);
            state().opens = 0;
            state().open = false;
        }
        ecuio_elm327_set_port_opener(port_open);
    }
    ~installed() { ecuio_elm327_set_port_opener(nullptr); }
    installed(const installed&) = delete;
    installed& operator=(const installed&) = delete;
};

} // namespace fake_elm327

#endif /* ECUIO_TESTS_FAKE_ELM327_H */
