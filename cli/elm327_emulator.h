// File/layer: cli/elm327_emulator.h | havne-cli, the ELM327 emulator's
//             text (ADR-0055); public headers only
// Testing: UNIT: CliElm327Test (test_cli); HARNESS: CliElm327RunTest
//
// An ELM327 v1.4b as its datasheet describes it and tests/fake_elm327.h
// models it, bytes in and bytes out, in front of an unbound simulated
// ECU (ecuio_sim_ask): the AT settings it keeps (echo, linefeeds,
// spaces, headers, the DLC digit, responses, the protocol, the header,
// the receive filter, the timeout), its replies ended by a blank line
// and the '>' prompt, and requests answered by the sim as the CAN frames
// an ISO 15765 ECU sends, one line each. Pure: the caller passes the
// time in (microseconds, the sim's clock), and each byte for the host is
// due at a time, so a reply 130 ms late prints 130 ms late.
//
// The simulated car is on 11-bit CAN at 500 kbaud: protocol 6, or the
// automatic search (0), which finds 6 and says SEARCHING... once; a
// 29-bit protocol (7, 9) hears no ECU (NO DATA), 250 kbaud (8) is the
// wrong rate (CAN ERROR), and the protocols before CAN don't connect
// (UNABLE TO CONNECT). The adapter waits AT ST's timeout (4.096 ms a
// count, 0x32 by default) for each next frame, P2* (5 s) after a
// 7F <service> 78, so a reply past it is lost, as an adapter loses it;
// the response count digit ends a request after that many frame lines.
// A plain ELM327: ST commands answer '?'.

#ifndef HAVNE_CLI_ELM327_EMULATOR_H
#define HAVNE_CLI_ELM327_EMULATOR_H

#include "ecuio/core.h"
#include "ecuio/sim.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace cli {

class Elm327Emulator
{
public:
    static constexpr const char* kVersion = "ELM327 v1.4b";
    static constexpr const char* kDescription = "OBDII to RS232 Interpreter";
    static constexpr const char* kVoltage = "13.8V";
    /* AT ST's unit, and its default count */
    static constexpr int64_t kTimeoutStepUs = 4096;
    static constexpr uint8_t kTimeoutDefault = 0x32;
    /* P2*: how long an ECU's 7F <service> 78 has the adapter wait on */
    static constexpr int64_t kPendingWaitUs = 5000000;
    /* Consecutive Frames one after another at STmin 0 */
    static constexpr int64_t kFrameGapUs = 500;
    /* The stalled fault's gap before the last Consecutive Frame */
    static constexpr int64_t kStalledGapUs = 160000;

    /* sim: an unbound sim (ecuio_sim_create_unbound), not owned */
    explicit Elm327Emulator(ecuio_sim_t* sim) : sim_(sim) { power_on(); }

    /* A new link: the settings as at power-on, nothing queued */
    void power_on()
    {
        defaults();
        protocol_ = '0';
        searched_ = false;
        out_.clear();
        due_at_ = 0;
        line_.clear();
        last_.clear();
    }

    /* Bytes from the host at now_us */
    void from_host(const uint8_t* data, size_t n, int64_t now_us)
    {
        for (size_t i = 0; i < n; ++i) {
            const char c = static_cast<char>(data[i]);
            if (monitoring_) {
                /* any byte stops the monitor and is discarded */
                monitoring_ = false;
                reply("STOPPED", now_us);
                continue;
            }
            if (echo_ && c != '\n') {
                put(std::string(1, c), now_us);
                if (c == '\r' && linefeeds_) {
                    put("\n", now_us);
                }
            }
            if (c == '\r') {
                std::string cmd;
                bool too_long = line_.size() > kLineMax;
                for (const char k : line_) {
                    if (k == ' ' || k == '\t') {
                        continue;
                    }
                    cmd +=
                        (k >= 'a' && k <= 'z') ? static_cast<char>(k - 32) : k;
                }
                line_.clear();
                if (cmd.empty() && !too_long) {
                    /* a CR alone repeats the last command */
                    if (last_.empty()) {
                        prompt(now_us);
                        continue;
                    }
                    cmd = last_;
                }
                if (too_long) {
                    reply("?", now_us);
                    continue;
                }
                last_ = cmd;
                ++commands;
                run(cmd, now_us);
            } else if (c != '\n' && c != '\0') {
                if (line_.size() <= kLineMax) {
                    line_ += c;
                }
            }
        }
    }

