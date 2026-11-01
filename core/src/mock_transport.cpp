// File/layer: core/src/mock_transport.cpp | C++17, Qt-free, CI-validated headless build
// Testing: UNIT: CoreTest.MockFramesDecodeAgainstMockSignalSet decodes
//          real worker frames against the mock set; the drive cycle
//          itself is tested deterministically (CoreTest.MockDriveCycle*);
//          CoreTest.MockAnswersTheSessionsDids decodes its DID replies
//          under uds-generic; CoreTest.MockFramesKeepTheTickInterval
//          bounds the worker's cadence against its timestamps;
//          CoreTest.SessionTimeRunsOnTheClock (stamps on ecuio_clock_us);
//          its one channel and diagnostics (mock_ecu.h, ADR-0037):
//          CoreTest.MockSessionHasOneQuietChannel,
//          CoreTest.MockReportsTheFakeEcusDiagnostics,
//          CoreTest.MockReadAgainReadsTheSameReport,
//          CoreTest.MockDiagnoseOffReportsNone; its clear (ADR-0038),
//          taken on the tick at the drive cycle's engine speed:
//          CoreTest.MockClearsTheReportLikeTheFakeEcu

#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "session.h"
#include "ecuio/signals.h"
#include "ecuio/signal_sets.h"
#include "mock_drive_cycle.h"
#include "mock_ecu.h"
#include "host_clock.h"
#include "request_setup.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
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
    host_clock::time_point epoch{};
    ecuio_mock::DriveCycle sim;    /* owned by worker: only touched on worker thread */
    /* The simulated ECU's diagnostics (mock_ecu.h): an engine asking
     * only the list, read once at attach and again on each Read again
     * or clear; nullptr under ?diagnose=0. Attach's, then the worker's
     * alone. */
    std::unique_ptr<ecuio_request_engine> diag;
    ecuio_mock_ecu ecu;          /* sim's engine, cleared or not */
    int64_t diag_now_us = 0;     /* the engine's clock (mock_ecu.h) */
    uint32_t diag_seen = 0;      /* ecuio_take_diag_request's count */
    uint32_t clear_seen[1] = {}; /* ecuio_take_clear_requests' count */
    uint32_t diag_published = 0; /* the report's last generation out */
};

namespace {

void answer_dids(ecuio_mock_transport* t, uint64_t timestamp_us);

/* Reads the simulated ECU's diagnostics to their end and hands the
 * report to the session */
void read_diagnostics(ecuio_mock_transport* t)
{
    ecuio_mock_ecu_read(t->diag.get(), &t->ecu, &t->diag_now_us);
    ecuio_publish_diagnostics(t->session, *t->diag, 0, ECUIO_MOCK_ECU_RX_ID,
                              t->diag_published);
}

/* Worker thread entrypoint */
void mock_worker(ecuio_mock_transport* t)
{
    const auto interval = std::chrono::milliseconds(t->interval_ms);
    /* Each tick is due one interval after the last one was due, not after
     * it ended: Windows rounds a sleep up to its 15.6 ms timer step, so a
     * plain 50 ms sleep ticks every ~62 ms and the drive cycle, whose
     * physics advance kTickS per frame, plays at 80 % against the frames'
     * own timestamps. A worker a whole interval behind (a stall, a
     * debugger) restarts the schedule rather than bursting to catch up. */
    auto due = std::chrono::steady_clock::now();

    while (!t->stop_requested.load()) {
        ecuio_frame_t frame{};
        /* Session-relative and monotonic, per the core.h contract. */
        frame.timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                host_clock::now() - t->epoch).count());
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
        answer_dids(t, frame.timestamp_us);
        if (t->diag) {
            const uint32_t before = t->diag->chans[0].diag.generation;
            ecuio_take_diag_request(t->session, t->diag.get(), t->diag_seen);
            ecuio_take_clear_requests(t->session, t->diag.get(), t->clear_seen);
            if (t->diag->chans[0].diag.generation != before) {
                read_diagnostics(t);
            }
        }
        due += interval;
        const auto now = std::chrono::steady_clock::now();
        if (due < now) {
            due = now;
        }
        std::this_thread::sleep_until(due);
    }
}

/* Under uds-generic (ADR-0020) the mock stands in for the ECUs the URI
 * named: each tick it answers every DID of the session's set once, as
 * the J2534 transport would deliver the reply, 62 <DID> and a value
 * that moves with the tick: the tick count in a number's width (big-
 * endian, as the defs read), four bytes of it for a hex def, and a
 * line naming the DID and the tick for a text def. The set is written
 * once before the worker starts (core.cpp), so reading it needs no
 * lock. The mock set has no such def, so mock:// is unchanged. */
