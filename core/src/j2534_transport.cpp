// File/layer: core/src/j2534_transport.cpp | C++17, Qt-free, Win32-guarded
// Testing: HARNESS — test_j2534_stream drives the full
//          InjectFrame -> ReadMsgs -> callback pipeline on a worker thread,
//          and asserts ?baud=/?flags= values reach PassThruConnect
// CI: non-Windows builds a linking stub; core.cpp routes j2534://
//     only on _WIN32, so the stub is unreachable there

#include "qttune/core.h"
#include "qttune/internal.h"
#include "session.h"
#include "qttune/j2534.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

#ifdef _WIN32

namespace {

constexpr uint32_t J2534_POLL_INTERVAL_MS = 20;
constexpr uint32_t J2534_READ_TIMEOUT_MS  = 100;

struct qttune_j2534_transport {
    qttune_session_t* session = nullptr;
    qttune_j2534_handle_t* handle = nullptr;
    unsigned long channel = 0;
    /* Connect parameters from the URI query. baud == 0 is the
     * "unset" sentinel — the URI parser rejects an explicit baud=0 —
     * and j2534_loader's connect_channel substitutes the ISO 15765
     * default. flags pass through verbatim: 0 is both the default and
     * a legitimate value. Semantics of individual flag bits are NOT
     * interpreted here (BENCH tier, pending the vendor header diff). */
    unsigned long baud = 0;
    unsigned long flags = 0;
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

/* Strict unsigned parse: decimal by default, '0x'/'0X' prefix enables
 * hex digits. Rejects empty digits, '+', '-', whitespace, and overflow.
 * No strtoul — its tolerance for signs/whitespace is a liability at
 * an ABI boundary where garbage must become an error, not a value. */
bool parse_number(const char* s, size_t len, unsigned long* out_val)
{
    if (len == 0) {
        return false;
    }

    bool hex = false;
    size_t i = 0;
    if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        hex = true;
        i = 2;
        if (i == len) {
            return false;   /* bare "0x" */
        }
    }

    const unsigned long max =
        std::numeric_limits<unsigned long>::max();
    const unsigned long base = hex ? 16ul : 10ul;
    unsigned long value = 0;
    for (; i < len; ++i) {
        unsigned long digit;
        const char c = s[i];
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned long>(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned long>(c - 'a' + 10);
        } else if (hex && c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned long>(c - 'A' + 10);
        } else {
            return false;
        }
        if (value > (max - digit) / base) {
            return false;   /* would overflow unsigned long */
        }
        value = value * base + digit;
    }

    *out_val = value;
    return true;
}

/* URI query parser. Contract (strict — malformed means NO_TRANSPORT,
 * surfaces at session_create before any DLL load):
 *   - lowercase keys only: "baud" (decimal, nonzero) and
 *     "flags" (0x-hex or decimal; 0 is legal)
 *   - unknown keys, missing '=', empty segments, trailing '&',
 *     and a bare trailing '?' on the URI are all rejected
 *   - duplicate keys: last occurrence wins
 */
qttune_status_t parse_query(const char* query, unsigned long* baud,
                            unsigned long* flags)
{
    const char* p = query;
    for (;;) {
        const char* amp = strchr(p, '&');
        const char* end = (amp != nullptr) ? amp : p + strlen(p);

        const char* eq = static_cast<const char*>(
            memchr(p, '=', static_cast<size_t>(end - p)));
        if (eq == nullptr) {
            return QT_ERR_NO_TRANSPORT;   /* no '=' in segment */
        }

        const size_t key_len = static_cast<size_t>(eq - p);
        const char* val = eq + 1;
        const size_t val_len = static_cast<size_t>(end - val);

        if (key_len == 4 && strncmp(p, "baud", 4) == 0) {
            unsigned long v = 0;
            if (!parse_number(val, val_len, &v) || v == 0) {
                return QT_ERR_NO_TRANSPORT;
            }
            *baud = v;
        } else if (key_len == 5 && strncmp(p, "flags", 5) == 0) {
            unsigned long v = 0;
            if (!parse_number(val, val_len, &v)) {
                return QT_ERR_NO_TRANSPORT;
            }
            *flags = v;
        } else {
            return QT_ERR_NO_TRANSPORT;   /* unknown parameter */
        }

        if (*end == '\0') {
            return QT_OK;
        }
        p = end + 1;
        if (*p == '\0') {
            return QT_ERR_NO_TRANSPORT;   /* trailing '&' */
        }
    }
}

/* Resolve a j2534:// URI to a concrete dll_path + connect params.
 * Grammar (see internal.h): [selector][?query]. The query is split off
 * and parsed BEFORE selector resolution — previously a query glued onto
 * a path reached LoadLibraryEx as part of the filename, and a query on
 * a vendor name defeated the discovery match. Selector routing itself
 * is unchanged: empty = first device; path-like = direct; else
 * vendor-name match. All misses are QT_ERR_NO_TRANSPORT. */
qttune_status_t resolve_uri(const char* uri, qttune_j2534_device_t* dev,
                            unsigned long* out_baud, unsigned long* out_flags)
{
    const char* rest = uri + 8;   /* core.cpp verified the prefix */

    const char* q = strchr(rest, '?');
    if (q != nullptr && *(q + 1) == '\0') {
        return QT_ERR_NO_TRANSPORT;   /* bare trailing '?' */
    }

    char selector[QTTUNE_J2534_DLL_PATH_LEN];
    const size_t n = (q != nullptr)
                     ? static_cast<size_t>(q - rest)
                     : strlen(rest);
    if (n >= sizeof(selector)) {
        return QT_ERR_NO_TRANSPORT;   /* longer than any legal selector */
    }
    memcpy(selector, rest, n);
    selector[n] = '\0';

    if (q != nullptr) {
        qttune_status_t st = parse_query(q + 1, out_baud, out_flags);
        if (st != QT_OK) {
            return st;   /* fail fast — no DLL load on malformed URIs */
        }
    }

    qttune_j2534_device_t* devices = nullptr;
    uint32_t count = 0;

    if (selector[0] == '\0') {
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

    const bool looks_like_path =
        n >= 4 && _stricmp(selector + n - 4, ".dll") == 0;
    const bool has_separator =
        strchr(selector, '\\') != nullptr || strchr(selector, '/') != nullptr;

    if (looks_like_path || has_separator) {
        strncpy(dev->dll_path, selector, sizeof(dev->dll_path) - 1);
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
        if (_stricmp(devices[i].vendor_name, selector) == 0) {
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
    qttune_status_t st = qttune_j2534_connect_channel(t->handle,
                                                      t->baud, t->flags,
                                                      &channel);
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
    unsigned long baud = 0;
    unsigned long flags = 0;
    qttune_status_t st = resolve_uri(uri, &dev, &baud, &flags);
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
        impl->baud = baud;
        impl->flags = flags;
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
