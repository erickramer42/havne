// File/layer: fuzz/fuzz_wasm_module.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: arbitrary bytes as a plugin's module through the check,
//          the runtime's loader and instantiate, and its exports under a
//          budget (core/src/wasm_check.h, wasm_sandbox.h; ADR-0066
//          section 2.2)
//
// The first four bytes are the budget's size and three argument picks;
// the rest is the module. The exports called are those of kWanted the
// module has with the wanted type, so a seed names its functions a to e.
// The runtime's loader is upstream's code built with core's switches and
// instrumented here with core: under ADR-0066 section 2.4 that loader is
// the wall, so a fault in it found here is a finding. fuzz/README.md
// lists what each input must hold to.

#include "wasm_check.h"
#include "wasm_sandbox.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace {

using ecuio_wasm::call_end;
using ecuio_wasm::wanted_export;

const wanted_export kWanted[] = {
    {"a", ":"}, {"b", ":i"}, {"c", "i:i"}, {"d", "ii:i"}, {"e", "iiii:i"},
};
const size_t kWantedCount = sizeof kWanted / sizeof kWanted[0];
const uint32_t kArgCounts[kWantedCount] = {0, 0, 1, 2, 4};

/* Arguments at the edges an address or a count could trip on */
const uint32_t kEdges[8] = {0,      1,       2,          0x7F,
                            0xFFFF, 0x10000, 0x7FFFFFFF, 0xFFFFFFFF};

const uint32_t kPage = 65536;

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

/* One line of printable ASCII that fits a refusal's room */
void check_words(const char* why)
{
    const size_t n = strlen(why);
    check(n < ecuio_wasm::REFUSAL_MAX);
    for (size_t i = 0; i < n; ++i) {
        check(why[i] >= 0x20 && why[i] < 0x7F);
    }
}

/* The memory's size by the sandbox's own bounds: whole pages, 1 to the
 * cap, and every byte of it readable (the sanitizer reports one that isn't) */
void check_memory(ecuio_wasm::instance* i)
{
    uint32_t pages = 0;
    while (pages < ecuio_wasm::MEMORY_MAX_PAGES &&
           ecuio_wasm::instance_memory(i, 0, (pages + 1) * kPage) != nullptr) {
        ++pages;
    }
    check(pages >= 1);
    const uint32_t size = pages * kPage;
    check(ecuio_wasm::instance_memory(i, 0, size + 1) == nullptr);
    check(ecuio_wasm::instance_memory(i, size, 1) == nullptr);
    const uint8_t* base = ecuio_wasm::instance_memory(i, 0, size);
    check(base != nullptr &&
          ecuio_wasm::instance_memory(i, size, 0) == base + size);
    volatile uint8_t sum = 0;
    for (uint32_t k = 0; k < size; ++k) {
        sum = static_cast<uint8_t>(sum + base[k]);
    }
}

/* What one making and its calls gave, for the second run to equal */
struct transcript {
    ecuio_status_t status = ECUIO_OK;
    std::string text;
};

