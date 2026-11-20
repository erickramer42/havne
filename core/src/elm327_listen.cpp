// File/layer: core/src/elm327_listen.cpp | C++17, Qt-free, every
//             platform (ADR-0026's appendix "passive listening")
// Testing: HARNESS: Elm327SessionTest (test_elm327)
//
// A session with nothing to ask monitors with AT MA, which keeps the
// receive filter the setup set, so it hears the channels' reply IDs on
// every adapter. The monitor is silent: no ACK, no flow control.

#include "elm327_transport.h"

#include <chrono>

namespace ecuio_elm327 {

namespace {

/* BUFFER FULL: the adapter dropped lines it couldn't send in time. The
 * next frame dispatched says so (ADR-0026's appendix "passive
 * listening"); the link's state is unchanged. */
void note_overflow(transport* t)
{
    t->overflow_pending = true;
    elm327_log_write(t->log, "BUFFER FULL: frames were lost");
}

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

/* A monitor's line handler, and what its lines said */
struct monitor_lines {
    transport* t;
    link_health& h;
    monitor_end& end;
    bool full = false;
    bool reset = false;
    bool heard_now = false; /* a message in this 100 ms window */

    void operator()(const elm327_reader& r)
    {
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
    }
};

/* AT MA, its time kept; false when the port failed */
bool start_monitor(transport* t)
{
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
        return false;
    }
    return true;
}

/* The check after check_ms with no line: the monitor stopped, and what
 * its answer says of the adapter */
void check_adapter(transport* t, uint32_t check_ms, monitor_lines& lines)
{
    monitor_end& end = lines.end;
    elm327_log_write(t->log,
                     "no line for %s: the monitor stops to "
                     "check the adapter",
                     words_of(check_ms).text);
    end.io = stop_monitor(t, check_ms, lines);
    if (lines.reset) {
        end.io = elm327_io::reset;
    } else if (end.io == elm327_io::prompt) {
        end.at_once = true;
        elm327_log_write(t->log, "the adapter answered the stop: AT MA "
                                 "again");
    } else if (end.io == elm327_io::silent) {
        elm327_log_write(t->log, "no prompt within %s of the stop",
                         words_of(check_ms).text);
    }
}

/* The monitor's lines in read slices until something ends it */
void run_monitor(transport* t, monitor_lines& lines)
{
    monitor_end& end = lines.end;
    auto last_line = t->sent_at;
    auto window = t->sent_at;
    for (;;) {
        link_judge_waiting(lines.h);
        bool line = false;
        const bool prompt = take_bytes(t, "ATMA", lines, &line);
        if (lines.reset) {
            end.io = elm327_io::reset;
            break;
        }
        if (prompt) {
            end.at_once = lines.full;
            elm327_log_write(t->log,
                             lines.full
                                 ? "the monitor ended at BUFFER FULL: AT MA "
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
            (void)stop_monitor(t, ELM327_MONITOR_DETACH_MS, lines);
            elm327_log_write(t->log, "the monitor stopped: the session "
                                     "closes");
            end.io = elm327_io::stopped;
            break;
        }
        if (now - window >= std::chrono::milliseconds(ELM327_IDLE_READ_MS)) {
            if (!lines.heard_now) {
                link_on_read(lines.h, false, false, true);
            }
            lines.heard_now = false;
            window = now;
        }
        const uint32_t check_ms = monitor_check_ms();
        if (now - last_line >= std::chrono::milliseconds(check_ms)) {
            check_adapter(t, check_ms, lines);
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
}

} // namespace

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
    if (!start_monitor(t)) {
        end.io = elm327_io::failed;
        return end;
    }
    monitor_lines lines{t, h, end};
    run_monitor(t, lines);
    /* a message under way when the monitor ended never finishes */
    (void)isotp_end_all(t->isotp);
    return end;
}

} // namespace ecuio_elm327
