// File/layer: fuzz/fuzz_operation.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_operation
//
// A dump's run (core/src/operation_run.h) stepped by the input: a plan of
// one to three steps and up to two identifiers, then a script that
// answers each request (the right bytes, a silence, a refusal with any
// code, a pending answer with any wait, one bit wrong, one byte short or
// long, or bytes of the script's own), cancels, fills the sink and
// refuses a session. The module is the harness's own: every byte a
// function of its address. The harness holds the run to its rules by its
// own reading of them, and aborts on any break:
//   - a request is a 22 of a declared identifier, or inside the tier-3
//     read ceiling: a 23 of a declared read step's shape that stays in
//     its range and its chunk, a 35 of a declared upload step whole, a
//     36 with the next counter while a block is owed, a 37;
//   - after a request that got no answer of its shape, the next is the
//     same again or a 37: a chunk or an identifier three times at most,
//     a 35 and a block once, and an upload one 37 at most;
//   - no request that brings data goes out while the sink is full;
//   - every byte reaches the sink once, in order, inside its range;
//   - the run ends, and ends complete only when every range was handed
//     over twice and equal, the identifiers read, and no cancel asked.

#include "operation_run.h" /* core/src: the machine */
#include "tier3_ceiling.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

namespace {

using namespace ecuio_operation;
using bytes = std::vector<uint8_t>;

constexpr int kStepsMost = 400000;
constexpr uint32_t kFirstDid = 0xD220;
constexpr uint64_t kRamAt = 0x80000;
constexpr uint64_t kRamBytes = 0x2000;
constexpr uint16_t kRoutine = 0x0201;

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

/* The bytes left of the input, taken from the front; 0 once it is out */
struct reader {
    const uint8_t* p = nullptr;
    size_t n = 0;
    uint8_t byte()
    {
        if (n == 0) {
            return 0;
        }
        --n;
        return *p++;
    }
    /* Two bytes, the low one first, the high one cut to mask */
    uint32_t pair(uint8_t mask)
    {
        const uint32_t low = byte();
        return low | static_cast<uint32_t>(byte() & mask) << 8;
    }
};

uint8_t memory_at(uint64_t address)
{
    return static_cast<uint8_t>(address * 31 + (address >> 8) + 7);
}

bool kernel_from(reader& in, plan& p, uint32_t* wire_max)
{
    kernel& k = p.helper;
    const uint8_t shape = in.byte();
    k.stays = (shape & 1) == 0;
    k.quiet_session = (shape & 2) != 0 ? 0x82 : 0;
    k.ram_address = k.address = kRamAt;
    k.ram_bytes = kRamBytes;
    k.routine = kRoutine;
    k.starts = {kRoutine};
    k.blob.resize(1 + in.pair(0x1F));
    for (size_t i = 0; i < k.blob.size(); ++i) {
        k.blob[i] = memory_at(i + 0x5A);
    }
    *wire_max = in.pair(0x0F);
    return kernel_fault(p) == nullptr;
}

/* The plan the input's next bytes make, or one with no step when they
 * make none a loader would take */
plan plan_from(reader& in, uint32_t* wire_max)
{
    plan p;
    const uint8_t head = in.byte();
    const unsigned steps = 1 + (head & 0x0Fu) % 3u;
    p.has_kernel = (head & 0x80) != 0;
    for (unsigned i = 0; i < ((head >> 4) & 3u) % 3u; ++i) {
        p.idents.push_back({kFirstDid + i, ECUIO_DUMP_ROLE_SOFTWARE});
    }
    for (unsigned i = 0; i < steps; ++i) {
        const uint8_t shape = in.byte();
        step s;
        s.kind = (shape & 1) != 0 ? ECUIO_STEP_UPLOAD : ECUIO_STEP_READ;
        s.session = (shape & 2) != 0 || p.has_kernel
                        ? ECUIO_STEP_SESSION_PROGRAMMING
                        : ECUIO_STEP_SESSION_EXTENDED;
        s.address_bytes = (shape & 4) != 0 ? 3 : 4;
        s.size_bytes = static_cast<uint8_t>(1 + (shape >> 3) % 4);
        s.address = 0x10000 + 0x4000 * i;
        s.bytes = 1 + in.pair(0x1F);
        s.chunk = s.kind == ECUIO_STEP_READ ? 1 + in.pair(0x0F) : 0;
        if (step_fault(s) != nullptr) {
            return plan{};
        }
        p.steps.push_back(s);
    }
    if (p.has_kernel && !kernel_from(in, p, wire_max)) {
        return plan{};
    }
    return p;
}

/* What the run hands over: each byte once, in order, inside its range */
struct sink {
    const plan* p = nullptr;
    bool full = false;
    bool differ = false;
    uint32_t idents = 0;
    bytes got[2][3];

