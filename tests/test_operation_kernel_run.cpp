#include "operation_run_support.h"

#include "../core/src/sim_session.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using namespace operation_run_test;
using ecuio_operation::helper_stage;
using sim_memory_test::Node;
using sim_memory_test::nrc;

constexpr uint32_t kCode = ECUIO_SIM_MEMORY_CODE_ADDRESS;
constexpr uint32_t kHelperBytes = 2000;

plan kernel_plan(uint64_t bytes = 3000, bool stays = true)
{
    step s = upload_step(kCode, bytes);
    s.session = ECUIO_STEP_SESSION_PROGRAMMING;
    plan p = plan_of({s}, false);
    p.kind = ECUIO_OPERATION_KIND_KERNEL_READ;
    p.has_kernel = true;
    ecuio_operation::kernel& k = p.helper;
    k.ram_address = ECUIO_SIM_KERNEL_RAM_ADDRESS;
    k.ram_bytes = ECUIO_SIM_KERNEL_RAM_BYTES;
    k.starts = {ECUIO_SIM_KERNEL_ROUTINE};
    k.routine = ECUIO_SIM_KERNEL_ROUTINE;
    k.address = ECUIO_SIM_KERNEL_RAM_ADDRESS;
    k.stays = stays;
    k.quiet_session = 0x82;
    k.quiet_keepalive = 0x80;
    k.keepalive_ms = 2000;
    k.blob.resize(kHelperBytes);
    ecuio_sim_kernel_image(k.blob.data(), kHelperBytes);
    return p;
}

/* A kernel read against one node that wants a helper. A script, when
 * set, answers in the node's place as Drive's does. */
struct KernelDrive {
    plan p;
    run r;
    Node n;
    Sink sink{1};
    std::vector<Bytes> sent;
    std::vector<bool> functional;
    Drive::Script script;
    std::function<void(KernelDrive&)> while_out;

    explicit KernelDrive(plan made, const ecuio_sim_kernel_faults& f = {},
                         uint32_t wire_max = ECUIO_ENGINE_WRITE_MAX)
        : p(std::move(made))
    {
        ecuio_operation::begin(r, p, wire_max);
        n.s.kernel.on = true;
        n.s.kernel.faults = f;
    }

    Bytes node_answer(const Bytes& q, bool to_all)
    {
        if (!to_all) {
            return n.ask(q);
        }
        ecuio_sim_session_functional(n.s, true, n.now, q.data(),
                                     static_cast<uint32_t>(q.size()));
        return {};
    }

    void send(const run_request& q)
    {
        const Bytes request = q.wide != nullptr
                                  ? Bytes(q.wide, q.wide + q.wide_size)
                                  : Bytes(q.bytes, q.bytes + q.size);
        sent.push_back(request);
        functional.push_back(q.functional);
        Bytes answer;
        uint32_t waited = 10;
        if (!script || !script(request, sent.size() - 1, &answer, &waited)) {
            answer = node_answer(request, q.functional);
        }
        if (while_out) {
            while_out(*this);
        }
        if (answer.empty()) {
            ecuio_operation::silence(r);
            return;
        }
        while (ecuio_operation::reply(r, answer.data(), answer.size(), waited,
                                      sink) == run_wait::keep_waiting) {
            waited += 10000;
        }
    }

    run_end go()
    {
        for (int guard = 0; guard < 100000; ++guard) {
            run_request q;
            switch (ecuio_operation::next(r, q, sink)) {
            case run_action::done:
                return r.result;
            case run_action::hold:
                break;
            case run_action::enter:
                EXPECT_EQ(q.session, ECUIO_STEP_SESSION_PROGRAMMING);
                n.ask({0x10, 0x02});
                n.unlock();
                ecuio_operation::entered(r);
                break;
            case run_action::send:
                send(q);
                break;
            case run_action::leave:
                sent.push_back(Bytes{0x10, 0x01});
                functional.push_back(false);
                n.ask({0x10, 0x01});
                ecuio_operation::left(r);
                break;
            }
        }
        ADD_FAILURE() << "the run never ended";
        return r.result;
    }

