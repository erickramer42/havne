// File/layer: core/src/j2534_transport.cpp | C++17, Qt-free, Win32-guarded
// Testing: HARNESS — test_j2534_stream drives the full
//          InjectFrame -> ReadMsgs -> callback pipeline on a worker thread
// CI: non-Windows builds a linking stub; core.cpp routes j2534://
//     only on _WIN32, so the stub is unreachable there

#include "qttune/core.h"
#include "qttune/internal.h"
#include "session.h"
#include "qttune/j2534.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32

namespace {

constexpr uint32_t J2534_POLL_INTERVAL_MS = 20;
constexpr uint32_t J2534_READ_TIMEOUT_MS  = 100;

struct qttune_j2534_transport {
    qttune_session_t* session = nullptr;
    qttune_j2534_handle_t* handle = nullptr;
    unsigned long channel = 0;
    std::atomic<bool> stop_requested{false};
    std::thread worker;
};

/* Worker entrypoint — mirrors mock_worker's discipline: impl-owned
 * state, dispatch through the session snapshot path, stop via atom. */
void j2534_worker(qttune_j2534_transport* t)
{
    while (!t->stop_requested.load()) {
        qttune_frame_t frame{};
        bool got = false;
        qttune_status_t st = qttune_j2534_read_frame(t->handle, t->channel,
                                                     &frame,
                                                     J2534_READ_TIMEOUT_MS,
                                                     &got);
        if (st == QT_OK && got) {
            qttune_session_dispatch_callbacks(t->session, &frame);
        }
        /* ReadMsgs on a quiet device (or the fake) returns immediately;
         * the poll interval bounds CPU and caps added latency. */
        std::this_thread::sleep_for(
            std::chrono::milliseconds(J2534_POLL_INTERVAL_MS));
    }
}

/* Resolve a j2534:// URI to a concrete dll_path. Grammar (see
 * internal.h): empty = first device; path-like = direct; else
 * vendor-name match. All misses are QT_ERR_NO_TRANSPORT. */
qttune_status_t resolve_uri(const char* uri, qttune_j2534_device_t* dev)
{
    const char* rest = uri + 8;   /* core.cpp verified the prefix */

    qttune_j2534_device_t* devices = nullptr;
    uint32_t count = 0;

    if (rest[0] == '\0') {
        qttune_status_t st = qttune_j2534_discover_devices(&devices, &count);
        if (st != QT_OK) {
            return st;
        }
        if (count == 0) {
            qttune_j2534_free_devices(devices);
            return QT_ERR_NO_TRANSPORT;
        }
        *dev = devices[0];
        qttune_j2534_free_devices(devices);
        return QT_OK;
    }

    const size_t n = strlen(rest);
    const bool looks_like_path =
        n >= 4 && _stricmp(rest + n - 4, ".dll") == 0;
    const bool has_separator =
        strchr(rest, '\\') != nullptr || strchr(rest, '/') != nullptr;

    if (looks_like_path || has_separator) {
        strncpy(dev->dll_path, rest, sizeof(dev->dll_path) - 1);
        strncpy(dev->vendor_name, "direct",
                sizeof(dev->vendor_name) - 1);
        dev->vendor_id = 0;
        return QT_OK;
    }

    /* Vendor-name match, case-insensitive */
    qttune_status_t st = qttune_j2534_discover_devices(&devices, &count);
    if (st != QT_OK) {
        return st;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (_stricmp(devices[i].vendor_name, rest) == 0) {
            *dev = devices[i];
            qttune_j2534_free_devices(devices);
            return QT_OK;
        }
    }
    qttune_j2534_free_devices(devices);
    return QT_ERR_NO_TRANSPORT;
}

qttune_status_t j2534_start(qttune_session_t* /*session*/, void* impl)
{
    qttune_j2534_transport* t =
        static_cast<qttune_j2534_transport*>(impl);
    if (t == nullptr) {
        return QT_ERR_NO_TRANSPORT;
    }
    if (t->worker.joinable()) {
        return QT_OK;   /* already running — idempotent per ops contract */
    }

    unsigned long channel = 0;
    qttune_status_t st = qttune_j2534_connect_channel(t->handle, &channel);
    if (st != QT_OK) {
        return QT_ERR_NO_TRANSPORT;
    }
    t->channel = channel;

    try {
        t->worker = std::thread(j2534_worker, t);
        return QT_OK;
    } catch (...) {
        t->channel = 0;
        return QT_ERR_OUT_OF_MEMORY;
    }
}

void j2534_detach(qttune_session_t* session, void* impl)
{
    qttune_j2534_transport* t =
        static_cast<qttune_j2534_transport*>(impl);
    if (t == nullptr) {
        return;
    }

    qttune_transport_t* wrapper = session->transport;
    session->transport = nullptr;

    /* ORDER: stop + join the worker BEFORE touching the handle —
     * the worker calls read_msgs through function pointers that
     * dangle the moment j2534_close unmaps the module. */
    t->stop_requested.store(true);
    if (t->worker.joinable()) {
        t->worker.join();
    }
    if (t->handle != nullptr) {
        if (t->channel != 0) {
            qttune_j2534_disconnect_channel(t->handle, t->channel);
        }
        qttune_j2534_close(t->handle);
    }
    delete t;
    delete wrapper;
}

const qttune_transport_ops_t kJ2534TransportOps = {
    j2534_start,
    j2534_detach
};

} // namespace

extern "C" {

qttune_status_t qttune_j2534_transport_attach(qttune_session_t* session,
                                             const char* uri)
{
    if (session == nullptr || uri == nullptr) {
        return QT_ERR_NULL_ARGUMENT;
    }
    if (session->transport != nullptr) {
        return QT_OK;   /* idempotent, mirroring mock attach */
    }

    qttune_j2534_device_t dev = {};
    qttune_status_t st = resolve_uri(uri, &dev);
    if (st != QT_OK) {
        return st;
    }

    /* Handle opens eagerly at ATTACH (not start) so load failures
     * surface at session_create, where the caller can pick another
     * URI — matching the mock's failure-point ergonomics. */
    qttune_j2534_handle_t* handle = nullptr;
    st = qttune_j2534_open(&dev, &handle);
    if (st != QT_OK) {
        return st;
    }

    try {
        qttune_j2534_transport* impl = new qttune_j2534_transport();
        try {
            session->transport =
                new qttune_transport_t{&kJ2534TransportOps, impl};
        } catch (...) {
            delete impl;
            qttune_j2534_close(handle);
            return QT_ERR_OUT_OF_MEMORY;
        }
        impl->session = session;
        impl->handle = handle;
    } catch (...) {
        qttune_j2534_close(handle);
        return QT_ERR_OUT_OF_MEMORY;
    }
    return QT_OK;
}

} // extern "C"

#else

extern "C" {

qttune_status_t qttune_j2534_transport_attach(qttune_session_t* /*session*/,
                                             const char* /*uri*/)
{
    return QT_ERR_NOT_IMPLEMENTED;
}

} // extern "C"

#endif /* _WIN32 */
