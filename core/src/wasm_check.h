// File/layer: core/src/wasm_check.h | internal, C++17, Qt-free, header-only, pure
// Testing: UNIT: WasmCheckTest (test_sandbox); fuzz_wasm_module
//
// A plugin's WebAssembly module, judged by its shape before the runtime
// loads it (ADR-0066 section 2.2). The runtime checks that a module is
// valid; this checks that it is one core can bound:
//
// - no start function, and no export the runtime would call unasked:
//   those run inside instantiate, where no instruction limit is set;
// - no import: nothing outside the sandbox is reachable;
// - one memory of a stated, fixed size (its maximum equal to its
//   minimum, at most MEMORY_MAX_PAGES), so the worker that calls the
//   module never allocates and the cap is the module's own number;
// - each export the caller wants, once, a function of the type it wants;
// - nothing that makes the runtime's own load slow: at most TYPES_MAX
//   function types (it compares each with every one before it).
//
// Every section is judged wherever it stands and however often, so a
// repeated or misplaced one hides nothing from this check whatever the
// runtime then makes of it. Numbers are read strictly (a LEB128 form
// past 32 bits is refused), and a refusal is one line of words.

#ifndef ECUIO_WASM_CHECK_H
#define ECUIO_WASM_CHECK_H

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ecuio_wasm {

constexpr size_t MODULE_MAX_BYTES = 1u << 20; /* ADR-0066 section 2.8 */
constexpr uint32_t MEMORY_MAX_PAGES = 16;     /* of 64 KiB: 1 MiB */
constexpr size_t REFUSAL_MAX = 160; /* a refusal's words, NUL included */
constexpr size_t WANTED_MAX = 32;   /* exports a caller may ask for */
constexpr uint32_t TYPES_MAX = 1024; /* function types a module may state */

/* An export the module must have: a function whose parameters, then ':',
 * then results are each 'i' (i32), 'I' (i64), 'f' (f32) or 'F' (f64):
 * "ii:i" takes two i32 and returns one, ":" takes and returns nothing */
struct wanted_export {
    const char* name;
    const char* type;
};

struct check_result {
    bool ok = false;
    char why[REFUSAL_MAX] = {};
};

namespace check_detail {

/* A read position in the module: bad once anything ran past its end */
struct cursor {
    const uint8_t* at = nullptr;
    const uint8_t* end = nullptr;
    bool bad = false;

    uint8_t u8()
    {
        if (bad || at >= end) {
            bad = true;
            return 0;
        }
        return *at++;
    }

    /* An unsigned LEB128 number of at most 32 bits */
    uint32_t u32()
    {
        uint32_t v = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            const uint8_t b = u8();
            if (bad || (shift == 28 && (b & 0xF0) != 0)) {
                bad = true;
                return 0;
            }
            v |= static_cast<uint32_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) {
                return v;
            }
        }
        bad = true;
        return 0;
    }

    /* The next n bytes as a cursor of their own, stepped over here */
    cursor take(uint32_t n)
    {
        cursor c;
        if (bad || n > static_cast<size_t>(end - at)) {
            bad = true;
            return c;
        }
        c.at = at;
        c.end = at + n;
        at += n;
        return c;
    }
};

/* What the walk keeps for the exports' types: the sections' bodies */
struct shape {
    cursor types;
    cursor funcs;
    cursor exports;
    bool has_types = false;
    bool has_funcs = false;
    bool has_exports = false;
    uint32_t memories = 0;
};