    size_t count_of(uint8_t service) const
    {
        size_t k = 0;
        for (const Bytes& q : sent) {
            k += q[0] == service ? 1u : 0u;
        }
        return k;
    }

    std::vector<uint8_t> services() const
    {
        std::vector<uint8_t> out;
        for (const Bytes& q : sent) {
            out.push_back(q[0]);
        }
        return out;
    }
};

const Bytes kQuiet = {0x10, 0x82};
const Bytes kStart = {0x31, 0x01, 0x02, 0x01};

Drive::Script answer_to(uint8_t service, Bytes reply)
{
    return [=](const Bytes& q, size_t, Bytes* out, uint32_t*) {
        if (q[0] != service) {
            return false;
        }
        *out = reply;
        return true;
    };
}

} // namespace

TEST(OperationRunTest, AKernelReadQuietsDownloadsStartsThenUploadsTwice)
{
    KernelDrive d(kernel_plan());
    EXPECT_EQ(d.go(), run_end::complete);
    ASSERT_GE(d.sent.size(), 6u);
    EXPECT_EQ(d.sent[0], kQuiet);
    EXPECT_TRUE(d.functional[0]) << "the quieter goes to every module";
    EXPECT_EQ(d.sent[1], (Bytes{0x34, 0x00, 0x44, 0x00, 0xF0, 0x00, 0x00, 0x00,
                                0x00, 0x07, 0xD0}));
    EXPECT_EQ(d.sent[2].size(), 2u + 1024u) << "the module's 0x0402 less two";
    EXPECT_EQ(d.sent[2][1], 0x01);
    EXPECT_EQ(Bytes(d.sent[2].begin() + 2, d.sent[2].end()),
              Bytes(d.p.helper.blob.begin(), d.p.helper.blob.begin() + 1024));
    EXPECT_EQ(d.sent[3].size(), 2u + 976u) << "a short last block";
    EXPECT_EQ(d.sent[3][1], 0x02);
    EXPECT_EQ(d.sent[4], Bytes{0x37});
    EXPECT_EQ(d.sent[5], kStart);
    const std::vector<uint8_t> pass = {0x35, 0x36, 0x36, 0x36, 0x37};
    std::vector<uint8_t> want = {0x10, 0x34, 0x36, 0x36, 0x37, 0x31};
    want.insert(want.end(), pass.begin(), pass.end());
    want.insert(want.end(), pass.begin(), pass.end());
    want.push_back(0x10); /* the leave */
    EXPECT_EQ(d.services(), want) << "both passes back to back, one helper";
    for (size_t i = 1; i < d.functional.size(); ++i) {
        EXPECT_FALSE(d.functional[i]) << i;
    }
    EXPECT_EQ(d.r.h.downloads, 1u);
    EXPECT_EQ(d.r.h.starts, 1u);
    EXPECT_EQ(d.n.s.kernel.starts, 1u);
    ASSERT_EQ(d.sink.got[0][0].size(), 3000u);
    EXPECT_EQ(d.sink.got[0][0], d.sink.got[1][0]);
}

TEST(OperationRunTest, AKernelsBlockIsTheLesserOfTheModulesAndTheTransports)
{
    KernelDrive narrow(kernel_plan(), {}, 514);
    EXPECT_EQ(narrow.go(), run_end::complete);
    EXPECT_EQ(narrow.sent[2].size(), 514u) << "the transport's bound";
    EXPECT_EQ(narrow.sent[5].size(), 2u + 2000u - 3u * 512u);

    KernelDrive small(kernel_plan());
    small.script = [&small](const Bytes& q, size_t, Bytes* out, uint32_t*) {
        if (q[0] != 0x34) {
            return false;
        }
        small.n.ask(q);
        *out = {0x74, 0x20, 0x00, 0x42};
        return true;
    };
    EXPECT_EQ(small.go(), run_end::complete);
    ASSERT_GE(small.sent.size(), 3u);
    EXPECT_EQ(small.sent[2].size(), 0x42u) << "the module's bound";

    for (const Bytes& bad :
         {Bytes{0x74, 0x20, 0x00, 0x02},
          Bytes{0x74, 0x50, 0x00, 0x00, 0x00, 0x00, 0x10},
          Bytes{0x74, 0x20, 0x04}, Bytes{0x75, 0x20, 0x04, 0x02}}) {
        KernelDrive d(kernel_plan());
        d.script = answer_to(0x34, bad);
        EXPECT_EQ(d.go(), run_end::bad_reply);
        EXPECT_EQ(d.services(), (std::vector<uint8_t>{0x10, 0x34, 0x37}))
            << "the 34 may have opened one: closed by one 37";
    }
}

