// File/layer: core/src/core_awake.cpp | C++17, Qt-free; the session's
//             keep-awake (ADR-0033's appendix "the host kept awake while a
//             session reads")
// Testing: UNIT: CoreTest.*Awake* (test_core), Elm327AwakeTest
//          (test_elm327); c_abi
//
// Two claims on one session's hold: the caller's and core's own through a
// write step. Each takes a hold of its own level, so a caller's 0 never
// drops a write's. Only a live transport holds. A take runs outside the
// mutex and publishes after, so a release and the close never wait on it.

#include "ecuio/core.h"
#include "error_detail.h"
#include "host_awake.h"
#include "session.h"

#include <cstddef>
#include <mutex>

namespace {

constexpr char kReadWhy[] = "Reading a vehicle";
constexpr char kWriteWhy[] = "Writing to a vehicle's module";

/* A reason of ECUIO_AWAKE_WHY_MAX bytes or more, read no further */
bool too_long(const char* why)
{
    if (why == nullptr) {
        return false;
    }
    size_t n = 0;
    while (n < ECUIO_AWAKE_WHY_MAX && why[n] != 0) {
        ++n;
    }
    return n >= ECUIO_AWAKE_WHY_MAX;
}

/* Under the state's mutex */
void show(ecuio_awake_state& a)
{
    const bool held = a.caller.held != nullptr || a.core.held != nullptr;
    a.shown.store(held ? ECUIO_AWAKE_SYSTEM : 0u);
}

/* Under the state's mutex: what the claim held, for a release after */
host_awake::hold* end_claim(ecuio_awake_state& a, host_awake::claim& c)
{
    host_awake::hold* const h = c.held;
    c.held = nullptr;
    ++c.ends;
    show(a);
    return h;
}

/* Takes outside the mutex, then publishes unless a release, the close or
 * another take came first; a hold that lost is released at once */
ecuio_status_t take_claim(ecuio_session_t* s, host_awake::claim& c,
                          host_awake::level lv, const char* why)
{
    ecuio_awake_state& a = s->awake;
    uint64_t ends = 0;
    {
        std::lock_guard<std::mutex> lock(a.mutex);
        if (s->closed.load()) {
            return ECUIO_ERR_SESSION_CLOSED;
        }
        if (!host_awake::live_backend(s->info.backend) || c.held != nullptr) {
            return ECUIO_OK;
        }
        ends = c.ends;
    }
    host_awake::hold* got = nullptr;
    const ecuio_status_t st = host_awake::take(lv, why, &got);
    if (st != ECUIO_OK) {
        return st;
    }
    bool closed = false;
    {
        std::lock_guard<std::mutex> lock(a.mutex);
        /* Under the mutex: a close after this check releases what this
         * publishes; one before it is seen here */
        closed = s->closed.load();
        if (!closed && c.ends == ends && c.held == nullptr) {
            c.held = got;
            got = nullptr;
            show(a);
        }
    }
    host_awake::release(got);
    return closed ? ECUIO_ERR_SESSION_CLOSED : ECUIO_OK;
}

/* Never waits on a take: the claim's end is under the mutex alone */
void release_claim(ecuio_awake_state& a, host_awake::claim& c)
{
    host_awake::hold* h = nullptr;
    {
        std::lock_guard<std::mutex> lock(a.mutex);
        h = end_claim(a, c);
    }
    host_awake::release(h);
}

} // namespace

bool host_awake::live_backend(uint32_t backend)
{
    /* The mock and a replay are desk tools and hold nothing */
    return backend == ECUIO_BACKEND_ELM327 ||
           backend == ECUIO_BACKEND_J2534_SURROGATE ||
           backend == ECUIO_BACKEND_SOCKETCAN;
}

extern "C" {

ecuio_status_t ecuio_session_keep_awake(ecuio_session_t* session,
                                        uint32_t what, const char* why)
{
    ecuio_detail_clear();
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if ((what & ~ECUIO_AWAKE_SYSTEM) != 0 || too_long(why)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_awake_state& a = session->awake;
    if (what != 0) {
        return take_claim(session, a.caller, host_awake::level::idle,
                          why != nullptr ? why : kReadWhy);
    }
    {
        std::lock_guard<std::mutex> lock(a.mutex);
        if (session->closed.load()) {
            return ECUIO_ERR_SESSION_CLOSED;
        }
    }
    release_claim(a, a.caller);
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_awake(const ecuio_session_t* session,
                                       uint32_t* out_what)
{
    if (session == nullptr || out_what == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out_what = session->awake.shown.load();
    return ECUIO_OK;
}

} // extern "C"

ecuio_status_t ecuio_awake_core_hold(ecuio_session_t* session, bool on)
{
    ecuio_awake_state& a = session->awake;
    if (!on) {
        release_claim(a, a.core);
        return ECUIO_OK;
    }
    return take_claim(session, a.core, host_awake::level::sleep, kWriteWhy);
}

void ecuio_awake_release_all(ecuio_session_t* session)
{
    ecuio_awake_state& a = session->awake;
    host_awake::hold* caller = nullptr;
    host_awake::hold* core = nullptr;
    {
        std::lock_guard<std::mutex> lock(a.mutex);
        caller = end_claim(a, a.caller);
        core = end_claim(a, a.core);
    }
    host_awake::release(caller);
    host_awake::release(core);
}
