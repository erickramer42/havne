// File/layer: fuzz/fuzz_uds_query.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ — arbitrary ?did= values through the query split and the
//          uds-generic builder: the status is one of two, the detail is a
//          terminated line, a success builds a set the validator accepts
//          whose polled head fits the engine's table, two calls agree

#include <ecuio/core.h>
#include <ecuio/profile.h>
#include <ecuio/signals.h>

#include "uds_generic.h"      /* core/src: the builder */
#include "uri_query.h"        /* core/src: the split */
#include "request_engine.h"   /* core/src: derive, the limits */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    /* Longer than any URI the app builds; the limits sit well inside. */
    if (size > 8192) return 0;

    /* The input is the query after "?did=": '&' and '=' in it make more
     * keys, did= among them or not, as a typed URI could. A NUL would end
     * the C string early, so it becomes a letter. */
    std::string query = "did=";
    for (size_t i = 0; i < size; ++i) {
        query.push_back(data[i] == 0 ? 'x' : static_cast<char>(data[i]));
    }

    ecuio_uri_query split;
    if (ecuio_uri::split(query.c_str(), &split) != ecuio_uri::split_result::ok) {
        return 0;   /* create refuses it before the builder sees it */
    }

    static ecuio_uds_session a;
    static ecuio_uds_session b;
    const ecuio_status_t rc1 = ecuio_uds_build(split, &a);
    const ecuio_status_t rc2 = ecuio_uds_build(split, &b);
    if (rc1 != rc2) abort();
    if (rc1 != ECUIO_OK && rc1 != ECUIO_ERR_BAD_URI) abort();

    const char* detail = ecuio_last_error_detail();
    if (detail == nullptr || strlen(detail) >= 512) abort();
    if (rc1 == ECUIO_ERR_BAD_URI && detail[0] == '\0') abort();

    if (rc1 != ECUIO_OK) return 0;

    /* What create hands the transport and the bridge */
    if (a.set.count != b.set.count || a.polled != b.polled ||
        a.scan.size() != b.scan.size() ||
        a.channels.size() != b.channels.size()) abort();
    if (a.set.count != a.defs.size() || a.polled > a.set.count) abort();
    if (a.set.count - a.polled != a.scan.size()) abort();
    if (a.polled > ECUIO_PROFILE_MAX_REQUESTS) abort();
    if (a.scan.size() > ECUIO_ENGINE_SCAN_MAX) abort();
    if (a.channels.size() == 0 || a.channels.size() > ECUIO_MAX_CHANNELS) abort();
    if (ecuio_validate_signal_set(&a.set, nullptr) != ECUIO_VALIDATE_OK) abort();

    /* Every def asks for a DID, and the polled head fits the table */
    for (uint32_t i = 0; i < a.set.count; ++i) {
        uint8_t req[3] = {};
        uint8_t len = 0;
        if (!ecuio_engine_request_for_def(&a.set.defs[i], req, &len)) abort();
        if (len != 3 || req[0] != 0x22) abort();
    }
    uint32_t rx[ECUIO_MAX_CHANNELS] = {};
    for (size_t i = 0; i < a.channels.size(); ++i) {
        rx[i] = a.channels[i].rx_id;
    }
    auto e = std::make_unique<ecuio_request_engine>();
    ecuio_engine_init(e.get(), static_cast<uint32_t>(a.channels.size()));
    const ecuio_signal_set_t head = ecuio_uds_polled_set(a);
    if (!ecuio_engine_derive(e.get(), &head, rx,
                             static_cast<uint32_t>(a.channels.size()), nullptr)) abort();
    if (e->count != a.polled) abort();   /* polled DIDs are distinct */
    for (const ecuio_uds_scan_target& s : a.scan) {
        uint32_t c = 0;
        for (uint32_t i = 0; i < a.channels.size(); ++i) {
            if (rx[i] == s.rx_id) c = i;
        }
        if (!ecuio_engine_scan_add(e.get(), c, s.did)) abort();
    }
    if (e->scan_count != a.scan.size()) abort();   /* scanned DIDs are distinct */
    return 0;
}
