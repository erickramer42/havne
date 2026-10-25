// File/layer: core/src/error_detail.h | C++17, Qt-free, core-internal
// Testing: CoreTest.CreateFailuresNameTheirCause,
//          .LastErrorDetailIsPerThread; J2534ConnectTest on both backends
//
// The text behind ecuio_last_error_detail (core.h): why the last
// ecuio_session_create or ecuio_session_start on this thread failed.
// Both clear it on entry. The layer that knows the cause sets it once,
// where the failure happens; the layers above pass the status up and
// leave the text alone.

#ifndef ECUIO_ERROR_DETAIL_H
#define ECUIO_ERROR_DETAIL_H

void ecuio_detail_clear();

/* printf-style; replaces this thread's detail, cut at 511 bytes. */
void ecuio_detail_set(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* A refused vendor call: "<call> returned status 8 (0x8,
 * ERR_DEVICE_NOT_CONNECTED): <vendor text>". The status in decimal and
 * in hex, as vendors list their own codes, then named when J2534-1
 * v04.04 defines it and called vendor-specific otherwise; the
 * vendor's own description (PassThruGetLastError) follows when it gave
 * one. */
void ecuio_detail_set_vendor(const char* call, long status,
                             const char* vendor_text);

/* A vendor's text as one line, in place: control characters become
 * spaces and trailing spaces go. The detail above and the session
 * info's version strings both pass through it. */
void ecuio_text_one_line(char* text);

#endif /* ECUIO_ERROR_DETAIL_H */
