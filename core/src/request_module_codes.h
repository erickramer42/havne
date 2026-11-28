#ifndef ECUIO_REQUEST_MODULE_CODES_H
#define ECUIO_REQUEST_MODULE_CODES_H

#include "request_engine.h"
#include "session.h"

#include <cstdint>
#include <mutex>

inline void ecuio_take_codes_requests(ecuio_session_t* session,
                                      ecuio_request_engine* e)
{
    std::lock_guard<std::mutex> lock(session->codes.mutex);
    for (uint32_t c = 0; c < e->chan_count && c < ECUIO_MAX_CHANNELS; ++c) {
        if (session->codes.asked[c] != session->codes.taken[c]) {
            session->codes.taken[c] = session->codes.asked[c];
            ecuio_engine_read_codes(e, c, session->codes.mask[c],
                                    session->codes.asked[c]);
            ecuio_engine_visit_want(e, c);
        }
    }
}

inline void ecuio_publish_module_codes(ecuio_session_t* session,
                                       const ecuio_request_engine& e,
                                       uint32_t c, uint32_t rx_id,
                                       uint32_t& published)
{
    const ecuio_module_codes_t& r = e.chans[c].codes.report;
    const uint32_t key = r.generation + r.read_id;
    if (key == published) {
        return;
    }
    published = key;
    ecuio_module_codes_t s = r;
    s.rx_id = rx_id;
    std::lock_guard<std::mutex> lock(session->support_mutex);
    if (c < session->codes.reports.size()) {
        session->codes.reports[c] = s;
    }
}

#endif
