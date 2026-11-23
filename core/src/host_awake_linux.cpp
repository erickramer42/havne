// File/layer: core/src/host_awake_linux.cpp | C++17, Qt-free; Linux only
//             (not Android)
// Testing: UNIT: CoreTest.AWriteHoldNeedsLogindsSleepLock (judge_take);
//          HARNESS: Elm327AwakeTest.LogindListsTheHoldUntilTheClose
//          (skipped without logind), .AWedgedBusRefusesInsideTheSecond;
//          BENCH: BENCH prep #39 (a desktop's own idle suspend)
//
// Linux holds the host awake through logind and the desktop both, since a
// desktop's power manager may suspend on its own idle timer without asking
// logind (Xfce's does). logind's lock is "idle" for a read and "sleep:idle"
// for a write step, so a sleep the user asks for still sleeps a read; a
// write holds only with logind's lock (judge_take). The desktop's inhibits
// are org.freedesktop.PowerManagement's (Xfce, KDE) and
// org.gnome.SessionManager's with flag 4, suspend (GNOME's power daemon
// suspends on idle through it; the screen keeps its own timer), on the
// session bus. Their cookies are never passed back: closing that
// connection ends both. libsystemd's sd-bus is loaded when first asked,
// so core has no build dependency. Every step shares one second: an open
// connects without waiting, and its authentication is waited out inside
// the second (ready), since sd_bus_call waits for it by a far longer bound
// of its own.

#include "host_awake_platform.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include "error_detail.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

/* sd-bus's public layout (sd-bus.h): name, message, an internal flag */
struct bus_error {
    const char* name;
    const char* message;
    int need_free;
};

using open_bus_t = int (*)(void** bus);
using new_call_t = int (*)(void* bus, void** m, const char* dest,
                           const char* path, const char* iface,
                           const char* member);
using append_t = int (*)(void* m, const char* types, ...);
using call_t = int (*)(void* bus, void* m, uint64_t usec, bus_error* error,
                       void** reply);
using read_t = int (*)(void* m, const char* types, ...);
using unref_t = void* (*)(void* p);
using close_t = void (*)(void* bus);
using error_free_t = void (*)(bus_error* error);
using is_ready_t = int (*)(void* bus);
using process_t = int (*)(void* bus, void** m);
using wait_t = int (*)(void* bus, uint64_t usec);

struct sd_bus_api {
    bool loaded = false;
    open_bus_t open_system = nullptr;
    open_bus_t open_user = nullptr;
    new_call_t new_call = nullptr;
    append_t append = nullptr;
    call_t call = nullptr;
    read_t read = nullptr;
    unref_t message_unref = nullptr;
    unref_t bus_unref = nullptr;
    close_t bus_close = nullptr;
    error_free_t error_free = nullptr;
    is_ready_t is_ready = nullptr;
    process_t process = nullptr;
    wait_t wait = nullptr;
};

template <typename F>
bool find(void* lib, const char* name, F* out)
{
    *out = reinterpret_cast<F>(dlsym(lib, name));
    return *out != nullptr;
}

/* Kept for the process: unloading would leave a hold's bus dangling */
const sd_bus_api& api()
{
    static sd_bus_api a;
    static std::once_flag once;
    std::call_once(once, [] {
        void* lib = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr) {
            return;
        }
        a.loaded = find(lib, "sd_bus_open_system", &a.open_system) &&
                   find(lib, "sd_bus_open_user", &a.open_user) &&
                   find(lib, "sd_bus_message_new_method_call", &a.new_call) &&
                   find(lib, "sd_bus_message_append", &a.append) &&
                   find(lib, "sd_bus_call", &a.call) &&
                   find(lib, "sd_bus_message_read", &a.read) &&
                   find(lib, "sd_bus_message_unref", &a.message_unref) &&
                   find(lib, "sd_bus_unref", &a.bus_unref) &&
                   find(lib, "sd_bus_close", &a.bus_close) &&
                   find(lib, "sd_bus_error_free", &a.error_free) &&
                   find(lib, "sd_bus_is_ready", &a.is_ready) &&
                   find(lib, "sd_bus_process", &a.process) &&
                   find(lib, "sd_bus_wait", &a.wait);
    });
    return a;
}

using clock_type = std::chrono::steady_clock;

/* What is left of the take's second, in microseconds; 0 once it's spent
 * (sd_bus_call reads 0 as its own 25 s default, so 0 is never passed) */
uint64_t left_us(clock_type::time_point until)
{
    const auto now = clock_type::now();
    if (now >= until) {
        return 0;
    }
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(until - now)
            .count());
}

/* The first refusal's words, kept for the detail if nothing holds */
struct first_error {
    char text[160] = {};
    void note(const char* what, int r, const bus_error& e)
    {
        if (text[0] != '\0') {
            return;
        }
        std::snprintf(text, sizeof text, "%s: %s", what,
                      e.message != nullptr ? e.message : std::strerror(-r));
    }
};

/* 0 once the bus is authenticated and named, inside the second; else a
 * negative errno (-ETIMEDOUT when the second ran out) */
int ready(const sd_bus_api& a, void* bus, clock_type::time_point until)
{
    for (;;) {
        if (a.is_ready(bus) > 0) {
            return 0;
        }
        const uint64_t usec = left_us(until);
        if (usec == 0) {
            return -ETIMEDOUT;
        }
        const int r = a.process(bus, nullptr);
        if (r < 0) {
            return r;
        }
        if (r == 0) {
            const int w = a.wait(bus, usec);
            if (w < 0) {
                return w;
            }
        }
    }
}