    /* The bytes for the host due by now_us, up to cap */
    size_t to_host(uint8_t* out, size_t cap, int64_t now_us)
    {
        size_t n = 0;
        while (n < cap && !out_.empty() && out_.front().at <= now_us) {
            Chunk& c = out_.front();
            const size_t take = std::min(cap - n, c.text.size() - c.sent);
            std::copy_n(c.text.data() + c.sent, take, out + n);
            n += take;
            c.sent += take;
            if (c.sent == c.text.size()) {
                out_.pop_front();
            }
        }
        return n;
    }

    /* When the next byte for the host is due; INT64_MAX when none */
    int64_t next_due_us() const
    {
        return out_.empty() ? INT64_MAX : out_.front().at;
    }

    bool monitoring() const { return monitoring_; }

    /* Commands and requests taken since construction */
    uint64_t commands = 0;
    uint64_t requests = 0;

private:
    static constexpr size_t kLineMax = 64;

    struct Chunk {
        int64_t at;
        std::string text;
        size_t sent;
    };

    /* One CAN frame a reply puts on the bus */
    struct Frame {
        int64_t at;
        uint32_t id;
        uint8_t data[8];
        bool pending;     /* a 7F <service> 78 */
        uint32_t message; /* its message's place in the replies */
    };

    ecuio_sim_t* sim_;
    std::deque<Chunk> out_;
    int64_t due_at_ = 0; /* output stays in order: nothing due before */
    std::string line_;
    std::string last_;

    bool echo_ = true;
    bool linefeeds_ = false;
    bool spaces_ = true;
    bool headers_ = false;
    bool dlc_ = false;
    bool responses_ = true;
    bool monitoring_ = false;
    char protocol_ = '0';
    bool searched_ = false; /* the automatic search found protocol 6 */
    uint32_t header_ = 0x7DF;
    bool cra_set_ = false;
    uint32_t cra_ = 0;
    bool cfm_set_ = false;
    uint32_t cf_ = 0;
    uint32_t cm_ = 0x7FF;
    uint8_t timeout_ = kTimeoutDefault;

    void defaults()
    {
        monitoring_ = false;
        echo_ = true;
        linefeeds_ = false;
        spaces_ = true;
        headers_ = false;
        dlc_ = false;
        responses_ = true;
        header_ = 0x7DF;
        cra_set_ = cfm_set_ = false;
        cf_ = 0;
        cm_ = 0x7FF;
        timeout_ = kTimeoutDefault;
    }

    void put(const std::string& text, int64_t at)
    {
        at = std::max(at, due_at_);
        due_at_ = at;
        if (!out_.empty() && out_.back().at == at) {
            out_.back().text += text;
        } else {
            out_.push_back(Chunk{at, text, 0});
        }
    }

    void end_line(int64_t at) { put(linefeeds_ ? "\r\n" : "\r", at); }

    void text_line(const std::string& text, int64_t at)
    {
        put(text, at);
        end_line(at);
    }

    void prompt(int64_t at) { put(">", at); }

    /* An answer's lines, a blank line and the prompt */
    void reply(const std::string& text, int64_t at)
    {
        text_line(text, at);
        end_line(at);
        prompt(at);
    }

