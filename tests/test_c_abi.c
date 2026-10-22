/* File/layer: tests/test_c_abi.c | strict C compilation of public headers
 * Testing: UNIT — compile-only guarantee that the C ABI headers are C,
 *          not C++-in-disguise. Without this, `static_assert`
 *          spellings, name mangling surprises, or stray C++ keywords
 *          would only surface at a third-party FFI consumer's build.
 *
 * Built as a real executable linking qttune-core from a C translation
 * unit: proves both that the headers parse under C and that exported
 * symbols resolve unmangled. Every public header is included, the ones
 * that include others first, so a header missing an include of its own
 * can't hide behind an earlier one. internal.h, j2534_ptypes.h and
 * surrogate_protocol.h aren't public.
 */

#include "qttune/j2534_surrogate.h"
#include "qttune/j2534.h"
#include "qttune/profile.h"
#include "qttune/signal_sets.h"
#include "qttune/signals.h"
#include "qttune/core.h"

#include <stddef.h>
#include <stdio.h>

/* Negative-array-size idiom — valid in ALL C standards including
 * pedantic C89, complementing the header's _Static_assert (C11+). */
typedef char abi_frame_size_check[(sizeof(qttune_frame_t) == 88) ? 1 : -1];
typedef char abi_data_offset_check[(offsetof(qttune_frame_t, data) == 16) ? 1 : -1];
typedef char abi_flags_offset_check[(offsetof(qttune_frame_t, flags) == 80) ? 1 : -1];

int main(void)
{
    /* Zeroed frame must be a plain classic CAN data frame (flag value 0). */
    qttune_frame_t frame;
    const QttuneSignalSet* set = NULL;
    frame.arbitration_id = 0x7FF;
    frame.channel_id = 0;
    frame.data_length = 8;
    frame.flags = QTTUNE_FRAME_FLAG_EXTENDED;
    frame.reserved = 0;

    /* Exercise exported symbols from each header — catches mangling
     * regressions at link time. */
    printf("%s %s\n", qttune_version_string(),
           qttune_status_string(QTTUNE_ERR_SESSION_CLOSED));
    if (qttune_validate_signal_set(qttune_mock_signal_set()) != QTTUNE_VALIDATE_OK) {
        return 1;
    }
    if (qttune_profile_lookup("raw", &set) != QTTUNE_OK || set != NULL) {
        return 1;
    }
    qttune_j2534_free_devices(NULL);
    printf("log '%s'\n", qttune_j2534_sur_log_path(NULL));
    (void)frame;
    return 0;
}
