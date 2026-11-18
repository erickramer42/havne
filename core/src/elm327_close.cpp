// File/layer: core/src/elm327_close.cpp | C++17, Qt-free, every
//             platform (ADR-0060)
// Testing: HARNESS: Elm327SessionTest (test_elm327)
//
// The close: 10 01 to each channel that entered the extended session,
// after the reply the stop cut off, all within ELM327_CLOSE_MS.

#include "elm327_transport.h"

#include <chrono>

namespace ecuio_elm327 {

namespace {

/* How a channel answered the close's 10 01. */
struct leave_answer {
    bool stopped = false; /* STOPPED: a byte interrupted the adapter */
    bool positive = false;
    bool refused = false;
    uint8_t nrc = 0;
};

/* 10 01 on channel c, sent once more if the adapter answers STOPPED.
 * prompt when the adapter took it, whatever the ECU said. */
elm327_io leave_one(transport* t, uint32_t c, const char* cmd)
{
    leave_answer a;
    elm327_io io = elm327_io::prompt;
    for (int attempt = 0; attempt < 2; ++attempt) {
        a = leave_answer{};
        io = command(t, cmd, false, [&](const elm327_reader& r) {
            elm327_can_line f;
            const elm327_line kind = elm327_classify(r, t->ext29, &f);
            if (kind == elm327_line::stopped) {
                a.stopped = true;
                return;
            }
            /* A Single Frame from the channel's ECU: 50 01, or 7F 10 xx
             * (78 says it's still working, not how it ended) */
            const uint8_t n = f.data[0];
            if (kind != elm327_line::frame || f.id != t->rx_ids[c] ||
                (n & 0xF0u) != 0 || n < 2 || f.len < n + 1u) {
                return;
            }
            if (f.data[1] == ECUIO_SESSION_CONTROL + 0x40 &&
                f.data[2] == ECUIO_SESSION_DEFAULT) {
                a.positive = true;
            } else if (n >= 3 && f.data[1] == ECUIO_NEGATIVE_RESPONSE &&
                       f.data[2] == ECUIO_SESSION_CONTROL &&
                       f.data[3] != ECUIO_NRC_RESPONSE_PENDING) {
                a.refused = true;
                a.nrc = f.data[3];
            }
        });
        if (io != elm327_io::prompt || !a.stopped) {
            break;
        }
        elm327_log_write(t->log, "10 01 was answered STOPPED: sent again");
    }
    if (io != elm327_io::prompt) {
        return io;
    }
    const auto rx = static_cast<unsigned>(t->rx_ids[c]);
    if (a.positive) {
        elm327_log_write(t->log, "%X is back in the default session (50 01)",
                         rx);
    } else if (a.refused) {
        elm327_log_write(t->log, "%X refused 10 01 (7F 10 %02X)", rx,
                         static_cast<unsigned>(a.nrc));
    } else {
        elm327_log_write(t->log, "%X didn't answer 10 01", rx);
    }
    return io;
}

/* The reply the stop cut off, read to its prompt; one still running at
 * its wait is stopped by a space, which the adapter answers STOPPED. */
bool finish_owed(transport* t)
{
    auto skip = [](const elm327_reader&) {};
    elm327_log_write(t->log, "the close waits for %s's prompt", t->owed_cmd);
    const elm327_io io = reply_to(t, t->owed_cmd, skip);
    if (io != elm327_io::silent) {
        return io == elm327_io::prompt;
    }
    elm327_log_write(t->log, "the close stops %s's reply with a space",
                     t->owed_cmd);
    t->sent_at = std::chrono::steady_clock::now();
    const uint8_t stop = ' ';
    if (!t->port.ops->write(t->port.impl, &stop, 1)) {
        elm327_log_write(t->log, "the port failed (writing the close's stop)");
        return false;
    }
    return reply_to(t, " ", skip) == elm327_io::prompt;
}

} // namespace

/* At the close (ADR-0060): 10 01 to each channel that entered the
 * extended session, after any reply the stop cut off (typed into, it's lost). */
void leave_sessions(transport* t)
{
    const bool owed = t->prompt_owed;
    t->prompt_owed = false;
    if (!t->configured || !t->port_open) {
        return;
    }
    ecuio_request q[ECUIO_MAX_CHANNELS];
    bool leave[ECUIO_MAX_CHANNELS] = {};
    bool any = false;
    for (uint32_t c = 0; c < t->chan_count; ++c) {
        leave[c] = ecuio_engine_session_leave(&t->engine, c, &q[c]);
        any = any || leave[c];
    }
    if (!any) {
        return;
    }
    t->leaving = true;
    t->close_by = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(ELM327_CLOSE_MS);
    if (owed && !finish_owed(t)) {
        t->leaving = false;
        return;
    }
    for (uint32_t c = 0; c < t->chan_count; ++c) {
        if (!leave[c]) {
            continue;
        }
        char failed[ELM327_CMD_MAX];
        char cmd[ELM327_CMD_MAX];
        if (!set_header(t, t->tx_ids[c], failed).ok ||
            elm327_format_request(q[c].bytes, q[c].len, 0, cmd, sizeof(cmd)) ==
                0) {
            break;
        }
        elm327_log_write(t->log,
                         "the close returns %X to the default session (10 01)",
                         static_cast<unsigned>(t->rx_ids[c]));
        if (leave_one(t, c, cmd) != elm327_io::prompt) {
            break;
        }
    }
    t->leaving = false;
}

} // namespace ecuio_elm327
