// File/layer: tests/pacing_bench.h | test-only, C++17, Qt-free; includes
//             core/src (the request engine, its setup, the session's
//             decoder) and cli/summary_lines.h
// Testing: UNIT: PacingTest (test_pacing); BENCH: bench_pacing
//
// The request engine driven on a simulated clock: a model of a
// transport's worker asks the engine as that worker does, each request
// crosses one simulated bus, the simulated ECU answers it
// (ecuio_sim_ask) with each module's reply timing (sim_timing.h's draw), and
// each reply's frames cross the bus in their airtime, first come first
// served. The replies go through the session's own decoder into
// havne-cli log's tally (cli/summary_lines.h), so a run gives the log's
// summary figures, the same on every machine and in milliseconds.
//
// The engine reads no clock: the loop hands it the simulated time, so
// nothing in core changes. The models mirror their workers' calls (the
// J2534 worker's pass, SocketCAN's one request awaiting its echo, the
// ELM327 worker's turn and its one command to the prompt); each constant
// is the adapter's, calibrated against the car.

#ifndef ECUIO_TESTS_PACING_BENCH_H
#define ECUIO_TESTS_PACING_BENCH_H

#include "summary_lines.h"

#include "elm327_protocol.h"
#include "isotp.h"
#include "j2534_hold.h"
#include "request_engine.h"
#include "request_setup.h"
#include "request_turns.h"
#include "session.h"
#include "sim_timing.h"

#include "ecuio/core.h"
#include "ecuio/sim.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace pacing {

using ecuio_sim_timing::inverse_normal;
using ecuio_sim_timing::sim_timing;
using ecuio_sim_timing::sim_timing_draw;
using ecuio_sim_timing::sim_timing_rng;

enum class adapter { j2534, socketcan, elm327 };

// An adapter model's constants. The J2534 ones are the OBDX Pro VX's,
// fitted to the car (docs/bench-sessions/2026-11-18-pacing-car.md);
// the rest are the 2026-11-09 baseline's and the 2026-11-03 tap's
struct adapter_model {
    adapter kind = adapter::j2534;
    int64_t write_us = 1500;   /* J2534: a write through the surrogate */
    int64_t deliver_us = 1000; /* J2534: the last frame to the engine */
    bool hold = true;         /* J2534: ?hold= (j2534_hold.h) */
    /* J2534: Known Issue #42 through the VX, a Single Frame reply ending
     * while another module's segmented reply is on the bus (its First
     * Frame's end to its last Consecutive Frame's end) is lost */
    bool lost_under_segments = false;
    int64_t fc_turn_us = 170; /* the adapter's Flow Control after a FF */
    int64_t echo_us = 100;    /* SocketCAN: the request's echo */
    /* ELM327: the link's one way (half a Bluetooth or USB round trip), a
     * character on the line, a command's CR to its frame on the bus, the
     * adaptive wait after a reply's last frame with no count digit, and
     * AT ST19's wait for a first frame */
    int64_t link_us = 7000;
    int64_t char_us = 50;
    int64_t command_us = 500;
    int64_t wait_us = 20000;
    int64_t st_us = 100000;
    /* The VX in ELM mode: a multi-frame reply's prompt this long after its
     * last frame, the count digit or not (ADR-0056's appendix of
     * 2026-11-08); 0 for an adapter that keeps the digit */
    int64_t late_prompt_us = 0;
};

// The MX+ over Bluetooth, fitted to the car (docs/bench-sessions/
// 2026-11-18-pacing-car.md), and the VX in ELM mode over USB at 115,200
// baud (10 bits a character), uncalibrated
inline adapter_model elm327_bluetooth()
{
    adapter_model m;
    m.kind = adapter::elm327;
    m.link_us = 2000;
    m.char_us = 87;
    m.command_us = 2000;
    return m;
}
inline adapter_model vx_elm_mode()
{
    adapter_model m;
    m.kind = adapter::elm327;
    m.link_us = 500;
    m.char_us = 87;
    m.late_prompt_us = 100000;
    return m;
}