void answer_dids(ecuio_mock_transport* t, uint64_t timestamp_us)
{
    const ecuio_signal_set_t* set = t->session->active_signal_set;
    if (set == nullptr || set->defs == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (d.mux_offset != 0 || d.mux_length != 3 || (d.mux_key >> 16) != 0x62u) {
            continue;
        }
        bool answered = false;   /* two defs of one reply: one frame */
        for (uint32_t j = 0; j < i && !answered; ++j) {
            answered = set->defs[j].message_id == d.message_id &&
                       set->defs[j].mux_length == 3 &&
                       set->defs[j].mux_key == d.mux_key;
        }
        if (answered) {
            continue;
        }
        ecuio_frame_t frame{};
        frame.timestamp_us = timestamp_us;
        frame.arbitration_id = d.message_id;
        frame.flags = (d.message_id > 0x7FFu) ? ECUIO_FRAME_FLAG_EXTENDED : 0u;
        frame.data[0] = 0x62;
        frame.data[1] = static_cast<uint8_t>(d.mux_key >> 8);
        frame.data[2] = static_cast<uint8_t>(d.mux_key);
        const uint64_t tick = t->frame_counter;
        uint16_t n = 3;
        if (d.kind == ECUIO_VALUE_TEXT) {
            n = static_cast<uint16_t>(n + std::snprintf(
                reinterpret_cast<char*>(frame.data + n),
                sizeof(frame.data) - n, "MOCK %04X #%llu",
                static_cast<unsigned>(d.mux_key & 0xFFFFu),
                static_cast<unsigned long long>(tick)));
        } else {
            const uint32_t bytes = (d.kind == ECUIO_VALUE_HEX) ? 4u
                                 : (d.bit_length + 7u) / 8u;
            for (uint32_t b = 0; b < bytes; ++b) {
                frame.data[n++] = static_cast<uint8_t>(
                    tick >> (8 * (bytes - 1 - b)));
            }
        }
        frame.data_length = n;
        ecuio_session_dispatch_callbacks(t->session, &frame);
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
        return ECUIO_OK; /* already running: idempotent */
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
    (void)session;
    ecuio_mock_transport* t = static_cast<ecuio_mock_transport*>(impl);
    if (t == nullptr) {
        return;
    }
    t->stop_requested.store(true);
    if (t->worker.joinable()) {
        t->worker.join();
    }
    delete t;
}

/* Static ops table: one per transport implementation, immutable. */
const ecuio_transport_ops_t kMockTransportOps = {
    mock_start,
    mock_detach
};

} // namespace

extern "C" {

/* profile and did are core's; diagnose is the request key
 * (ecuio_request_param), turning the simulated ECU's diagnostics off.
 * Create refuses every other key, as on j2534://. */
const char* const ecuio_mock_query_keys[] = {"profile", "did", "diagnose",
                                             nullptr};

ecuio_status_t ecuio_mock_transport_attach(ecuio_session_t* session,
                                           const char* uri,
                                           const ecuio_uri_query* query)
{
    /* The text before the query is a free label (mock://demo); create
     * has split the query into its keys. */
    (void)uri;
    if (session->transport != nullptr) {
        return ECUIO_OK; /* idempotent */
    }
    request_params params;
    for (size_t i = 0; i < query->count; ++i) {
        const char* wanted = nullptr;
        if (ecuio_request_param(query->params[i], &params, &wanted) &&
            wanted != nullptr) {
            ecuio_detail_bad_value(query->params[i], wanted);
            return ECUIO_ERR_BAD_URI;
        }
    }
    ecuio_mock_transport* impl = nullptr;
    try {
        impl = new ecuio_mock_transport();
        if (params.diagnose) {
            impl->diag.reset(new ecuio_request_engine());
        }
        try {
            session->transport = new ecuio_transport_t{&kMockTransportOps, impl};
        } catch (...) {
            delete impl;   /* wrapper alloc failed: no leak */
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        impl->session = session;  /* for callback dispatch */
        impl->epoch = host_clock::now();  /* session start */
    } catch (...) {
        delete impl;
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    session->info.backend = ECUIO_BACKEND_MOCK;

    /* One channel, the simulated engine ECU: its support and identity
     * reports stay empty (the mock answers no Service 01 bitmap or 09
     * here), its diagnostics read now, before start, so the report is
     * there as the session is (ADR-0037). */
    session->info.channel_count = 1;
    session->info.rx_ids[0] = ECUIO_MOCK_ECU_RX_ID;
    session->info.tx_ids[0] = ECUIO_MOCK_ECU_TX_ID;
    {
        std::lock_guard<std::mutex> lock(session->support_mutex);
        session->support_count = 1;
        session->support[0].rx_id = ECUIO_MOCK_ECU_RX_ID;
        session->identity[0].rx_id = ECUIO_MOCK_ECU_RX_ID;
        session->diagnostics[0].rx_id = ECUIO_MOCK_ECU_RX_ID;
    }
    if (impl->diag) {
        impl->ecu.engine = &impl->sim;
        ecuio_engine_init(impl->diag.get(), 1);
        ecuio_engine_diagnose(impl->diag.get(), ecuio_obd2_signal_set());
        read_diagnostics(impl);
    }
    /* NOTE: worker thread NOT started here; explicit start required.
     * Keeps registration-only sessions single-threaded for tests. */
    return ECUIO_OK;
}

} // extern "C"