    static int hex(char c)
    {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    /* s as hex (at most 8 digits); -1 when it isn't */
    static long long hex_value(const std::string& s)
    {
        if (s.empty() || s.size() > 8) {
            return -1;
        }
        long long v = 0;
        for (const char c : s) {
            const int d = hex(c);
            if (d < 0) {
                return -1;
            }
            v = (v << 4) | d;
        }
        return v;
    }

    static std::string hex2(unsigned v)
    {
        char b[4];
        std::snprintf(b, sizeof b, "%02X", v & 0xFFu);
        return b;
    }

    bool passes(uint32_t id) const
    {
        if (cra_set_) {
            return id == cra_;
        }
        return !cfm_set_ || (id & cm_) == (cf_ & cm_);
    }

    int64_t timeout_us() const
    {
        return int64_t{timeout_ == 0 ? kTimeoutDefault : timeout_} *
               kTimeoutStepUs;
    }

    /* The protocol's name, as AT DP says it */
    std::string protocol_name() const
    {
        static const char* const kNames[] = {
            "AUTO",
            "SAE J1850 PWM",
            "SAE J1850 VPW",
            "ISO 9141-2",
            "ISO 14230-4 (KWP 5BAUD)",
            "ISO 14230-4 (KWP FAST)",
            "ISO 15765-4 (CAN 11/500)",
            "ISO 15765-4 (CAN 29/500)",
            "ISO 15765-4 (CAN 11/250)",
            "ISO 15765-4 (CAN 29/250)",
            "SAE J1939 (CAN 29/250)",
            "USER1 CAN (11* bit ID, 125* kbaud)",
            "USER2 CAN (11* bit ID, 50* kbaud)",
        };
        const int p = hex(protocol_);
        if (protocol_ == '0') {
            return searched_ ? std::string("AUTO, ") + kNames[6] : kNames[0];
        }
        return (p > 0 && p <= 0xC) ? kNames[p] : "?";
    }

    void run(const std::string& cmd, int64_t now)
    {
        if (cmd.compare(0, 2, "AT") == 0) {
            at_command(cmd.substr(2), now);
        } else if (cmd.compare(0, 2, "ST") == 0) {
            reply("?", now); /* a plain ELM327: no STN commands */
        } else {
            request(cmd, now);
        }
    }

    /* An AT command (after the AT) answered OK and changing nothing the
     * emulator prints: the CAN formatting it always has (CAF1, CFC1),
     * the timing, the memory, the other protocols' settings */
    static bool taken_as_is(const std::string& c)
    {
        const auto starts = [&](const char* p) {
            return c.compare(0, std::strlen(p), p) == 0;
        };
        const auto bit = [&](const char* name) {
            const size_t len = std::strlen(name);
            return c.size() == len + 1 && c.compare(0, len, name) == 0 &&
                   (c[len] == '0' || c[len] == '1');
        };
        const auto hex_after = [&](size_t at) {
            return hex_value(c.substr(at)) >= 0;
        };
        static const char* const kAlone[] = {"AL",    "NL",   "BI",   "PC",
                                             "SS",    "LP",   "IB10", "IB96",
                                             "FCSM0", "FCSM1"};
        for (const char* k : kAlone) {
            if (c == k) {
                return true;
            }
        }
        return bit("CAF") || bit("CFC") || bit("CSM") || bit("M") || bit("V") ||
               bit("KW") || bit("AL") || bit("NL") ||
               (c.size() == 3 && starts("AT") && c[2] >= '0' && c[2] <= '2') ||
               (starts("CP") && c.size() == 4 && hex_after(2)) ||
               (starts("FCSH") && (c.size() == 7 || c.size() == 12) &&
                hex_after(4)) ||
               (starts("FCSD") && c.size() >= 6 && hex_after(4)) ||
               (starts("CV") && c.size() == 6) ||
               (starts("SR") && c.size() == 4 && hex_after(2)) ||
               (starts("CEA") && c.size() <= 5);
    }

    void at_command(const std::string& c, int64_t now)
    {
        const auto is = [&](const char* name) { return c == name; };
        const auto starts = [&](const char* p) {
            return c.compare(0, std::strlen(p), p) == 0;
        };
        const auto flag = [&](const char* name, bool* v) {
            const size_t len = std::strlen(name);
            if (c.size() == len + 1 && c.compare(0, len, name) == 0 &&
                (c[len] == '0' || c[len] == '1')) {
                *v = c[len] == '1';
                return true;
            }
            return false;
        };
        if (is("Z") || is("WS")) {
            defaults();
            end_line(now);
            end_line(now);
            reply(kVersion, now);
            return;
        }
        if (is("D")) {
            defaults();
            protocol_ = '0';
            searched_ = false;
            reply("OK", now);
        } else if (is("I")) {
            reply(kVersion, now);
        } else if (is("@1")) {
            reply(kDescription, now);
        } else if (is("RV")) {
            reply(kVoltage, now);
        } else if (is("DP")) {
            reply(protocol_name(), now);
        } else if (is("DPN")) {
            if (protocol_ == '0') {
                reply(searched_ ? "A6" : "0", now);
            } else {
                reply(std::string(1, protocol_), now);
            }
        } else if (is("MA")) {
            monitoring_ = true; /* nothing else on the bus: quiet */
        } else if (is("CS")) {
            reply("T:00 R:00", now);
        } else if (is("AR") || is("CRA")) {
            cra_set_ = cfm_set_ = false;
            reply("OK", now);
        } else if (flag("E", &echo_) || flag("L", &linefeeds_) ||
                   flag("S", &spaces_) || flag("H", &headers_) ||
                   flag("D", &dlc_) || flag("R", &responses_) ||
                   taken_as_is(c)) {
            reply("OK", now);
        } else if ((starts("ST") || starts("SW") || starts("TA")) &&
                   c.size() == 4 && hex_value(c.substr(2)) >= 0) {
            if (starts("ST")) {
                timeout_ = static_cast<uint8_t>(hex_value(c.substr(2)));
            }
            reply("OK", now);
        } else if ((starts("SP") || starts("TP")) &&
                   (c.size() == 3 || (c.size() == 4 && c[2] == 'A')) &&
                   hex(c.back()) >= 0 && hex(c.back()) <= 0xC) {
            /* SPAh: automatic, h tried first; the car's is 6 either way */
            protocol_ = c.size() == 4 ? '0' : c.back();
            searched_ = false;
            reply("OK", now);
        } else if (starts("SH") &&
                   (c.size() == 5 || c.size() == 8 || c.size() == 10) &&
                   hex_value(c.substr(2)) >= 0) {
            /* 11-bit: the last three digits */
            header_ = static_cast<uint32_t>(hex_value(c.substr(2))) & 0xFFFu;
            reply("OK", now);
        } else if (starts("CRA") && (c.size() == 6 || c.size() == 11) &&
                   hex_value(c.substr(3)) >= 0) {
            cra_ = static_cast<uint32_t>(hex_value(c.substr(3)));
            cra_set_ = true;
            cfm_set_ = false;
            reply("OK", now);
        } else if ((starts("CF") || starts("CM")) &&
                   (c.size() == 5 || c.size() == 10) &&
                   hex_value(c.substr(2)) >= 0) {
            const uint32_t v = static_cast<uint32_t>(hex_value(c.substr(2)));
            (c[1] == 'F' ? cf_ : cm_) = v;
            cfm_set_ = true;
            cra_set_ = false;
            reply("OK", now);
        } else {
            reply("?", now);
        }
    }

    /* A request: hex bytes, then an optional response count digit */
    void request(const std::string& cmd, int64_t now)
    {
        const size_t digits = cmd.size() % 2 == 1 ? cmd.size() - 1 : cmd.size();
        const int count_digit = cmd.size() % 2 == 1 ? hex(cmd.back()) : 0;
        std::vector<uint8_t> req;
        for (size_t i = 0; i < digits; i += 2) {
            const int hi = hex(cmd[i]);
            const int lo = hex(cmd[i + 1]);
            if (hi < 0 || lo < 0) {
                reply("?", now);
                return;
            }
            req.push_back(static_cast<uint8_t>(hi * 16 + lo));
        }
        if (count_digit < 0 || req.empty() || req.size() > 7) {
            reply("?", now);
            return;
        }
        ++requests;
        if (protocol_ == '0' && !searched_) {
            text_line("SEARCHING...", now);
            searched_ = true;
        }
        const char p = protocol_ == '0' ? '6' : protocol_;
        const int64_t wait = timeout_us();
        if (p == '7' || p == '9') {
            reply("NO DATA", now + wait); /* the car's ECUs are 11-bit */
            return;
        }
        if (p == '8') {
            reply("CAN ERROR", now + wait); /* the car's bus is 500 kbaud */
            return;
        }
        if (p != '6') {
            reply("UNABLE TO CONNECT", now + wait);
            return;
        }
        std::vector<ecuio_sim_reply_t> replies(ECUIO_SIM_MAX_REPLIES);
        uint32_t n = 0;
        if (ecuio_sim_ask(sim_, header_ & 0x7FFu, req.data(),
                          static_cast<uint32_t>(req.size()), now,
                          replies.data(), ECUIO_SIM_MAX_REPLIES,
                          &n) != ECUIO_OK) {
            n = 0;
        }
        replies.resize(std::min<uint32_t>(n, ECUIO_SIM_MAX_REPLIES));
        if (!responses_) {
            prompt(now);
            return;
        }
        print_frames(frames_of(replies, now), now, count_digit, wait);
    }

    /* The CAN frames the replies put on the bus, in time order */
    static std::vector<Frame>
    frames_of(const std::vector<ecuio_sim_reply_t>& replies, int64_t now)
    {
        std::vector<Frame> frames;
        for (uint32_t m = 0; m < replies.size(); ++m) {
            const ecuio_sim_reply_t& r = replies[m];
            const uint32_t len = std::min<uint32_t>(r.length, 4095);
            int64_t at = now + r.delay_us;
            Frame f{};
            f.id = r.reply_id;
            f.message = m;
            f.pending = len == 3 && r.data[0] == 0x7F && r.data[2] == 0x78;
            if (len <= 7) {
                f.at = at;
                f.data[0] = static_cast<uint8_t>(len);
                std::memcpy(f.data + 1, r.data, len);
                frames.push_back(f);
                continue;
            }
            f.at = at;
            f.data[0] = static_cast<uint8_t>(0x10 | ((len >> 8) & 0x0F));
            f.data[1] = static_cast<uint8_t>(len & 0xFF);
            std::memcpy(f.data + 2, r.data, 6);
            frames.push_back(f);
            uint32_t i = 6;
            uint8_t seq = 1;
            while (i < len) {
                const uint32_t take = std::min<uint32_t>(len - i, 7);
                const bool last = i + take >= len;
                Frame cf{};
                cf.id = r.reply_id;
                cf.message = m;
                at += (last && (r.flags & ECUIO_SIM_REPLY_STALLED) != 0)
                          ? kStalledGapUs
                          : kFrameGapUs;
                cf.at = at;
                const uint8_t s =
                    (last && (r.flags & ECUIO_SIM_REPLY_BAD_SEQUENCE) != 0)
                        ? static_cast<uint8_t>((seq + 1) & 0x0F)
                        : seq;
                cf.data[0] = static_cast<uint8_t>(0x20 | s);
                std::memcpy(cf.data + 1, r.data + i, take);
                frames.push_back(cf);
                i += take;
                seq = static_cast<uint8_t>((seq + 1) & 0x0F);
            }
        }
        std::stable_sort(
            frames.begin(), frames.end(),
            [](const Frame& a, const Frame& b) { return a.at < b.at; });
        return frames;
    }

    /* The frames as the adapter prints them: each within its wait of
     * the one before (P2* after a pending reply), at most count_digit
     * lines (0: no count), then the prompt */
    void print_frames(const std::vector<Frame>& frames, int64_t now,
                      int count_digit, int64_t wait)
    {
        int64_t deadline = now + wait;
        int64_t last_at = now;
        int lines = 0;
        bool any = false;
        bool counted_out = false;
        for (const Frame& f : frames) {
            if (f.at > deadline) {
                break; /* the adapter stopped listening before it */
            }
            if (!passes(f.id)) {
                continue;
            }
            last_at = f.at;
            frame_line(f, frames);
            any = true;
            deadline = f.at + (f.pending ? kPendingWaitUs : wait);
            if (!f.pending && count_digit > 0 && ++lines >= count_digit) {
                counted_out = true;
                break;
            }
        }
        if (!any) {
            reply("NO DATA", now + wait);
            return;
        }
        const int64_t end = counted_out ? last_at : last_at + wait;
        end_line(end);
        prompt(end);
    }

    /* One frame's line: with headers, the ID, the DLC digit and the
     * eight bytes; without, the data alone (a long message's First
     * Frame as its length and line 0:, each Consecutive Frame its
     * sequence number and its bytes) */
    void frame_line(const Frame& f, const std::vector<Frame>& all)
    {
        const char* sep = spaces_ ? " " : "";
        std::string line;
        const uint8_t pci = f.data[0] >> 4;
        if (headers_) {
            char id[8];
            std::snprintf(id, sizeof id, "%03X", static_cast<unsigned>(f.id));
            line += id;
            line += sep;
            if (dlc_) {
                line += "8";
                line += sep;
            }
            for (int i = 0; i < 8; ++i) {
                line += hex2(f.data[i]);
                if (i < 7) {
                    line += sep;
                }
            }
            text_line(line, f.at);
            return;
        }
        if (pci == 0) {
            for (uint8_t i = 0; i < f.data[0]; ++i) {
                if (i > 0) {
                    line += sep;
                }
                line += hex2(f.data[1 + i]);
            }
            text_line(line, f.at);
            return;
        }
        if (pci == 1) {
            const unsigned len = ((f.data[0] & 0x0Fu) << 8) | f.data[1];
            char head[8];
            std::snprintf(head, sizeof head, "%03X", len);
            text_line(head, f.at);
            line = "0:";
            for (int i = 2; i < 8; ++i) {
                line += sep;
                line += hex2(f.data[i]);
            }
            text_line(line, f.at);
            return;
        }
        /* A Consecutive Frame: its bytes, short of the message's end */
        unsigned len = 0;
        unsigned before = 0;
        for (const Frame& g : all) {
            if (g.message != f.message) {
                continue;
            }
            if ((g.data[0] >> 4) == 1) {
                len = ((g.data[0] & 0x0Fu) << 8) | g.data[1];
                before = 6;
            } else if (&g == &f) {
                break;
            } else {
                before += 7;
            }
        }
        const unsigned take =
            len > before ? std::min<unsigned>(len - before, 7) : 7;
        char idx[4];
        std::snprintf(idx, sizeof idx, "%X:", f.data[0] & 0x0Fu);
        line = idx;
        for (unsigned i = 0; i < take; ++i) {
            line += sep;
            line += hex2(f.data[1 + i]);
        }
        text_line(line, f.at);
    }
};

} // namespace cli

#endif // HAVNE_CLI_ELM327_EMULATOR_H
