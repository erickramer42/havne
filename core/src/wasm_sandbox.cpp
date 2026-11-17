// File/layer: core/src/wasm_sandbox.cpp | internal, Qt-free; embeds WAMR (third_party/wamr.cmake)
// Testing: UNIT: SandboxTest (test_sandbox); fuzz_wasm_module

#include "wasm_sandbox.h"

#include "wasm_export.h"

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

namespace ecuio_wasm {

namespace {

/* Each block the runtime takes starts with its size, so a free and the
 * counts know it */
constexpr size_t kHead = 16;
static_assert(kHead >= sizeof(size_t) && kHead % alignof(std::max_align_t) == 0,
              "the size fits ahead of a block and leaves it aligned");

std::atomic<size_t> g_in_use{0};
std::atomic<uint64_t> g_allocations{0};

/* While an instance is made on this thread: what its making may still
 * take, and whether the allocator refused it anything */
thread_local bool t_scoped = false;
thread_local size_t t_scope_left = 0;
thread_local bool t_refused = false;

/* WAMR's allocator functions as third_party/wamr.cmake builds it
 * (WASM_MEM_ALLOC_WITH_USAGE): each is told what the block is for. A
 * module's memory is zeroed here: the runtime doesn't, and a block the
 * heap used before would show the guest the host's bytes. */
void* take(mem_alloc_usage_t usage, unsigned int n)
{
    const size_t total = static_cast<size_t>(n) + kHead;
    if (t_scoped && total > t_scope_left) {
        t_refused = true;
        return nullptr;
    }
    size_t held = g_in_use.load(std::memory_order_relaxed);
    do {
        if (held > RUNTIME_BYTES_MAX || total > RUNTIME_BYTES_MAX - held) {
            t_refused = true;
            return nullptr;
        }
    } while (!g_in_use.compare_exchange_weak(held, held + total,
                                             std::memory_order_relaxed));
    void* block = std::malloc(total);
    if (block == nullptr) {
        g_in_use.fetch_sub(total, std::memory_order_relaxed);
        t_refused = true;
        return nullptr;
    }
    if (t_scoped) {
        t_scope_left -= total;
    }
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    std::memcpy(block, &total, sizeof total);
    uint8_t* p = static_cast<uint8_t*>(block) + kHead;
    if (usage == Alloc_For_LinearMemory) {
        std::memset(p, 0, n);
    }
    return p;
}

size_t total_of(const void* p)
{
    size_t total = 0;
    std::memcpy(&total, static_cast<const uint8_t*>(p) - kHead, sizeof total);
    return total;
}

void give(mem_alloc_usage_t, void* p)
{
    if (p == nullptr) {
        return;
    }
    const size_t total = total_of(p);
    g_in_use.fetch_sub(total, std::memory_order_relaxed);
    if (t_scoped) {
        t_scope_left += total;
    }
    std::free(static_cast<uint8_t*>(p) - kHead);
}

void* retake(mem_alloc_usage_t usage, bool, void* p, unsigned int n)
{
    if (p == nullptr) {
        return take(usage, n);
    }
    void* q = take(usage, n);
    if (q == nullptr) {
        return nullptr;
    }
    const size_t kept = total_of(p) - kHead;
    std::memcpy(q, p, kept < n ? kept : n);
    give(usage, p);
    return q;
}

/* An instance's making on this thread, bounded */
struct making {
    making()
    {
        t_scoped = true;
        t_scope_left = INSTANCE_BYTES_MAX;
        t_refused = false;
    }
    ~making() { t_scoped = false; }
    making(const making&) = delete;
    making& operator=(const making&) = delete;
};

/* One maker at a time: the runtime's start, and each load, instantiate
 * and unload, which touch the runtime's own tables */
std::mutex& makers()
{
    static std::mutex m;
    return m;
}

/* The runtime, started at the first instance and kept for the process;
 * under makers() */
bool runtime_up()
{
    static bool up = false;
    if (!up) {
        RuntimeInitArgs init;
        std::memset(&init, 0, sizeof init);
        /* WAMR holds its allocator's functions as void pointers */
        init.mem_alloc_type = Alloc_With_Allocator;
        init.mem_alloc_option.allocator.malloc_func =
            reinterpret_cast<void*>(&take);
        init.mem_alloc_option.allocator.realloc_func =
            reinterpret_cast<void*>(&retake);
        init.mem_alloc_option.allocator.free_func =
            reinterpret_cast<void*>(&give);
        up = wasm_runtime_full_init(&init);
        if (up) {
            /* Nothing a module does prints: WAMR's warnings go to stdout */
            wasm_runtime_set_log_level(WASM_LOG_LEVEL_FATAL);
        }
    }
    return up;
}

} // namespace

class instance
{
public:
    uint8_t* code =
        nullptr; /* the module's bytes: the runtime reads them in place */
    wasm_module_t module = nullptr;
    wasm_module_inst_t inst = nullptr;
    wasm_exec_env_t env = nullptr;
    uint8_t* memory = nullptr; /* fixed for the instance's life: no grow */
    uint64_t memory_bytes = 0;
    wasm_function_inst_t funcs[WANTED_MAX] = {};
    uint8_t params[WANTED_MAX] = {};
    bool has_result[WANTED_MAX] = {};
    size_t func_count = 0;
    bool dead = false;
    char trap[REFUSAL_MAX] = {};
};

namespace {

/* The runtime's words as printable ASCII on one line, whatever they quote */
void plain(char* text)
{
    for (char* p = text; *p != '\0'; ++p) {
        if (*p < 0x20 || *p > 0x7E) {
            *p = '?';
        }
    }
}

/* Frees what i holds, in the runtime's order; under makers() */
void unmake(instance* i)
{
    if (i->env != nullptr) {
        wasm_runtime_destroy_exec_env(i->env);
    }
    if (i->inst != nullptr) {
        wasm_runtime_deinstantiate(i->inst);
    }
    if (i->module != nullptr) {
        wasm_runtime_unload(i->module);
    }
    give(Alloc_For_Runtime, i->code);
    delete i;
}

/* The wanted exports as the runtime's functions, with each one's
 * argument count and whether it returns a value */
bool find_exports(instance* i, const wanted_export* wants, size_t want_count)
{
    for (size_t k = 0; k < want_count; ++k) {
        i->funcs[k] = wasm_runtime_lookup_function(i->inst, wants[k].name);
        const char* colon = std::strchr(wants[k].type, ':');
        if (i->funcs[k] == nullptr || colon == nullptr ||
            static_cast<size_t>(colon - wants[k].type) > ARGS_MAX) {
            return false;
        }
        i->params[k] = static_cast<uint8_t>(colon - wants[k].type);
        i->has_result[k] = colon[1] != '\0';
    }
    i->func_count = want_count;
    return true;
}

/* Loads, instantiates and readies i from bytes; false with the runtime's
 * words in why; under makers(), inside a making */
bool build(instance* i, const uint8_t* bytes, size_t n,
           const wanted_export* wants, size_t want_count, char* why)
{
    char said[96] = {};
    i->code = static_cast<uint8_t*>(
        take(Alloc_For_Runtime, static_cast<unsigned int>(n)));
    if (i->code == nullptr) {
        return false;
    }
    std::memcpy(i->code, bytes, n);
    i->module =
        wasm_runtime_load(i->code, static_cast<uint32_t>(n), said, sizeof said);
    if (i->module != nullptr) {
        /* No heap of the host's in the guest's memory; the memory's size
         * is the module's own, which the check held to the cap */
        i->inst = wasm_runtime_instantiate(i->module, STACK_BYTES, 0, said,
                                           sizeof said);
    }
    if (i->inst != nullptr) {
        i->env = wasm_runtime_create_exec_env(i->inst, STACK_BYTES);
    }
    if (i->env == nullptr) {
        plain(said);
        std::snprintf(why, REFUSAL_MAX, "the runtime refused the module: %s",
                      said[0] != '\0' ? said : "no execution environment");
        return false;
    }
    wasm_memory_inst_t memory = wasm_runtime_get_default_memory(i->inst);
    if (memory == nullptr || !find_exports(i, wants, want_count)) {
        std::snprintf(
            why, REFUSAL_MAX,
            "the runtime refused the module: an export or its memory is "
            "missing");
        return false;
    }
    i->memory = static_cast<uint8_t*>(wasm_memory_get_base_address(memory));
    i->memory_bytes = wasm_memory_get_cur_page_count(memory) *
                      wasm_memory_get_bytes_per_page(memory);
    return true;
}

} // namespace

ecuio_status_t make_instance(const uint8_t* bytes, size_t n,
                             const wanted_export* wants, size_t want_count,
                             instance** out, char* why) noexcept
{
    if (out == nullptr || why == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    why[0] = '\0';
    const check_result checked = check_module(bytes, n, wants, want_count);
    if (!checked.ok) {
        std::snprintf(why, REFUSAL_MAX, "%s", checked.why);
        return ECUIO_ERR_LOAD_FAILED;
    }
    std::lock_guard<std::mutex> lock(makers());
    making bounded;
    instance* i = runtime_up() ? new (std::nothrow) instance : nullptr;
    if (i == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    if (!build(i, bytes, n, wants, want_count, why)) {
        unmake(i);
        if (t_refused) {
            why[0] = '\0';
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        return ECUIO_ERR_LOAD_FAILED;
    }
    *out = i;
    return ECUIO_OK;
}

void free_instance(instance* i) noexcept
{
    if (i == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(makers());
    unmake(i);
}

call_result call_instance(instance* i, size_t index, const uint32_t* args,
                          uint32_t arg_count, uint32_t budget) noexcept
{
    call_result r;
    if (i == nullptr || i->dead) {
        return r;
    }
    if (index >= i->func_count || arg_count != i->params[index] ||
        (arg_count > 0 && args == nullptr)) {
        r.end = call_end::no_function;
        return r;
    }
    uint32_t cells[ARGS_MAX] = {}; /* the arguments in, the result out */
    for (uint32_t k = 0; k < arg_count; ++k) {
        cells[k] = args[k];
    }
    const uint32_t capped = budget > INT_MAX ? INT_MAX : budget;
    const uint32_t limit = capped == 0 ? 1 : capped;
    wasm_runtime_set_instruction_count_limit(i->env, static_cast<int>(limit));
    if (wasm_runtime_call_wasm(i->env, i->funcs[index], arg_count, cells)) {
        r.end = call_end::returned;
        r.value = i->has_result[index] ? cells[0] : 0;
        return r;
    }
    i->dead = true;
    const char* said = wasm_runtime_get_exception(i->inst);
    std::snprintf(i->trap, sizeof i->trap, "%s",
                  said != nullptr ? said : "a trap");
    plain(i->trap);
    r.end = std::strstr(i->trap, "instruction limit exceeded") != nullptr
                ? call_end::budget
                : call_end::trapped;
    return r;
}

uint8_t* instance_memory(instance* i, uint32_t offset, uint32_t len) noexcept
{
    if (i == nullptr || i->dead || offset > i->memory_bytes ||
        len > i->memory_bytes - offset) {
        return nullptr;
    }
    return i->memory + offset;
}

bool instance_dead(const instance* i) noexcept
{
    return i == nullptr || i->dead;
}

const char* instance_trap(const instance* i) noexcept
{
    return i != nullptr ? i->trap : "";
}

size_t runtime_bytes_in_use() noexcept
{
    return g_in_use.load(std::memory_order_relaxed);
}

uint64_t runtime_allocations() noexcept
{
    return g_allocations.load(std::memory_order_relaxed);
}

} // namespace ecuio_wasm
