// File/layer: core/src/mock_transport.cpp | C++17, Qt-free, CI-validated headless build
// Testing: UNIT — CoreTest.MockFramesDecodeAgainstMockSignalSet decodes
//          real worker frames against the mock set; the drive cycle
//          itself is tested deterministically (CoreTest.MockDriveCycle*)

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "session.h"
#include "ecuio/signals.h"
#include "ecuio/signal_sets.h"
#include "mock_drive_cycle.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

/* One drive-cycle step per frame, so the interval must match
 * ecuio_mock::kTickS. */
constexpr uint32_t DEFAULT_FRAME_INTERVAL_MS = 50;
constexpr uint32_t MOCK_DIAG_ID = 0x18FF0C00u;

} // namespace

struct ecuio_mock_transport {
    std::atomic<bool> stop_requested{false};
    std::thread worker;
    ecuio_session_t* session = nullptr;   // Need this for dispatch
    uint32_t interval_ms = DEFAULT_FRAME_INTERVAL_MS;
    uint64_t frame_counter = 0;
    /* Session epoch, captured at attach (inside session_create).
     * Written once before the worker spawns and read-only after, so
     * thread creation publishes it; no further synchronization. */
    std::chrono::steady_clock::time_point epoch{};
    ecuio_mock::DriveCycle sim;    /* owned by worker — only touched on worker thread */
};

namespace {

/* Worker thread entrypoint */
void mock_worker(ecuio_mock_transport* t)
{
    const auto interval = std::chrono::milliseconds(t->interval_ms);

    while (!t->stop_requested.load()) {
        ecuio_frame_t frame{};
        /* Session-relative and monotonic, per the core.h contract. */
        frame.timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t->epoch).count());
        frame.data_length = ECUIO_MOCK_TELEMETRY_PAYLOAD_LEN;

        /* Every 16th frame is a 29-bit "diagnostic" frame carrying the
         * same payload. It exercises FLAG_EXTENDED, and because decode
         * is gated on message ID, it proves unrelated IDs are rejected. */
        if ((t->frame_counter % 16) == 0) {
            frame.flags = ECUIO_FRAME_FLAG_EXTENDED | ECUIO_FRAME_FLAG_FD;
            frame.arbitration_id = MOCK_DIAG_ID;
        } else {
            frame.flags = ECUIO_FRAME_FLAG_FD;
            frame.arbitration_id = ECUIO_MOCK_TELEMETRY_ARB_ID;
        }

        t->sim.step();
        ecuio_mock::encode_payload(t->sim, frame.data);
        t->frame_counter++;

        ecuio_session_dispatch_callbacks(t->session, &frame);
        std::this_thread::sleep_for(interval);
    }
}

/* ---- ops implementations: static, dispatched via the vtable ---- */

ecuio_status_t mock_start(ecuio_session_t* session, void* impl)
{
    (void)session;   /* impl carries everything; fixes CI warning C4100 */
    ecuio_mock_transport* t = static_cast<ecuio_mock_transport*>(impl);
    if (t == nullptr) {
        return ECUIO_ERR_NO_TRANSPORT;
    }
    if (t->worker.joinable()) {
        return ECUIO_OK; /* already running — idempotent */
    }
    try {
        t->worker = std::thread(mock_worker, t);
        return ECUIO_OK;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

void mock_detach(ecuio_session_t* session, void* impl)
{
    ecuio_mock_transport* t = static_cast<ecuio_mock_transport*>(impl);
    if (t == nullptr) {
        return;
    }

    /* The transport wrapper holds `impl`; we must free both. */
    ecuio_transport_t* wrapper = session->transport;
    session->transport = nullptr;

    t->stop_requested.store(true);
    if (t->worker.joinable()) {
        t->worker.join();
    }
    delete t;
    delete wrapper;
}

/* Static ops table — one per transport implementation, immutable. */
const ecuio_transport_ops_t kMockTransportOps = {
    mock_start,
    mock_detach
};

} // namespace

extern "C" {

/* The mock has no parameters of its own: profile is core's, so create
 * refuses every other key, as on j2534://. */
const char* const ecuio_mock_query_keys[] = {"profile", nullptr};

ecuio_status_t ecuio_mock_transport_attach(ecuio_session_t* session,
                                           const char* uri,
                                           const ecuio_uri_query* query)
{
    /* The text before the query is a free label (mock://demo); create
     * has judged the query. */
    (void)uri;
    (void)query;
    if (session->transport != nullptr) {
        return ECUIO_OK; /* idempotent */
    }
    try {
        ecuio_mock_transport* impl = new ecuio_mock_transport();
        try {
            session->transport = new ecuio_transport_t{&kMockTransportOps, impl};
        } catch (...) {
            delete impl;   /* wrapper alloc failed — no leak */
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        impl->session = session;  /* for callback dispatch */
        impl->epoch = std::chrono::steady_clock::now();  /* session start */
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    session->info.backend = ECUIO_BACKEND_MOCK;
    /* NOTE: worker thread NOT started here — explicit start required.
     * Keeps registration-only sessions single-threaded for tests. */
    return ECUIO_OK;
}

} // extern "C"
