/* File/layer: tests/test_c_abi.c | strict C compilation of public headers
 * Testing: UNIT: compile-only guarantee that the C ABI headers are C,
 *          not C++-in-disguise. Without this, `static_assert`
 *          spellings, name mangling surprises, or stray C++ keywords
 *          would only surface at a third-party FFI consumer's build.
 *
 * Built as a real executable linking ecuio-core from a C translation
 * unit: proves both that the headers parse under C and that exported
 * symbols resolve unmangled. Every public header is included, the ones
 * that include others first, so a header missing an include of its own
 * can't hide behind an earlier one. internal.h, j2534_ptypes.h and
 * surrogate_protocol.h aren't public, but they declare C (core's
 * transport seam, the vendor types, the surrogate's wire), so they're
 * compiled here too and can't take a C++-only construct unnoticed;
 * j2534_ptypes.h comes in through j2534_surrogate.h.
 *
 * c_abi_shared builds this file again against the shared core
 * (HAVNE_SHARED_CORE, ADR-0053), which exports the public headers'
 * functions and nothing else: there each call resolves through the
 * library's export table, and the calls it never exports (internal.h's
 * test hooks, the surrogate client's log path) are left out
 * (ECUIO_C_ABI_SHARED, INTERNAL()).
 *
 * The calls themselves live in tests/test_c_abi_*.c, one strict-C part
 * each (test_c_abi_parts.h lists them); the include list below is the
 * standalone-header check, so a new public header goes in it, and its
 * calls in the part that matches.
 */

#include "ecuio/conditions.h"
#include "ecuio/queue.h"
#include "ecuio/obdb.h"
#include "ecuio/recording.h"
#include "ecuio/internal.h"
#include "ecuio/surrogate_protocol.h"
#include "ecuio/j2534_surrogate.h"
#include "ecuio/j2534.h"
#include "ecuio/stream.h"
#include "ecuio/serial.h"
#include "ecuio/plugin.h"
#include "ecuio/vin.h"
#include "ecuio/sim.h"
#include "ecuio/socketcan.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "ecuio/core.h"
#include "ecuio/version.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "test_c_abi_parts.h"

int main(void)
{
    /* Exercise exported symbols from each header: catches mangling
     * regressions at link time. */
    printf("%s %s\n", ecuio_version_string(),
           ecuio_status_string(ECUIO_ERR_SESSION_CLOSED));
    if (c_abi_conditions() != 0) {
        return 1;
    }
    if (c_abi_core() != 0) {
        return 1;
    }
    if (c_abi_signalfile() != 0) {
        return 1;
    }
    if (c_abi_plugin() != 0) {
        return 1;
    }
    if (c_abi_diag() != 0) {
        return 1;
    }
    if (c_abi_recording() != 0) {
        return 1;
    }
    if (c_abi_exchange() != 0) {
        return 1;
    }
    if (c_abi_frames() != 0) {
        return 1;
    }
    printf("log '%s'\n", INTERNAL(ecuio_j2534_sur_log_path(NULL), ""));
    return 0;
}
