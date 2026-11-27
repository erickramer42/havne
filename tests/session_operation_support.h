// File/layer: tests/session_operation_support.h | test_core, Qt-free
// Testing: UNIT: what SessionOperationTest's files share: a bench that
//          drives a bound session's worker half (request_conversation.h,
//          which steps a run too) on an engine of its own against a
//          simulated node, with the time passed in, over
//          operation_plan_support.h's plugin
//
// The session is created over the plugin's set as mock://, then named a
// requesting transport's and marked started, so the bind and the start
// take it: the bench is its worker.

#ifndef ECUIO_TESTS_SESSION_OPERATION_SUPPORT_H
#define ECUIO_TESTS_SESSION_OPERATION_SUPPORT_H

#include "conversation_session_support.h"
#include "dump_file_support.h"
#include "operation_plan_support.h"
#include "sim_memory_support.h"

#include "../core/src/operation_session.h"

#include <ecuio/operation.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace session_operation {

using conversation_session::Bound;
using engine_talk_test::Bytes;

/* What the bench's node answers a request with, when a case says: each
 * frame in turn, the last one settling it; none is a silence */
struct Answer {
    std::vector<Bytes> frames;
    int64_t pending_us = 0; /* the time before each frame but the first */
};

struct Bench {
    using Script =
        std::function<bool(const Bytes& request, size_t nth, Answer* out)>;

    conversation_session::DoorOpen door;
    Bound bound;
    dump_file_test::Folder folder;
    ecuio_session_t* s = nullptr;
    std::unique_ptr<ecuio_request_engine> e =
        std::make_unique<ecuio_request_engine>();
    sim_memory_test::Node node;
    int64_t now = 0;
    std::vector<Bytes> sent;
    Script script;
    std::function<bool(const Bytes&)> unsent; /* the adapter takes it not */
    std::function<void(Bench&)> while_out; /* a request is on the wire */
    bool waits_for_writer = true;
    /* The adapter's battery: the nth reading's millivolts, 0 none */
    bool battery_source = true;
    std::function<uint32_t(uint32_t nth)> battery = [](uint32_t) {
        return 12600u;
    };
    uint32_t battery_reads = 0;
    int battery_held = 0; /* serves an ask only after this many tries */
    const char* uri = "j2534://bench";
    uint32_t write_max = ECUIO_ENGINE_WRITE_MAX;
    std::vector<std::pair<uint32_t, Bytes>> functional; /* to, request */
    std::vector<int64_t> functional_at;
    std::vector<size_t> functional_after; /* the requests sent before */

    /* The session bound, the engine made and the module's begin past:
     * the extended session entered, nothing unlocked. worker false: bound
     * and started, the worker not yet at its first pass. */
    void start(const conversation_test::Made& m = made(),
               uint32_t pdu_max = ECUIO_ENGINE_PDU_MAX,
               const char* store = nullptr, bool worker = true)
    {
        ASSERT_NO_FATAL_FAILURE(
            conversation_session::make_session(&bound, m, uri));
        s = bound.session.get();
        if (store != nullptr) {
            ASSERT_EQ(ecuio_session_security_store(s, store, "bench car"),
                      ECUIO_OK)
                << ecuio_last_error_detail();
        }
        ASSERT_EQ(ecuio_conversation_bind(s), ECUIO_OK)
            << ecuio_last_error_detail();
        s->info.flags |= ECUIO_SESSION_INFO_REQUESTS;
        s->started = true;
        ASSERT_NO_FATAL_FAILURE(engine_talk_test::engine(e.get()));
        e->pdu_max = pdu_max;
        e->write_max = write_max;
        node.s.security.strict = true;
        node.ecu.config.values = ecuio_sim_values::parked;
        if (!worker) {
            return;
        }
        ecuio_conversations_start(s, e.get());
        passes(8);
    }

    Answer answer(const Bytes& q)
    {
        Answer a;
        node.now = now;
        if (script && script(q, sent.size() - 1, &a)) {
            return a;
        }
        if (q == engine_talk_test::kRow) {
            a.frames = {Bytes{0x62, 0x1E, 0x1C, 0x12, 0x34}};
        } else {
            a.frames = {node.ask(q)};
        }
        return a;
    }

    void frame(const Bytes& b)
    {
        (void)ecuio_engine_on_frame(e.get(), 0, b.data(),
                                    static_cast<uint16_t>(b.size()), now,
                                    nullptr);
        ecuio_conversations_frame(s, e.get(), 0, b.data(),
                                  static_cast<uint32_t>(b.size()));
    }

    /* As a transport serves it, between requests */
    void serve_battery()
    {
        ecuio_battery_slot& b = s->battery;
        b.source = battery_source;
        if (b.want != b.served && battery_held > 0) {
            --battery_held;
        } else if (b.want != b.served) {
            const uint32_t mv = battery ? battery(++battery_reads) : 0u;
            b.got = mv != 0;
            b.mv = mv;
            b.served = b.want;
        }
    }

