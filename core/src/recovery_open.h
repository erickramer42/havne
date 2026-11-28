#ifndef ECUIO_RECOVERY_OPEN_H
#define ECUIO_RECOVERY_OPEN_H

#include "operation_journal.h"

#include <cstdint>

struct ecuio_session;

/* The start's, after the bind and before the worker: each module with an
 * unfinished journal in the session's journal folder is held */
void ecuio_recovery_open(ecuio_session* s) noexcept;

/* What the report says of an unfinished journal, under support_mutex */
void ecuio_recovery_needed(ecuio_session* s, uint32_t channel,
                           const ecuio_operation::journal_record& j) noexcept;

#endif /* ECUIO_RECOVERY_OPEN_H */
