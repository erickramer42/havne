// File/layer: tests/operation_run_support.h | test-only, C++17, Qt-free,
//             every platform; header-only
// Testing: UNIT: OperationRunTest (test_obd2)
//
// What the dump machine's cases share (core/src/operation_run.h): a sink
// that keeps what it is handed and compares the passes, and a driver that
// steps a run against one simulated ECU's memory, a case's script
// answering in its place where it says. Every address and identifier is
// the simulated ECU's, made up.
#ifndef ECUIO_TESTS_OPERATION_RUN_SUPPORT_H
#define ECUIO_TESTS_OPERATION_RUN_SUPPORT_H

#include "operation_run.h"
#include "sim_memory_support.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace operation_run_test {

using ecuio_operation::plan;
using ecuio_operation::run;
using ecuio_operation::run_action;
using ecuio_operation::run_end;
using ecuio_operation::run_request;
using ecuio_operation::run_stage;
using ecuio_operation::run_wait;
using ecuio_operation::step;
using sim_memory_test::Bytes;
using sim_memory_test::kBase;

/* What a run hands over, kept by step and pass, and where the second
 * pass first differs from the first */
struct Sink {
    bool full = false;
    std::vector<Bytes> got[2];
    std::vector<Bytes> idents;
    bool differ = false;
    uint32_t differ_step = 0;
    uint64_t differ_at = 0;

    explicit Sink(size_t steps)
    {
        got[0].resize(steps);
        got[1].resize(steps);
    }
    bool room(size_t) const { return !full; }
    void data(uint32_t step, uint32_t pass, uint64_t offset, const uint8_t* p,
              size_t n)
    {
        Bytes& to = got[pass][step];
        EXPECT_EQ(offset, to.size()) << "each byte once, in order";
        to.insert(to.end(), p, p + n);
        const Bytes& first = got[0][step];
        for (size_t i = 0; pass == 1 && i < n && !differ; ++i) {
            if (offset + i >= first.size() || first[offset + i] != p[i]) {
                differ = true;
                differ_step = step;
                differ_at = offset + i;
            }
        }
    }
    void ident(uint32_t index, const uint8_t* p, size_t n)
    {
        EXPECT_EQ(index, idents.size());
        idents.emplace_back(p, p + n);
    }
};

inline step read_step(uint64_t address, uint64_t bytes, uint32_t chunk)
{
    step s;
    s.kind = ECUIO_STEP_READ;
    s.address = address;
    s.bytes = bytes;
    s.chunk = chunk;
    s.name = "read";
    return s;
}

inline step upload_step(uint64_t address, uint64_t bytes)
{
    step s;
    s.kind = ECUIO_STEP_UPLOAD;
    s.address = address;
    s.bytes = bytes;
    s.size_bytes = 4;
    s.name = "upload";
    return s;
}

/* A plan of these steps, reading the sim's two identifiers when asked */
inline plan plan_of(std::vector<step> steps, bool idents = true)
{
    plan p;
    p.level = 0x01;
    p.steps = std::move(steps);
    if (idents) {
        p.idents = {{ECUIO_SIM_HARDWARE_DID, ECUIO_DUMP_ROLE_HARDWARE},
                    {ECUIO_SIM_SOFTWARE_DID, ECUIO_DUMP_ROLE_SOFTWARE}};
    }
    return p;
}

/* One run against one ECU. A script, when set, sees each request with
 * its place among those sent (from 0) and answers in the ECU's place by
 * returning true: an empty reply is a silence, *waited the time since
 * the request went out. */
struct Drive {
    using Script = std::function<bool(const Bytes& request, size_t nth,
                                      Bytes* reply, uint32_t* waited)>;

    plan p;
    run r;
    sim_memory_test::Ecu ecu;
    std::unique_ptr<Sink> sink;
    std::vector<Bytes> sent;
    std::vector<uint32_t> entered;
    Script script;
    std::function<void(Drive&)> before_next; /* nothing is out */
    std::function<void(Drive&)> while_out;   /* a request is out */
    bool enter_fails = false;
    int holds = 0;
    int waited_on = 0; /* the 78s the run went on waiting behind */

    explicit Drive(plan made) : p(std::move(made))
    {
        ecuio_operation::begin(r, p);
        sink = std::make_unique<Sink>(p.steps.size());
    }

    Bytes ecu_answer(const Bytes& request)
    {
        if (request[0] != 0x22) {
            return ecu.ask(request);
        }
        uint8_t out[3 + ECUIO_SIM_IDENT_BYTES];
        const auto did = static_cast<uint16_t>(request[1] << 8 | request[2]);
        const uint32_t n = ecuio_sim_memory_did(ecu.m, true, did, out);
        return n != 0 ? Bytes(out, out + n) : sim_memory_test::nrc(0x22, 0x31);
    }

    void send(const run_request& q)
    {
        const Bytes request(q.bytes, q.bytes + q.size);
        sent.push_back(request);
        Bytes answer;
        uint32_t waited = 10;
        if (!script || !script(request, sent.size() - 1, &answer, &waited)) {
            answer = ecu_answer(request);
        }
        if (while_out) {
            while_out(*this);
        }
        if (answer.empty()) {
            ecuio_operation::silence(r);
        } else if (ecuio_operation::reply(r, answer.data(), answer.size(),
                                          waited,
                                          *sink) == run_wait::keep_waiting) {
            /* the pending answer, then the ECU's own */
            ++waited_on;
            answer = ecu_answer(request);
            EXPECT_EQ(ecuio_operation::reply(r, answer.data(), answer.size(),
                                             waited + 10, *sink),
                      run_wait::go_on);
        }
        /* The sink's word, over the machine's complete too */
        if (sink->differ &&
            (r.at != run_stage::done || r.result == run_end::complete)) {
            ecuio_operation::stop(r, run_end::differ);
        }
    }

    /* The run stepped to its end */
    run_end go()
    {
        for (int guard = 0; guard < 400000; ++guard) {
            if (before_next) {
                before_next(*this);
            }
            run_request q;
            switch (ecuio_operation::next(r, q, *sink)) {
            case run_action::done:
                return r.result;
            case run_action::hold:
                ++holds;
                sink->full = false;
                break;
            case run_action::enter:
                entered.push_back(q.session);
                if (enter_fails) {
                    ecuio_operation::not_entered(r, run_end::unlock_refused);
                } else {
                    ecuio_operation::entered(r);
                }
                break;
            case run_action::send:
                send(q);
                break;
            }
        }
        ADD_FAILURE() << "the run never ended";
        return r.result;
    }

    /* How many requests of this service went out */
    size_t count(uint8_t service) const
    {
        size_t n = 0;
        for (const Bytes& q : sent) {
            n += q[0] == service ? 1u : 0u;
        }
        return n;
    }
};

/* The region's bytes as the sim holds them */
inline Bytes region(uint32_t address, uint32_t size)
{
    Bytes b = sim_memory_test::data_of(address, size, ECUIO_SIM_DEFAULT_VIN);
    b.erase(b.begin()); /* the 63 */
    return b;
}

} // namespace operation_run_test

#endif /* ECUIO_TESTS_OPERATION_RUN_SUPPORT_H */