TEST(OperationRunTest, AKernelsDownloadCounterWrapsAndStartsAtOne)
{
    KernelDrive d(kernel_plan(), {}, 9);
    EXPECT_EQ(d.go(), run_end::complete);
    std::vector<uint8_t> counters;
    for (const Bytes& q : d.sent) {
        if (q[0] == 0x36 && counters.size() < 286) {
            counters.push_back(q[1]);
        }
    }
    ASSERT_EQ(counters.size(), 286u) << "7 bytes a block";
    EXPECT_EQ(counters[0], 0x01);
    EXPECT_EQ(counters[254], 0xFF);
    EXPECT_EQ(counters[255], 0x00);
    EXPECT_EQ(counters[256], 0x01);
}

TEST(OperationRunTest, AHelperThatEndsAtItsExitIsDownloadedForEachPass)
{
    ecuio_sim_kernel_faults once;
    once.once = true;
    KernelDrive d(kernel_plan(3000, false), once);
    EXPECT_EQ(d.go(), run_end::complete);
    EXPECT_EQ(d.r.h.downloads, 2u);
    EXPECT_EQ(d.r.h.starts, 2u);
    EXPECT_EQ(d.n.s.kernel.downloads, 2u);

    KernelDrive kept(kernel_plan(3000, true), once);
    EXPECT_EQ(kept.go(), run_end::refused) << "declared to stay, it didn't";
    EXPECT_EQ(kept.r.nrc, 0x7F);
    EXPECT_EQ(kept.r.h.downloads, 1u);
}

TEST(OperationRunTest, AHelperThatStaysIsDownloadedOnceForEveryRange)
{
    plan p = kernel_plan(1000);
    step second = p.steps[0];
    second.address = kCode + 0x4000;
    p.steps.push_back(second);
    KernelDrive d(p);
    d.sink = Sink(2);
    EXPECT_EQ(d.go(), run_end::complete);
    EXPECT_EQ(d.r.h.downloads, 1u);
    EXPECT_EQ(d.sink.got[1][1].size(), 1000u);
}

TEST(OperationRunTest, AStartRefusedOrPendingForeverSendsNoUpload)
{
    ecuio_sim_kernel_faults refuse;
    refuse.refuse = true;
    KernelDrive refused(kernel_plan(), refuse);
    EXPECT_EQ(refused.go(), run_end::refused);
    EXPECT_EQ(refused.r.nrc, 0x22);
    EXPECT_EQ(refused.services().back(), 0x31) << "and nothing after it";

    ecuio_sim_kernel_faults storm;
    storm.storm = true;
    KernelDrive pending(kernel_plan(), storm);
    EXPECT_EQ(pending.go(), run_end::no_answer);
    EXPECT_EQ(pending.services().back(), 0x31);
    EXPECT_EQ(pending.count_of(0x35), 0u);
}

TEST(OperationRunTest, ASilentHelperEndsTheRunAfterItsUploadsOne37)
{
    ecuio_sim_kernel_faults silent;
    silent.silent = true;
    KernelDrive d(kernel_plan(), silent);
    EXPECT_EQ(d.go(), run_end::no_answer);
    const std::vector<uint8_t> all = d.services();
    const std::vector<uint8_t> tail(all.end() - 2, all.end());
    EXPECT_EQ(tail, (std::vector<uint8_t>{0x35, 0x37}))
        << "the 35 is asked once and closed by one 37";
}

