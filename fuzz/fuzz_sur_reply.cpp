// File/layer: fuzz/fuzz_sur_reply.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_sur_reply
//
// The surrogate client's reading of the child's replies
// (core/src/j2534_sur_reply.h). The input is a script, then the pipe's
// bytes: [count] then [kind, cap] for each request, count 1 to 8. A
// kind's low two bits pick a READ, an IOCTL, a text reply or a plain
// one; 0x04 makes its reply late (its first read times out with nothing
// taken, so the next request drains it, as the client does), 0x08 fails
// the pipe at its first read. cap is the IOCTL's output room. Each
// request's ID is one past the last. A read takes what the pipe holds: a
// short one is cut, an empty one times out.

#include <ecuio/core.h>
#include <ecuio/surrogate_protocol.h>

#include "j2534_sur_reply.h" /* core/src: pure, no platform headers */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <vector>

namespace {

/* fuzz/seeds.py writes replies in this layout */
static_assert(sizeof(ecuio_sur_response_t) == 328, "seeds.py's response");
static_assert(sizeof(ecuio_pt_rx_t) == 4140, "seeds.py's frame");
/* A batch holds one frame, so every READ reads the pipe: Reader::read's
 * !called branch (a frame out of the batch) can't run, and a frame past
 * the cap is judged by check_count alone. A larger cap needs both. */
static_assert(ECUIO_SUR_MAX_FRAMES_PER_READ == 1, "Reader::read's cap of one");

constexpr uint8_t kRead = 0;
constexpr uint8_t kIoctl = 1;
constexpr uint8_t kText = 2;
constexpr uint8_t kLate = 0x04;
constexpr uint8_t kFail = 0x08;
constexpr size_t kResp = sizeof(ecuio_sur_response_t);
constexpr size_t kFrame = sizeof(ecuio_pt_rx_t);

struct Pipe {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    bool late = false; /* the next read times out, nothing taken */
    bool fail = false; /* the next read fails, nothing taken */