    bool room(size_t) const { return !full; }
    void data(uint32_t step, uint32_t pass, uint64_t offset, const uint8_t* d,
              size_t n)
    {
        check(step < p->steps.size() && pass < 2 && n > 0);
        bytes& to = got[pass][step];
        check(offset == to.size() && offset + n <= p->steps[step].bytes);
        check(pass == 0 || got[0][step].size() == p->steps[step].bytes);
        to.insert(to.end(), d, d + n);
        for (size_t i = 0; pass == 1 && i < n; ++i) {
            differ = differ || got[0][step][offset + i] != d[i];
        }
    }
    void ident(uint32_t index, const uint8_t*, size_t n)
    {
        check(index == idents && index < p->idents.size() && n >= 1 &&
              n <= ECUIO_DUMP_IDENT_MAX);
        ++idents;
    }
};

/* The harness's own module, and its reading of what may be asked of it */
struct model {
    const plan* p = nullptr;
    bool open = false;   /* a 35 went out and no 37 since */
    bool owed = false;   /* the open upload's next block may be asked */
    bool exited = false; /* the upload's 37 has gone */
    bool settled = true; /* the last request got an answer of its shape */
    bool loose = false;  /* the 75 wasn't this model's: its block is the
                             run's to know, so a 36's length isn't judged */
    bool writing = false; /* the transfer is the helper's download */
    bool begun = false;   /* a request past the quieter went out */
    bool quieted = false;
    bool running = false; /* the helper started, its session kept */
    bool gone = false;    /* the plan's session was left */
    uint32_t wire = 0;
    uint8_t counter = 0;
    uint32_t block = 0;
    uint32_t tries = 0;
    uint64_t at = 0;
    uint64_t left = 0;
    bytes last;

    static uint64_t field(const uint8_t* d, unsigned n)
    {
        uint64_t v = 0;
        for (unsigned i = 0; i < n; ++i) {
            v = v << 8 | d[i];
        }
        return v;
    }

    /* The step whose shape and range a 23's or a 35's fields at d name */
    const step* named(const uint8_t* d, size_t n, uint32_t kind,
                      uint64_t* address, uint64_t* size) const
    {
        for (const step& s : p->steps) {
            if (s.kind != kind || n != 1u + s.address_bytes + s.size_bytes ||
                d[0] != ((s.size_bytes << 4) | s.address_bytes)) {
                continue;
            }
            *address = field(d + 1, s.address_bytes);
            *size = field(d + 1 + s.address_bytes, s.size_bytes);
            if (*address >= s.address && *size >= 1 &&
                *address + *size <= s.address + s.bytes) {
                return &s;
            }
        }
        return nullptr;
    }

    /* A 35's judging, and the 75 that names block_named */
    void open_upload(const bytes& q, uint16_t block_named, bytes* out)
    {
        uint64_t address = 0;
        uint64_t size = 0;
        const step* s = q.size() > 2 && q[1] == 0x00
                            ? named(q.data() + 2, q.size() - 2,
                                    ECUIO_STEP_UPLOAD, &address, &size)
                            : nullptr;
        check(s != nullptr && address == s->address && size == s->bytes &&
              !open && tries == 1);
        open = owed = true;
        writing = false;
        exited = false;
        counter = 1;
        block = block_named >= 3 ? block_named - 2u : 0;
        at = address;
        left = size;
        *out = {0x75, 0x20, static_cast<uint8_t>(block_named >> 8),
                static_cast<uint8_t>(block_named)};
    }

    void judge_helper(const bytes& q, bool write, uint16_t block_named,
                      bytes* out)
    {
        const kernel& k = p->helper;
        check(p->has_kernel && !gone && (write || q[0] == 0x37) &&
              ecuio_tier3_write_allowed(q[0], q.size() > 1 ? q[1] : 0));
        if (q[0] == 0x34) {
            bytes want = {0x34, 0x00, 0x44};
            for (int i = 3; i >= 0; --i) {
                want.push_back(static_cast<uint8_t>(k.address >> (8 * i)));
            }
            for (int i = 3; i >= 0; --i) {
                want.push_back(static_cast<uint8_t>(k.blob.size() >> (8 * i)));
            }
            check(q == want && !open && !running && tries == 1);
            open = owed = writing = true;
            exited = false;
            counter = 1;
            uint32_t most = block_named < wire ? block_named : wire;
            most = most < kWireMost ? most : kWireMost;
            block = most >= 3 ? most - 2 : 0;
            at = 0;
            left = k.blob.size();
            *out = {0x74, 0x20, static_cast<uint8_t>(block_named >> 8),
                    static_cast<uint8_t>(block_named)};
        } else if (q[0] == 0x36) {
            const size_t n = q.size() - 2;
            check(q.size() >= 3 && open && owed && q[1] == counter &&
                  tries == 1 && n <= left &&
                  (loose || n == (std::min)(left, uint64_t{block})) &&
                  memcmp(q.data() + 2, k.blob.data() + at, n) == 0);
            ++counter;
            at += n;
            left -= n;
            *out = {0x76, q[1]};
        } else if (q[0] == 0x37) {
            check(q.size() == 1 && !exited);
            exited = true;
            open = false;
            *out = {0x77};
        } else {
            check(q.size() == 4 && q[1] == 0x01 &&
                  q[2] == static_cast<uint8_t>(k.routine >> 8) &&
                  q[3] == static_cast<uint8_t>(k.routine) && !open && exited &&
                  left == 0 && tries == 1);
            *out = {0x71, 0x01, q[2], q[3]};
        }
    }