TEST(OperationRunTest, ADownloadsFaultIsClosedByOne37)
{
    KernelDrive wrong(kernel_plan());
    wrong.script = [](const Bytes& q, size_t, Bytes* out, uint32_t*) {
        if (q[0] != 0x36 || q[1] != 0x02) {
            return false;
        }
        *out = {0x76, 0x05};
        return true;
    };
    EXPECT_EQ(wrong.go(), run_end::bad_reply);
    EXPECT_EQ(wrong.services(),
              (std::vector<uint8_t>{0x10, 0x34, 0x36, 0x36, 0x37}));

    KernelDrive lost(kernel_plan());
    lost.script = answer_to(0x34, {});
    EXPECT_EQ(lost.go(), run_end::no_answer);
    EXPECT_EQ(lost.services(), (std::vector<uint8_t>{0x10, 0x34, 0x37}))
        << "an unanswered 34 may have opened it";

    KernelDrive refused(kernel_plan());
    refused.script = answer_to(0x34, nrc(0x34, 0x70));
    EXPECT_EQ(refused.go(), run_end::refused);
    EXPECT_EQ(refused.services(), (std::vector<uint8_t>{0x10, 0x34}))
        << "a 34 refused opened nothing";

    KernelDrive exit(kernel_plan());
    exit.script = [](const Bytes& q, size_t n, Bytes* out, uint32_t*) {
        if (q != Bytes{0x37} || n != 4) {
            return false;
        }
        *out = nrc(0x37, 0x24);
        return true;
    };
    EXPECT_EQ(exit.go(), run_end::refused);
    EXPECT_EQ(exit.count_of(0x37), 1u) << "the 37 refused was the one";
}

TEST(OperationRunTest, ACancelMidDownloadSendsOne37AndACancelInTheQuietNone)
{
    KernelDrive mid(kernel_plan());
    mid.while_out = [](KernelDrive& d) {
        if (d.sent.back()[0] == 0x36) {
            ecuio_operation::cancel(d.r);
        }
    };
    EXPECT_EQ(mid.go(), run_end::canceled);
    EXPECT_EQ(mid.services(), (std::vector<uint8_t>{0x10, 0x34, 0x36, 0x37}));

    KernelDrive quiet(kernel_plan());
    quiet.while_out = [](KernelDrive& d) { ecuio_operation::cancel(d.r); };
    EXPECT_EQ(quiet.go(), run_end::canceled);
    EXPECT_EQ(quiet.services(), std::vector<uint8_t>{0x10});
}

TEST(OperationRunTest, TheQuietersAnswersAreNotJudged)
{
    for (const Bytes& any : {nrc(0x10, 0x22), Bytes{0x50, 0x82}, Bytes{0x7F}}) {
        KernelDrive d(kernel_plan());
        d.script = answer_to(0x10, any);
        EXPECT_EQ(d.go(), run_end::complete);
    }
    plan p = kernel_plan();
    p.helper.quiet_session = 0;
    KernelDrive none(p);
    EXPECT_EQ(none.go(), run_end::complete);
    EXPECT_EQ(none.services()[0], 0x34) << "a plan without a quieter";
}

TEST(OperationRunTest, ASessionEnteredDropsTheHelper)
{
    plan p = kernel_plan();
    run r;
    ecuio_operation::begin(r, p);
    r.at = run_stage::enter;
    r.h.running = true;
    ecuio_operation::entered(r);
    EXPECT_FALSE(r.h.running);
    EXPECT_EQ(r.at, run_stage::helper);
    EXPECT_EQ(r.h.at, helper_stage::open);
}

