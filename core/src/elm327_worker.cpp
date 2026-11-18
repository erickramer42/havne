// File/layer: core/src/elm327_worker.cpp | C++17, Qt-free, every
//             platform (ADR-0026)
// Testing: HARNESS: Elm327SessionTest, Elm327TalkTest, Elm327TurnsTest
//          (test_elm327); BENCH
//
// The worker and a request's exchange: the I/O under every command,
// the header switch, a reply's lines into frames, a cut reply's hold.
// The whole requesting path is in this file and the header.

#include "elm327_transport.h"

#include "request_setup.h"
#include "request_turns.h"
#include "precise_wait.h"

#include <chrono>
#include <cstring>

namespace ecuio_elm327 {

namespace {

/* A reply the count digit cut after its First Frame goes on on the bus:
 * the adapter sent the flow control, and the ECU sends the rest and
 * takes no request meanwhile. Its channel is held this long for each
 * Consecutive Frame still to come, plus the margin (the test car's
 * engine ECU sent one every 8 ms, docs/bench-sessions/2026-11-01-android.md).
 * The first hold is at most a second (ISO-TP's longest message would be
 * 5.9 s); a request asked again after it that still gets NO DATA is
 * held again, twice as long, at most this many times. */
constexpr int64_t ELM327_CUT_FRAME_US = 10000;
constexpr int64_t ELM327_CUT_MARGIN_US = 50000;
constexpr int64_t ELM327_CUT_HOLD_MAX_US = 1000000;
constexpr uint8_t ELM327_CUT_RETRIES = 2;

int64_t now_us(const transport* t)
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            host_clock::now() - t->epoch)
            .count());
}

} // namespace

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

/* A frame line toward its message; a whole one from a channel's ECU is
 * dispatched, flagged reassembled, and handed to the engine when
 * to_engine (a monitor's
 * messages answer no request of ours). One from another ID got past the
 * receive filter's mask and is dropped, as J2534's are. */
void take_frame(transport* t, const elm327_can_line& f, reply_seen& seen,
                bool to_engine)
{
    isotp_message m;
    if (isotp_take(t->isotp, f, 0, &m) != isotp_result::complete) {
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
        /* A reply's payload or a monitored message's, either way core's
         * ISO-TP put it together from the adapter's lines */
        frame.flags = (t->ext29 ? ECUIO_FRAME_FLAG_EXTENDED : 0u) |
                      ECUIO_FRAME_FLAG_REASSEMBLED;
        if (t->overflow_pending) {
            frame.flags |= ECUIO_FRAME_FLAG_OVERFLOW;
            t->overflow_pending = false;
        }
        frame.reserved = 0;
        /* ISO-TP's longest message, 4095 bytes, fits the frame whole */
        memcpy(frame.data, m.data, m.len);
        frame.data_length = m.len;
        if (m.len == 3 && m.data[0] == ECUIO_NEGATIVE_RESPONSE &&
            m.data[2] == ECUIO_NRC_RESPONSE_PENDING) {
            seen.pending = true;
        }
        ecuio_session_dispatch_callbacks(t->session, &frame);
        if (to_engine) {
            (void)ecuio_engine_on_frame(&t->engine, c, frame.data,
                                        frame.data_length, at, nullptr);
        }
        seen.matched = true;
        return;
    }
}

