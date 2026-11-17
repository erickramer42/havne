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