// One run: the profile the session and the simulated car share (with any
// keys of the session's own: "uds-generic&did=..."), the simulated car's
// keys in its place when set ("modules=2"), the session's request
// parameters (as the URI's keys set them), the adapter, each module's
// timing, the picks (none: the whole set), the Service 01 PIDs the
// bitmaps list (none: the simulated car's own)
struct bench_config {
    std::string profile = "obd2";
    std::string sim_keys;
    request_params params;
    adapter_model model;
    sim_timing timing;
    std::vector<uint16_t> picks;
    std::vector<uint8_t> supported;
    int64_t seconds = 70;
};

// What a run reached: the log's tally, each request's time, and the waits
// that ran out
struct bench_result {
    cli::SummaryTally tally{nullptr};
    std::vector<int64_t> sent_us;
    uint64_t timeouts = 0;
    uint64_t cuts = 0;    /* ELM327: replies the prompt ended early */
    uint64_t headers = 0; /* ELM327: AT SH switches */
    uint64_t lost = 0;    /* J2534: replies lost_under_segments dropped */
    std::vector<uint64_t> asked;    /* each channel's requests */
    std::vector<uint64_t> answered; /* each channel's replies delivered */

    // Requests a second in the tally's window
    double requests_per_s() const
    {
        const int64_t from = static_cast<int64_t>(tally.from_us());
        const int64_t to = static_cast<int64_t>(tally.to_us());
        const auto n =
            std::count_if(sent_us.begin(), sent_us.end(),
                          [&](int64_t t) { return t >= from && t <= to; });
        return to > from ? static_cast<double>(n) * 1e6 / (to - from) : 0.0;
    }
    double values_per_s() const
    {
        const uint64_t span = tally.to_us() - tally.from_us();
        return span > 0 ? static_cast<double>(tally.window_values()) * 1e6 /
                              static_cast<double>(span)
                        : 0.0;
    }
};

// A classic CAN frame of 8 bytes at 500 kbit/s, 11-bit, with typical
// stuffing: about 120 bits
inline constexpr int64_t kFrameUs = 240;

// A Service 01 bitmap reply (41 <base> b1..b4) rewritten to list only the
// supported PIDs, its last bit set while one past its range is listed
inline void filter_bitmap(uint8_t* data, uint32_t len,
                          const std::vector<uint8_t>& supported)
{
    if (len < 6 || data[0] != 0x41 || data[1] % 0x20 != 0) {
        return;
    }
    const uint32_t base = data[1];
    uint8_t bits[4] = {};
    bool beyond = false;
    for (uint8_t pid : supported) {
        if (pid > base && pid <= base + 0x20) {
            const uint32_t k = pid - base - 1;
            bits[k / 8] |= static_cast<uint8_t>(0x80u >> (k % 8));
        }
        beyond = beyond || pid > base + 0x20;
    }
    if (beyond) {
        bits[3] |= 0x01;
    }
    std::memcpy(data + 2, bits, 4);
}

// The test car's engine ECU: the Service 01 PIDs its bitmaps list to 0x60
// (docs/bench-sessions/2026-10-22-vehicle.md), 0x60 the last read
inline std::vector<uint8_t> test_car_pids()
{
    std::vector<uint8_t> pids;
    const uint8_t bitmaps[3][4] = {{0xFE, 0x7F, 0xA8, 0x13},
                                   {0xA0, 0x17, 0xF0, 0x11},
                                   {0xFE, 0xD0, 0x8C, 0x81}};
    for (uint32_t b = 0; b < 3; ++b) {
        for (uint32_t k = 0; k < 32; ++k) {
            if (bitmaps[b][k / 8] & (0x80u >> (k % 8))) {
                pids.push_back(static_cast<uint8_t>(b * 0x20 + k + 1));
            }
        }
    }
    return pids;
}