/* A bus opened and ready inside the second, or NULL with the refusal
 * noted */
void* open_bus(const sd_bus_api& a, bool system, const char* who,
               clock_type::time_point until, first_error* err)
{
    void* bus = nullptr;
    int r = system ? a.open_system(&bus) : a.open_user(&bus);
    if (r >= 0) {
        r = ready(a, bus, until);
    }
    if (r >= 0) {
        return bus;
    }
    bus_error none{};
    err->note(who, r, none);
    if (bus != nullptr) {
        a.bus_close(bus);
        a.bus_unref(bus);
    }
    return nullptr;
}

/* logind's lock: a descriptor, duplicated before the reply is freed */
int logind_lock(const sd_bus_api& a, host_awake::level lv, const char* why,
                clock_type::time_point until, first_error* err)
{
    void* bus = open_bus(a, true, "logind", until, err);
    if (bus == nullptr) {
        return -1;
    }
    const char* what = lv == host_awake::level::sleep ? "sleep:idle" : "idle";
    void* m = nullptr;
    void* reply = nullptr;
    bus_error e{};
    int fd = -1;
    int r = a.new_call(bus, &m, "org.freedesktop.login1",
                   "/org/freedesktop/login1",
                   "org.freedesktop.login1.Manager", "Inhibit");
    if (r >= 0) {
        r = a.append(m, "ssss", what, program_invocation_short_name, why,
                     "block");
    }
    const uint64_t usec = left_us(until);
    if (r >= 0 && usec > 0) {
        r = a.call(bus, m, usec, &e, &reply);
        int got = -1;
        if (r >= 0 && a.read(reply, "h", &got) >= 0) {
            fd = fcntl(got, F_DUPFD_CLOEXEC, 3);
        }
    }
    if (fd < 0) {
        err->note("logind", r < 0 ? r : -EIO, e);
    }
    a.error_free(&e);
    a.message_unref(reply);
    a.message_unref(m);
    a.bus_close(bus);
    a.bus_unref(bus);
    return fd;
}

/* One desktop inhibit on the session bus; true when it answered */
bool desktop_inhibit(const sd_bus_api& a, void* bus, bool gnome,
                     const char* why, clock_type::time_point until,
                     first_error* err)
{
    void* m = nullptr;
    void* reply = nullptr;
    bus_error e{};
    int r = gnome ? a.new_call(bus, &m, "org.gnome.SessionManager",
                               "/org/gnome/SessionManager",
                               "org.gnome.SessionManager", "Inhibit")
                  : a.new_call(bus, &m, "org.freedesktop.PowerManagement",
                               "/org/freedesktop/PowerManagement/Inhibit",
                               "org.freedesktop.PowerManagement.Inhibit",
                               "Inhibit");
    if (r >= 0) {
        r = gnome ? a.append(m, "susu", program_invocation_short_name, 0u,
                             why, 4u)
                  : a.append(m, "ss", program_invocation_short_name, why);
    }
    const uint64_t usec = left_us(until);
    bool held = false;
    if (r >= 0 && usec > 0) {
        r = a.call(bus, m, usec, &e, &reply);
        uint32_t cookie = 0;
        held = r >= 0 && a.read(reply, "u", &cookie) >= 0;
    }
    if (!held) {
        err->note(gnome ? "GNOME's session" : "the desktop's power manager",
                  r < 0 ? r : -EIO, e);
    }
    a.error_free(&e);
    a.message_unref(reply);
    a.message_unref(m);
    return held;
}

/* The session bus, kept while either inhibit holds; NULL when neither */
void* desktop_bus(const sd_bus_api& a, const char* why,
                  clock_type::time_point until, first_error* err)
{
    void* bus = open_bus(a, false, "the session bus", until, err);
    if (bus == nullptr) {
        return nullptr;
    }
    const bool power = desktop_inhibit(a, bus, false, why, until, err);
    const bool gnome = desktop_inhibit(a, bus, true, why, until, err);
    if (power || gnome) {
        return bus;
    }
    a.bus_close(bus);
    a.bus_unref(bus);
    return nullptr;
}

} // namespace

namespace host_awake {

ecuio_status_t platform_take(level lv, const char* why, platform_hold* out)
{
    const sd_bus_api& a = api();
    if (!a.loaded) {
        ecuio_detail_set("libsystemd.so.0 isn't installed (or lacks sd-bus's "
                         "calls), so neither logind nor the desktop can be "
                         "asked");
        return ECUIO_ERR_NOT_IMPLEMENTED;
    }
    const auto until = clock_type::now() + std::chrono::seconds(1);
    first_error err;
    out->lock_fd = logind_lock(a, lv, why, until, &err);
    out->desktop_bus = desktop_bus(a, why, until, &err);
    const ecuio_status_t st = judge_take(lv, out->lock_fd >= 0,
                                         out->desktop_bus != nullptr, err.text);
    if (st != ECUIO_OK) {
        platform_release(out);
        *out = platform_hold{};
    }
    return st;
}

void platform_release(platform_hold* h)
{
    if (h->lock_fd >= 0) {
        close(h->lock_fd);
    }
    if (h->desktop_bus != nullptr) {
        const sd_bus_api& a = api();
        a.bus_close(h->desktop_bus);
        a.bus_unref(h->desktop_bus);
    }
}

} // namespace host_awake

#endif
