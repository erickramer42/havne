// File/layer: core/src/elm327_worker.cpp | C++17, Qt-free, every
//             platform (ADR-0026)
// Testing: HARNESS: Elm327SessionTest, Elm327TalkTest, Elm327TurnsTest,
//          Elm327ConversationTest (test_elm327); BENCH
//
// The worker and a request's exchange: the I/O under every command,
// the header switch, a reply's lines into frames, a cut reply's hold.
// The whole requesting path is in this file and the header.

#include "elm327_transport.h"

#include "request_setup.h"
#include "request_conversation.h"
#include "request_turns.h"
#include "precise_wait.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
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

/* A hex digit's value, or -1 */
int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Whether sent is a security access's command, and its form for the
 * log: "27NN" and its key's length and SHA-256 */
void cut_command(transport* t, const char* sent)
{
    t->cutting = sent[0] == '2' && sent[1] == '7';
    if (!t->cutting) {
        return;
    }
    uint8_t pdu[ELM327_CMD_MAX / 2] = {};
    size_t n = 0;
    for (; n < sizeof pdu; ++n) {
        const int hi = hex_value(sent[2 * n]);
        const int lo = hi < 0 ? -1 : hex_value(sent[2 * n + 1]);
        if (lo < 0) {
            break;
        }
        pdu[n] = static_cast<uint8_t>(hi << 4 | lo);
    }
    const int k = std::snprintf(t->cut_sent, sizeof t->cut_sent, "%.4s", sent);
    if (ecuio_access_cut(pdu, n)) {
        std::snprintf(t->cut_sent + k, sizeof t->cut_sent - k, " (");
        ecuio_access_cut_text(pdu, n, t->cut_sent + k + 2,
                              sizeof t->cut_sent - k - 3);
        const size_t end = strlen(t->cut_sent);
        std::snprintf(t->cut_sent + end, sizeof t->cut_sent - end, ")");
    }
}

/* A line read, as the log shows it: "<" for a command's, "unasked:"
 * when no command was out (sent nullptr), which always goes in; a
 * security access's line as its length and SHA-256 alone */
void log_line(transport* t, const char* sent, const elm327_reader& r)
{
    if (!elm327_log_is_open(t->log) || (sent != nullptr && !logging(t))) {
        return;
    }
    char text[ELM327_LINE_MAX * 4 + 1];
    if (t->cutting) {
        ecuio_sha256::hasher h;
        h.add(r.line, r.len);
        uint8_t d[32];
        h.finish(d);
        int n = std::snprintf(text, sizeof text,
                              "(a security access's line, cut: %u "
                              "characters, SHA-256 ",
                              static_cast<unsigned>(r.len));
        for (uint8_t b : d) {
            n += std::snprintf(text + n, sizeof text - n, "%02x", b);
        }
        std::snprintf(text + n, sizeof text - n, ")");
    } else {
        elm327_log_text(r.line, r.len, text, sizeof(text));
    }
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
                     (us % 1000) / 100, t->cutting ? t->cut_sent : sent);
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
            ecuio_engine_overflowed(&t->engine);
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
            ecuio_conversations_frame(t->session, &t->engine, c, frame.data,
                                      frame.data_length);
        }
        seen.matched = true;
        return;
    }
}

namespace {

/* A request as typed, and what its reply is judged by */
struct typed_request {
    char cmd[ELM327_CMD_MAX];
    uint8_t count;
    uint8_t pid;
    uint16_t did;
    bool pack;
    bool did_ask;
};

/* Request r of channel c's command, with the response-count digit when
 * its reply is taken to fit one line (elm327_response_count); false when
 * it won't format */
bool type_request(transport* t, uint32_t c, int32_t r, typed_request* out)
{
    ecuio_request_engine& e = t->engine;
    const ecuio_request& q = ecuio_engine_request_of(&e, c, r);
    out->pack = r == ECUIO_ENGINE_PACK;
    /* A talk: its whole request, no response count, never taken back */
    const bool talk = r == ECUIO_ENGINE_ONCE && e.chans[c].talk.out;
    uint32_t len = 0;
    const uint8_t* pdu =
        ecuio_engine_pdu_of(e.chans[c].talk, talk, q.bytes, q.len, &len);
    out->count = out->pack ? elm327_pack_count(q.reply_len, t->tx_ids[c])
                 : talk    ? uint8_t{0}
                           : elm327_response_count(q.bytes, q.len, t->tx_ids[c],
                                                   t->one_frame, t->long_dids);
    /* The one-shot settles in the reply */
    out->pid = q.bytes[1];
    out->did_ask = !out->pack && !talk && q.len == 3 && q.bytes[0] == 0x22;
    out->did = static_cast<uint16_t>((q.bytes[1] << 8) | q.bytes[2]);
    return elm327_format_request(pdu, len, out->count, out->cmd,
                                 sizeof(out->cmd)) != 0;
}

/* A line of a request's reply, by its kind */
void take_reply_line(transport* t, const elm327_reader& line, reply_seen& seen)
{
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
         * ECU still sending a cut reply ignored it (learn_reply_length) */
        seen.no_data = true;
        break;
    default:
        /* The read errors end this read without a reply; OK, a
         * search, an alert or anything else is ignored */
        break;
    }
}

