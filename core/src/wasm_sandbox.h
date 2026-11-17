// File/layer: core/src/wasm_sandbox.h | internal, C++17, Qt-free
// Testing: UNIT: SandboxTest (test_sandbox); fuzz_wasm_module
//
// The sandbox a conversation plugin's code runs in (ADR-0066 sections
// 2.1 and 2.2): one instance of a WebAssembly module in WAMR's classic
// interpreter, with nothing reachable but its own memory.
//
// - A module is judged by wasm_check.h, then by the runtime, when the
//   instance is made, on the caller's thread: a refusal or a lack of
//   memory is an answer there, never on a worker.
// - A call runs one export under an instruction budget. It allocates
//   nothing: the memory is a fixed size and the stacks were made with
//   the instance.
// - A call that doesn't return (the budget ran out, a trap) ends the
//   instance: every later call answers `dead` without running anything.
//   WAMR's state after a stopped call is not one to call into again.
// - Each instance has a module of its own, so two instances share no
//   runtime state and may be called from two threads at once. One
//   instance is called from one thread at a time.
// - Everything the runtime takes, a module's memory included, comes
//   through a counting allocator: each instance's making is bounded, and
//   all of them together.

#ifndef ECUIO_WASM_SANDBOX_H
#define ECUIO_WASM_SANDBOX_H

#include "ecuio/core.h"
#include "wasm_check.h"

#include <cstddef>
#include <cstdint>

namespace ecuio_wasm {

/* Instructions one call may run unless the caller asks for fewer
 * (ADR-0066 section 2.2): room for a key computation many times over,
 * and well under a tenth of a second on a slow phone */
constexpr uint32_t BUDGET_DEFAULT = 1000000;
/* A module's operand and call stack, made with the instance */
constexpr uint32_t STACK_BYTES = 64 * 1024;
/* What making one instance may take from the runtime's allocator, and
 * what every instance alive may hold together */
constexpr size_t INSTANCE_BYTES_MAX = 8u << 20;
constexpr size_t RUNTIME_BYTES_MAX = 64u << 20;
/* Arguments a call passes */
constexpr uint32_t ARGS_MAX = 4;

enum class call_end : uint8_t {
    returned,    /* value holds the result, 0 for a function with none */
    budget,      /* the instruction budget ran out: the instance is dead */
    trapped,     /* a trap (out of bounds, unreachable, a stack overflow):
                    the instance is dead */
    dead,        /* an earlier call ended the instance: nothing ran */
    no_function, /* no such export index: nothing ran */
};

struct call_result {
    call_end end = call_end::dead;
    uint32_t value = 0;
};

class instance;

/* Makes an instance of the module in bytes (copied): judged by
 * check_module with wants, loaded, instantiated. OK with *out set;
 * ECUIO_ERR_LOAD_FAILED with why (REFUSAL_MAX bytes) saying what the
 * check or the runtime refused; ECUIO_ERR_OUT_OF_MEMORY when its making
 * passed INSTANCE_BYTES_MAX or the runtime RUNTIME_BYTES_MAX, or the
 * process had none; ECUIO_ERR_INVALID_ARGUMENT for a wanted type a call
 * can't carry (i32 parameters, at most ARGS_MAX, and no result or one
 * i32), whatever the module. Nothing is thrown. Any thread, one maker at
 * a time inside. */
ecuio_status_t make_instance(const uint8_t* bytes, size_t n,
                             const wanted_export* wants, size_t want_count,
                             instance** out, char* why) noexcept;

/* Ends an instance (NULL is fine). Any thread, never during a call. */
void free_instance(instance* i) noexcept;

/* Calls wanted export `index` (its place in make_instance's wants) with
 * arg_count i32 arguments (at most ARGS_MAX) under `budget` instructions
 * (at least 1). Allocates nothing. */
call_result call_instance(instance* i, size_t index, const uint32_t* args,
                          uint32_t arg_count, uint32_t budget) noexcept;

/* The instance's memory from offset for len bytes, or NULL when that
 * range isn't all inside it or the instance is dead. The pointer holds
 * until the next call into the instance. */
uint8_t* instance_memory(instance* i, uint32_t offset, uint32_t len) noexcept;

/* Whether a call has ended the instance, and the runtime's words for
 * what ended it ("" while alive) */
bool instance_dead(const instance* i) noexcept;
const char* instance_trap(const instance* i) noexcept;

/* Bytes the runtime holds now, and how many times it has allocated: a
 * test's check that instances free what they took and calls take none */
size_t runtime_bytes_in_use() noexcept;
uint64_t runtime_allocations() noexcept;

} // namespace ecuio_wasm

#endif /* ECUIO_WASM_SANDBOX_H */
