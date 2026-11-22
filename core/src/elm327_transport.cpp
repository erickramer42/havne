// File/layer: core/src/elm327_transport.cpp | C++17, Qt-free, every
//             platform (ADR-0026)
// Testing: HARNESS: Elm327SessionTest, Elm327PortTest, Elm327TcpTest,
//          Elm327StreamTest, Elm327TalkTest, Elm327TurnsTest (test_elm327);
//          BENCH
//
// elm327://'s attach (the URI, the channels, the requests, the port,
// the setup), the port's reopening, start and detach, and the C calls.
// The other parts: elm327_setup.cpp (the setup's contract commands,
// the readings), elm327_worker.cpp (the worker, a request's exchange,
// the I/O under every command), elm327_listen.cpp (passive listening),
// elm327_close.cpp (the close's 10 01), elm327_transport.h (what they
// share).

#include "elm327_transport.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "session.h"
#include "elm327_log.h"
#include "elm327_port.h"
#include "elm327_tcp.h"
#include "elm327_stream.h"
#include "elm327_protocol.h"
#include "request_engine.h"
#include "request_setup.h"
#include "uri_query.h"
#include "error_detail.h"
#include "host_clock.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <thread>

/* The query keys attach reads, for create's check (internal.h). */
extern "C" const char* const ecuio_elm327_query_keys[] = {
    "baud",     "serial",   "filter",       "txid",       "poll", "probe",
    "identify", "diagnose", "pack",         "scan",       "gap",  "profile",
    "did",      "session",  "keepalive_ms", "packframes", nullptr};

namespace ecuio_elm327 {

namespace {

/* The opener ecuio_elm327_set_port_opener installed; nullptr is the
 * OS's. */
std::atomic<ecuio_elm327_port_open_t> g_opener{nullptr};

/* ecuio_elm327_set_timeouts_ms's waits; 0 is the constant */
std::atomic<uint32_t> g_prompt_timeout_ms{0};
std::atomic<uint32_t> g_monitor_check_ms{0};

/* ecuio_elm327_test_add_slept's sleeps, added to the clock's count */
std::atomic<uint64_t> g_slept_added_us{0};

} // namespace

uint64_t slept_us()
{
    return ecuio_clock_slept_us() + g_slept_added_us.load();
}

uint32_t prompt_timeout_ms()
{
    const uint32_t ms = g_prompt_timeout_ms.load();
    return (ms != 0) ? ms : ELM327_PROMPT_TIMEOUT_MS;
}

uint32_t monitor_check_ms()
{
    const uint32_t ms = g_monitor_check_ms.load();
    return (ms != 0) ? ms : ELM327_MONITOR_CHECK_MS;
}

wait_words words_of(uint32_t ms)
{
    wait_words w{};
    if (ms % 1000 == 0) {
        snprintf(w.text, sizeof(w.text), "%u s",
                 static_cast<unsigned>(ms / 1000));
    } else {
        snprintf(w.text, sizeof(w.text), "%u ms", static_cast<unsigned>(ms));
    }
    return w;
}

namespace {

/* Opens the port attach named: an app-fed stream through the registry,
 * its wait ended by a close; anything else through the opener. */
ecuio_status_t open_port(transport* t, char* why, uint32_t why_cap)
{
    if (t->stream) {
        return elm327_stream_open(t->port_name, &t->stop_requested, &t->port,
                                  why, why_cap);
    }
    return t->opener(t->port_name, t->serial_baud, &t->port, why, why_cap);
}

} // namespace

/* Closes the port; the worker reopens it on the backoff's schedule. */
void close_port(transport* t)
{
    if (t->port_open) {
        t->port.ops->close(t->port.impl);
        t->port_open = false;
        elm327_log_write(t->log, "%s closed", t->port_name);
    }
    t->configured = false;
    t->rx_len = t->rx_pos = 0;
}

/* Opens the port if it's closed and runs the setup's contract commands:
 * true when the adapter is ready for requests. */
bool reconnect(transport* t)
{
    if (!t->port_open) {
        char why[128];
        if (open_port(t, why, sizeof(why)) != ECUIO_OK) {
            elm327_log_write(t->log, "reopening %s failed: %s", t->port_name,
                             why);
            return false;
        }
        t->port_open = true;
        t->rx_len = t->rx_pos = 0;
        elm327_log_write(t->log, "%s reopened", t->port_name);
    }
    return run_setup(t, nullptr, 0) == ECUIO_OK;
}

/* Backoff-aware sleep, abortable within 20 ms of a stop request.
 * Returns true if stop was requested. */
bool backoff_sleep(transport* t, uint32_t ms)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!t->stop_requested.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        auto chunk = deadline - now;
        if (chunk > std::chrono::milliseconds(20)) {
            chunk = std::chrono::milliseconds(20);
        }
        std::this_thread::sleep_for(chunk);
    }
    return true;
}