    /* As a transport writes it, after the step */
    void serve_functional()
    {
        if (e->aside_due) {
            e->aside_due = false;
            functional.emplace_back(e->aside_tx, Bytes(e->aside, e->aside + 2));
            functional_at.push_back(now);
            functional_after.push_back(sent.size());
            ecuio_sim_session_functional(node.s, true, now, e->aside, 2);
        }
    }

    /* One pass as a worker's: the step, channel 0's request answered */
    void pass()
    {
        serve_battery();
        ecuio_step_conversations(s, e.get(), now);
        serve_functional();
        const int32_t r = ecuio_engine_due(e.get(), 0, now);
        if (r != ECUIO_ENGINE_NONE) {
            const Bytes q = engine_talk_test::wire(e.get(), 0, r);
            const bool written = !(unsent && unsent(q));
            ecuio_engine_sent(e.get(), 0, r, written, now);
            sent.push_back(q);
            const Answer a = written ? answer(q) : Answer();
            if (while_out) {
                while_out(*this);
            }
            for (size_t i = 0; i < a.frames.size(); ++i) {
                now += i == 0 ? 4 * engine_talk_test::kMs : a.pending_us;
                frame(a.frames[i]);
            }
            /* a wait still out (no frame, or pending ones alone) runs out */
            while (e->chans[0].outstanding != ECUIO_ENGINE_NONE) {
                now = e->chans[0].deadline_us;
                (void)ecuio_engine_expire(e.get(), 0, now);
            }
            writer_idle();
        }
        now += ECUIO_ENGINE_GAP_US;
        serve_battery();
        ecuio_step_conversations(s, e.get(), now);
        serve_functional();
    }

    /* The bench's clock is its own and the writer's thread is not: a
     * run's writer is given the time to take what it was handed, so its
     * word on a chunk is in before the next request, as on a real bus */
    void writer_idle() const
    {
        const ecuio_operation::session_run* o = s->operation.get();
        for (int i = 0;
             waits_for_writer && i < 5000 && o != nullptr &&
             o->sink != nullptr &&
             o->stage.load() == ecuio_operation::kStageRunning &&
             o->sink->state() == ecuio_operation::sink_state::writing &&
             !o->sink->idle();
             ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void passes(int n)
    {
        for (int i = 0; i < n; ++i) {
            pass();
        }
    }

    ecuio_operation_state_t state() const
    {
        ecuio_operation_state_t v{};
        v.struct_size = sizeof v;
        EXPECT_EQ(ecuio_session_get_operation(s, &v), ECUIO_OK);
        return v;
    }

    ecuio_status_t begin(uint32_t flags = ECUIO_OPERATION_MAY_UNLOCK)
    {
        const ecuio_operation_request_t q = request(flags);
        return ecuio_session_operation_start(s, &q, folder.str().c_str());
    }

    /* Passes until the run has ended, the writer given its time: the
     * bench's clock is its own, the writer's thread is not */
    ecuio_operation_state_t until_ended(int most = 4000)
    {
        for (int i = 0; i < most; ++i) {
            const ecuio_operation_state_t v = state();
            if (v.end != ECUIO_OPERATION_END_NONE) {
                return v;
            }
            pass();
            if (v.phase == ECUIO_OPERATION_PHASE_SAVING ||
                sent_at_ == sent.size()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            sent_at_ = sent.size();
        }
        return state();
    }

    /* Passes until the run's phase is phase (or it ended) */
    void until_phase(uint32_t phase, int most = 4000)
    {
        for (int i = 0; i < most; ++i) {
            const ecuio_operation_state_t v = state();
            if (v.phase == phase || v.end != ECUIO_OPERATION_END_NONE) {
                return;
            }
            pass();
        }
    }

    size_t count(uint8_t service, size_t from = 0) const
    {
        size_t n = 0;
        for (size_t i = from; i < sent.size(); ++i) {
            n += !sent[i].empty() && sent[i][0] == service ? 1u : 0u;
        }
        return n;
    }

    std::vector<std::string> files() const
    {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& entry :
             dump_file_test::fs::directory_iterator(folder.path, ec)) {
            names.push_back(entry.path().filename().generic_u8string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    /* The region's bytes as the node holds them */
    Bytes region(uint32_t address, uint32_t bytes) const
    {
        Bytes out;
        for (uint32_t i = 0; i < bytes; ++i) {
            out.push_back(ecuio_sim_memory_byte(node.s.memory, address + i));
        }
        return out;
    }

private:
    size_t sent_at_ = 0;
};

} // namespace session_operation

#endif /* ECUIO_TESTS_SESSION_OPERATION_SUPPORT_H */