class bench
{
public:
    explicit bench(const bench_config& cfg) : m_cfg(cfg) {}
    ~bench()
    {
        ecuio_sim_destroy(m_sim);
        ecuio_session_destroy(m_session);
    }
    bench(const bench&) = delete;
    bench& operator=(const bench&) = delete;

    // Sets up the session, the engine and the simulated car; false, with
    // why, when any refuses
    bool open(std::string* why);
    // Runs the configured seconds and gives what they reached
    bench_result run();

private:
    enum kind { look, request, reply_frame, tester_fc, echo, deliver, prompt };
    struct event {
        int64_t at;
        uint64_t seq;
        kind what;
        uint32_t slot;
        bool operator>(const event& o) const
        {
            return at != o.at ? at > o.at : seq > o.seq;
        }
    };
    struct flight {
        uint32_t chan = 0;
        uint32_t can_id = 0; /* a request's request ID, a reply's reply ID */
        std::vector<uint8_t> data;
        uint32_t frames = 0;
        uint32_t next = 0;      /* the next frame to send */
        int64_t last_start = 0; /* the last Consecutive Frame's start */
        int32_t row = 0;        /* the request it answers (SocketCAN) */
        int64_t line_at = 0;    /* ELM327: its last line at the host */
        bool cut = false;       /* ELM327: the prompt came before its end */
    };
    // ELM327: the one command out, from its write to its prompt
    struct command {
        bool out = false;
        uint32_t chan = 0;
        int32_t row = 0;
        uint8_t count = 0;  /* the response-count digit, 0 for none */
        uint32_t lines = 0; /* the frame lines it has read */
        bool pack = false;
        bool did_ask = false; /* a one-DID 0x22 request */
        uint16_t did = 0;
        uint8_t pid = 0;
        bool prompted = false;
        bool cut = false;         /* a reply still under way at the prompt */
        uint32_t frames_left = 0; /* its Consecutive Frames still to come */
    };

    void at(int64_t t, kind what, uint32_t slot = 0)
    {
        m_events.push(event{t, m_seq++, what, slot});
    }
    void look_at(int64_t t)
    {
        if (t < m_look) {
            m_look = t;
            at(t, look);
        }
    }
    // The bus's end for a frame ready at t, or -1 when it must wait
    int64_t on_bus(const event& ev)
    {
        if (ev.at < m_bus_free) {
            at(m_bus_free, ev.what, ev.slot);
            return -1;
        }
        m_bus_free = ev.at + kFrameUs;
        return m_bus_free;
    }

    void look_j2534(int64_t now);
    void look_socketcan(int64_t now);
    void look_elm327(int64_t now);
    void ask_elm327(uint32_t c, int32_t r, int64_t now);
    void line_elm327(uint32_t slot, int64_t end);
    void prompt_at(int64_t from_adapter, uint32_t chars);
    void on_prompt(const event& ev);
    void send(uint32_t c, int32_t r, int64_t now);
    void on_request(const event& ev);
    void on_reply_frame(const event& ev);
    void on_deliver(const event& ev);
    void schedule_look(int64_t now);
    // Whether a Single Frame from can_id ends under another module's
    // segmented reply with adapter_model::lost_under_segments on
    bool lost_under_segments(uint32_t can_id) const
    {
        return m_cfg.model.kind == adapter::j2534 &&
               m_cfg.model.lost_under_segments &&
               std::any_of(m_segments.begin(), m_segments.end(),
                           [can_id](uint32_t id) { return id != can_id; });
    }