namespace {

ecuio_status_t elm327_start(ecuio_session_t* session, void* impl)
{
    (void)session;
    auto* t = static_cast<transport*>(impl);
    if (t->worker.joinable()) {
        return ECUIO_OK; /* internal.h: a second start is OK */
    }
    t->stop_requested.store(false);
    try {
        t->worker = std::thread(elm327_worker, t);
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

/* Joins the worker, then closes the port. The adapter keeps the setup
 * until its next reset; nothing it holds was written (ADR-0026 section 5). */
void elm327_detach(ecuio_session_t* session, void* impl)
{
    (void)session;
    auto* t = static_cast<transport*>(impl);
    if (t->worker.joinable()) {
        t->stop_requested.store(true);
        t->worker.join();
    }
    elm327_log_write(t->log, "the session closes");
    close_port(t);
    delete t;
}

const ecuio_transport_ops_t kElm327Ops = {elm327_start, elm327_detach};

/* The query's values (ecuio_elm327_query_keys), strict as J2534's: a bad
 * value is BAD_URI at create, before the port opens.
 *   baud    the CAN rate: 500000 (the default) or 250000, ISO 15765-4's
 *           two, which the ELM327 has protocols for
 *   serial  the serial port's rate for a wired adapter, default 38400;
 *           a Bluetooth port ignores it, a TCP address refuses it
 *   the request keys (ecuio_request_param lists them), as for j2534://;
 *           without ?gap= the floor is the engine's default
 *   profile, did: core's, read by create */
ecuio_status_t parse_query(const ecuio_uri_query* query, transport* t,
                           request_params* req)
{
    for (size_t i = 0; i < query->count; ++i) {
        const ecuio_uri::param& q = query->params[i];
        unsigned long v = 0;
        const char* wanted = nullptr;
        if (q.is("baud")) {
            if (!ecuio_parse_number(q.val, q.val_len, &v) ||
                (v != 500000 && v != 250000)) {
                wanted = "500000 or 250000";
            } else {
                t->can_baud = v;
            }
        } else if (q.is("serial")) {
            if (!ecuio_parse_number(q.val, q.val_len, &v) ||
                v < ELM327_SERIAL_MIN || v > ELM327_SERIAL_MAX) {
                wanted = "a serial rate from 300 to 4000000";
            } else {
                t->serial_baud = static_cast<uint32_t>(v);
                t->serial_given = true;
            }
        } else {
            (void)ecuio_request_param(q, req, &wanted);
        }
        if (wanted != nullptr) {
            ecuio_detail_bad_value(q, wanted);
            return ECUIO_ERR_BAD_URI;
        }
    }
    return ECUIO_OK;
}

/* The port's name and kind, then the query's values: BAD_URI with its
 * detail before anything opens */
ecuio_status_t read_uri(const char* uri, const ecuio_uri_query* query,
                        transport* t, request_params* req)
{
    /* The port: the text before the query, judged before any value */
    const char* name = uri + strlen("elm327://");
    const char* q = strchr(name, '?');
    const size_t name_len =
        (q != nullptr) ? static_cast<size_t>(q - name) : strlen(name);
    if (name_len == 0) {
        ecuio_detail_set("elm327:// needs a port: %s, or %s",
                         elm327_port_name_example(), elm327_tcp_example());
        return ECUIO_ERR_BAD_URI;
    }
    t->tcp = elm327_tcp_address(name, name_len, nullptr, nullptr);
    t->stream = !t->tcp && elm327_stream_name(name, name_len);
    if (!t->tcp && !t->stream && !elm327_port_name_ok(name, name_len)) {
        ecuio_detail_set("the port must be %s, or %s",
                         elm327_port_name_example(), elm327_tcp_example());
        return ECUIO_ERR_BAD_URI;
    }
    memcpy(t->port_name, name, name_len);
    t->port_name[name_len] = '\0';

    const ecuio_status_t st = parse_query(query, t, req);
    if (st != ECUIO_OK) {
        return st;
    }
    /* A socket has no serial rate; a run sheet that carries one over from
     * a USB session is told so rather than ignored */
    if ((t->tcp || t->stream) && t->serial_given) {
        ecuio_detail_set("?serial= is a serial port's rate, and %s is %s, "
                         "which has none",
                         t->port_name,
                         t->tcp ? "a TCP address" : "an app's stream");
        return ECUIO_ERR_BAD_URI;
    }
    return ECUIO_OK;
}

/* Channels, their addressing mode, and the requests, before the port
 * opens. One mode for the session: AT SP takes one */
ecuio_status_t plan_requests(ecuio_session_t* session,
                             const request_params& req, transport* t)
{
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    t->chan_count = ecuio_resolve_channels(set, req, t->rx_ids, t->tx_ids);
    uint32_t wide = 0;
    for (uint32_t i = 0; i < t->chan_count; ++i) {
        if (ecuio_channel_is_29bit(t->rx_ids[i], t->tx_ids[i])) {
            ++wide;
        }
    }
    if (wide != 0 && wide != t->chan_count) {
        ecuio_detail_set("the channels mix 11-bit and 29-bit IDs, which "
                         "one connection can't carry");
        return ECUIO_ERR_BAD_URI;
    }
    t->ext29 = wide != 0;
    t->protocol = elm327_protocol_for(t->ext29, t->can_baud);
    const ecuio_status_t st = ecuio_build_requests(
        session, set, req, t->rx_ids, t->tx_ids, t->chan_count, &t->engine,
        &t->probe, &t->identify);
    if (st != ECUIO_OK) {
        return st;
    }
    ecuio_attach_timing(t->engine, t->timing, ECUIO_TIMING_CLOCK_HOST);
    elm327_one_frame_pids_for(set, &t->one_frame);
    /* A session that asks nothing listens on its channels (AT MA keeps
     * the receive filter); with none, the whole bus would be raw frames
     * a serial link can't carry, a step of its own */
    if (asks_nothing(t) && t->chan_count == 0) {
        ecuio_detail_set("elm327:// listens on its channels' IDs, and this "
                         "session has none: add ?filter=, or use a profile "
                         "that declares a channel");
        return ECUIO_ERR_NOT_IMPLEMENTED;
    }
    return ECUIO_OK;
}

/* The wire log's first lines: the core, the URI, the port and the CAN
 * link, and what the log holds */
void log_heading(transport* t, const char* uri)
{
    if (!elm327_log_is_open(t->log)) {
        return;
    }
    char text[512];
    elm327_log_text(uri, strlen(uri), text, sizeof(text));
    elm327_log_write(t->log, "Havne ELM327 wire log (ADR-0026), core %s",
                     ecuio_version_string());
    elm327_log_write(t->log, "uri %s", text);
    char link[48];
    if (t->tcp) {
        snprintf(link, sizeof(link), "over TCP");
    } else if (t->stream) {
        snprintf(link, sizeof(link), "(app stream)");
    } else {
        snprintf(link, sizeof(link), "at %u bit/s",
                 static_cast<unsigned>(t->serial_baud));
    }
    elm327_log_write(
        t->log, "%s %s; CAN at %lu bit/s, protocol %c; %s", t->port_name,
        link, t->can_baud, t->protocol,
        t->log.requests
            ? "every request and reply (ECUIO_SUR_LOG_FRAMES=1)"
            : "the setup, the readings and the link's events; the "
              "requests with ECUIO_SUR_LOG_FRAMES=1");
}

/* The port, then the adapter's setup and readings; the port closed again
 * when the setup fails */
ecuio_status_t open_adapter(transport* t, const char* uri, char* version,
                            size_t version_cap)
{
    ecuio_elm327_port_open_t opener = g_opener.load();
    t->opener = (opener != nullptr) ? opener : elm327_port_open_os;
    char why[128] = "";
    ecuio_status_t st = open_port(t, why, sizeof(why));
    if (st != ECUIO_OK) {
        ecuio_detail_set("%s: %s", t->port_name, why);
        return st;
    }
    t->port_open = true;
    elm327_log_open(t->log);
    log_heading(t, uri);
    st = run_setup(t, version, version_cap);
    if (st != ECUIO_OK) {
        close_port(t);
        return st;
    }
    return ECUIO_OK;
}

/* The session takes the transport: the wrapper, the info and modules,
 * the reports' seeds, the epoch; the port closed on a refusal */
ecuio_status_t hand_over(ecuio_session_t* session,
                         std::unique_ptr<transport>& t, const char* version)
{
    auto* wrap = new (std::nothrow) ecuio_transport_t();
    if (wrap == nullptr) {
        close_port(t.get());
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    wrap->ops = &kElm327Ops;
    t->session = session;
    describe_session(t.get(), version, &session->info);
    const ecuio_status_t st =
        ecuio_session_set_modules(session, t->rx_ids, t->tx_ids, t->chan_count);
    if (st != ECUIO_OK) {
        delete wrap;
        close_port(t.get());
        return st;
    }
    ecuio_seed_reports(session, t->engine, t->rx_ids, t->chan_count);
    t->epoch = host_clock::now(); /* as mock: end of attach */
    session->judges_link = true;
    wrap->impl = t.release();
    session->transport = wrap;
    return ECUIO_OK;
}

ecuio_status_t elm327_attach(ecuio_session_t* session, const char* uri,
                             const ecuio_uri_query* query)
{
    if (session->transport != nullptr) {
        return ECUIO_OK; /* internal.h: idempotent */
    }
    std::unique_ptr<transport> t(new (std::nothrow) transport());
    if (t == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    request_params req;
    ecuio_status_t st = read_uri(uri, query, t.get(), &req);
    if (st != ECUIO_OK) {
        return st;
    }
    st = plan_requests(session, req, t.get());
    if (st != ECUIO_OK) {
        return st;
    }
    char version[ELM327_LINE_MAX + 1] = {};
    st = open_adapter(t.get(), uri, version, sizeof(version));
    if (st != ECUIO_OK) {
        return st;
    }
    return hand_over(session, t, version);
}

} // namespace

} // namespace ecuio_elm327

using namespace ecuio_elm327;

extern "C" ecuio_status_t
ecuio_elm327_transport_attach(ecuio_session_t* session, const char* uri,
                              const ecuio_uri_query* query)
{
    return elm327_attach(session, uri, query);
}

extern "C" void ecuio_elm327_set_port_opener(ecuio_elm327_port_open_t open)
{
    g_opener.store(open);
}

extern "C" void ecuio_elm327_set_timeouts_ms(uint32_t prompt_ms,
                                             uint32_t monitor_check_ms)
{
    g_prompt_timeout_ms.store(prompt_ms);
    g_monitor_check_ms.store(monitor_check_ms);
}

extern "C" void ecuio_elm327_test_add_slept(uint64_t us)
{
    g_slept_added_us.fetch_add(us);
}

extern "C" void ecuio_elm327_get_timeouts_ms(uint32_t* prompt_ms,
                                             uint32_t* check_ms)
{
    *prompt_ms = prompt_timeout_ms();
    *check_ms = monitor_check_ms();
}