/* Whether a reply was cut at its prompt; its channel c is held while the
 * ECU sends the rest */
bool end_cut_reply(transport* t, uint32_t c, elm327_io io)
{
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
    return cut;
}

/* What a reply says of its request's length: a DID taken back and asked
 * again with no digit, a held DID's NO DATA held again or closed, a PID
 * taken off the digit */
void learn_reply_length(transport* t, uint32_t c, int32_t r,
                        const typed_request& typed, bool cut,
                        const reply_seen& seen)
{
    ecuio_request_engine& e = t->engine;
    auto& held = t->cut[c];
    if (typed.did_ask && typed.count > 0 && (cut || seen.pending) &&
        e.chans[c].outstanding == r) {
        /* The digit ended the reply before its answer: a First Frame, or
         * the ECU's 7F 22 78 while it works. Taken back, the request goes
         * again with no digit, so a found DID isn't lost to the wait; after
         * a cut, once the ECU has sent the rest */
        elm327_long_did_set(t->long_dids, typed.did);
        ecuio_engine_sent(&e, c, r, false, now_us(t));
        held.did = typed.did;
        held.open = cut;
        held.tries = 0;
        elm327_log_write(t->log,
                         "22 %04X's reply %s: asked again, and with no "
                         "response count from now on",
                         static_cast<unsigned>(typed.did),
                         cut ? "spans frames" : "is pending");
    } else if (typed.did_ask && held.open && held.did == typed.did &&
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
                             static_cast<unsigned>(typed.did),
                             static_cast<long long>(held.hold_us / 1000));
        } else {
            held.open = false;
        }
    } else if (cut && typed.count > 0 && !typed.pack && !typed.did_ask) {
        /* The digit ended the reply after a First Frame: the PID's reply
         * spans frames, and this one is lost to the engine's wait. From
         * now on it goes without the digit */
        elm327_one_frame_set(t->one_frame, typed.pid, false);
        elm327_log_write(t->log,
                         "01 %02X's reply spans frames: asked with no "
                         "response count from now on",
                         static_cast<unsigned>(typed.pid));
    }
}

/* Asks request r of channel c (ADR-0026 section 3): its header if another
 * channel's is set, then the request (type_request), then the reply to
 * its prompt. A 0x22 request whose digit ended the reply before its
 * answer is taken back, so it's asked again with none. */
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
    typed_request typed;
    if (!type_request(t, c, r, &typed)) {
        return unsent(elm327_io::prompt); /* never: 1 to 7 bytes */
    }
    ecuio_engine_sent(&e, c, r, true, now_us(t));
    const elm327_io io =
        command(t, typed.cmd, true, [&](const elm327_reader& line) {
            take_reply_line(t, line, seen);
        });
    const bool cut = end_cut_reply(t, c, io);
    learn_reply_length(t, c, r, typed, cut, seen);
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

