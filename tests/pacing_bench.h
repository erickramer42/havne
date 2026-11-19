// File/layer: tests/pacing_bench.h | test-only, C++17, Qt-free; includes
//             core/src (the request engine, its setup, the session's
//             decoder) and cli/summary_lines.h
// Testing: UNIT: PacingTest (test_pacing); BENCH: bench_pacing
//
// The request engine driven on a simulated clock: a model of a
// transport's worker asks the engine as that worker does, each request
// crosses one simulated bus, the simulated ECU answers it
// (ecuio_sim_ask) with each module's reply timing (pacing_timing.h), and
// each reply's frames cross the bus in their airtime, first come first
// served. The replies go through the session's own decoder into
// havne-cli log's tally (cli/summary_lines.h), so a run gives the log's
// summary figures, the same on every machine and in milliseconds.
//
// The engine reads no clock: the loop hands it the simulated time, so
// nothing in core changes. The models mirror their workers' calls (the
// J2534 worker's pass, SocketCAN's one request awaiting its echo); each
// constant is the adapter's, calibrated against the car.

#ifndef ECUIO_TESTS_PACING_BENCH_H
#define ECUIO_TESTS_PACING_BENCH_H

#include "pacing_timing.h"
#include "summary_lines.h"

#include "request_engine.h"
#include "request_setup.h"
#include "session.h"

#include "ecuio/core.h"
#include "ecuio/sim.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace pacing {

enum class adapter { j2534, socketcan };

// An adapter model's constants (the defaults are the 2026-11-09
// baseline's and the 2026-11-03 tap's, uncalibrated)
struct adapter_model {
    adapter kind = adapter::j2534;
    int64_t write_us = 1000;  /* J2534: a write through the surrogate */
    int64_t deliver_us = 500; /* J2534: the last frame to the engine */
    int64_t fc_turn_us = 170; /* the adapter's Flow Control after a FF */
    int64_t echo_us = 100;    /* SocketCAN: the request's echo */
};

// One run: the profile the session and the simulated car share, the
// session's request parameters (as the URI's keys set them), the
// adapter, each module's timing, the picks (none: the whole set), the
// Service 01 PIDs the bitmaps list (none: the simulated car's own)
struct bench_config {
    std::string profile = "obd2";
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

// The frames an ISO-TP message of len bytes takes (classic CAN)
inline uint32_t isotp_frames_of(uint32_t len)
{
    return len <= 7 ? 1u : 1u + (len - 6u + 6u) / 7u;
}

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
    enum kind { look, request, reply_frame, tester_fc, echo, deliver };
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
    void send(uint32_t c, int32_t r, int64_t now);
    void on_request(const event& ev);
    void on_reply_frame(const event& ev);
    void on_deliver(const event& ev);
    void schedule_look(int64_t now);

    bench_config m_cfg;
    ecuio_session_t* m_session = nullptr;
    ecuio_sim_t* m_sim = nullptr;
    std::unique_ptr<ecuio_request_engine> m_engine;
    std::vector<std::mt19937_64> m_rng; /* one for each channel's module */
    std::priority_queue<event, std::vector<event>, std::greater<event>>
        m_events;
    std::vector<flight> m_flights;
    bench_result m_result;
    uint64_t m_seq = 0;
    int64_t m_look = INT64_MAX;
    int64_t m_bus_free = 0;
    bool m_echo_pending = false;
    uint32_t m_next_chan = 0;
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
    const std::string sim_uri =
        m_cfg.profile == "obd2"
            ? std::string("mock://pacing?values=canned")
            : "mock://pacing?values=canned&profile=" + m_cfg.profile;
    if (ecuio_sim_create_unbound(sim_uri.c_str(), 0, &m_sim) != ECUIO_OK) {
        *why = std::string("sim: ") + ecuio_last_error_detail();
        return false;
    }
    for (uint32_t c = 0; c < m_engine->chan_count; ++c) {
        m_rng.push_back(
            sim_timing_rng(m_cfg.timing.seed, m_session->info.rx_ids[c]));
    }
    m_result.tally = cli::SummaryTally(set, m_cfg.picks);
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
                m_cfg.model.kind == adapter::j2534 ? look_j2534(ev.at)
                                                   : look_socketcan(ev.at);
            }
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

// The J2534 worker's pass: the waits that ran out, then each idle
// channel's next request once its floor has passed, each write taking
// the surrogate's time
inline void bench::look_j2534(int64_t now)
{
    ecuio_request_engine* e = m_engine.get();
    for (uint32_t c = 0; c < e->chan_count; ++c) {
        if (ecuio_engine_expire(e, c, now)) {
            ++m_result.timeouts;
        }
        const int32_t r = ecuio_engine_due(e, c, now);
        if (r != ECUIO_ENGINE_NONE) {
            const int64_t started = now;
            now += m_cfg.model.write_us;
            send(c, r, now);
            ecuio_engine_sent(e, c, r, true, now, started);
        }
    }
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
    const int64_t next = ecuio_engine_next_event_us(m_engine.get(), now);
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
        return;
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
        f.frames = isotp_frames_of(replies[i].length);
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
    if (f.frames == 1) {
        at(end + m_cfg.model.deliver_us, deliver, ev.slot);
    } else if (k == 0) {
        at(end + m_cfg.model.fc_turn_us, tester_fc, ev.slot);
    } else if (k + 1 < f.frames) {
        const int64_t start = end - kFrameUs;
        at(std::max(start + m_cfg.timing.cf_us, end), reply_frame, ev.slot);
    } else {
        at(end + m_cfg.model.deliver_us, deliver, ev.slot);
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
    look_at(ev.at);
}

} // namespace pacing

#endif // ECUIO_TESTS_PACING_BENCH_H