    bench_config m_cfg;
    ecuio_session_t* m_session = nullptr;
    ecuio_sim_t* m_sim = nullptr;
    std::unique_ptr<ecuio_request_engine> m_engine;
    j2534_hold m_hold; /* J2534: the worker's hold, holding no row before
                        * the first look */
    std::vector<std::mt19937_64> m_rng; /* one for each channel's module */
    std::priority_queue<event, std::vector<event>, std::greater<event>>
        m_events;
    std::vector<flight> m_flights;
    bench_result m_result;
    uint64_t m_seq = 0;
    int64_t m_look = INT64_MAX;
    int64_t m_bus_free = 0;
    std::vector<uint32_t> m_segments; /* J2534: reply IDs with a segmented
                                       * reply on the bus */
    bool m_echo_pending = false;
    uint32_t m_next_chan = 0;
    /* ELM327: the worker's turn, its one-frame PIDs and long DIDs, the
     * header set, the line toward the host, each channel's cut hold */
    ecuio_engine_turns m_turns;
    elm327_one_frame_pids m_one_frame;
    std::unique_ptr<elm327_long_dids> m_long_dids =
        std::make_unique<elm327_long_dids>();
    uint32_t m_header_tx = 0;
    bool m_header_set = false;
    int64_t m_line_free = 0;
    int64_t m_hold_until[ECUIO_MAX_CHANNELS] = {};
    command m_cmd;
};

inline bool bench::open(std::string* why)
{
    const std::string uri = "mock://pacing?profile=" + m_cfg.profile;
    if (ecuio_session_create(uri.c_str(), &m_session) != ECUIO_OK) {
        *why = std::string("session: ") + ecuio_last_error_detail();
        return false;
    }
    const ecuio_signal_set_t* set = ecuio_session_signal_set(m_session);
    m_engine = std::make_unique<ecuio_request_engine>();
    bool probe = false;
    bool identify = false;
    if (ecuio_build_requests(m_session, set, m_cfg.params,
                             m_session->info.rx_ids,
                             m_session->info.channel_count, m_engine.get(),
                             &probe, &identify) != ECUIO_OK) {
        *why = std::string("requests: ") + ecuio_last_error_detail();
        return false;
    }
    if (m_engine->gap_us == ECUIO_ENGINE_GAP_US && m_cfg.params.gap_us < 0 &&
        m_cfg.model.kind == adapter::j2534) {
        m_engine->gap_us = 5000; /* J2534_GAP_US, the transport's floor */
    }
    if (probe) {
        ecuio_engine_probe(m_engine.get());
    }
    if (!m_cfg.picks.empty() &&
        ecuio_session_set_focus(m_session, m_cfg.picks.data(),
                                static_cast<uint32_t>(m_cfg.picks.size())) !=
            ECUIO_OK) {
        *why = std::string("focus: ") + ecuio_last_error_detail();
        return false;
    }
    ecuio_focus_seen seen;
    ecuio_take_focus(m_session, m_engine.get(), seen);
    std::string sim_uri = "mock://pacing?values=canned";
    if (!m_cfg.sim_keys.empty()) {
        sim_uri += "&" + m_cfg.sim_keys;
    } else if (m_cfg.profile != "obd2") {
        sim_uri += "&profile=" + m_cfg.profile;
    }
    if (ecuio_sim_create_unbound(sim_uri.c_str(), 0, &m_sim) != ECUIO_OK) {
        *why = std::string("sim: ") + ecuio_last_error_detail();
        return false;
    }
    for (uint32_t c = 0; c < m_engine->chan_count; ++c) {
        m_rng.push_back(
            sim_timing_rng(m_cfg.timing.seed, m_session->info.rx_ids[c]));
    }
    m_result.tally = cli::SummaryTally(set, m_cfg.picks);
    m_result.asked.assign(m_engine->chan_count, 0);
    m_result.answered.assign(m_engine->chan_count, 0);
    elm327_one_frame_pids_for(set, &m_one_frame);
    return true;
}