    sur_piece read(void* buf, size_t len)
    {
        if (fail) {
            fail = false;
            return sur_piece::failed;
        }
        if (late) {
            late = false;
            return sur_piece::timeout;
        }
        if (pos == size) {
            return sur_piece::timeout;
        }
        const size_t n = size - pos < len ? size - pos : len;
        memcpy(buf, data + pos, n);
        pos += n;
        return n == len ? sur_piece::whole : sur_piece::cut;
    }
};

/* What any verdict leaves in the pipe: a timeout took nothing, a cut
 * took the rest, a count judged before its frames took the response
 * alone */
void check_taken(sur_reply v, const Pipe& p, size_t before)
{
    const size_t taken = p.pos - before;
    if (v == sur_reply::timeout && taken != 0) {
        abort();
    }
    if ((v == sur_reply::cut || v == sur_reply::frames_cut) &&
        p.pos != p.size) {
        abort();
    }
    if (v == sur_reply::frames_unasked && taken != kResp) {
        abort();
    }
}

/* A response read whole and claiming frames it may not carry (past cap,
 * which is 0 for a reply that takes none) stops the reading there */
void check_count(sur_reply v, const ecuio_sur_response_t& r, uint32_t cap)
{
    const bool head = v != sur_reply::timeout && v != sur_reply::cut &&
                      v != sur_reply::failed;
    if (head && r.num_frames > cap && v != sur_reply::frames_unasked) {
        abort();
    }
}

/* A whole reply took its response and its frames, at most the cap */
void check_whole(const ecuio_sur_response_t& r, const Pipe& p, size_t before)
{
    if (r.num_frames > ECUIO_SUR_MAX_FRAMES_PER_READ ||
        p.pos - before != kResp + r.num_frames * kFrame) {
        abort();
    }
}

/* A late reply drained: any opcode's, frames up to the cap */
sur_reply drain(Pipe& p, uint32_t id)
{
    ecuio_sur_response_t stale = {};
    ecuio_pt_rx_t scratch = {};
    const size_t before = p.pos;
    const sur_reply v = sur_drop_reply(
        [&p](void* b, size_t n) { return p.read(b, n); }, id, &stale, &scratch);
    check_taken(v, p, before);
    check_count(v, stale, ECUIO_SUR_MAX_FRAMES_PER_READ);
    if (v == sur_reply::whole || v == sur_reply::wrong_id) {
        check_whole(stale, p, before);
        if ((stale.request_id == id) != (v == sur_reply::whole)) {
            abort();
        }
    }
    if (v == sur_reply::frames_unasked &&
        stale.num_frames <= ECUIO_SUR_MAX_FRAMES_PER_READ) {
        abort();
    }
    return v;
}

void check_ioctl(const ecuio_sur_response_t& r, uint8_t cap)
{
    uint8_t room[256];
    memset(room, 0xA5, sizeof(room));
    uint32_t len = 0xA5A5A5A5u;
    const bool ok = sur_ioctl_payload(r, room, cap, &len);
    if (ok != (r.data_len <= ECUIO_SUR_IOCTL_DATA_MAX)) {
        abort();
    }
    const size_t copied = !ok ? 0 : r.data_len < cap ? r.data_len : cap;
    if ((!ok && len != 0xA5A5A5A5u) || (ok && len != r.data_len) ||
        memcmp(room, r.data, copied) != 0) {
        abort();
    }
    for (size_t i = copied; i < sizeof(room); ++i) {
        if (room[i] != 0xA5) {
            abort();
        }
    }
}

void check_texts(const ecuio_sur_response_t& r)
{
    for (int f = 0; f < ECUIO_SUR_TEXT_FIELDS; ++f) {
        char t[ECUIO_SUR_TEXT_MAX];
        memset(t, 0x5A, sizeof(t));
        sur_reply_text(r, f, t);
        if (memchr(t, '\0', sizeof(t)) == nullptr ||
            memcmp(t, r.text[f], sizeof(t) - 1) != 0) {
            abort();
        }
    }
}

/* One request other than a READ, read as the client reads it */
sur_reply ask(Pipe& p, uint32_t id, uint8_t kind, uint8_t cap)
{
    ecuio_sur_response_t r = {};
    const size_t before = p.pos;
    const sur_reply v = sur_take_reply(
        [&p](void* b, size_t n) { return p.read(b, n); }, id, &r, nullptr);
    check_taken(v, p, before);
    check_count(v, r, 0);
    if (v == sur_reply::frames_unasked && r.num_frames == 0) {
        abort();
    }
    if (v == sur_reply::whole || v == sur_reply::wrong_id) {
        if (r.num_frames != 0 || p.pos - before != kResp ||
            (r.request_id == id) != (v == sur_reply::whole)) {
            abort();
        }
    }
    if (v == sur_reply::whole && (kind & 3u) == kIoctl) {
        check_ioctl(r, cap);
    }
    if (v == sur_reply::whole && (kind & 3u) == kText) {
        check_texts(r);
    }
    return v;
}

/* A READ through sur_read_msg: a frame is the pipe's bytes, marked when
 * a loss came before it; an empty batch is judged by its status */
struct Reader {
    sur_batch batch;
    bool loss_owed = false;

    sur_reply read(Pipe& p, uint32_t id)
    {
        sur_reply v = sur_reply::whole;
        bool called = false;
        size_t before = p.pos;
        uint32_t frames = 0;
        int32_t status = 0;
        const auto transact = [&](ecuio_sur_response_t* r,
                                  ecuio_pt_rx_t* into) {
            called = true;
            before = p.pos;
            v = sur_take_reply([&p](void* b, size_t n) { return p.read(b, n); },
                               id, r, into);
            check_taken(v, p, before);
            check_count(v, *r, ECUIO_SUR_MAX_FRAMES_PER_READ);
            if (v == sur_reply::whole || v == sur_reply::wrong_id) {
                check_whole(*r, p, before);
            }
            frames = r->num_frames;
            status = r->status;
            return v == sur_reply::whole;
        };
        ecuio_pt_rx_t out = {};
        bool got = false;
        const ecuio_status_t st = sur_read_msg(&batch, transact, &out, &got);
        if (!called) {
            abort(); /* at the cap of one a READ always reads the pipe */
        }
        if (v != sur_reply::whole) {
            if (st != ECUIO_ERR_TRANSPORT_HARD_FAILURE || got) {
                abort();
            }
            return v;
        }
        if (got != (frames > 0)) {
            abort();
        }
        if (got) {
            check_frame(p, before, out);
        } else {
            check_empty(st, status);
        }
        return v;
    }

