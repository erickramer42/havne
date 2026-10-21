// File/layer: core/src/mock_transport.cpp | C++17, Qt-free, CI-validated headless build
// Testing: UNIT — CoreTest.MockFramesDecodeAgainstMockSignalSet decodes
//          real worker frames against the mock set; the drive cycle
//          itself is tested deterministically (CoreTest.MockDriveCycle*)

#include "qttune/core.h"
#include "qttune/internal.h"
#include "session.h"
#include "qttune/signals.h"
#include "qttune/signal_sets.h"
#include "mock_drive_cycle.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

/* One drive-cycle step per frame, so the interval must match
 * qttune_mock::kTickS. */
constexpr uint32_t DEFAULT_FRAME_INTERVAL_MS = 50;
constexpr uint32_t MOCK_DIAG_ID = 0x18FF0C00u;

} // namespace

struct qttune_mock_transport {
    std::atomic<bool> stop_requested{false};
    std::thread worker;
    qttune_session_t* session = nullptr;  // Need this for dispatch
    uint32_t interval_ms = DEFAULT_FRAME_INTERVAL_MS;
    uint64_t frame_counter = 0;
    /* Session epoch, captured at attach (inside session_create).
     * Written once before the worker spawns and read-only after, so
     * thread creation publishes it; no further synchronization. */
    std::chrono::steady_clock::time_point epoch{};
    qttune_mock::DriveCycle sim;   /* owned by worker — only touched on worker thread */
};

namespace {

/* Worker thread entrypoint */
void mock_worker(qttune_mock_transport* t)
{
    const auto interval = std::chrono::milliseconds(t->interval_ms);

    while (!t->stop_requested.load()) {
        qttune_frame_t frame{};
        /* Session-relative and monotonic, per the core.h contract. */
        frame.timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t->epoch).count());
        frame.data_length = MOCK_TELEMETRY_PAYLOAD_LEN;

        /* Every 16th frame is a 29-bit "diagnostic" frame carrying the
         * same payload. It exercises FLAG_EXTENDED, and because decode
         * is gated on message ID, it proves unrelated IDs are rejected. */
        if ((t->frame_counter % 16) == 0) {
            frame.flags = QTTUNE_FRAME_FLAG_EXTENDED | QTTUNE_FRAME_FLAG_FD;
            frame.arbitration_id = MOCK_DIAG_ID;
        } else {
            frame.flags = QTTUNE_FRAME_FLAG_FD;
            frame.arbitration_id = MOCK_TELEMETRY_ARB_ID;
        }

        t->sim.step();
        qttune_mock::encode_payload(t->sim, frame.data);
        t->frame_counter++;

        qttune_session_dispatch_callbacks(t->session, &frame);
        std::this_thread::sleep_for(interval);
    }
}

/* ---- ops implementations: static, dispatched via the vtable ---- */

qttune_status_t mock_start(qttune_session_t* session, void* impl)
{
    (void)session;   /* impl carries everything; fixes CI warning C4100 */
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
        impl->epoch = std::chrono::steady_clock::now();  /* session start */
    } catch (...) {
        return QT_ERR_OUT_OF_MEMORY;
    }
    /* NOTE: worker thread NOT started here — explicit start required.
     * Keeps registration-only sessions single-threaded for tests. */
    return QT_OK;
}

} // extern "C"