namespace {

/* Asks request r of channel c (ADR-0026 section 3): its header if another
 * channel's is set, then the request with the response-count digit when
 * its reply is taken to fit one line (elm327_response_count), then the
 * reply to its prompt. A 0x22 request whose digit ended the reply before
 * its answer is taken back, so it's asked again with none. */
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
    const bool pack = r == ECUIO_ENGINE_PACK;
    /* A talk: its whole request, no response count, never taken back */
    const bool talk = r == ECUIO_ENGINE_ONCE && e.chans[c].talk.out;
    uint32_t len = 0;
    const uint8_t* pdu =
        ecuio_engine_pdu_of(e.chans[c].talk, talk, q.bytes, q.len, &len);
    const uint8_t count =
        pack   ? elm327_pack_count(q.reply_len, t->tx_ids[c])
        : talk ? uint8_t{0}
               : elm327_response_count(q.bytes, q.len, t->tx_ids[c],
                                       t->one_frame, t->long_dids);
    /* The one-shot settles in the reply */
    const uint8_t pid = q.bytes[1];
    const bool did_ask = !pack && !talk && q.len == 3 && q.bytes[0] == 0x22;
    const auto did = static_cast<uint16_t>((q.bytes[1] << 8) | q.bytes[2]);
    char cmd[ELM327_CMD_MAX];
    if (elm327_format_request(pdu, len, count, cmd, sizeof(cmd)) == 0) {
        return unsent(elm327_io::prompt); /* never: 1 to 7 bytes */
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
        case elm327_line::no_data:
            /* Left to the engine's wait to judge the request, unless an
             * ECU still sending a cut reply ignored it (below) */
            seen.no_data = true;
            break;
        default:
            /* The read errors end this read without a reply; OK, a
             * search, an alert or anything else is ignored */
            break;
        }
    });
    /* A message still under way at the prompt was cut there: the ECU goes
     * on sending it, for as many Consecutive Frames as it had left */
    uint32_t frames_left = 0;
    if (io == elm327_io::prompt) {
        for (const isotp_rx_slot& s : t->isotp.slots) {
            if (s.active && s.expected > s.have) {
                const uint32_t n = (uint32_t{s.expected} - s.have + 6u) / 7u;
                frames_left = (n > frames_left) ? n : frames_left;
            }
        }
    }
    const bool cut = io == elm327_io::prompt && isotp_end_all(t->isotp) > 0;
    auto& held = t->cut[c];
    if (cut) {
        held.hold_us =
            int64_t{frames_left} * ELM327_CUT_FRAME_US + ELM327_CUT_MARGIN_US;
        if (held.hold_us > ELM327_CUT_HOLD_MAX_US) {
            held.hold_us = ELM327_CUT_HOLD_MAX_US;
        }
        held.hold_until_us = now_us(t) + held.hold_us;
    }
    if (did_ask && count > 0 && (cut || seen.pending) &&
        e.chans[c].outstanding == r) {
        /* The digit ended the reply before its answer: a First Frame, or
         * the ECU's 7F 22 78 while it works. Taken back, the request goes
         * again with no digit, so a found DID isn't lost to the wait; after
         * a cut, once the ECU has sent the rest */
        elm327_long_did_set(t->long_dids, did);
        ecuio_engine_sent(&e, c, r, false, now_us(t));
        held.did = did;
        held.open = cut;
        held.tries = 0;
        elm327_log_write(t->log,
                         "22 %04X's reply %s: asked again, and with no "
                         "response count from now on",
                         static_cast<unsigned>(did),
                         cut ? "spans frames" : "is pending");
    } else if (did_ask && held.open && held.did == did &&
               e.chans[c].outstanding == r) {
        if (seen.no_data && !seen.matched && !cut &&
            held.tries < ELM327_CUT_RETRIES) {
            /* Asked again after a cut and nothing came: the ECU was still
             * sending, slower than the hold allowed. Held again, longer */
            ++held.tries;
            held.hold_us *= 2;
            held.hold_until_us = now_us(t) + held.hold_us;
            ecuio_engine_sent(&e, c, r, false, now_us(t));
            elm327_log_write(t->log,
                             "22 %04X: NO DATA after a cut reply, the ECU "
                             "still sending it: asked again in %lld ms",
                             static_cast<unsigned>(did),
                             static_cast<long long>(held.hold_us / 1000));
        } else {
            held.open = false;
        }
    } else if (cut && count > 0 && !pack && !did_ask) {
        /* The digit ended the reply after a First Frame: the PID's reply
         * spans frames, and this one is lost to the engine's wait. From
         * now on it goes without the digit */
        elm327_one_frame_set(t->one_frame, pid, false);
        elm327_log_write(t->log,
                         "01 %02X's reply spans frames: asked with no "
                         "response count from now on",
                         static_cast<unsigned>(pid));
    }
    return io;
}

/* The channels whose ECU is still sending a cut reply, a bit each, passed
 * over until it's done; *left is the soonest hold's rest, INT64_MAX for
 * none */
uint32_t held_channels(const transport* t, int64_t now, int64_t* left)
{
    uint32_t held = 0;
    *left = INT64_MAX;
    for (uint32_t k = 0; k < t->engine.chan_count; ++k) {
        if (t->cut[k].hold_until_us > now) {
            const int64_t rest = t->cut[k].hold_until_us - now;
            *left = (rest < *left) ? rest : *left;
            held |= 1u << k;
        }
    }
    return held;
}

} // namespace