inline check_result refused(const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

inline check_result refused(const char* format, ...)
{
    check_result r;
    va_list args;
    va_start(args, format);
    std::vsnprintf(r.why, sizeof r.why, format, args);
    va_end(args);
    return r;
}

inline check_result passed()
{
    check_result r;
    r.ok = true;
    return r;
}

inline check_result cut_short()
{
    return refused("the module is cut short or malformed");
}

/* A name as printable text for a refusal: other bytes shown as '?' */
inline void printable(const cursor& name, char* out, size_t cap)
{
    size_t n = 0;
    for (const uint8_t* p = name.at; p < name.end && n + 1 < cap; ++p) {
        out[n++] = (*p >= 0x20 && *p < 0x7F) ? static_cast<char>(*p) : '?';
    }
    out[n] = '\0';
}

inline bool name_is(const cursor& name, const char* text)
{
    const size_t n = std::strlen(text);
    return static_cast<size_t>(name.end - name.at) == n &&
           std::memcmp(name.at, text, n) == 0;
}

/* The import section: any import at all is refused, by name */
inline check_result judge_imports(cursor body)
{
    const uint32_t count = body.u32();
    if (body.bad) {
        return cut_short();
    }
    if (count == 0) {
        return passed();
    }
    const cursor module = body.take(body.u32());
    const cursor field = body.take(body.u32());
    if (body.bad) {
        return cut_short();
    }
    char m[40];
    char f[40];
    printable(module, m, sizeof m);
    printable(field, f, sizeof f);
    return refused(
        "the module imports %s.%s: a plugin's module imports nothing", m, f);
}

/* The memory section: one memory, its whole size stated and within the cap */
inline check_result judge_memory(cursor body, shape& s)
{
    const uint32_t count = body.u32();
    if (body.bad) {
        return cut_short();
    }
    if (count != 1) {
        return refused("the module has %u memories: it needs exactly one",
                       count);
    }
    ++s.memories;
    const uint8_t flags = body.u8();
    const uint32_t least = body.u32();
    if (body.bad) {
        return cut_short();
    }
    if (flags == 0x00) {
        return refused(
            "the module's memory states no maximum (minimum %u): a "
            "plugin's is one fixed size (link with --max-memory equal "
            "to --initial-memory)",
            least);
    }
    if (flags != 0x01) {
        return refused(
            "the module's memory is shared or 64-bit, which a plugin's "
            "can't be");
    }
    const uint32_t most = body.u32();
    if (body.bad) {
        return cut_short();
    }
    if (least != most) {
        return refused(
            "the module's memory grows from %u to %u pages: a plugin's "
            "is one fixed size (link with --max-memory equal to "
            "--initial-memory)",
            least, most);
    }
    if (most == 0 || most > MEMORY_MAX_PAGES) {
        return refused(
            "the module's memory is %u pages: 1 to %u (1 MiB) are allowed",
            most, MEMORY_MAX_PAGES);
    }
    return passed();
}

/* The type index of function `index`, from the function section */
inline bool type_of_function(cursor funcs, uint32_t index, uint32_t* type)
{
    const uint32_t count = funcs.u32();
    if (funcs.bad || index >= count) {
        return false;
    }
    /* Stopped where the section ends: count is the module's own claim */
    for (uint32_t i = 0; i < index && !funcs.bad; ++i) {
        funcs.u32();
    }
    *type = funcs.u32();
    return !funcs.bad;
}

inline uint8_t value_type(char c)
{
    switch (c) {
    case 'i':
        return 0x7F;
    case 'I':
        return 0x7E;
    case 'f':
        return 0x7D;
    case 'F':
        return 0x7C;
    default:
        return 0;
    }
}

/* Whether the next `count`-prefixed list of value types in t is `want`
 * (n characters of wanted_export's alphabet) */
inline bool types_are(cursor& t, const char* want, size_t n)
{
    const cursor list = t.take(t.u32());
    if (t.bad || static_cast<size_t>(list.end - list.at) != n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (list.at[i] != value_type(want[i])) {
            return false;
        }
    }
    return true;
}

/* Whether type `index` of the type section is the function type `want` */
inline bool type_is(cursor types, uint32_t index, const char* want)
{
    const uint32_t count = types.u32();
    if (types.bad || index >= count) {
        return false;
    }
    for (uint32_t i = 0; i < index; ++i) {
        if (types.u8() != 0x60) {
            return false;
        }
        types.take(types.u32());
        types.take(types.u32());
    }
    const char* colon = std::strchr(want, ':');
    if (types.u8() != 0x60 || types.bad || colon == nullptr) {
        return false;
    }
    return types_are(types, want, static_cast<size_t>(colon - want)) &&
           types_are(types, colon + 1, std::strlen(colon + 1));
}

/* A wanted type in words: "(i32, i32) -> i32", "() -> ()" */
inline void type_words(const char* want, char* out, size_t cap)
{
    size_t n = 0;
    auto put = [&](const char* text) {
        const size_t len = std::strlen(text);
        if (n + len + 1 < cap) {
            std::memcpy(out + n, text, len);
            n += len;
        }
    };
    auto word = [](char c) {
        return c == 'i' ? "i32" : c == 'I' ? "i64" : c == 'f' ? "f32" : "f64";
    };
    const char* colon = std::strchr(want, ':');
    const char* results = colon != nullptr ? colon + 1 : "";
    put("(");
    for (const char* p = want; *p != '\0' && p != colon; ++p) {
        put(p == want ? "" : ", ");
        put(word(*p));
    }
    put(") -> ");
    if (*results == '\0') {
        put("()");
    }
    for (const char* p = results; *p != '\0'; ++p) {
        put(p == results ? "" : ", ");
        put(word(*p));
    }
    out[n] = '\0';
}

/* The names the runtime calls by itself at instantiate, with no limit set */
constexpr const char* RESERVED_EXPORTS[] = {"__post_instantiate",
                                            "__wasm_call_ctors", "_initialize"};

/* One export: refused when its name holds a NUL or is reserved, or when
 * it is a wanted one of another kind or type, or a second of its name;
 * found[i] set for wanted export i */
inline check_result judge_export(const cursor& name, uint8_t kind,
                                 uint32_t index, const shape& s,
                                 const wanted_export* wants, size_t want_count,
                                 bool* found)
{
    /* The runtime finds an export by its name as a C string, so one
     * holding a NUL could pass for the name before it */
    if (name.at != name.end &&
        std::memchr(name.at, 0, static_cast<size_t>(name.end - name.at)) !=
            nullptr) {
        return refused("the module exports a name that holds a NUL");
    }
    for (const char* reserved : RESERVED_EXPORTS) {
        if (name_is(name, reserved)) {
            return refused(
                "the module exports %s, which would run before any limit "
                "is set",
                reserved);
        }
    }
    for (size_t i = 0; i < want_count; ++i) {
        if (!name_is(name, wants[i].name)) {
            continue;
        }
        if (found[i]) {
            return refused("the module exports %s twice", wants[i].name);
        }
        char words[64];
        type_words(wants[i].type, words, sizeof words);
        uint32_t type = 0;
        if (kind != 0x00 || !s.has_funcs || !s.has_types ||
            !type_of_function(s.funcs, index, &type) ||
            !type_is(s.types, type, wants[i].type)) {
            return refused("the module's export %s isn't a function of type %s",
                           wants[i].name, words);
        }
        found[i] = true;
    }
    return passed();
}

/* The export section, then every wanted export accounted for */
inline check_result judge_exports(const shape& s, const wanted_export* wants,
                                  size_t want_count)
{
    bool found[WANTED_MAX] = {};
    if (s.has_exports) {
        cursor body = s.exports;
        const uint32_t count = body.u32();
        for (uint32_t i = 0; i < count && !body.bad; ++i) {
            const cursor name = body.take(body.u32());
            const uint8_t kind = body.u8();
            const uint32_t index = body.u32();
            if (body.bad) {
                break;
            }
            const check_result r =
                judge_export(name, kind, index, s, wants, want_count, found);
            if (!r.ok) {
                return r;
            }
        }
        if (body.bad) {
            return cut_short();
        }
    }
    for (size_t i = 0; i < want_count; ++i) {
        if (!found[i]) {
            return refused("the module lacks the export %s", wants[i].name);
        }
    }
    return passed();
}

/* One section by its id: judged at once, or its body kept in s */
inline check_result judge_section(uint8_t id, const cursor& body, shape& s)
{
    auto keep = [&](cursor& slot, bool& has, const char* what) {
        if (has) {
            return refused("the module has two %s sections", what);
        }
        slot = body;
        has = true;
        return passed();
    };
    switch (id) {
    case 1: {
        cursor head = body;
        const uint32_t count = head.u32();
        if (head.bad) {
            return cut_short();
        }
        if (count > TYPES_MAX) {
            return refused("the module states %u function types: %u are "
                           "the most",
                           count, TYPES_MAX);
        }
        return keep(s.types, s.has_types, "type");
    }
    case 2:
        return judge_imports(body);
    case 3:
        return keep(s.funcs, s.has_funcs, "function");
    case 5:
        return judge_memory(body, s);
    case 7:
        return keep(s.exports, s.has_exports, "export");
    case 8:
        return refused(
            "the module has a start function, which would run before any "
            "limit is set");
    case 0:  /* custom: names, producers */
    case 4:  /* tables */
    case 6:  /* globals */
    case 9:  /* elements */
    case 10: /* code */
    case 11: /* data */
        return passed();
    default:
        return refused(
            "the module has a section of kind %u, which a plugin's can't "
            "have",
            static_cast<unsigned>(id));
    }
}

} // namespace check_detail

