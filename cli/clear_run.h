// File/layer: cli/clear_run.h | havne-cli clear (ADR-0038's appendix of
//             2026-11-06); public headers only
// Testing: UNIT: CliClearTest (test_cli); HARNESS: CliClearRunTest
//          (tests/cli_clear.py); golden clear-mock.jsonl
//
// `havne-cli clear <uri> --yes [--engine-running [--moving]]` clears
// each ECU's trouble codes through core's own gate (ADR-0038): the
// report the caller saved (its generation), the engine off by its own
// 01 0C unless the caller gives its word, each ECU by its own request
// ID. A script's word is --yes; core's gate is the rest. The report is
// written first: every ECU's `diagnostics` line goes out and the output
// is flushed before any clear is asked, and output that fails asks none.
// Then a `clear` line per ECU as its clear settles, the `diagnostics`
// line of the read that follows a 44 (or no answer), and the caller's
// end line from the result.

#ifndef HAVNE_CLI_CLEAR_RUN_H
#define HAVNE_CLI_CLEAR_RUN_H

#include "diagnostics_line.h"
#include "jsonl.h"

#include "ecuio/core.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace cli {

/* clear lines' outcome, in words. The run adds its own: "not-read" for
 * an ECU whose report never completed, "report-changed" when core
 * refused the clear at once, "not-asked" when it took none for another
 * reason, "no-outcome" for a clear that never settled */
inline const char* clear_outcome_word(uint8_t outcome)
{
    switch (outcome) {
    case ECUIO_CLEAR_NONE:
        return "none";
    case ECUIO_CLEAR_CLEARED:
        return "cleared";
    case ECUIO_CLEAR_REFUSED:
        return "refused";
    case ECUIO_CLEAR_UNANSWERED:
        return "unanswered";
    case ECUIO_CLEAR_ENGINE_RUNNING:
        return "engine-running";
    case ECUIO_CLEAR_NO_ENGINE_SPEED:
        return "no-engine-speed";
    case ECUIO_CLEAR_REPORT_CHANGED:
        return "report-changed";
    case ECUIO_CLEAR_MOVING:
        return "moving";
    case ECUIO_CLEAR_NO_VEHICLE_SPEED:
        return "no-vehicle-speed";
    default:
        return "unknown";
    }
}

/* {"type":"clear", rx, outcome, reply (a refusal's bytes, when one
 * came), rpm, kmh (what the clear read, null for none)}; outcome_word
 * overrides the report's (not-read, no-outcome) */
inline void clear_line(Line& l, const ecuio_channel_diagnostics_t& d,
                       const char* outcome_word = nullptr)
{
    l.raw("{\"type\":\"clear\"");
    l.key("rx");
    l.hex_id(d.rx_id);
    l.key("outcome");
    l.string(outcome_word != nullptr ? outcome_word
                                     : clear_outcome_word(d.clear),
             SIZE_MAX);
    if (outcome_word == nullptr && d.clear_nrc != 0) {
        /* 04's refusal, or 01 0C's or 01 0D's */
        const uint8_t service = d.clear == ECUIO_CLEAR_REFUSED ? 0x04 : 0x01;
        const uint8_t reply[3] = {0x7F, service, d.clear_nrc};
        l.key("reply");
        l.hex_bytes(reply, sizeof reply);
    }
    l.key("rpm");
    if (outcome_word == nullptr && (d.flags & ECUIO_DIAG_CLEAR_RPM) != 0) {
        l.u64(d.clear_rpm);
    } else {
        l.raw("null");
    }
    l.key("kmh");
    if (outcome_word == nullptr && (d.flags & ECUIO_DIAG_CLEAR_KMH) != 0) {
        l.u64(d.clear_kmh);
    } else {
        l.raw("null");
    }
    l.ch('}');
}

/* Whether d holds the outcome of a clear asked at report generation
 * asked: the worker takes a clear by moving the generation on with the
 * outcome none, which it stays while the clear runs (ECUIO_DIAG_CLEARING),
 * and settles it by moving it on again with an outcome */
inline bool clear_settled(const ecuio_channel_diagnostics_t& d, uint32_t asked)
{
    return d.generation > asked && d.clear != ECUIO_CLEAR_NONE;
}

/* Whether the ECU's report is read again after the outcome: a 44, or no
 * answer to 04 (which may still have cleared) */
inline bool clear_reads_again(uint8_t outcome)
{
    return outcome == ECUIO_CLEAR_CLEARED || outcome == ECUIO_CLEAR_UNANSWERED;
}

/* How a clear run ended */
struct ClearResult {
    const char* reason = "cleared"; /* cleared, not-cleared, nothing-read,
                                       output-failed, interrupted */
    uint32_t cleared = 0;           /* ECUs that answered 44 */
    uint32_t not_cleared = 0;       /* every other ECU of the session */
};

/* What the run writes through and is stopped by */
struct ClearIo {
    std::function<void(Line&)> write; /* one line, its newline added */
    std::function<bool()> flush;      /* false: the output failed */
    std::function<bool()> stop;       /* true: interrupted */
};

/* Clears every ECU of a started session, its `channels` reports read
 * through core: waits up to wait for them to complete, writes them,
 * asks each complete one's clear at its generation under flags
 * (ECUIO_CLEAR_*), writes each outcome as it settles (up to wait), then
 * each read that follows (up to wait). The caller writes start,
 * session and end. */
