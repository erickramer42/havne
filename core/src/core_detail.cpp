// File/layer: core/src/core_detail.cpp | C++17, Qt-free, CI-validated headless build
// Testing: UNIT: CoreTest (test_core): ecuio_last_error_detail's text, per thread

#include "ecuio/core.h"
#include "error_detail.h"
#include "ecuio/j2534_ptypes.h"   /* ecuio_pt_status_name; plain C, any OS */
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
    /* ecuio_last_error_detail's text (error_detail.h), per thread. */
    thread_local char t_detail[ecuio_detail_max + 1] = "";
} // namespace

void ecuio_detail_clear()
{
    t_detail[0] = '\0';
}

void ecuio_detail_set(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t_detail, sizeof(t_detail), fmt, ap);
    va_end(ap);
}

void ecuio_text_one_line(char* text)
{
    size_t n = 0;
    for (; text[n] != '\0'; ++n) {
        if (static_cast<unsigned char>(text[n]) < 0x20) {
            text[n] = ' ';
        }
    }
    while (n > 0 && text[n - 1] == ' ') {
        --n;
    }
    text[n] = '\0';
}

void ecuio_detail_set_vendor(const char* call, long status,
                             const char* vendor_text)
{
    /* The vendor's text on one line: it lands in a single UI line and a
     * log line. */
    char text[2 * ECUIO_PT_TEXT_LEN] = "";
    if (vendor_text != nullptr) {
        size_t n = 0;
        for (; vendor_text[n] != '\0' && n + 1 < sizeof(text); ++n) {
            text[n] = vendor_text[n];
        }
        text[n] = '\0';
        ecuio_text_one_line(text);
    }
    const char* name = ecuio_pt_status_name(status);
    /* A vendor status with no text isn't actionable; the usual causes are
     * no adapter or another program holding it (the VX answers 34 to both:
     * docs/bench-sessions/2026-11-02-vehicle.md). The vendor's text goes
     * first below. */
    const bool open_hint = name == nullptr && strcmp(call, "PassThruOpen") == 0;
    if (name == nullptr) {
        name = "vendor-specific";
    }
    if (text[0] != '\0') {
        ecuio_detail_set("%s returned status %ld (0x%lX, %s): %s", call, status,
                         static_cast<unsigned long>(status), name, text);
    } else if (open_hint) {
        ecuio_detail_set("%s returned status %ld (0x%lX, %s): is the adapter "
                         "attached, and not open in another program?",
                         call, status, static_cast<unsigned long>(status),
                         name);
    } else {
        ecuio_detail_set("%s returned status %ld (0x%lX, %s)", call, status,
                         static_cast<unsigned long>(status), name);
    }
}

extern "C" {

const char* ecuio_last_error_detail(void)
{
    return t_detail;
}

} // extern "C"
