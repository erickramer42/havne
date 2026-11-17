/* File/layer: third_party/wamr_port.c | what WAMR's kept sources name in the ones left out
 * Testing: WamrBuildTest (tests/test_wamr_build.cpp); a missing one fails the link
 *
 * Havne's own, compiled with WAMR's objects (wamr.cmake). WAMR's sources
 * call a few functions that live in files Havne does not compile: its
 * sockets and its wasm-c-api layer. Each is given here with no body to
 * speak of, so those files stay out of core. None is reached: a module
 * imports nothing (core refuses one that does), and no host function is
 * ever linked through the wasm-c-api. */

#include <stddef.h>

#include "wamr/core/config.h"

/* Instruction metering bounds a call's time only while each instruction's
 * work is counted. wamr-patches/0001 counts it for the instructions built
 * here; those of the features below do work a module sizes (memory.fill
 * and memory.copy, the table instructions, return_call, try, the GC's
 * arrays) that nothing counts. Turning one on needs its charges first. */
#if WASM_ENABLE_INSTRUCTION_METERING != 0 &&                                   \
    (WASM_ENABLE_BULK_MEMORY != 0 || WASM_ENABLE_REF_TYPES != 0 ||             \
     WASM_ENABLE_TAIL_CALL != 0 || WASM_ENABLE_EXCE_HANDLING != 0 ||           \
     WASM_ENABLE_GC != 0)
#error "wamr-patches/0001 charges nothing for these features' instructions"
#endif

struct wasm_trap_t;

#if defined(_WIN32)
/* win_socket.c: Winsock's start and end, which bh_platform_init calls.
 * WAMR opens no socket here, so it starts no Winsock. */
int init_winsock(void)
{
    return 0;
}

void deinit_winsock(void) {}
#endif

/* wasm_c_api.c: frees the trap a wasm-c-api host function returned
 * (wasm_runtime_invoke_c_api_native). No such function is linked. */
void wasm_trap_delete(struct wasm_trap_t* trap)
{
    (void)trap;
}
