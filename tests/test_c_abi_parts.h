/* File/layer: tests/test_c_abi_parts.h | strict C part of test_c_abi
 * Testing: UNIT: compile and link, run as c_abi and c_abi_shared
 *
 * Each part is one translation unit of test_c_abi; a part returns 0
 * when its checks hold, else 1 (main stops at the first failure).
 */

#ifndef TEST_C_ABI_PARTS_H
#define TEST_C_ABI_PARTS_H

/* An internal hook's call, or the value it gives in the archive's build
 * where the shared core has no such export */
#ifdef ECUIO_C_ABI_SHARED
#define INTERNAL(call, value) (value)
#else
#define INTERNAL(call, value) (call)
#endif

int c_abi_conditions(void);

#endif
