// File/layer: core/src/host_awake.cpp | C++17, Qt-free; the hold's handle,
//             the test seam, and Windows', macOS's and the stubs' takes
// Testing: UNIT: Elm327AwakeTest through the seam (test_elm327);
//          HARNESS: Elm327AwakeTest.WindowsTakesARealRequest,
//          .MacOsListsTheAssertion; BENCH: BENCH prep #39
//
// Windows takes a power request, which any thread may clear and which
// carries a reason `powercfg /requests` lists; macOS a user-idle sleep
// assertion. Neither can hold off a sleep the user asks for, so a write
// step's level takes the same. Linux is host_awake_linux.cpp.

#include "host_awake_platform.h"

#include "ecuio/internal.h"
#include "error_detail.h"

#include <atomic>
#include <new>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif
#endif

namespace {

std::atomic<ecuio_test_awake_take_t> g_seam_take{nullptr};
std::atomic<ecuio_test_awake_release_t> g_seam_release{nullptr};

} // namespace

struct host_awake::hold {
    bool from_seam = false;
    ecuio_test_awake_release_t seam_release = nullptr;
    void* seam = nullptr;
    platform_hold platform;
};

namespace host_awake {

ecuio_status_t take(level lv, const char* why, hold** out)
{
    *out = nullptr;
    hold* h = new (std::nothrow) hold;
    if (h == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    const ecuio_test_awake_take_t seam_take = g_seam_take.load();
    ecuio_status_t st = ECUIO_OK;
    if (seam_take != nullptr) {
        const char* words = nullptr;
        st = seam_take(static_cast<uint32_t>(lv), why, &h->seam, &words);
        if (st == ECUIO_OK) {
            h->from_seam = true;
            h->seam_release = g_seam_release.load();
        } else if (words != nullptr) {
            ecuio_detail_set("%s", words);
        }
    } else {
        st = platform_take(lv, why, &h->platform);
    }
    if (st != ECUIO_OK) {
        delete h;
        return st;
    }
    *out = h;
    return ECUIO_OK;
}

void release(hold* h)
{
    if (h == nullptr) {
        return;
    }
    if (h->from_seam) {
        if (h->seam_release != nullptr) {
            h->seam_release(h->seam);
        }
    } else {
        platform_release(&h->platform);
    }
    delete h;
}

ecuio_status_t judge_take(level lv, bool logind, bool desktop,
                          const char* refusal)
{
    if (logind || (desktop && lv == level::idle)) {
        return ECUIO_OK;
    }
    if (desktop) {
        ecuio_detail_set("logind didn't hold the sleep lock a write step "
                         "needs (the desktop holds off only its idle "
                         "timer): %s", refusal);
    } else {
        ecuio_detail_set("neither logind nor the desktop held the host "
                         "awake: %s", refusal);
    }
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

#if defined(_WIN32)

ecuio_status_t platform_take(level, const char* why, platform_hold* out)
{
    wchar_t wide[ECUIO_AWAKE_WHY_MAX];
    if (MultiByteToWideChar(CP_UTF8, 0, why, -1, wide, ECUIO_AWAKE_WHY_MAX) ==
        0) {
        wide[0] = L'\0';
    }
    REASON_CONTEXT reason{};
    reason.Version = POWER_REQUEST_CONTEXT_VERSION;
    reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    reason.Reason.SimpleReasonString = wide;
    HANDLE request = PowerCreateRequest(&reason);
    if (request == INVALID_HANDLE_VALUE) {
        ecuio_detail_set("PowerCreateRequest failed: error %lu",
                         GetLastError());
        return ECUIO_ERR_NOT_IMPLEMENTED;
    }
    if (!PowerSetRequest(request, PowerRequestSystemRequired)) {
        const DWORD error = GetLastError();
        CloseHandle(request);
        ecuio_detail_set("PowerSetRequest failed: error %lu", error);
        return ECUIO_ERR_NOT_IMPLEMENTED;
    }
    out->request = request;
    return ECUIO_OK;
}

void platform_release(platform_hold* h)
{
    PowerClearRequest(h->request, PowerRequestSystemRequired);
    CloseHandle(h->request);
}

#elif defined(__APPLE__) && TARGET_OS_OSX

ecuio_status_t platform_take(level, const char* why, platform_hold* out)
{
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, why,
                                                 kCFStringEncodingUTF8);
    if (name == nullptr) {
        name = CFStringCreateWithCString(kCFAllocatorDefault,
                                         "Reading a vehicle",
                                         kCFStringEncodingUTF8);
    }
    IOPMAssertionID id = kIOPMNullAssertionID;
    const IOReturn r = IOPMAssertionCreateWithName(
        kIOPMAssertionTypePreventUserIdleSystemSleep, kIOPMAssertionLevelOn,
        name, &id);
    if (name != nullptr) {
        CFRelease(name);
    }
    if (r != kIOReturnSuccess) {
        ecuio_detail_set("IOPMAssertionCreateWithName failed: 0x%x",
                         static_cast<unsigned>(r));
        return ECUIO_ERR_NOT_IMPLEMENTED;
    }
    out->assertion = id;
    return ECUIO_OK;
}

void platform_release(platform_hold* h)
{
    IOPMAssertionRelease(h->assertion);
}

#elif defined(__linux__) && !defined(__ANDROID__)

/* host_awake_linux.cpp */

#else

ecuio_status_t platform_take(level, const char*, platform_hold*)
{
#if defined(__ANDROID__)
    ecuio_detail_set("the app holds the phone awake: core has no wake lock");
#elif defined(__APPLE__)
    ecuio_detail_set("iOS has no sleep an app can hold off");
#else
    ecuio_detail_set("this platform has no way to hold the host awake");
#endif
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

void platform_release(platform_hold*)
{
}

#endif

} // namespace host_awake

extern "C" void ecuio_test_set_awake_platform(
    ecuio_test_awake_take_t take, ecuio_test_awake_release_t release)
{
    g_seam_release.store(release);
    g_seam_take.store(take);
}
