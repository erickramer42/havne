#ifndef ECUIO_PART_WRITTEN_H
#define ECUIO_PART_WRITTEN_H

#include "error_detail.h"
#include "session.h"

#include <cstdint>
#include <mutex>

inline bool ecuio_module_part_written(const ecuio_session* s, uint32_t c)
{
    return c < ECUIO_MAX_CHANNELS && ((s->part_written.load() >> c) & 1u) != 0;
}

/* support_mutex held */
inline ecuio_status_t ecuio_refuse_part_written_held(const ecuio_session* s,
                                                     uint32_t c)
{
    const ecuio_module_recovery_t none{};
    const ecuio_module_recovery_t& r =
        c < ECUIO_MAX_CHANNELS ? s->recovery[c] : none;
    if (r.state == ECUIO_RECOVERY_UNREADABLE) {
        ecuio_detail_set("%s", r.words);
        return ECUIO_ERR_IN_USE;
    }
    ecuio_detail_set("the module's software was left part-written: recover "
                     "it first");
    return ECUIO_ERR_IN_USE;
}

inline ecuio_status_t ecuio_refuse_part_written(const ecuio_session* s,
                                                uint32_t c)
{
    const std::lock_guard<std::mutex> lock(s->support_mutex);
    return ecuio_refuse_part_written_held(s, c);
}

#endif /* ECUIO_PART_WRITTEN_H */