inline ClearResult
run_clear(ecuio_session_t* s, uint32_t channels, uint32_t flags,
          const ClearIo& io, std::chrono::milliseconds wait,
          std::chrono::milliseconds poll = std::chrono::milliseconds(50))
{
    using clock = std::chrono::steady_clock;
    ClearResult r;
    std::vector<ecuio_channel_diagnostics_t> d(channels);
    std::vector<uint32_t> asked(channels, 0);
    std::vector<uint8_t> state(channels, 0); /* see below */
    enum : uint8_t { kUnread, kRead, kAsked, kSettled, kDone };
    const auto read = [&](uint32_t c) {
        if (ecuio_session_get_channel_diagnostics(s, c, &d[c]) != ECUIO_OK) {
            d[c] = ecuio_channel_diagnostics_t{};
        }
    };
    const auto put = [&](const std::function<void(Line&)>& fill) {
        char buf[8192];
        Line l(buf, sizeof buf);
        fill(l);
        io.write(l);
    };
    /* Polls until every channel in from has left it, the wait passes or
     * a stop; step(c) moves a channel on and says whether it did */
    const auto until = [&](uint8_t from,
                           const std::function<bool(uint32_t)>& step) {
        const auto deadline = clock::now() + wait;
        for (;;) {
            bool left = false;
            for (uint32_t c = 0; c < channels; ++c) {
                if (state[c] == from) {
                    read(c);
                    if (!step(c)) {
                        left = true;
                    }
                }
            }
            if (!left || clock::now() >= deadline) {
                return true;
            }
            if (io.stop()) {
                return false;
            }
            std::this_thread::sleep_for(poll);
        }
    };

    /* 1. The report, every ECU's, written before anything is asked */
    if (!until(kUnread, [&](uint32_t c) {
            if ((d[c].flags & ECUIO_DIAG_COMPLETE) == 0) {
                return false;
            }
            put([&](Line& l) { diagnostics_line(l, d[c]); });
            asked[c] = d[c].generation;
            state[c] = kRead;
            return true;
        })) {
        r.reason = "interrupted";
        r.not_cleared = channels;
        return r;
    }
    if (!io.flush()) {
        r.reason = "output-failed";
        r.not_cleared = channels;
        return r;
    }
    bool any = false;
    for (uint32_t c = 0; c < channels; ++c) {
        any = any || state[c] == kRead;
    }
    if (!any) {
        r.reason = "nothing-read";
        for (uint32_t c = 0; c < channels; ++c) {
            put([&](Line& l) { clear_line(l, d[c], "not-read"); });
        }
        r.not_cleared = channels;
        return r;
    }

    /* 2. Each ECU's clear at the generation written */
    for (uint32_t c = 0; c < channels; ++c) {
        if (state[c] == kUnread) {
            put([&](Line& l) { clear_line(l, d[c], "not-read"); });
            state[c] = kDone;
            continue;
        }
        const ecuio_status_t st =
            ecuio_session_clear_diagnostics_with(s, c, asked[c], flags);
        if (st == ECUIO_OK) {
            state[c] = kAsked;
            continue;
        }
        /* refused before anything was sent: the report moved on since
         * it was written (or core took no clear at all) */
        put([&](Line& l) {
            clear_line(l, d[c],
                       st == ECUIO_ERR_REPORT_CHANGED ? "report-changed"
                                                      : "not-asked");
        });
        state[c] = kDone;
    }
    io.flush();

    /* 3. Each outcome as it settles */
    const bool settled_all = until(kAsked, [&](uint32_t c) {
        if (!clear_settled(d[c], asked[c])) {
            return false;
        }
        put([&](Line& l) { clear_line(l, d[c]); });
        if (d[c].clear == ECUIO_CLEAR_CLEARED) {
            ++r.cleared;
        }
        asked[c] = d[c].generation;
        state[c] = clear_reads_again(d[c].clear) ? kSettled : kDone;
        return true;
    });
    for (uint32_t c = 0; c < channels; ++c) {
        if (state[c] == kAsked) {
            put([&](Line& l) { clear_line(l, d[c], "no-outcome"); });
            state[c] = kDone;
        }
    }
    io.flush();

    /* 4. The read that follows a 44 or no answer */
    const bool read_all = settled_all && until(kSettled, [&](uint32_t c) {
                              if ((d[c].flags & ECUIO_DIAG_COMPLETE) == 0 ||
                                  d[c].generation < asked[c]) {
                                  return false;
                              }
                              put([&](Line& l) { diagnostics_line(l, d[c]); });
                              state[c] = kDone;
                              return true;
                          });
    io.flush();
    r.not_cleared = channels - r.cleared;
    if (!settled_all || !read_all) {
        r.reason = "interrupted";
    } else if (r.not_cleared > 0) {
        r.reason = "not-cleared";
    }
    return r;
}

/* {"type":"end", reason, cleared, not_cleared} */
inline void clear_end_line(Line& l, const ClearResult& r)
{
    l.raw("{\"type\":\"end\"");
    l.key("reason");
    l.string(r.reason, SIZE_MAX);
    l.key("cleared");
    l.u64(r.cleared);
    l.key("not_cleared");
    l.u64(r.not_cleared);
    l.ch('}');
}

} // namespace cli

#endif // HAVNE_CLI_CLEAR_RUN_H