inline bench_result bench::run()
{
    const int64_t start = 1000000;
    const int64_t end = start + m_cfg.seconds * 1000000;
    look_at(start);
    while (!m_events.empty() && m_events.top().at <= end) {
        const event ev = m_events.top();
        m_events.pop();
        switch (ev.what) {
        case look:
            if (ev.at == m_look) {
                m_look = INT64_MAX;
                if (m_cfg.model.kind == adapter::j2534) {
                    look_j2534(ev.at);
                } else if (m_cfg.model.kind == adapter::socketcan) {
                    look_socketcan(ev.at);
                } else {
                    look_elm327(ev.at);
                }
            }
            break;
        case prompt:
            on_prompt(ev);
            break;
        case request:
        case tester_fc:
            on_request(ev);
            break;
        case reply_frame:
            on_reply_frame(ev);
            break;
        case echo: {
            const flight& f = m_flights[ev.slot];
            ecuio_engine_sent(m_engine.get(), f.chan, f.row, true, ev.at);
            m_echo_pending = false;
            look_at(ev.at);
            break;
        }
        case deliver:
            on_deliver(ev);
            break;
        }
    }
    return std::move(m_result);
}

// The J2534 worker's pass (j2534_hold_pass): the waits that ran out, then
// what the hold gives each channel to write, each write taking the
// surrogate's time
inline void bench::look_j2534(int64_t now)
{
    ecuio_request_engine* e = m_engine.get();
    m_hold.on = m_cfg.model.hold;
    j2534_hold_pass(
        m_hold, e, now, [this](uint32_t) { ++m_result.timeouts; },
        [this, e, &now](uint32_t c, int32_t r) {
            const int64_t started = now;
            now += m_cfg.model.write_us;
            send(c, r, now);
            ecuio_engine_sent(e, c, r, true, now, started);
        });
    schedule_look(now);
}

// SocketCAN's: one request awaiting its echo at a time for the socket,
// the channels taken in turn
inline void bench::look_socketcan(int64_t now)
{
    ecuio_request_engine* e = m_engine.get();
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (ecuio_engine_expire(e, c, now)) {
            ++m_result.timeouts;
        }
    }
    for (uint32_t k = 0; k < e->chan_count && !m_echo_pending; ++k) {
        const uint32_t c = (m_next_chan + k) % e->chan_count;
        const int32_t r = ecuio_engine_due(e, c, now);
        if (r != ECUIO_ENGINE_NONE) {
            m_next_chan = (c + 1) % e->chan_count;
            m_echo_pending = true;
            send(c, r, now);
        }
    }
    schedule_look(now);
}

inline void bench::schedule_look(int64_t now)
{
    const int64_t next =
        m_cfg.model.kind == adapter::j2534
            ? j2534_hold_next_event_us(m_hold, m_engine.get(), now)
            : ecuio_engine_next_event_us(m_engine.get(), now);
    if (next != INT64_MAX) {
        look_at(now + std::max<int64_t>(next, 1));
    }
}

// Request r of channel c onto the bus at now, as a flight of its own
inline void bench::send(uint32_t c, int32_t r, int64_t now)
{
    ecuio_request_engine* e = m_engine.get();
    const ecuio_request& q = ecuio_engine_request_of(e, c, r);
    uint32_t len = 0;
    const uint8_t* pdu = ecuio_engine_pdu_of(
        e->chans[c].talk, r == ECUIO_ENGINE_ONCE, q.bytes, q.len, &len);
    flight f;
    f.chan = c;
    f.can_id = m_session->info.tx_ids[c];
    f.data.assign(pdu, pdu + len);
    f.row = r;
    m_flights.push_back(std::move(f));
    m_result.sent_us.push_back(now);
    ++m_result.asked[c];
    at(now, request, static_cast<uint32_t>(m_flights.size() - 1));
}

