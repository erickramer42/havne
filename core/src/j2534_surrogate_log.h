// File/layer: core/src/j2534_surrogate_log.h | C++17, Qt-free, core-internal,
//             Win32 only
// Testing: HARNESS: J2534SurrogateTest.EarlierSessionLogIsKept; Cross-arch
//
// The session log's file for one spawn (j2534_surrogate_log.cpp). The
// surrogate client (j2534_surrogate_client.cpp) is its one caller.

#ifndef ECUIO_J2534_SURROGATE_LOG_H
#define ECUIO_J2534_SURROGATE_LOG_H

#ifdef _WIN32

#include <windows.h>

#include <cstddef>

namespace ecuio_sur_log {

/* The log a spawn writes, its earlier one kept and the old ones pruned
 * first (the definition says how) */
HANDLE open_session_log(LONG seq, SECURITY_ATTRIBUTES* sa,
                        char* path, size_t cap);

} // namespace ecuio_sur_log

#endif /* _WIN32 */

#endif /* ECUIO_J2534_SURROGATE_LOG_H */