/* What the worker keeps across its passes, made in this order */
struct worker_state {
    link_health h;
    ecuio_unpicked_check unpicked_check;
    uint32_t published[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_identity[ECUIO_MAX_CHANNELS] = {};
    uint32_t published_diagnostics[ECUIO_MAX_CHANNELS] = {};
    uint32_t diag_seen = 0;
    uint32_t clear_seen = 0;     /* ecuio_take_clear_requests' count */
    ecuio_focus_seen focus_seen; /* what ecuio_take_focus took */
    ecuio_modules_seen modules_seen; /* what ecuio_take_modules took */
    ecuio_engine_turns turns;    /* which channel asks next */
    const bool passive;
    /* A requesting worker's waits to the engine's next event end on
     * time (precise_wait.h) */
    ecuio_precise_wait precise;
    uint32_t relisten_ms = 0;
    int64_t timing_at = 0;

    explicit worker_state(transport* t)
        : h(t->session->last_error, t->session->error_count,
            t->session->record_link.get(), t->session, t->epoch),
          passive(asks_nothing(t)), precise(!passive)
    {}
};

/* The pass's start: the waits that ran out end; the time, read once */
int64_t expire_waits(transport* t, worker_state& w)
{
    ecuio_request_engine& e = t->engine;
    link_judge_waiting(w.h);
    const int64_t now = now_us(t);
    ecuio_key_windows_judge(&e, w.h);
    for (uint32_t c = 0; c < e.chan_count; ++c) {
        if (ecuio_engine_expire(&e, c, now)) {
            ecuio_request_timed_out(e, w.h);
        }
    }
    return now;
}

/* The port and the setup again; false when the session closed meanwhile */
bool setup_pass(transport* t, worker_state& w)
{
    const bool ready = reconnect(t);
    if (t->stop_requested.load()) {
        return false;
    }
    if (!ready) {
        /* The adapter answered wrongly or not at all: start over
         * from the port */
        close_port(t);
    }
    /* The adapter answering its setup ends the backoff */
    link_on_read(w.h, !ready, false, false);
    return true;
}

/* One monitor, and AT MA again at once or on the relisten backoff;
 * stopped when the session closed in that wait */
elm327_io listen_pass(transport* t, worker_state& w)
{
    const monitor_end m = listen(t, w.h);
    const elm327_io io = m.io;
    if (m.heard) {
        w.relisten_ms = 0;
    }
    if (io == elm327_io::prompt && !m.at_once) {
        link_on_read(w.h, false, false, true);
        w.relisten_ms = (w.relisten_ms == 0) ? LINK_BACKOFF_BASE_MS
                                             : link_backoff_next(w.relisten_ms);
        if (backoff_sleep(t, w.relisten_ms)) {
            return elm327_io::stopped;
        }
    }
    return io;
}

/* Request r of channel c asked, and its reply judged for the link */
elm327_io ask_pass(transport* t, worker_state& w, uint32_t c, int32_t r)
{
    reply_seen seen;
    elm327_io io = ask(t, c, r, seen);
    if (io == elm327_io::prompt) {
        if (seen.reset) {
            io = elm327_io::reset;
        } else if (seen.no_bus || seen.refused) {
            /* never reached the bus: not sent */
            ecuio_engine_sent(&t->engine, c, r, false, now_us(t));
            link_on_write(w.h, false);
        } else {
            link_on_write(w.h, true);
            link_on_read(w.h, false, seen.matched, false);
        }
    }
    return io;
}

/* Nothing due: the wait to the next event, or with nothing left to ask
 * the idle read, then what came unasked */
elm327_io idle_pass(transport* t, worker_state& w, int64_t now,
                    int64_t floor_us, int64_t held_left)
{
    ecuio_request_engine& e = t->engine;
    /* the turn's channel waits out its own floor */
    int64_t next_us =
        (floor_us > 0) ? floor_us : ecuio_engine_next_event_us(&e, now);
    if (next_us == 0 && held_left != INT64_MAX) {
        /* what's due is a held channel's: until its hold ends */
        next_us = held_left;
    }
    next_us =
        (std::min)(next_us, ecuio_conversations_next_us(t->session, &e, now));
    const bool asking = next_us != INT64_MAX;
    uint32_t wait = ELM327_IDLE_READ_MS;
    if (asking) {
        const int64_t ms = (next_us + 999) / 1000;
        wait = (ms < 1)                      ? 1u
               : (ms > ELM327_WAIT_SLICE_MS) ? ELM327_WAIT_SLICE_MS
                                             : static_cast<uint32_t>(ms);
    }
    elm327_io io = elm327_io::prompt;
    if (asking) {
        /* No command is out (ask reads to the prompt), so the adapter is
         * idle until the engine's next event: wait for it on time, then
         * take what came unasked */
        w.precise.sleep_for(std::chrono::milliseconds(wait));
        io = t->stop_requested.load() ? elm327_io::stopped : drain(t, 0);
    } else {
        io = drain(t, wait);
    }
    if (io == elm327_io::prompt && !asking) {
        if (ecuio_engine_unpicked(&e)) {
            ecuio_unpicked_pass(&e, w.h, w.unpicked_check, now_us(t));
        } else {
            link_on_read(w.h, false, false, true);
        }
    }
    return io;
}

/* A pass on a set-up adapter: the turn taken, then the monitor, the
 * request or the wait */
elm327_io adapter_pass(transport* t, worker_state& w, int64_t now)
{
    int64_t held_left = INT64_MAX;
    const uint32_t held = held_channels(t, now, &held_left);
    uint32_t c = 0;
    int64_t floor_us = 0;
    const int32_t r =
        ecuio_engine_take_turn(&t->engine, w.turns, held, now, &c, &floor_us);
    if (w.passive) {
        return listen_pass(t, w);
    }
    if (r != ECUIO_ENGINE_NONE) {
        return ask_pass(t, w, c, r);
    }
    return idle_pass(t, w, now, floor_us, held_left);
}

/* The pass's end: a failed port or silence closes it, a reset runs the
 * setup again */
void judge_io(transport* t, worker_state& w, elm327_io io)
{
    if (io == elm327_io::failed || io == elm327_io::silent) {
        close_port(t);
        link_on_read(w.h, true, false, false);
    } else if (io == elm327_io::reset) {
        /* setup again next pass; hard only if that fails */
        elm327_log_write(t->log, "the adapter reset: the setup runs again");
        t->configured = false;
    }
}

/* What the session asked for since the last pass, and what it's told */
void serve_session(transport* t, worker_state& w)
{
    ecuio_request_engine& e = t->engine;
    if (w.h.recovered) {
        w.h.recovered = false;
        if (t->probe) {
            ecuio_engine_probe(&e);
        }
        /* the scan's and identification's unanswered requests */
        ecuio_engine_rescan(&e);
    }
    ecuio_take_diag_request(t->session, &e, w.diag_seen);
    ecuio_take_clear_requests(t->session, &e, &w.clear_seen);
    ecuio_take_test_talk(t->session, &e, now_us(t));
    ecuio_take_focus(t->session, &e, w.focus_seen);
    (void)ecuio_take_modules(t->session, &e, w.modules_seen, now_us(t));
    ecuio_step_conversations(t->session, &e, now_us(t));
    for (uint32_t c = 0; c < t->chan_count; ++c) {
        ecuio_publish_support(t->session, e, c, t->rx_ids[c], w.published[c]);
        ecuio_publish_identity(t->session, e, c, t->rx_ids[c],
                               w.published_identity[c]);
        ecuio_publish_diagnostics(t->session, e, c, t->rx_ids[c],
                                  w.published_diagnostics[c]);
    }
    ecuio_publish_timing(t->session, e, &t->isotp, now_us(t), w.timing_at,
                         false);
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
    worker_state w(t);
    if (t->probe) {
        ecuio_engine_probe(&e);
    }
    /* A focus set before start applies from the first request */
    ecuio_take_focus(t->session, &e, w.focus_seen);
    (void)ecuio_take_modules(t->session, &e, w.modules_seen, now_us(t));
    ecuio_conversations_start(t->session, &e);

    while (!t->stop_requested.load()) {
        const int64_t now = expire_waits(t, w);
        if (!t->configured) {
            if (!setup_pass(t, w)) {
                break;
            }
        } else {
            const elm327_io io = adapter_pass(t, w, now);
            if (io == elm327_io::stopped) {
                break;
            }
            judge_io(t, w, io);
        }
        serve_session(t, w);
        if (w.h.backoff_ms > 0) {
            if (backoff_sleep(t, w.h.backoff_ms)) {
                break;
            }
            w.h.backoff_ms = link_backoff_next(w.h.backoff_ms);
        }
    }
    ecuio_conversations_end(t->session, &e);
    leave_sessions(t);
    ecuio_publish_timing(t->session, e, &t->isotp, now_us(t), w.timing_at,
                         true);
}

} // namespace ecuio_elm327