// A request's frame (or the tester's Flow Control) on the bus: at its
// end the simulated car answers it, each reply's first frame after its
// module's drawn delay
inline void bench::on_request(const event& ev)
{
    const int64_t end = on_bus(ev);
    if (end < 0) {
        return;
    }
    if (ev.what == tester_fc) {
        flight& f = m_flights[ev.slot];
        at(end + m_cfg.timing.fc_us, reply_frame, ev.slot);
        f.last_start = end + m_cfg.timing.fc_us;
        return;
    }
    const flight req = m_flights[ev.slot];
    if (m_cfg.model.kind == adapter::socketcan) {
        at(end + m_cfg.model.echo_us, echo, ev.slot);
    }
    ecuio_sim_reply_t replies[ECUIO_SIM_MAX_REPLIES];
    uint32_t n = 0;
    if (ecuio_sim_ask(m_sim, req.can_id, req.data.data(),
                      static_cast<uint32_t>(req.data.size()), end, replies,
                      ECUIO_SIM_MAX_REPLIES, &n) != ECUIO_OK) {
        n = 0;
    }
    if (n == 0 && m_cfg.model.kind == adapter::elm327) {
        prompt_at(end + m_cfg.model.st_us, 10); /* "NO DATA", CR CR, '>' */
    }
    for (uint32_t i = 0; i < n && i < ECUIO_SIM_MAX_REPLIES; ++i) {
        flight f;
        f.chan = req.chan;
        f.can_id = replies[i].reply_id;
        f.data.assign(replies[i].data, replies[i].data + replies[i].length);
        if (req.data.size() == 2 && req.data[0] == 0x01 &&
            !m_cfg.supported.empty()) {
            filter_bitmap(f.data.data(), static_cast<uint32_t>(f.data.size()),
                          m_cfg.supported);
        }
        f.frames = isotp_frames(replies[i].length);
        const int64_t delay = sim_timing_draw(m_cfg.timing, m_rng[req.chan]);
        m_flights.push_back(std::move(f));
        at(end + delay + replies[i].delay_us, reply_frame,
           static_cast<uint32_t>(m_flights.size() - 1));
    }
}

// A reply's next frame on the bus: a Single Frame or the last
// Consecutive Frame delivers it; a First Frame draws the adapter's Flow
// Control; a Consecutive Frame the next at the module's pace
inline void bench::on_reply_frame(const event& ev)
{
    const int64_t end = on_bus(ev);
    if (end < 0) {
        return;
    }
    flight& f = m_flights[ev.slot];
    const uint32_t k = f.next++;
    const bool elm = m_cfg.model.kind == adapter::elm327;
    if (elm) {
        line_elm327(ev.slot, end);
    }
    if (k > 0 && k + 1 < f.frames) {
        const int64_t start = end - kFrameUs;
        at(std::max(start + m_cfg.timing.cf_us, end), reply_frame, ev.slot);
    } else if (k == 0 && f.frames > 1) {
        if (m_cfg.model.kind == adapter::j2534) {
            m_segments.push_back(f.can_id);
        }
        at(end + m_cfg.model.fc_turn_us, tester_fc, ev.slot);
    } else if (!elm) {
        const auto open =
            std::find(m_segments.begin(), m_segments.end(), f.can_id);
        if (f.frames > 1 && open != m_segments.end()) {
            m_segments.erase(open);
        } else if (f.frames == 1 && lost_under_segments(f.can_id)) {
            ++m_result.lost; /* never delivered: the engine's wait runs out */
            return;
        }
        at(end + m_cfg.model.deliver_us, deliver, ev.slot);
    } else if (!f.cut) {
        at(f.line_at, deliver, ev.slot); /* read as its last line arrives */
    }
}

