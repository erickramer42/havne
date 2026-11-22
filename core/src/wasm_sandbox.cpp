// File/layer: core/src/wasm_sandbox.cpp | internal, Qt-free; embeds WAMR (third_party/wamr.cmake)
// Testing: UNIT: SandboxTest (test_sandbox); fuzz_wasm_module

#include "wasm_sandbox.h"

#include "ecuio/plugin.h"
#include "ecuio/signals.h" /* ECUIO_MAX_CHANNELS */
#include "wasm_export.h"

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

namespace ecuio_wasm {

/* ADR-0066: a manifest's budget lowers the sandbox's, never raises it */
static_assert(BUDGET_DEFAULT == ECUIO_PLUGIN_BUDGET_MAX,
              "the sandbox's default budget is the most a manifest takes");
/* A session's module and an instance for each of its most ECUs, with
 * room left for the runtime's own start */
static_assert(MODULE_BYTES_MAX + ECUIO_MAX_CHANNELS * INSTANCE_BYTES_MAX <=
                  RUNTIME_BYTES_MAX - (4u << 20),
              "a session's whole plugin fits the runtime's bound");

namespace {

/* Each block the runtime takes starts with its size, so a free and the
 * counts know it */
constexpr size_t kHead = 16;
static_assert(kHead >= sizeof(size_t) && kHead % alignof(std::max_align_t) == 0,
              "the size fits ahead of a block and leaves it aligned");

std::atomic<size_t> g_in_use{0};
std::atomic<uint64_t> g_allocations{0};

/* While a module is loaded or an instance made on this thread: what that
 * may still take, and whether the allocator refused it anything */
thread_local bool t_scoped = false;
thread_local size_t t_scope_left = 0;
thread_local bool t_refused = false;

/* WAMR's allocator functions as third_party/wamr.cmake builds it
 * (WASM_MEM_ALLOC_WITH_USAGE): each is told what the block is for. A
 * module's memory is zeroed here: the runtime doesn't, and a block the
 * heap used before would show the guest the host's bytes. */
void* take(mem_alloc_usage_t usage, unsigned int n)
{
    /* Judged before the head is added: on a 32-bit core the sum wraps, and
     * WAMR's loader asks for nearly 4 GiB for a function that declares it */
    if (n > RUNTIME_BYTES_MAX) {
        t_refused = true;
        return nullptr;
    }
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

/* A load or an instance's making on this thread, bounded */
struct making {
    explicit making(size_t bound)
    {
        t_scoped = true;
        t_scope_left = bound;
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

class loaded
{
public:
    uint8_t* code =
        nullptr; /* the module's bytes: the runtime reads them in place */
    wasm_module_t module = nullptr;
    const char* names[WANTED_MAX] = {}; /* the wanted exports', the module's */
    uint8_t params[WANTED_MAX] = {};
    bool has_result[WANTED_MAX] = {};
    size_t func_count = 0;
    size_t holds = 1; /* the loader's and each instance's; under makers() */
};

class instance
{
public:
    loaded* of = nullptr;
    wasm_module_inst_t inst = nullptr;
    wasm_exec_env_t env = nullptr;
    uint8_t* memory = nullptr; /* fixed for the instance's life: no grow */
    uint64_t memory_bytes = 0;
    wasm_function_inst_t funcs[WANTED_MAX] = {};
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

/* Lets go of one hold on m, unloading it with the last; under makers() */
void release(loaded* m)
{
    if (--m->holds > 0) {
        return;
    }
    if (m->module != nullptr) {
        wasm_runtime_unload(m->module);
    }
    give(Alloc_For_Runtime, m->code);
    delete m;
}

/* Frees what i's making made, in the runtime's order; under makers() */
void unbuild(instance* i)
{
    if (i->env != nullptr) {
        wasm_runtime_destroy_exec_env(i->env);
    }
    if (i->inst != nullptr) {
        wasm_runtime_deinstantiate(i->inst);
    }
    delete i;
}

/* Frees a made instance and its hold on its module; under makers() */
void unmake(instance* i)
{
    loaded* const of = i->of;
    unbuild(i);
    release(of);
}

/* Whether a call can carry a wanted type: i32 parameters, at most
 * ARGS_MAX, and no result or one i32. The runtime writes a call's
 * results over its argument cells, as many as the type has. */
bool callable(const char* type)
{
    const char* colon = type != nullptr ? std::strchr(type, ':') : nullptr;
    if (colon == nullptr || static_cast<size_t>(colon - type) > ARGS_MAX) {
        return false;
    }
    for (const char* p = type; p != colon; ++p) {
        if (*p != 'i') {
            return false;
        }
    }
    return colon[1] == '\0' || (colon[1] == 'i' && colon[2] == '\0');
}

/* The wanted exports' names as the module keeps them, with each one's
 * argument count and whether it returns a value */
bool name_exports(loaded* m, const wanted_export* wants, size_t want_count)
{
    const int32_t exports = wasm_runtime_get_export_count(m->module);
    for (size_t k = 0; k < want_count; ++k) {
        for (int32_t e = 0; e < exports && m->names[k] == nullptr; ++e) {
            wasm_export_t x;
            wasm_runtime_get_export_type(m->module, e, &x);
            if (x.kind == WASM_IMPORT_EXPORT_KIND_FUNC &&
                std::strcmp(x.name, wants[k].name) == 0) {
                m->names[k] = x.name;
            }
        }
        const char* colon = std::strchr(wants[k].type, ':');
        if (m->names[k] == nullptr) {
            return false;
        }
        m->params[k] = static_cast<uint8_t>(colon - wants[k].type);
        m->has_result[k] = colon[1] != '\0';
    }
    m->func_count = want_count;
    return true;
}

/* Loads m from bytes; false with the runtime's words in why; under
 * makers(), inside a making */
bool load(loaded* m, const uint8_t* bytes, size_t n, const wanted_export* wants,
          size_t want_count, char* why)
{
    char said[96] = {};
    m->code = static_cast<uint8_t*>(
        take(Alloc_For_Runtime, static_cast<unsigned int>(n)));
    if (m->code == nullptr) {
        return false;
    }
    std::memcpy(m->code, bytes, n);
    m->module =
        wasm_runtime_load(m->code, static_cast<uint32_t>(n), said, sizeof said);
    if (m->module == nullptr) {
        plain(said);
        std::snprintf(why, REFUSAL_MAX, "the runtime refused the module: %s",
                      said[0] != '\0' ? said : "no module");
        return false;
    }
    if (!name_exports(m, wants, want_count)) {
        std::snprintf(why, REFUSAL_MAX,
                      "the runtime refused the module: an export is missing");
        return false;
    }
    return true;
}

/* Instantiates and readies i from its module; false with the runtime's
 * words in why; under makers(), inside a making */
bool build(instance* i, char* why)
{
    char said[96] = {};
    /* No heap of the host's in the guest's memory; the memory's size is
     * the module's own, which the check held to the cap */
    i->inst = wasm_runtime_instantiate(i->of->module, STACK_BYTES, 0, said,
                                       sizeof said);
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
    bool found = memory != nullptr;
    for (size_t k = 0; found && k < i->of->func_count; ++k) {
        i->funcs[k] = wasm_runtime_lookup_function(i->inst, i->of->names[k]);
        found = i->funcs[k] != nullptr;
    }
    if (!found) {
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

/* A failed load's or making's status: out of memory, its words dropped,
 * when the allocator refused anything */
ecuio_status_t failed(char* why)
{
    if (t_refused) {
        why[0] = '\0';
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_ERR_LOAD_FAILED;
}

} // namespace

ecuio_status_t load_module(const uint8_t* bytes, size_t n,
                           const wanted_export* wants, size_t want_count,
                           loaded** out, char* why) noexcept
{
    if (out == nullptr || why == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    why[0] = '\0';
    for (size_t k = 0; wants != nullptr && k < want_count && k < WANTED_MAX;
         ++k) {
        if (!callable(wants[k].type)) {
            std::snprintf(why, REFUSAL_MAX,
                          "wanted export %zu has a type a call can't carry", k);
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    /* The check judges a copy and the runtime loads that copy, so bytes
     * the caller changes meanwhile never load unjudged; past the size
     * bound the check refuses whatever they hold */
    std::unique_ptr<uint8_t[]> copy;
    const uint8_t* judged = bytes;
    if (bytes != nullptr && n > 0 && n <= MODULE_MAX_BYTES) {
        copy.reset(new (std::nothrow) uint8_t[n]);
        if (!copy) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        std::memcpy(copy.get(), bytes, n);
        judged = copy.get();
    }
    const check_result checked = check_module(judged, n, wants, want_count);
    if (!checked.ok) {
        std::snprintf(why, REFUSAL_MAX, "%s", checked.why);
        return ECUIO_ERR_LOAD_FAILED;
    }
    std::lock_guard<std::mutex> lock(makers());
    making bounded(MODULE_BYTES_MAX);
    loaded* m = runtime_up() ? new (std::nothrow) loaded : nullptr;
    if (m == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    if (!load(m, judged, n, wants, want_count, why)) {
        release(m);
        return failed(why);
    }
    *out = m;
    return ECUIO_OK;
}

void free_module(loaded* m) noexcept
{
    if (m == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(makers());
    release(m);
}

ecuio_status_t make_instance(loaded* m, instance** out, char* why) noexcept
{
    if (m == nullptr || out == nullptr || why == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    why[0] = '\0';
    std::lock_guard<std::mutex> lock(makers());
    making bounded(INSTANCE_BYTES_MAX);
    instance* i = new (std::nothrow) instance;
    if (i == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    i->of = m;
    if (!build(i, why)) {
        unbuild(i); /* it took no hold */
        return failed(why);
    }
    ++m->holds;
    *out = i;
    return ECUIO_OK;
}

ecuio_status_t make_instance(const uint8_t* bytes, size_t n,
                             const wanted_export* wants, size_t want_count,
                             instance** out, char* why) noexcept
{
    if (out == nullptr || why == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    loaded* m = nullptr;
    const ecuio_status_t loaded_status =
        load_module(bytes, n, wants, want_count, &m, why);
    if (loaded_status != ECUIO_OK) {
        return loaded_status;
    }
    const ecuio_status_t made = make_instance(m, out, why);
    free_module(m);
    return made;
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
    if (index >= i->of->func_count || arg_count != i->of->params[index] ||
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
        r.value = i->of->has_result[index] ? cells[0] : 0;
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
