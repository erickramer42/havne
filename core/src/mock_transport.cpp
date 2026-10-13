// File/layer: core/src/mock_transport.cpp | C++17, Qt-free, CI-validated headless build
// Testing: All core tests must pass UNCHANGED after refactor

#include "qttune/core.h"
#include "qttune/internal.h"
#include "session.h"
#include "qttune/signals.h"   /* SimState only — decode stays consumer-side */

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace {

constexpr uint32_t DEFAULT_FRAME_INTERVAL_MS = 50;

/* ---------------------------------------------------------------
 * Simulated vehicle state: physically plausible, tick-driven.
 *
 * Encode side of the signal contract with kMockDefs in
 * signal_sets.cpp. The transport NEVER decodes — rule 2 — it
 * only guarantees "emitted payloads are decodable by
 * qttune_mock_signal_set()", which the integration test enforces.
 * --------------------------------------------------------------- */
struct SimState {
    uint64_t tick = 0;
    float coolant_c = 20.0f;
    float iat_c = 20.0f;
    float load_pct = 0.0f;
    float throttle_pct = 0.0f;

    float rpm() const
    {
        const float idle =
            730.0f + 25.0f * std::sin(static_cast<float>(tick) * 0.13f);
        const uint64_t phase = tick % 200;
        if (phase < 30) {
            return idle + std::sin(phase / 30.0f * 3.14159f) * 2400.0f;
        }
        return idle;
    }

    float speed_kmh() const
    {
        const float t = static_cast<float>(tick);
        return 47.5f + 47.5f * std::sin(t * 0.0063f);
    }

    void step()
    {
        /* throttle follows the rev blip, decays to idle creep */
        const uint64_t phase = tick % 200;
        if (phase < 30) {
            throttle_pct = 80.0f * std::sin(phase / 30.0f * 3.14159f);
        } else {
            throttle_pct += (8.0f - throttle_pct) * 0.08f;
        }

        coolant_c += (88.0f - coolant_c) * 0.004f;
        iat_c += ((coolant_c - 25.0f) - iat_c) * 0.001f;
        ++tick;
    }
};

uint8_t clamp_u8(float v)
{
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return static_cast<uint8_t>(v + 0.5f);
}

uint16_t clamp_u16(float v)
{
    if (v < 0.0f) return 0;
    if (v > 65535.0f) return 65535;
    return static_cast<uint16_t>(v + 0.5f);
}

/* Layout MUST mirror kMockDefs in signal_sets.cpp:
 *   [0..1] RPM      LE uint16, scale 0.25
 *   [2]    coolant  int8, offset -40
 *   [3]    speed    uint8, km/h
 *   [4]    throttle uint8, scale 100/255
 *   [5]    IAT      int8, offset -40
 *   [6]    load     uint8, scale 100/255
 *   [7]    pad
 */
void encode_payload(const SimState& sim, uint8_t out[8])
{
    const uint16_t rpm_raw = clamp_u16(sim.rpm() / 0.25f);
    out[0] = static_cast<uint8_t>(rpm_raw & 0xFF);
    out[1] = static_cast<uint8_t>((rpm_raw >> 8) & 0xFF);

    out[2] = clamp_u8(sim.coolant_c + 40.0f);
    out[3] = clamp_u8(sim.speed_kmh());
    out[4] = clamp_u8(sim.throttle_pct * 255.0f / 100.0f);
    out[5] = clamp_u8(sim.iat_c + 40.0f);
    out[6] = clamp_u8(sim.load_pct * 255.0f / 100.0f);
    out[7] = 0;
}

} // namespace

struct qttune_mock_transport {
    std::atomic<bool> stop_requested{false};
    std::thread worker;
    qttune_session_t* session = nullptr;  // Need this for dispatch
    uint32_t interval_ms = DEFAULT_FRAME_INTERVAL_MS;
    uint64_t frame_counter = 0;
    SimState sim;   /* owned by worker — only touched on worker thread */
};

namespace {

/* Worker thread entrypoint */
void mock_worker(qttune_mock_transport* t)
{
    const auto interval = std::chrono::milliseconds(t->interval_ms);

    while (!t->stop_requested.load()) {
        qttune_frame_t frame{};
        frame.timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        frame.is_extended = static_cast<uint8_t>((t->frame_counter % 16) == 0);
        frame.data_length = 8;

        const float throttle = t->sim.throttle_pct;
        t->sim.step();
        t->sim.load_pct = throttle * 0.9f + t->sim.load_pct * 0.1f;
        encode_payload(t->sim, frame.data);
        t->frame_counter++;

        qttune_session_dispatch_callbacks(t->session, &frame);
        std::this_thread::sleep_for(interval);
    }
}

/* ---- ops implementations: static, dispatched via the vtable ---- */

qttune_status_t mock_start(qttune_session_t* session, void* impl)
{
    qttune_mock_transport* t = static_cast<qttune_mock_transport*>(impl);
    if (t == nullptr) {
        return QT_ERR_NO_TRANSPORT;
    }
    if (t->worker.joinable()) {
        return QT_OK; /* already running — idempotent */
    }
    try {
        t->worker = std::thread(mock_worker, t);
        return QT_OK;
    } catch (...) {
        return QT_ERR_OUT_OF_MEMORY;
    }
}

void mock_detach(qttune_session_t* session, void* impl)
{
    qttune_mock_transport* t = static_cast<qttune_mock_transport*>(impl);
    if (t == nullptr) {
        return;
    }

    /* The transport wrapper holds `impl`; we must free both. */
    qttune_transport_t* wrapper = session->transport;
    session->transport = nullptr;

    t->stop_requested.store(true);
    if (t->worker.joinable()) {
        t->worker.join();
    }
    delete t;
    delete wrapper;
}

/* Static ops table — one per transport implementation, immutable. */
const qttune_transport_ops_t kMockTransportOps = {
    mock_start,
    mock_detach
};

} // namespace

extern "C" {

qttune_status_t qttune_mock_transport_attach(qttune_session_t* session,
                                             const char* /*uri*/)
{
    /* uri params (?interval=...) are a later addition */
    if (session->transport != nullptr) {
        return QT_OK; /* idempotent */
    }
    try {
        qttune_mock_transport* impl = new qttune_mock_transport();
        try {
            session->transport = new qttune_transport_t{&kMockTransportOps, impl};
        } catch (...) {
            delete impl;   /* wrapper alloc failed — no leak */
            return QT_ERR_OUT_OF_MEMORY;
        }
        impl->session = session;  /* for callback dispatch */
    } catch (...) {
        return QT_ERR_OUT_OF_MEMORY;
    }
    /* NOTE: worker thread NOT started here — explicit start required.
     * Keeps registration-only sessions single-threaded for tests. */
    return QT_OK;
}

/* ---- Legacy stubs removed: session now routes through ops only. ---- */

} // extern "C"