transcript run(const uint8_t* module, size_t n, const wanted_export* wants,
               const size_t* which, size_t want_count, uint32_t budget,
               const uint32_t* args)
{
    transcript t;
    const size_t before = ecuio_wasm::runtime_bytes_in_use();
    ecuio_wasm::instance* i = nullptr;
    char why[ecuio_wasm::REFUSAL_MAX];
    memset(why, 0xA5, sizeof why);
    t.status = ecuio_wasm::make_instance(module, n, wants, want_count, &i, why);
    check(memchr(why, '\0', sizeof why) != nullptr);
    check_words(why);
    t.text = why;
    if (t.status != ECUIO_OK) {
        check(i == nullptr);
        check(t.status == ECUIO_ERR_LOAD_FAILED ||
              t.status == ECUIO_ERR_OUT_OF_MEMORY);
        check((t.status == ECUIO_ERR_LOAD_FAILED) == (why[0] != '\0'));
        check(ecuio_wasm::runtime_bytes_in_use() == before);
        return t;
    }
    check(i != nullptr && why[0] == '\0');
    check(!ecuio_wasm::instance_dead(i) &&
          ecuio_wasm::instance_trap(i)[0] == '\0');
    check_memory(i);

    const uint64_t allocations = ecuio_wasm::runtime_allocations();
    bool dead = false;
    for (size_t k = 0; k < want_count; ++k) {
        const ecuio_wasm::call_result r =
            ecuio_wasm::call_instance(i, k, args, kArgCounts[which[k]], budget);
        t.text += '|';
        t.text += static_cast<char>('0' + static_cast<int>(r.end));
        t.text += std::to_string(r.value);
        if (dead) {
            check(r.end == call_end::dead);
            continue;
        }
        check(r.end == call_end::returned || r.end == call_end::budget ||
              r.end == call_end::trapped);
        if (r.end == call_end::returned) {
            check(!ecuio_wasm::instance_dead(i));
            check(ecuio_wasm::instance_trap(i)[0] == '\0');
            check_memory(i);
        } else {
            dead = true;
            check(ecuio_wasm::instance_dead(i));
            check(ecuio_wasm::instance_trap(i)[0] != '\0');
            check_words(ecuio_wasm::instance_trap(i));
            check(ecuio_wasm::instance_memory(i, 0, 0) == nullptr);
            t.text += ecuio_wasm::instance_trap(i);
        }
    }
    /* A call is the worker's: it takes nothing from the allocator */
    check(ecuio_wasm::runtime_allocations() == allocations);
    ecuio_wasm::free_instance(i);
    check(ecuio_wasm::runtime_bytes_in_use() == before);
    return t;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 4) {
        return 0;
    }
    const uint32_t budget = 1u
                            << (data[0] & 0x0F); /* 1 to 32768 instructions */
    const uint32_t args[ecuio_wasm::ARGS_MAX] = {
        kEdges[data[1] & 7], kEdges[data[2] & 7], kEdges[data[3] & 7], data[1]};
    /* An exact-size copy, so a read past the module is the sanitizer's to report */
    const size_t n = size - 4;
    uint8_t* module = static_cast<uint8_t*>(malloc(n ? n : 1));
    check(module != nullptr);
    memcpy(module, data + 4, n);

    /* The check alone: it never faults, twice the same, a refusal in words */
    const ecuio_wasm::check_result bare =
        ecuio_wasm::check_module(module, n, nullptr, 0);
    const ecuio_wasm::check_result again =
        ecuio_wasm::check_module(module, n, nullptr, 0);
    check(bare.ok == again.ok && strcmp(bare.why, again.why) == 0);
    check(bare.ok == (bare.why[0] == '\0'));
    check_words(bare.why);

    /* The wanted exports this module has, of their types */
    wanted_export wants[kWantedCount];
    size_t which[kWantedCount];
    size_t want_count = 0;
    for (size_t k = 0; k < kWantedCount; ++k) {
        const ecuio_wasm::check_result one =
            ecuio_wasm::check_module(module, n, &kWanted[k], 1);
        check_words(one.why);
        check(!one.ok || bare.ok);
        if (one.ok) {
            which[want_count] = k;
            wants[want_count++] = kWanted[k];
        }
    }
    const ecuio_wasm::check_result all =
        ecuio_wasm::check_module(module, n, wants, want_count);
    check(all.ok == bare.ok);

    const uint64_t allocations = ecuio_wasm::runtime_allocations();
    const transcript first =
        run(module, n, wants, which, want_count, budget, args);
    if (!bare.ok) {
        /* Refused by the check, in its words, before the runtime saw a byte */
        check(first.status == ECUIO_ERR_LOAD_FAILED && first.text == bare.why);
        check(ecuio_wasm::runtime_allocations() == allocations);
    }
    const transcript second =
        run(module, n, wants, which, want_count, budget, args);
    check(first.status == second.status && first.text == second.text);

    free(module);
    return 0;
}

/* The runtime's start takes memory it keeps: before the first input, so
 * each run's bytes in use come back to where they stood */
extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    static const uint8_t kSmallest[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00, /* the header */
        0x05, 0x04, 0x01, 0x01, 0x01, 0x01, /* one memory of one page */
    };
    ecuio_wasm::instance* i = nullptr;
    char why[ecuio_wasm::REFUSAL_MAX] = {};
    check(ecuio_wasm::make_instance(kSmallest, sizeof kSmallest, nullptr, 0, &i,
                                    why) == ECUIO_OK);
    ecuio_wasm::free_instance(i);
    return 0;
}