// A whole reply to the engine and through the session's decoder into
// the log's tally; the worker then looks again
inline void bench::on_deliver(const event& ev)
{
    const flight& f = m_flights[ev.slot];
    uint32_t c = 0;
    while (c < m_engine->chan_count && m_session->info.rx_ids[c] != f.can_id) {
        ++c;
    }
    if (c < m_engine->chan_count) {
        (void)ecuio_engine_on_frame(m_engine.get(), c, f.data.data(),
                                    static_cast<uint16_t>(f.data.size()), ev.at,
                                    nullptr);
        j2534_hold_answered(m_hold, c);
        ++m_result.answered[c];
    }
    ecuio_frame_t frame;
    std::memset(&frame, 0, sizeof frame);
    frame.timestamp_us = static_cast<uint64_t>(ev.at);
    frame.arbitration_id = f.can_id;
    frame.data_length = static_cast<uint16_t>(
        std::min<size_t>(f.data.size(), sizeof frame.data));
    std::memcpy(frame.data, f.data.data(), frame.data_length);
    const uint32_t count = m_session->decoder.decode(&frame);
    m_result.tally.on_frame();
    if (count > 0) {
        ecuio_value_batch_t batch{};
        batch.timestamp_us = frame.timestamp_us;
        batch.arbitration_id = frame.arbitration_id;
        batch.count = count;
        batch.values = m_session->decoder.values.data();
        m_result.tally.on_batch(batch);
    }
    if (m_cfg.model.kind != adapter::elm327) {
        look_at(ev.at); /* an ELM327 asks again at its prompt */
    }
}

// The ELM327 worker's pass: the waits that ran out, then the turn's
// request (request_turns.h) with the cut channels held; with nothing due,
// a wait to the floor or the engine's next event in whole milliseconds,
// at most ELM327_WAIT_SLICE_MS, or the idle read's 100 ms
inline void bench::look_elm327(int64_t now)
{
    ecuio_request_engine* e = m_engine.get();
    if (m_cmd.out) {
        return;
    }
    uint32_t held = 0;
    int64_t held_left = INT64_MAX;
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (ecuio_engine_expire(e, c, now)) {
            ++m_result.timeouts;
        }
        if (m_hold_until[c] > now) {
            held |= 1u << c;
            held_left = std::min(held_left, m_hold_until[c] - now);
        }
    }
    uint32_t c = 0;
    int64_t floor_us = 0;
    const int32_t r =
        ecuio_engine_take_turn(e, m_turns, held, now, &c, &floor_us);
    if (r != ECUIO_ENGINE_NONE) {
        ask_elm327(c, r, now);
        return;
    }
    int64_t next_us =
        floor_us > 0 ? floor_us : ecuio_engine_next_event_us(e, now);
    if (next_us == 0 && held_left != INT64_MAX) {
        next_us = held_left;
    }
    if (next_us == INT64_MAX) {
        look_at(now + 100000);
        return;
    }
    const int64_t ms = (next_us + 999) / 1000;
    look_at(now + std::min<int64_t>(std::max<int64_t>(ms, 1), 10) * 1000);
}

// ask() in elm327_worker.cpp: AT SH when another channel's header is set,
// then the request with its response-count digit, sent as it's written
inline void bench::ask_elm327(uint32_t c, int32_t r, int64_t now)
{
    ecuio_request_engine* e = m_engine.get();
    const adapter_model& m = m_cfg.model;
    const uint32_t tx = m_session->info.tx_ids[c];
    if (!m_header_set || m_header_tx != tx) {
        char cmds[2][24];
        const size_t n = elm327_format_header(tx, tx > 0x7FFu, cmds);
        for (size_t i = 0; i < n; ++i) {
            /* the command, then "OK", CR CR and the prompt */
            const auto chars = static_cast<int64_t>(std::strlen(cmds[i]) + 5);
            now += 2 * m.link_us + chars * m.char_us + m.command_us;
        }
        m_header_set = true;
        m_header_tx = tx;
        ++m_result.headers;
    }
    const ecuio_request& q = ecuio_engine_request_of(e, c, r);
    const bool talk = r == ECUIO_ENGINE_ONCE && e->chans[c].talk.out;
    uint32_t len = 0;
    const uint8_t* pdu =
        ecuio_engine_pdu_of(e->chans[c].talk, talk, q.bytes, q.len, &len);
    command k;
    k.out = true;
    k.chan = c;
    k.row = r;
    k.pack = r == ECUIO_ENGINE_PACK;
    k.count = k.pack ? elm327_pack_count(q.reply_len, tx)
              : talk ? uint8_t{0}
                     : elm327_response_count(q.bytes, q.len, tx, m_one_frame,
                                             *m_long_dids);
    k.did_ask = !k.pack && !talk && q.len == 3 && q.bytes[0] == 0x22;
    k.did = static_cast<uint16_t>((q.bytes[1] << 8) | q.bytes[2]);
    k.pid = q.bytes[1];
    char cmd[40];
    const size_t chars =
        elm327_format_request(pdu, len, k.count, cmd, sizeof cmd);
    ecuio_engine_sent(e, c, r, true, now);
    m_cmd = k;
    send(c, r,
         now + static_cast<int64_t>(chars) * m.char_us + m.link_us +
             m.command_us);
}