    void check_frame(const Pipe& p, size_t before, const ecuio_pt_rx_t& out)
    {
        ecuio_pt_rx_t sent;
        memcpy(&sent, p.data + before + kResp, kFrame);
        if (loss_owed) {
            sent.rx_status |= ECUIO_PT_BUFFER_OVERFLOW;
        }
        loss_owed = false;
        if (memcmp(&sent, &out, kFrame) != 0) {
            abort();
        }
    }

    void check_empty(ecuio_status_t st, int32_t status)
    {
        if (status == static_cast<int32_t>(ECUIO_PT_STATUS_BUFFER_OVERFLOW)) {
            loss_owed = true;
        }
        if (batch.loss_pending != loss_owed || st != empty_verdict(status)) {
            abort();
        }
    }

    /* J2534's statuses for a READ with no message: a quiet vehicle,
     * nothing yet, a loss (the next frame marked), else a hard failure */
    static ecuio_status_t empty_verdict(int32_t status)
    {
        switch (status) {
        case ECUIO_PT_STATUS_TIMEOUT:
            return ECUIO_ERR_VEHICLE_TIMEOUT;
        case ECUIO_PT_STATUS_NOERROR:
        case ECUIO_PT_STATUS_BUFFER_EMPTY:
        case ECUIO_PT_STATUS_BUFFER_OVERFLOW:
            return ECUIO_OK;
        default:
            return ECUIO_ERR_TRANSPORT_HARD_FAILURE;
        }
    }
};

/* Each request's verdict and the bytes it took; the handle ends at a
 * verdict out of step, a failed pipe, or a late reply's drain timing out */
std::vector<uint64_t> run(const uint8_t* data, size_t size)
{
    const auto at = [&](size_t i) -> uint8_t {
        return i < size ? data[i] : uint8_t{0};
    };
    const size_t count = 1u + at(0) % 8u;
    const size_t script = 1 + 2 * count < size ? 1 + 2 * count : size;
    Pipe p;
    p.data = data + script;
    p.size = size - script;
    auto reader = std::make_unique<Reader>();
    std::vector<uint64_t> trace;
    bool pending = false;
    for (size_t k = 0; k < count; ++k) {
        const uint8_t kind = at(1 + 2 * k);
        const uint8_t cap = static_cast<uint8_t>(at(2 + 2 * k) % 128u);
        const auto id = static_cast<uint32_t>(k + 1);
        if (pending) {
            const sur_reply d = drain(p, id - 1);
            trace.push_back(0x100u | static_cast<uint64_t>(d));
            if (d != sur_reply::whole) {
                break;
            }
            pending = false;
        }
        p.late = (kind & kLate) != 0;
        p.fail = (kind & kFail) != 0;
        const size_t before = p.pos;
        const sur_reply v =
            (kind & 3u) == kRead ? reader->read(p, id) : ask(p, id, kind, cap);
        trace.push_back(static_cast<uint64_t>(v) << 32 | (p.pos - before));
        if (v == sur_reply::timeout) {
            pending = true;
        } else if (v != sur_reply::whole) {
            break;
        }
    }
    trace.push_back(p.pos);
    return trace;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    /* A heap copy of the input's exact size, so ASan sees a read past it */
    std::unique_ptr<uint8_t[]> copy(new uint8_t[size > 0 ? size : 1]);
    if (size > 0) {
        memcpy(copy.get(), data, size);
    }
    if (run(copy.get(), size) != run(copy.get(), size)) {
        abort();
    }
    return 0;
}
