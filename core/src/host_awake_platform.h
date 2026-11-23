// File/layer: core/src/host_awake_platform.h | internal, C++17, Qt-free
// Testing: HARNESS: Elm327AwakeTest's platform cases (test_elm327)
//
// The platform's half of host_awake.h: what a hold keeps, and the take
// and release each platform's source defines (host_awake.cpp for
// Windows, macOS and the rest; host_awake_linux.cpp for Linux).

#ifndef ECUIO_HOST_AWAKE_PLATFORM_H
#define ECUIO_HOST_AWAKE_PLATFORM_H

#include "host_awake.h"

namespace host_awake {

struct platform_hold {
    void* request = nullptr;  /* Windows: the power request's HANDLE */
    uint32_t assertion = 0;   /* macOS: the IOPMAssertionID */
    int lock_fd = -1;         /* Linux: logind's inhibitor descriptor */
    void* desktop_bus = nullptr; /* Linux: the session bus holding the
                                    desktop's inhibits */
};

/* The detail is set on a refusal (ECUIO_ERR_NOT_IMPLEMENTED) */
ecuio_status_t platform_take(level lv, const char* why, platform_hold* out);
void platform_release(platform_hold* h);

/* Linux's rule, built everywhere for its unit case: a read holds with
 * logind's lock or the desktop's inhibit, a write only with logind's
 * (sleep:idle), since the desktop's hold off its idle timer alone. A
 * refusal sets the detail, `refusal` the first refusal's words. */
ecuio_status_t judge_take(level lv, bool logind, bool desktop,
                          const char* refusal);

} // namespace host_awake

#endif /* ECUIO_HOST_AWAKE_PLATFORM_H */
