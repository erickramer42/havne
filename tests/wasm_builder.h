// File/layer: tests/wasm_builder.h | test helper: a WebAssembly module's bytes from its parts
// Testing: used by test_wamr_build.cpp and the sandbox's test files; a wrong
//          section fails WAMR's load there
//
// The sandbox tests need small modules, most of them hostile (an endless
// loop, a start function, an import), on every platform and with no
// WebAssembly toolchain. This writes the binary format directly: each
// function type once, as a linker writes them, at most one memory, and a
// function's body given as its instruction bytes.

#ifndef ECUIO_TESTS_WASM_BUILDER_H
#define ECUIO_TESTS_WASM_BUILDER_H

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace wasm_builder {

using bytes = std::vector<uint8_t>;

/* Value types and the instructions the tests write */
constexpr uint8_t I32 = 0x7F;
constexpr uint8_t UNREACHABLE = 0x00;
constexpr uint8_t NOP = 0x01;
constexpr uint8_t IF = 0x04;
constexpr uint8_t BR_TABLE = 0x0E;
constexpr uint8_t RETURN = 0x0F;
constexpr uint8_t DROP = 0x1A;
constexpr uint8_t BLOCK = 0x02;
constexpr uint8_t LOOP = 0x03;
constexpr uint8_t END = 0x0B;
constexpr uint8_t BR = 0x0C;
constexpr uint8_t BR_IF = 0x0D;
constexpr uint8_t CALL = 0x10;
constexpr uint8_t LOCAL_GET = 0x20;
constexpr uint8_t LOCAL_SET = 0x21;
constexpr uint8_t I32_LOAD = 0x28;
constexpr uint8_t I32_STORE = 0x36;
constexpr uint8_t MEMORY_SIZE = 0x3F;
constexpr uint8_t MEMORY_GROW = 0x40;
constexpr uint8_t I32_CONST = 0x41;
constexpr uint8_t I32_EQZ = 0x45;
constexpr uint8_t I32_ADD = 0x6A;
constexpr uint8_t I32_SUB = 0x6B;
constexpr uint8_t VOID_BLOCK = 0x40; /* a block's type: no result */

/* An unsigned LEB128 number, appended */
inline void leb(bytes& out, uint32_t v)
{
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if (v != 0) {
            b |= 0x80;
        }
        out.push_back(b);
    } while (v != 0);
}