/* The worker (ADR-0026 section 3): the request engine driven as the J2534
 * worker drives it, with one command outstanding for the whole adapter,
 * since an ELM327 takes the next command only after its prompt. Each
 * pass ends the waits that ran out, asks the channel holding the turn
 * (request_turns.h), and reads its reply to the prompt. With nothing due it
 * listens for what the adapter says unasked (a reset): until the next
 * request's floor or wait, or, with nothing left to ask, for the idle
 * read's 100 ms, which counts toward quiet as J2534's passive reads do.
 *
 * Link health (ADR-0026 section 6, link_health.h): a reply with a channel's
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
    link_health h(t->session->last_error, t->session->error_count,
                  t->session->record_link.get(), t->session, t->epoch);
    ecuio_unpicked_check unpicked_check;
    uint32_t published[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_identity[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_diagnostics[ECUIO_MAX_CHANNELS] = {};
    uint32_t diag_seen = 0;
    uint32_t clear_seen[ECUIO_MAX_CHANNELS] = {};
    ecuio_focus_seen focus_seen; /* what ecuio_take_focus took */
    ecuio_engine_turns turns;    /* which channel asks next */
    const bool passive = asks_nothing(t);
    /* A requesting worker's waits to the engine's next event end on
     * time (precise_wait.h) */
    ecuio_precise_wait precise(!passive);
    uint32_t relisten_ms = 0;
    int64_t timing_at = 0;
    if (t->probe) {
        ecuio_engine_probe(&e);
    }
    /* A focus set before start applies from the first request */
    ecuio_take_focus(t->session, &e, focus_seen);

    while (!t->stop_requested.load()) {
        link_judge_waiting(h);
        const int64_t now = now_us(t);
        for (uint32_t c = 0; c < e.chan_count; ++c) {
            if (ecuio_engine_expire(&e, c, now)) {
                ecuio_request_timed_out(e, h);
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
            int64_t held_left = INT64_MAX;
            const uint32_t held = held_channels(t, now, &held_left);
            uint32_t c = 0;
            int64_t floor_us = 0;
            const int32_t r =
                ecuio_engine_take_turn(&e, turns, held, now, &c, &floor_us);
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
                /* the turn's channel waits out its own floor */
                int64_t next_us = (floor_us > 0)
                                      ? floor_us
                                      : ecuio_engine_next_event_us(&e, now);
                if (next_us == 0 && held_left != INT64_MAX) {
                    /* what's due is a held channel's: until its hold ends */
                    next_us = held_left;
                }
                const bool asking = next_us != INT64_MAX;
                uint32_t wait = ELM327_IDLE_READ_MS;
                if (asking) {
                    const int64_t ms = (next_us + 999) / 1000;
                    wait = (ms < 1) ? 1u
                           : (ms > ELM327_WAIT_SLICE_MS)
                               ? ELM327_WAIT_SLICE_MS
                               : static_cast<uint32_t>(ms);
                }
                if (asking) {
                    /* No command is out (ask reads to the prompt), so
                     * the adapter is idle until the engine's next event:
                     * wait for it on time, then take what came unasked */
                    precise.sleep_for(std::chrono::milliseconds(wait));
                    io = t->stop_requested.load() ? elm327_io::stopped
                                                  : drain(t, 0);
                } else {
                    io = drain(t, wait);
                }
                if (io == elm327_io::prompt && !asking) {
                    if (ecuio_engine_unpicked(&e)) {
                        ecuio_unpicked_pass(&e, h, unpicked_check, now_us(t));
                    } else {
                        link_on_read(h, false, false, true);
                    }
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
                ecuio_engine_probe(&e);
            }
            /* the scan's and identification's unanswered requests */
            ecuio_engine_rescan(&e);
        }
        ecuio_take_diag_request(t->session, &e, diag_seen);
        ecuio_take_clear_requests(t->session, &e, clear_seen);
        ecuio_take_test_talk(t->session, &e);
        ecuio_take_focus(t->session, &e, focus_seen);
        for (uint32_t c = 0; c < t->chan_count; ++c) {
            ecuio_publish_support(t->session, e, c, t->rx_ids[c], published[c]);
            ecuio_publish_identity(t->session, e, c, t->rx_ids[c],
                                   published_identity[c]);
            ecuio_publish_diagnostics(t->session, e, c, t->rx_ids[c],
                                      published_diagnostics[c]);
        }
        ecuio_publish_timing(t->session, e, &t->isotp, now_us(t), timing_at,
                             false);

        if (h.backoff_ms > 0) {
            if (backoff_sleep(t, h.backoff_ms)) {
                break;
            }
            h.backoff_ms = link_backoff_next(h.backoff_ms);
        }
    }
    leave_sessions(t);
    ecuio_publish_timing(t->session, e, &t->isotp, now_us(t), timing_at, true);
}

} // namespace ecuio_elm327
