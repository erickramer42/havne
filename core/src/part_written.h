#ifndef ECUIO_PART_WRITTEN_H
#define ECUIO_PART_WRITTEN_H

#include "error_detail.h"
#include "session.h"

#include <cstdint>

inline bool ecuio_module_part_written(const ecuio_session* s, uint32_t c)
{
    return c < ECUIO_MAX_CHANNELS && ((s->part_written.load() >> c) & 1u) != 0;
}

inline ecuio_status_t ecuio_refuse_part_written()
{
    ecuio_detail_set("the module's software was left part-written; "
                     "reconnect to recover it");
    return ECUIO_ERR_IN_USE;
}

#endif /* ECUIO_PART_WRITTEN_H */