inline void name(bytes& out, const std::string& text)
{
    leb(out, static_cast<uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

/* A section: its id, its size, its content */
inline void section(bytes& out, uint8_t id, const bytes& content)
{
    if (content.empty()) {
        return;
    }
    out.push_back(id);
    leb(out, static_cast<uint32_t>(content.size()));
    out.insert(out.end(), content.begin(), content.end());
}

struct func {
    std::vector<uint8_t> params;
    std::vector<uint8_t> results;
    bytes body;              /* its instructions; the closing END is added */
    std::string exported;    /* its export name, or none */
    uint32_t i32_locals = 0; /* locals after the parameters */
};

struct import {
    std::string module;
    std::string field;
};

struct module {
    std::vector<func> funcs;
    std::vector<import> imports; /* functions of type () -> (), before funcs */
    bool memory = true;
    uint32_t memory_min = 1;
    int32_t memory_max = 16; /* pages; negative: none declared */
    /* the limits' flag byte; negative: by memory_max */
    int32_t memory_flags = -1;
    std::string memory_export = "memory";
    int32_t start = -1; /* the start function's index in funcs */
    bytes tail;         /* sections appended as they are, after the rest */

    bytes build() const
    {
        bytes out = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
        std::vector<uint32_t> type_of; /* each function's type index */
        uint32_t import_type = 0;
        section(out, 1, type_section(type_of, import_type));
        section(out, 2, import_section(import_type));
        section(out, 3, function_section(type_of));
        section(out, 5, memory_section());
        section(out, 7, export_section());
        section(out, 8, start_section());
        section(out, 10, code_section());
        out.insert(out.end(), tail.begin(), tail.end());
        return out;
    }

private:
    uint32_t imported() const { return static_cast<uint32_t>(imports.size()); }

    /* Each type once, in the order first used; an import is () -> () */
    bytes type_section(std::vector<uint32_t>& type_of,
                       uint32_t& import_type) const
    {
        std::vector<const func*> typed;
        auto type_index = [&typed](const func& f) {
            for (size_t t = 0; t < typed.size(); ++t) {
                if (typed[t]->params == f.params &&
                    typed[t]->results == f.results) {
                    return static_cast<uint32_t>(t);
                }
            }
            typed.push_back(&f);
            return static_cast<uint32_t>(typed.size() - 1);
        };
        type_of.reserve(funcs.size());
        for (const func& f : funcs) {
            type_of.push_back(type_index(f));
        }
        const func nothing{};
        import_type = imports.empty() ? 0 : type_index(nothing);

        bytes s;
        leb(s, static_cast<uint32_t>(typed.size()));
        for (const func* f : typed) {
            s.push_back(0x60);
            leb(s, static_cast<uint32_t>(f->params.size()));
            s.insert(s.end(), f->params.begin(), f->params.end());
            leb(s, static_cast<uint32_t>(f->results.size()));
            s.insert(s.end(), f->results.begin(), f->results.end());
        }
        return s;
    }

    bytes import_section(uint32_t import_type) const
    {
        bytes s;
        if (imports.empty()) {
            return s;
        }
        leb(s, imported());
        for (const import& i : imports) {
            name(s, i.module);
            name(s, i.field);
            s.push_back(0x00); /* a function */
            leb(s, import_type);
        }
        return s;
    }

    bytes function_section(const std::vector<uint32_t>& type_of) const
    {
        bytes s;
        if (funcs.empty()) {
            return s;
        }
        leb(s, static_cast<uint32_t>(funcs.size()));
        for (uint32_t type : type_of) {
            leb(s, type);
        }
        return s;
    }

    bytes memory_section() const
    {
        bytes s;
        if (!memory) {
            return s;
        }
        s.push_back(0x01);
        if (memory_flags >= 0) {
            s.push_back(static_cast<uint8_t>(memory_flags));
        } else {
            s.push_back(memory_max >= 0 ? 0x01 : 0x00);
        }
        leb(s, memory_min);
        if (memory_max >= 0) {
            leb(s, static_cast<uint32_t>(memory_max));
        }
        return s;
    }

    bytes export_section() const
    {
        bytes exports;
        uint32_t count = 0;
        if (memory && !memory_export.empty()) {
            name(exports, memory_export);
            exports.insert(exports.end(), {0x02, 0x00});
            ++count;
        }
        for (uint32_t i = 0; i < funcs.size(); ++i) {
            if (!funcs[i].exported.empty()) {
                name(exports, funcs[i].exported);
                exports.push_back(0x00);
                leb(exports, imported() + i);
                ++count;
            }
        }
        bytes s;
        if (count > 0) {
            leb(s, count);
            s.insert(s.end(), exports.begin(), exports.end());
        }
        return s;
    }

    bytes start_section() const
    {
        bytes s;
        if (start >= 0) {
            leb(s, imported() + static_cast<uint32_t>(start));
        }
        return s;
    }

    bytes code_section() const
    {
        bytes s;
        if (funcs.empty()) {
            return s;
        }
        leb(s, static_cast<uint32_t>(funcs.size()));
        for (const func& f : funcs) {
            bytes body;
            if (f.i32_locals > 0) {
                body.push_back(0x01);
                leb(body, f.i32_locals);
                body.push_back(I32);
            } else {
                body.push_back(0x00);
            }
            body.insert(body.end(), f.body.begin(), f.body.end());
            body.push_back(END);
            leb(s, static_cast<uint32_t>(body.size()));
            s.insert(s.end(), body.begin(), body.end());
        }
        return s;
    }
};

/* (func (export name) (param i32 i32) (result i32)): the sum */
inline func add_func(const std::string& exported)
{
    func f;
    f.params = {I32, I32};
    f.results = {I32};
    f.body = {LOCAL_GET, 0, LOCAL_GET, 1, I32_ADD};
    f.exported = exported;
    return f;
}

/* (func (export name)): a loop that never ends */
inline func spin_func(const std::string& exported)
{
    func f;
    f.body = {LOOP, VOID_BLOCK, BR, 0, END};
    f.exported = exported;
    return f;
}

/* How many instructions one turn of countdown_func's loop runs */
constexpr uint32_t COUNTDOWN_TURN = 8;

/* (func (export name) (param i32)): counts its parameter down to 0, a
 * turn of COUNTDOWN_TURN instructions for each: three to test it and
 * leave (local.get, i32.eqz, br_if), four to take one off (local.get,
 * i32.const, i32.sub, local.set), one to go round (br) */
inline func countdown_func(const std::string& exported)
{
    func f;
    f.params = {I32};
    f.body = {BLOCK, VOID_BLOCK, LOOP,  VOID_BLOCK, LOCAL_GET,
              0,     I32_EQZ,    BR_IF, 1,          LOCAL_GET,
              0,     I32_CONST,  1,     I32_SUB,    LOCAL_SET,
              0,     BR,         0,     END,        END};
    f.exported = exported;
    return f;
}

/* (func): stores value at address 0 of the memory */
inline func store_func(const std::string& exported, uint8_t value)
{
    func f;
    f.body = {I32_CONST, 0, I32_CONST, value, I32_STORE, 2, 0};
    f.exported = exported;
    return f;
}

/* A function of type () -> i32 (or () -> () with no result) from its
 * instructions */
inline func plain_func(const std::string& exported, bytes body,
                       bool result = true)
{
    func f;
    if (result) {
        f.results = {I32};
    }
    f.body = std::move(body);
    f.exported = exported;
    return f;
}

} // namespace wasm_builder

#endif /* ECUIO_TESTS_WASM_BUILDER_H */