// A reply frame's line toward the host after the frame's end (the ID and
// 8 bytes in hex with ATH1 ATS0 ATD0, then CR), queued behind the line's
// earlier characters; the prompt after the count digit's last line, else
// after the adaptive wait (the VX in ELM mode: its late prompt)
inline void bench::line_elm327(uint32_t slot, int64_t end)
{
    flight& f = m_flights[slot];
    if (f.cut || !m_cmd.out || m_cmd.prompted) {
        f.cut = true; /* after the prompt the adapter reads no more of it */
        return;
    }
    const adapter_model& m = m_cfg.model;
    const int64_t chars = (f.can_id > 0x7FFu ? 8 : 3) + 16 + 1;
    m_line_free = std::max(end, m_line_free) + chars * m.char_us;
    f.line_at = m_line_free + m.link_us;
    ++m_cmd.lines;
    const bool last = f.next == f.frames;
    if (f.frames > 1 && m.late_prompt_us > 0) {
        if (last) {
            prompt_at(end + m.late_prompt_us, 2);
        }
    } else if (m_cmd.count > 0 && m_cmd.lines == m_cmd.count) {
        if (!last) {
            f.cut = true;
            m_cmd.cut = true;
            m_cmd.frames_left = f.frames - f.next;
            ++m_result.cuts;
        }
        prompt_at(m_line_free, 2);
    } else if (last) {
        prompt_at(end + m.wait_us, 2);
    }
}

// The command's prompt: chars more on the line from from_adapter, at the
// host one link later
inline void bench::prompt_at(int64_t from_adapter, uint32_t chars)
{
    if (!m_cmd.out || m_cmd.prompted) {
        return;
    }
    m_cmd.prompted = true;
    m_line_free = std::max(from_adapter, m_line_free) +
                  static_cast<int64_t>(chars) * m_cfg.model.char_us;
    at(m_line_free + m_cfg.model.link_us, prompt);
}

// ask()'s end: a cut reply holds its channel while the ECU sends the
// rest; a one-DID request whose digit cut it is taken back and its DID
// asked with none from then on, a Service 01 PID's leaves the digit; then
// the next pass. The worker's retry of a DID after NO DATA isn't modeled:
// the simulated car answers it.
inline void bench::on_prompt(const event& ev)
{
    const command q = m_cmd;
    m_cmd = command{};
    ecuio_request_engine* e = m_engine.get();
    if (q.cut) {
        const int64_t hold = int64_t{q.frames_left} * 10000 + 50000;
        m_hold_until[q.chan] = ev.at + std::min<int64_t>(hold, 1000000);
    }
    if (q.cut && q.count > 0 && q.did_ask &&
        e->chans[q.chan].outstanding == q.row) {
        elm327_long_did_set(*m_long_dids, q.did);
        ecuio_engine_sent(e, q.chan, q.row, false, ev.at);
    } else if (q.cut && q.count > 0 && !q.pack && !q.did_ask) {
        elm327_one_frame_set(m_one_frame, q.pid, false);
    }
    look_at(ev.at);
}

} // namespace pacing

#endif // ECUIO_TESTS_PACING_BENCH_H