    /* Aborts unless the request is one the run may send now; the right
     * answer into out */
    void judge(const bytes& q, bool functional, bool write,
               uint16_t block_named, bytes* out)
    {
        const bool next_unread =
            gone && q.size() == 3 && last.size() == 3 && q[0] == 0x22 &&
            last[0] == 0x22 &&
            field(q.data() + 1, 2) == field(last.data() + 1, 2) + 1;
        check(!q.empty() &&
              (settled || q == last || q[0] == 0x37 || next_unread));
        check(functional == (q[0] == 0x10));
        tries = settled || q != last ? 1 : tries + 1;
        last = q;
        check(tries <= kAsksMost);
        uint64_t address = 0;
        uint64_t size = 0;
        if (q[0] == 0x10) {
            check(q.size() == 2 && q[1] == p->helper.quiet_session &&
                  q[1] != 0 && !begun && !quieted);
            quieted = true;
            *out = {0x50, q[1]};
            return;
        }
        check(!p->has_kernel || p->helper.quiet_session == 0 || quieted);
        begun = true;
        if (q[0] == 0x34 || q[0] == 0x31 ||
            (writing && (q[0] == 0x36 || q[0] == 0x37))) {
            judge_helper(q, write, block_named, out);
            return;
        }
        check(!write);
        if (q[0] == 0x22) {
            const uint64_t did = q.size() == 3 ? field(q.data() + 1, 2) : 0;
            check(did >= kFirstDid && did < kFirstDid + p->idents.size());
            *out = {0x62, q[1], q[2], 0x48, 0x56, q[2], 0x01};
            return;
        }
        check(ecuio_tier3_read_allowed(q[0], 0));
        check(!p->has_kernel || running || (q[0] != 0x23 && q[0] != 0x35));
        if (q[0] == 0x35) {
            open_upload(q, block_named, out);
            return;
        }
        if (q[0] == 0x37) {
            check(q.size() == 1 && !exited);
            exited = true;
            open = false;
            *out = {0x77};
            return;
        }
        if (q[0] == 0x23) {
            const step* s = named(q.data() + 1, q.size() - 1, ECUIO_STEP_READ,
                                  &address, &size);
            check(s != nullptr && size <= s->chunk && !open);
            *out = {0x63};
        } else {
            check(q[0] == 0x36 && q.size() == 2 && open && owed &&
                  (loose || left > 0) && q[1] == counter && tries == 1);
            address = at;
            size = left < block ? left : block;
            *out = {0x76, q[1]};
            ++counter;
            at += size;
            left -= size;
        }
        for (uint64_t i = 0; i < size; ++i) {
            out->push_back(memory_at(address + i));
        }
    }