/* Whether a plugin's module of n bytes has a shape core can bound, with
 * each of the want_count exports (at most WANTED_MAX); the words say why
 * not. It reads the sections' frames, the imports, the memory and the
 * exports, never a function's code: validity is the runtime's to judge. */
inline check_result check_module(const uint8_t* bytes, size_t n,
                                 const wanted_export* wants, size_t want_count)
{
    using namespace check_detail;
    static const uint8_t kHead[8] = {0x00, 0x61, 0x73, 0x6D,
                                     0x01, 0x00, 0x00, 0x00};
    if (bytes == nullptr || want_count > WANTED_MAX ||
        (want_count > 0 && wants == nullptr)) {
        return refused("no module");
    }
    if (n > MODULE_MAX_BYTES) {
        return refused("the module is %zu bytes: 1 MiB is the most", n);
    }
    if (n < sizeof kHead || std::memcmp(bytes, kHead, sizeof kHead) != 0) {
        return refused("not a WebAssembly module of version 1");
    }
    cursor all;
    all.at = bytes + sizeof kHead;
    all.end = bytes + n;
    shape s;
    while (all.at < all.end) {
        const uint8_t id = all.u8();
        const cursor body = all.take(all.u32());
        if (all.bad) {
            return cut_short();
        }
        const check_result r = judge_section(id, body, s);
        if (!r.ok) {
            return r;
        }
    }
    if (s.memories != 1) {
        return refused("the module has %u memories: it needs exactly one",
                       s.memories);
    }
    return judge_exports(s, wants, want_count);
}

} // namespace ecuio_wasm

#endif /* ECUIO_WASM_CHECK_H */
