#ifndef ECUIO_OPERATION_FLASH_START_H
#define ECUIO_OPERATION_FLASH_START_H

#include "ecuio/core.h"
#include "operation_plan.h"

#include <cstdint>
#include <string>
#include <vector>

struct ecuio_session;

namespace ecuio_operation {

struct session_run;

/* Whether the transport's writes carry a flash's longest request: an
 * erase's 31 01 <routine> <ALFID> <address> <size> */
ecuio_status_t flash_fits(const plan& p, uint32_t write_bound);

/* The copy name in folder, its ranges the images', its bytes end to end */
ecuio_status_t flash_current(const std::string& folder, const char* name,
                             const plan& p, std::vector<uint8_t>* out);

/* The journal refused for a module that may be mid-flash, then written
 * "ready" for this run */
ecuio_status_t flash_journal(const ecuio_session* s, session_run& o,
                             const plan& p, const std::string& folder,
                             const char* copy, uint32_t* sectors,
                             uint64_t* bytes);

} // namespace ecuio_operation

#endif /* ECUIO_OPERATION_FLASH_START_H */