TEST(OperationRunTest, AStartAnsweredForAnotherRoutineIsABadReply)
{
    KernelDrive d(kernel_plan());
    d.script = answer_to(0x31, {0x71, 0x01, 0x02, 0x02});
    EXPECT_EQ(d.go(), run_end::bad_reply);
    EXPECT_EQ(d.count_of(0x35), 0u);
    KernelDrive high(kernel_plan());
    high.script = answer_to(0x31, {0x71, 0x01, 0x03, 0x01});
    EXPECT_EQ(high.go(), run_end::bad_reply);
    KernelDrive cut(kernel_plan());
    cut.script = answer_to(0x31, {0x71, 0x01, 0x02});
    EXPECT_EQ(cut.go(), run_end::bad_reply);
}

TEST(OperationRunTest, ACancelWhileThe34IsOutClosesWhatItOpened)
{
    KernelDrive d(kernel_plan());
    d.while_out = [](KernelDrive& k) {
        if (k.sent.back()[0] == 0x34) {
            ecuio_operation::cancel(k.r);
        }
    };
    EXPECT_EQ(d.go(), run_end::canceled);
    EXPECT_EQ(d.services(), (std::vector<uint8_t>{0x10, 0x34, 0x37}));
    KernelDrive refused(kernel_plan());
    refused.while_out = d.while_out;
    refused.script = answer_to(0x34, nrc(0x34, 0x70));
    EXPECT_EQ(refused.go(), run_end::canceled);
    EXPECT_EQ(refused.services(), (std::vector<uint8_t>{0x10, 0x34}))
        << "a 34 refused opened nothing";
}

TEST(OperationRunTest, AKernelReadReadsItsIdentifiersAgainAfterTheLeave)
{
    const Bytes hardware = {0x22, 0xD2, 0x20};
    plan p = kernel_plan(1000);
    p.idents = {{ECUIO_SIM_HARDWARE_DID, ECUIO_DUMP_ROLE_HARDWARE}};
    const auto first = [&](KernelDrive& d) {
        d.script = [](const Bytes& q, size_t n, Bytes* out, uint32_t*) {
            if (q[0] != 0x22 || n > 2) {
                return false;
            }
            *out = {0x62, 0xD2, 0x20, 0x48, 0x57};
            return true;
        };
    };
    const auto after = [&](KernelDrive& d, Bytes answer) {
        first(d);
        Drive::Script before = d.script;
        d.script = [=](const Bytes& q, size_t n, Bytes* out, uint32_t* w) {
            if (q[0] == 0x22 && n > 2) {
                *out = answer;
                return true;
            }
            return before(q, n, out, w);
        };
    };

    KernelDrive same(p);
    after(same, {0x62, 0xD2, 0x20, 0x48, 0x57});
    EXPECT_EQ(same.go(), run_end::complete);
    const std::vector<uint8_t> all = same.services();
    ASSERT_GE(all.size(), 3u);
    EXPECT_EQ(std::vector<uint8_t>(all.end() - 3, all.end()),
              (std::vector<uint8_t>{0x37, 0x10, 0x22}))
        << "the uploads, the leave, then the identifier";
    EXPECT_EQ(same.sent[same.sent.size() - 2], (Bytes{0x10, 0x01}));
    EXPECT_EQ(same.sent.back(), hardware);
    EXPECT_FALSE(same.r.ident_unread);

    for (const Bytes& none :
         {Bytes{}, nrc(0x22, 0x31), nrc(0x22, 0x33), nrc(0x22, 0x7F)}) {
        KernelDrive d(p);
        after(d, none);
        EXPECT_EQ(d.go(), run_end::complete) << "a warning, never a fault";
        EXPECT_TRUE(d.r.ident_unread);
    }

    KernelDrive changed(p);
    after(changed, {0x62, 0xD2, 0x20, 0x48, 0x58});
    EXPECT_EQ(changed.go(), run_end::identity_changed);

    plan dump = plan_of({upload_step(kBase, 64)});
    Drive d(dump);
    d.script = [](const Bytes& q, size_t n, Bytes* out, uint32_t*) {
        if (q[0] != 0x22 || n < 3) {
            return false;
        }
        *out = {};
        return true;
    };
    EXPECT_EQ(d.go(), run_end::no_answer) << "a dump's is still its end";
}