    /* Whether got is an answer the run may take for the last request,
     * whose right answer is right: by the machine's rules read apart */
    void settle(const bytes& right, const bytes* got)
    {
        const uint8_t sid = last[0];
        if (sid == 0x10) {
            settled = true;
        } else if (got == nullptr || got->empty()) {
            settled = false;
        } else if (sid == 0x31) {
            settled =
                got->size() >= 4 && memcmp(got->data(), right.data(), 4) == 0;
            running = settled;
        } else if (sid == 0x36 && writing) {
            settled =
                got->size() >= 2 && (*got)[0] == 0x76 && (*got)[1] == right[1];
        } else if (sid == 0x22) {
            settled = got->size() >= 4 &&
                      got->size() <= 3 + ECUIO_DUMP_IDENT_MAX &&
                      memcmp(got->data(), right.data(), 3) == 0;
        } else if (sid == 0x34 || sid == 0x35 || sid == 0x37) {
            settled = (*got)[0] == right[0];
            running = running &&
                      (sid != 0x37 || writing || !settled || p->helper.stays);
        } else {
            settled = got->size() == right.size() && (*got)[0] == right[0] &&
                      (sid == 0x23 || (*got)[1] == right[1]);
        }
        if (sid == 0x34 || sid == 0x35) {
            loose = got == nullptr || *got != right;
        }
        settled = settled || (sid == 0x36 && loose);
        owed = owed && (sid != 0x36 || settled);
    }
};

/* The script's answer in place of the right one in *answer: false for a
 * silence. *waited is the time since the request went out. */
bool scripted(reader& in, uint8_t sid, bytes* answer, uint32_t* waited)
{
    const uint8_t op = in.byte();
    *waited = 10;
    switch (op % 8) {
    case 3:
        return false;
    case 4:
        *answer = {0x7F, sid, in.byte()};
        *waited = in.byte() * 300u;
        return true;
    case 5:
        answer->back() ^= static_cast<uint8_t>(1u << (op >> 5));
        return true;
    case 6:
        if ((op & 0x80) != 0) {
            answer->push_back(0x00);
        } else {
            answer->pop_back();
        }
        return true;
    case 7:
        answer->clear();
        for (unsigned i = in.byte() % 16u; i > 0; --i) {
            answer->push_back(in.byte());
        }
        return true;
    default:
        return true;
    }
}

/* One request sent and settled */
void exchange(reader& in, run& r, sink& out, model& m, const run_request& q,
              uint16_t block_named, uint8_t control, bool* canceled)
{
    const bytes request = q.wide != nullptr
                              ? bytes(q.wide, q.wide + q.wide_size)
                              : bytes(q.bytes, q.bytes + q.size);
    check(q.wide != nullptr ? q.size == 0 && q.wide_size <= kWireMost
                            : q.size >= 1 && q.size <= kRequestMax);
    check(!out.full || (request[0] != 0x23 && (request[0] != 0x36 || q.write)));
    bytes answer;
    m.judge(request, q.functional, q.write, block_named, &answer);
    const bytes right = answer;
    uint32_t waited = 0;
    const bool answered = scripted(in, request[0], &answer, &waited);
    if (control == 0xFE) { /* a cancel while the request is out */
        *canceled = *canceled || r.at != run_stage::closing;
        cancel(r);
    }
    m.settle(right, answered ? &answer : nullptr);
    if (!answered || answer.empty()) {
        silence(r);
    } else if (reply(r, answer.data(), answer.size(), waited, out) ==
               run_wait::keep_waiting) {
        check(answer.size() == 3 && answer[2] == 0x78 &&
              waited < kPendingMostMs);
        silence(r); /* the answer never comes */
    }
    /* The sink's word, over the machine's complete too */
    if (out.differ &&
        (r.at != run_stage::done || r.result == run_end::complete)) {
        stop(r, run_end::differ);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    reader in{data, size};
    const auto block_named = static_cast<uint16_t>(in.pair(0x0F));
    uint32_t wire_max = 0;
    const plan p = plan_from(in, &wire_max);
    if (p.steps.empty()) {
        return 0;
    }
    run r;
    begin(r, p, wire_max);
    sink out;
    out.p = &p;
    model m;
    m.p = &p;
    m.wire = wire_max;
    bool canceled = false;
    for (int steps = 0;; ++steps) {
        check(steps < kStepsMost);
        const uint8_t control = in.byte();
        if (control == 0xFF && r.at != run_stage::done) {
            canceled = canceled || r.at != run_stage::closing;
            cancel(r);
        }
        out.full = (control & 0xC0) == 0x40;
        run_request q;
        const run_action action = next(r, q, out);
        if (action == run_action::done) {
            break;
        }
        if (action == run_action::hold) {
            check(out.full);
        } else if (action == run_action::send) {
            exchange(in, r, out, m, q, block_named, control, &canceled);
        } else if (action == run_action::leave) {
            check(p.has_kernel && !m.gone && !m.open);
            m.gone = true;
            m.running = false;
            left(r);
        } else {
            check(q.session == ECUIO_STEP_SESSION_EXTENDED ||
                  q.session == ECUIO_STEP_SESSION_PROGRAMMING);
            m.running = false;
            if ((control & 0x3F) == 0x3F) {
                not_entered(r, run_end::unlock_refused);
            } else {
                entered(r);
            }
        }
    }
    run_request after;
    check(next(r, after, out) == run_action::done &&
          r.result != run_end::none && r.handed <= total_bytes(r));
    if (r.result == run_end::complete) {
        check(!canceled && !out.differ && r.handed == total_bytes(r) &&
              out.idents == p.idents.size() && !m.open &&
              (!p.has_kernel || m.gone));
        for (size_t i = 0; i < p.steps.size(); ++i) {
            check(out.got[0][i].size() == p.steps[i].bytes &&
                  out.got[0][i] == out.got[1][i]);
        }
    }
    return 0;
}
