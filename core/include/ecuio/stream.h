// File/layer: core/include/ecuio/stream.h | C ABI, public interface
// Testing: HARNESS: Elm327StreamTest (test_elm327): the fake ELM327
//          behind a registered stream, a pump thread playing the app; UNIT
//          (test_c_abi): the header as strict C and the callbacks' layout

#ifndef ECUIO_STREAM_H
#define ECUIO_STREAM_H

#include "ecuio/core.h"    /* ecuio_status_t */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * App-fed streams (ADR-0028): the app opens a link core can't (classic
 * Bluetooth on Android, Bluetooth LE) and stands behind it as an
 * elm327:// port. Core asks through three callbacks and the app answers
 * with pushes; nothing blocks on either side.
 *
 * A stream is registered by name, "<kind>:<id>": the kind in lower-case
 * letters ("bt" for classic Bluetooth, "ble" for BLE), then one or more
 * printable ASCII characters but '?' and space, which core doesn't
 * interpret (the adapter's address); 127 bytes at most. The whole name
 * names one link, so the app registers one stream per adapter. Once
 * registered,
 * elm327://<name>[?…] connects through it, with every elm327:// key but
 * ?serial= (a stream has no serial rate). An unregistered name is
 * ECUIO_ERR_LOAD_FAILED at connect, "no such port", as a missing COM port
 * is.
 *
 * The open: core calls open, then waits up to 10 s for ecuio_stream_opened
 * or ecuio_stream_failed, on the thread that connects (a session's create)
 * or reconnects (its worker), never the app's. A session closing ends a
 * reconnect's wait. Failed's reason, or "the link didn't open within
 * 10 s", is the connect's detail. An open that doesn't succeed is
 * followed by close.
 *
 * Bytes in: ecuio_stream_received copies into the stream's 64 KB buffer,
 * which the session's reads drain. Bytes past it are dropped and the
 * session's next read sees the link failed (lost ELM327 text can't be
 * resynchronized). Bytes pushed while no session holds the stream, or
 * after close, are discarded.
 *
 * Failure: ecuio_stream_failed ends the link. The session counts it as a
 * hard failure, calls close, and later open again on its backoff, so the
 * app never reconnects on its own.
 */

typedef struct ecuio_stream ecuio_stream_t;   /* opaque */

/* POD. The three calls must return at once: open starts connecting,
 * write queues the bytes (split to the link's size by the app), close
 * starts disconnecting. They run on core's threads, never two at once
 * for one stream, and must never wait on a thread that is calling
 * ecuio_stream_unregister. A write that fails later is a
 * ecuio_stream_failed. */
typedef struct ecuio_stream_callbacks {
    void (*open)(void* user);
    void (*write)(void* user, const uint8_t* data, uint32_t len);
    void (*close)(void* user);
    void*    user;
    uint32_t flags;          /* 0 */
    uint32_t reserved[5];    /* zero; ABI growth space */
} ecuio_stream_callbacks_t;

/* Pointers first, so the offsets differ by width; the size is pinned
 * where pointers are 8 bytes, with no hidden padding */
#if defined(__cplusplus)
static_assert(offsetof(ecuio_stream_callbacks_t, flags) == 4 * sizeof(void*),
              "stream callbacks flags offset is ABI");
static_assert(sizeof(void*) != 8 || sizeof(ecuio_stream_callbacks_t) == 56,
              "ecuio_stream_callbacks_t size is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ecuio_stream_callbacks_t, flags) == 4 * sizeof(void*),
               "stream callbacks flags offset is ABI");
_Static_assert(sizeof(void*) != 8 || sizeof(ecuio_stream_callbacks_t) == 56,
               "ecuio_stream_callbacks_t size is ABI");
#endif

/*
 * Registers a stream under name (copied) with cb (copied) into *out.
 *
 * CONTRACTS:
 * - NULL name, cb or out, or a NULL open, write or close:
 *   ECUIO_ERR_NULL_ARGUMENT
 * - A name that isn't "<kind>:<id>" as above or is longer than 127
 *   bytes, a name already registered, or nonzero flags or reserved:
 *   ECUIO_ERR_INVALID_ARGUMENT
 * - ECUIO_ERR_OUT_OF_MEMORY when the buffer can't be allocated
 */
ecuio_status_t ecuio_stream_register(const char* name,
                                     const ecuio_stream_callbacks_t* cb,
                                     ecuio_stream_t** out);

/*
 * Ends a stream: after ECUIO_OK no callback runs and the handle is gone,
 * so the app stops pushing to it first.
 *
 * CONTRACTS:
 * - NULL: ECUIO_ERR_NULL_ARGUMENT
 * - ECUIO_ERR_INVALID_ARGUMENT while a session holds the stream (its
 *   link open or opening): close the session first
 */
ecuio_status_t ecuio_stream_unregister(ecuio_stream_t* stream);

/* The app's pushes, from any thread, a NULL stream ignored. opened and
 * failed answer the latest open; failed's reason is one line of UTF-8,
 * NULL for "the link failed". */
void ecuio_stream_opened(ecuio_stream_t* stream);
void ecuio_stream_received(ecuio_stream_t* stream,
                           const uint8_t* data, uint32_t len);
void ecuio_stream_failed(ecuio_stream_t* stream, const char* reason);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_STREAM_H */
