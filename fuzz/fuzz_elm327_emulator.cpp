// File/layer: fuzz/fuzz_elm327_emulator.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_elm327_emulator
//
// The ELM327 emulator's text (cli/elm327_emulator.h, ADR-0055): whatever
// a TCP host sends, before an unbound simulated ECU. The first byte picks
// the car (bit 0: modules=2; bit 1: every fault, each on its own period);
// the rest goes in a line at a time (each up to and with its CR), the
// clock moving on between lines by an amount the line picks (none, to a
// line arriving mid-reply, past the adapter's wait, past P2*). Checks:
// the output stays within a bound per input byte (a runaway loop or an
// echo of the whole history fails it); bytes come out only once due and
// never twice; everything queued is out once the clock passes the last
// wait; each request line also goes to ecuio_sim_ask directly, whose
// replies stay within ECUIO_SIM_MAX_REPLIES, 1 to 4095 bytes, on the
// car's reply IDs, never before the request, their reserved words and
// data past the length zero; two runs of the same input agree.

#include "elm327_emulator.h"

#include "ecuio/core.h"
#include "ecuio/sim.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

/* The most a byte of input may make: a CR alone repeats the last
 * command, which can be the longest reply's frames under headers, the
 * DLC digit and spaces, two ECUs' worth with a 7F xx 78 each */
constexpr size_t kOutPerByte = 2048;
constexpr int64_t kSecond = 1000000;

const char* uri_for(uint8_t mode)
{
    static const char* const kUris[] = {
        "mock://fuzz?values=canned",
        "mock://fuzz?values=canned&modules=2",
        "mock://fuzz?faults=silent:7,late:5,pending:3,bad_sequence:2,stalled:11",
        ("mock://fuzz?modules=2&faults=silent:7,late:5,pending:3,"
         "bad_sequence:2,stalled:11"),
    };
    return kUris[mode & 3];
}

int hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* The request a line holds, as the emulator would read it: hex pairs,
 * spaces dropped, an odd last digit the count; empty when it isn't one */
std::vector<uint8_t> request_of(const std::string& line)
{
    std::string digits;
    for (const char c : line) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0') {
            continue;
        }
        if (hex(c) < 0) {
            return {};
        }
        digits += c;
    }
    std::vector<uint8_t> req;
    for (size_t i = 0; i + 1 < digits.size(); i += 2) {
        req.push_back(static_cast<uint8_t>(hex(digits[i]) * 16 +
                                           hex(digits[i + 1])));
    }
    return req;
}

/* ecuio_sim_ask's contract for one request */
void check_ask(ecuio_sim_t* sim, uint32_t id, const std::vector<uint8_t>& req,
               int64_t t, bool modules)
{
    std::vector<ecuio_sim_reply_t> replies(ECUIO_SIM_MAX_REPLIES + 1);
    uint32_t n = 0;
    if (ecuio_sim_ask(sim, id, req.data(), static_cast<uint32_t>(req.size()),
                      t, replies.data(), ECUIO_SIM_MAX_REPLIES + 1,
                      &n) != ECUIO_OK) {
        abort();
    }
    if (n > ECUIO_SIM_MAX_REPLIES) {
        abort();
    }
    for (uint32_t i = 0; i < n; ++i) {
        const ecuio_sim_reply_t& r = replies[i];
        if (r.length == 0 || r.length > 4095 || r.delay_us < 0 ||
            (r.reply_id != 0x7E8 && !(modules && r.reply_id == 0x7E9))) {
            abort();
        }
        for (const uint32_t w : r.reserved) {
            if (w != 0) {
                abort();
            }
        }
        for (size_t b = r.length; b < sizeof r.data; ++b) {
            if (r.data[b] != 0) {
                abort();
            }
        }
        if ((r.flags & ~0xFu) != 0) {
            abort();
        }
    }
}

/* The emulator's output for data, checked as it goes */
std::string run(const uint8_t* data, size_t size)
{
    const uint8_t mode = data[0];
    const bool modules = (mode & 1) != 0;
    ecuio_sim_t* sim = nullptr;
    ecuio_sim_t* asked = nullptr;
    if (ecuio_sim_create_unbound(uri_for(mode), 0, &sim) != ECUIO_OK ||
        ecuio_sim_create_unbound(uri_for(mode), 0, &asked) != ECUIO_OK) {
        abort();
    }
    std::string out;
    {
        cli::Elm327Emulator emu(sim);
        int64_t now = kSecond;
        uint8_t buf[512];
        const auto drain = [&](int64_t until) {
            for (;;) {
                const int64_t due = emu.next_due_us();
                const size_t got = emu.to_host(buf, sizeof buf, until);
                if (got == 0) {
                    if (due <= until) {
                        abort(); /* due, yet nothing came out */
                    }
                    return;
                }
                if (due > until) {
                    abort(); /* out before it was due */
                }
                out.append(reinterpret_cast<const char*>(buf), got);
            }
        };
        size_t at = 1;
        while (at < size) {
            size_t end = at;
            while (end < size && data[end] != '\r') {
                ++end;
            }
            if (end < size) {
                ++end; /* with its CR */
            }
            const std::string line(reinterpret_cast<const char*>(data + at),
                                   end - at);
            emu.from_host(data + at, end - at, now);
            const std::vector<uint8_t> req = request_of(line);
            if (!req.empty() && req.size() <= 7) {
                check_ask(asked, 0x7DF, req, now, modules);
                check_ask(asked, 0x7E0 + (line.size() & 1), req, now,
                          modules);
            }
            /* the clock moves on by what the line picks */
            static const int64_t kSteps[] = {0, 1000, 120000, 210000,
                                             5 * kSecond + 300000};
            now += kSteps[line.size() % 5];
            drain(now);
            if (out.size() > 64 + kOutPerByte * end) {
                abort();
            }
            at = end;
        }
        /* past the last wait, nothing is left */
        now += 60 * kSecond;
        drain(now);
        if (emu.next_due_us() != INT64_MAX) {
            abort();
        }
        if (out.size() > 64 + kOutPerByte * size) {
            abort();
        }
    }
    ecuio_sim_destroy(asked);
    ecuio_sim_destroy(sim);
    return out;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 4096) {
        return 0;
    }
    const std::string first = run(data, size);
    if (run(data, size) != first) {
        abort();
    }
    return 0;
}
