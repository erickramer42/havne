// File/layer: core/src/sim.cpp | C++17, Qt-free, every platform; the bus
//             is sim_bus.cpp's (Linux), a refusal elsewhere
// Testing: UNIT: SimTest (test_socketcan); SimRecordingTest, SimAskTest
//          (test_obd2); HARNESS: SimBusTest (test_vcan)
//
// The simulated ECU (ADR-0046): its URI and profile planned, its ECUs
// built (the engine ECU, then modules=2's module or the profile's, a node
// each), what a node answers and the fate a fault gives it. A replay://
// recording is read whole at create (ecuio_sim_load_recording; a stretch
// of it alone under from= and to=, recording_stretch.h) and only read
// after.
//
// An unbound sim (ecuio_sim_create_unbound, ADR-0055) has the same ECUs
// and answers with no bus, worker or socket: ecuio_sim_ask ticks its
// clock to the caller's time and returns each ECU's whole reply, its
// fate as the time it starts and a flag.

#include "sim_impl.h"

#include "error_detail.h"
#include "frames_csv.h"
#include "sim_dids.h"

#include "ecuio/profile.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace ecuio_sim_impl {

/* The reply's send is over: count it */
void sent(ecuio_sim* s, const sim_sending& r)
{
    switch (r.fate) {
    case ecuio_sim_fate::bad_sequence:
    case ecuio_sim_fate::stalled:
        break; /* counted when decided: never whole */
    default:
        s->replies.fetch_add(1, std::memory_order_relaxed);
        if (r.len == 3 && r.data[0] == 0x7F) {
            s->refused.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }
}

/* n's answer to a whole request, into out: the profile's DIDs, the
 * engine ECU's or modules=2's module's; 0 for none */
uint32_t answer(ecuio_sim* s, sim_node& n, const uint8_t* req, uint32_t len,
                bool functional, uint8_t* out)
{
    const bool has_extended =
        n.module == 0 && !n.from_set && s->ecu.recording == nullptr;
    if (!functional && s->ecu.recording == nullptr) {
        bool handled = false;
        const uint32_t m = ecuio_sim_session_answer(
            n.session, has_extended, s->ecu.time_us, req, len, out, &handled);
        if (handled) {
            return m;
        }
    }
    if (n.from_set && (n.module != 0 || (len > 0 && req[0] == 0x22))) {
        const ecuio_sim* self = s;
        return ecuio_sim_did_answer(
            *s->set, n.reply_id, req, len, functional,
            [self](uint32_t i) {
                return self->values[i].load(std::memory_order_relaxed);
            },
            out, ECUIO_SIM_REPLY_MAX);
    }
    if (n.module == 0) {
        return ecuio_sim_answer(s->ecu, n.session, req, len, functional, out);
    }
    return ecuio_sim_module_answer(req, len, functional, out);
}

/* The fate of n's reply to req, a clear and the fault counted */
ecuio_sim_fate decide(ecuio_sim* s, sim_node& n, const uint8_t* req,
                      uint32_t len, const uint8_t* reply, uint32_t reply_len)
{
    if (n.module == 0 && reply[0] == 0x44) {
        s->clears.fetch_add(1, std::memory_order_relaxed);
    }
    const ecuio_sim_fate fate =
        ecuio_sim_next_fate(n.faults, n.counts, req, len, reply_len);
    switch (fate) {
    case ecuio_sim_fate::silent:
        s->silent.fetch_add(1, std::memory_order_relaxed);
        break;
    case ecuio_sim_fate::late:
        s->late.fetch_add(1, std::memory_order_relaxed);
        break;
    case ecuio_sim_fate::pending:
        s->pending.fetch_add(1, std::memory_order_relaxed);
        break;
    case ecuio_sim_fate::bad_sequence:
        s->bad_sequence.fetch_add(1, std::memory_order_relaxed);
        break;
    case ecuio_sim_fate::stalled:
        s->stalled.fetch_add(1, std::memory_order_relaxed);
        break;
    case ecuio_sim_fate::intact:
        break;
    }
    return fate;
}

} // namespace ecuio_sim_impl

namespace {

/* What both creates take from the URI: its config and its profile's
 * set, held */
struct sim_plan {
    ecuio_sim_config config;
    const ecuio_signal_set_t* set = nullptr;
    ecuio_profile_hold hold;
    ecuio_sim_module_plan modules;
};

/* The URI and its profile, on every platform; bus= only with a bus */
ecuio_status_t plan_sim(const char* uri, bool unbound, sim_plan* p)
{
    char why[256] = {};
    if (ecuio_sim_parse_uri(uri, &p->config, why, sizeof why) !=
        ecuio_sim_uri_result::ok) {
        ecuio_detail_set("%s", why);
        return ECUIO_ERR_BAD_URI;
    }
    if (unbound && p->config.timing.bus_kbps != 0) {
        ecuio_detail_set("bus: times the frames on a bus, and an unbound "
                         "sim has none: its caller times them");
        return ECUIO_ERR_BAD_URI;
    }
    if (p->config.profile_len != 0) {
        const std::string name(p->config.profile, p->config.profile_len);
        const ecuio_status_t found =
            ecuio_profile_acquire(name.c_str(), &p->set);
        p->hold.set = p->set;
        if (found != ECUIO_OK) {
            ecuio_detail_set("profile: no profile '%s' is registered",
                             name.c_str());
            return ECUIO_ERR_UNKNOWN_PROFILE;
        }
        if (p->set != nullptr &&
            !ecuio_sim_plan_modules(*p->set, &p->modules, why, sizeof why)) {
            ecuio_detail_set("profile '%s': %s", name.c_str(), why);
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    /* An aim needs the modules, known only once the profile is */
    uint32_t ids[1 + ECUIO_MAX_CHANNELS] = {ECUIO_SIM_REPLY_ID};
    uint32_t count = 1;
    if (p->config.second_module) {
        ids[count++] = ECUIO_SIM_MODULE_REPLY_ID;
    }
    for (uint32_t i = 0; i < p->modules.count; ++i) {
        ids[count++] = p->modules.rx[i];
    }
    if (!ecuio_sim_aims_land(p->config.aimed, ids, count, why, sizeof why) ||
        !ecuio_sim_timing::aims_land(p->config.timing, ids, count, why,
                                     sizeof why)) {
        ecuio_detail_set("%s", why);
        return ECUIO_ERR_BAD_URI;
    }
    p->config.profile = nullptr; /* the URI's, gone after create */
    p->config.profile_len = 0;
    return ECUIO_OK;
}

/* s's ECUs as p plans them: the engine ECU, then modules=2's module or
 * the profile's, each with its faults and timing; false out of memory */
bool build_nodes(ecuio_sim* s, const sim_plan& p)
{
    const ecuio_sim_config& config = p.config;
    const uint32_t nodes = p.set != nullptr       ? 1 + p.modules.count
                           : config.second_module ? 2u
                                                  : 1u;
    s->nodes.reset(new (std::nothrow) sim_node[nodes]);
    if (s->nodes == nullptr) {
        return false;
    }
    s->nodes[0].rx.timed = true;
    if (config.second_module) {
        sim_node& m = s->nodes[1];
        m.request_id = ECUIO_SIM_MODULE_REQUEST_ID;
        m.reply_id = ECUIO_SIM_MODULE_REPLY_ID;
        m.module = 1;
        m.rx.timed = true;
        s->node_count = 2;
    }
    if (p.set != nullptr) {
        s->nodes[0].from_set = p.modules.engine;
        for (uint32_t i = 0; i < p.modules.count; ++i) {
            sim_node& m = s->nodes[1 + i];
            m.request_id = p.modules.tx[i];
            m.reply_id = p.modules.rx[i];
            m.module = 1;
            m.from_set = true;
            m.rx.timed = true;
        }
        s->node_count = 1 + static_cast<int>(p.modules.count);
    }
    for (int i = 0; i < s->node_count; ++i) {
        sim_node& n = s->nodes[i];
        n.faults =
            ecuio_sim_faults_for(config.faults, config.aimed, n.reply_id);
        n.timing = ecuio_sim_timing::for_node(config.timing, n.reply_id);
        n.rng = ecuio_sim_timing::sim_timing_rng(n.timing.seed, n.reply_id);
    }
    return true;
}

/* The sim p plans, on no bus yet: its values, its recording and its
 * ECUs */
ecuio_status_t build_sim(sim_plan& p, std::unique_ptr<ecuio_sim>* out)
{
    std::unique_ptr<ecuio_sim> s(new (std::nothrow) ecuio_sim);
    if (s == nullptr) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    s->profile_hold = std::move(p.hold);
    const ecuio_signal_set_t* set = p.set;
    ecuio_sim_config& config = p.config;
    if (set != nullptr) {
        s->set = set;
        s->values.reset(new (std::nothrow) std::atomic<double>[set->count]);
        if (set->count > 0 && s->values == nullptr) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        /* Until it's given one, each field holds raw 0 */
        for (uint32_t i = 0; i < set->count; ++i) {
            s->values[i].store(set->defs[i].offset, std::memory_order_relaxed);
        }
    }
    if (config.source == ecuio_sim_source::replay) {
        ecuio_status_t loaded = ECUIO_OK;
        try {
            loaded = ecuio_sim_load_recording(
                std::string(config.path, config.path_len), &s->recording,
                config.stretch);
        } catch (const std::bad_alloc&) {
            loaded = ECUIO_ERR_OUT_OF_MEMORY;
        }
        if (loaded != ECUIO_OK) {
            return loaded;
        }
        /* A loop with no span would answer one instant forever */
        if (config.loop && s->recording.span_us == 0) {
            ecuio_detail_set(config.stretch.given()
                                 ? "loop=1 needs a stretch that spans time; "
                                   "its replies all share one time"
                                 : "loop=1 needs a recording that spans "
                                   "time; its replies all share one time");
            return ECUIO_ERR_BAD_URI;
        }
        s->recording.loop = config.loop;
        s->ecu.recording = &s->recording;
    }
    config.path = nullptr; /* the URI's, gone after create */
    config.path_len = 0;
    s->ecu.config = config;
    s->bus_frame_us = ecuio_sim_timing::frame_us(config.timing.bus_kbps);
    if (!build_nodes(s.get(), p)) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    *out = std::move(s);
    return ECUIO_OK;
}

/* A reply into the caller's entries, counted whether or not it fits */
void put_reply(ecuio_sim_reply_t* replies, uint32_t capacity, uint32_t* n,
               const sim_node& node, uint32_t flags, int64_t delay_us,
               const uint8_t* data, uint32_t len)
{
    if (*n < capacity) {
        ecuio_sim_reply_t& r = replies[*n];
        std::memset(&r, 0, sizeof r);
        r.reply_id = node.reply_id;
        r.flags = flags;
        r.delay_us = delay_us;
        r.length = len;
        r.fc_us = node.timing.fc_us;
        r.cf_us = node.timing.cf_us;
        std::memcpy(r.data, data, len);
    }
    ++*n;
}

/* A busy node's way with a request at t (busy=): false when it answers
 * as usual, true when it ignores it or answered 7F <service> 21 */
bool ask_busy(ecuio_sim* sim, const sim_node& node, const uint8_t* request,
              int64_t t, ecuio_sim_reply_t* replies, uint32_t capacity,
              uint32_t* n)
{
    using ecuio_sim_timing::busy_mode;
    if (node.timing.busy == busy_mode::unset || t >= node.busy_until_us) {
        return false;
    }
    if (node.timing.busy == busy_mode::nrc21) {
        const uint8_t refusal[3] = {0x7F, request[0], 0x21};
        put_reply(replies, capacity, n, node, 0, 0, refusal, sizeof refusal);
        sim->replies.fetch_add(1, std::memory_order_relaxed);
        sim->refused.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

/* node's answer to an ask's request at t, its fate decided, into
 * replies at *n (counted past capacity) */
void ask_node(ecuio_sim* sim, sim_node& node, const uint8_t* request,
              uint32_t length, bool functional, int64_t t,
              ecuio_sim_reply_t* replies, uint32_t capacity, uint32_t* n)
{
    sim->requests.fetch_add(1, std::memory_order_relaxed);
    if (ask_busy(sim, node, request, t, replies, capacity, n)) {
        return;
    }
    sim_sending& r = node.r;
    r.len = answer(sim, node, request, length, functional, r.data);
    if (r.len == 0) {
        return;
    }
    /* Drawn before the fate, so a fault never moves the next draws */
    const int64_t drawn =
        ecuio_sim_timing::sim_timing_draw(node.timing, node.rng);
    r.fate = decide(sim, node, request, length, r.data, r.len);
    uint32_t flags = 0;
    int64_t delay = drawn;
    switch (r.fate) {
    case ecuio_sim_fate::silent:
        return;
    case ecuio_sim_fate::pending: {
        const uint8_t wait[3] = {0x7F, request[0], 0x78};
        put_reply(replies, capacity, n, node, ECUIO_SIM_REPLY_PENDING, 0, wait,
                  sizeof wait);
        delay = ECUIO_SIM_PENDING_US; /* its own timing, the draw unused */
        break;
    }
    case ecuio_sim_fate::late:
        flags = ECUIO_SIM_REPLY_LATE;
        delay = drawn + ECUIO_SIM_LATE_US;
        break;
    case ecuio_sim_fate::bad_sequence:
        flags = ECUIO_SIM_REPLY_BAD_SEQUENCE;
        break;
    case ecuio_sim_fate::stalled:
        flags = ECUIO_SIM_REPLY_STALLED;
        break;
    case ecuio_sim_fate::intact:
        break;
    }
    put_reply(replies, capacity, n, node, flags, delay, r.data, r.len);
    sent(sim, r);
    /* As the bus holds it: a stalled reply's last frame waits too. Any
     * time from ECUIO_SIM_NOW up is an ask's, so the end saturates */
    int64_t span =
        delay + ecuio_sim_timing::frames_after_first_us(node.timing, r.len);
    if (r.fate == ecuio_sim_fate::stalled) {
        span += ECUIO_SIM_STALLED_US;
    }
    node.busy_until_us =
        t > (std::numeric_limits<int64_t>::max)() - span
            ? (std::numeric_limits<int64_t>::max)()
            : t + span;
}

/* An ask's clock catches up at most this much a call, tick by tick; a
 * longer step moves the ECU's time without stepping the drive cycle
 * through it */
constexpr int64_t kCatchUpUs = 3600LL * 1000000;

} // namespace

ecuio_status_t ecuio_sim_load_recording(const std::string& path,
                                        ecuio_sim_recording* out,
                                        const ecuio_stretch::stretch& stretch)
{
    using namespace ecuio_frames_csv;
    try {
        std::string file = path;
        to_file(file);
        native_path native;
        if (!native.set(file)) {
            ecuio_detail_set("couldn't open the recording's frames.csv");
            return ECUIO_ERR_LOAD_FAILED;
        }
        const std::unique_ptr<FILE, int (*)(FILE*)> f(native.open(),
                                                      &std::fclose);
        if (f == nullptr) {
            ecuio_detail_set("couldn't open the recording's frames.csv");
            return ECUIO_ERR_LOAD_FAILED;
        }
        std::vector<char> line(kLineMax + 2);
        const std::unique_ptr<ecuio_frame_t> frame(new ecuio_frame_t());
        if (!read_header(f.get(), line)) {
            return ECUIO_ERR_LOAD_FAILED;
        }
        ecuio_sim_recording rec;
        row prev, cur;
        uint64_t at_us = 0, rows = 0, played = 0, line_no = 1;
        uint64_t first_us = 0, last_us = 0;
        bool too_long = false;
        bool torn = false;
        ecuio_stretch::cursor in(stretch);
        while (read_line(f.get(), line, &too_long, &torn)) {
            ++line_no;
            if (torn) {
                break; /* a write cut short: left out (ADR-0049 section 7) */
            }
            const char* why = nullptr;
            if (too_long) {
                why = "the line is longer than any frame's row";
            } else if (!parse_row(line.data(), &cur, frame.get(), &why)) {
                /* why is set */
            }
            if (why != nullptr) {
                ecuio_detail_set("frames.csv line %llu: %s",
                                 static_cast<unsigned long long>(line_no), why);
                return ECUIO_ERR_LOAD_FAILED;
            }
            if (rows == 0) {
                first_us = cur.host_us;
            }
            last_us = cur.host_us;
            ++rows;
            if (!in.plays(cur.host_us)) {
                continue;
            }
            if (played > 0) {
                at_us += step_us(prev, cur);
            }
            prev = cur;
            ++played;
            ecuio_sim_recording_take(rec, static_cast<int64_t>(at_us), *frame);
        }
        if (rows > 0 && played == 0) {
            char why[192];
            ecuio_stretch::describe_empty(stretch, first_us, last_us, why,
                                          sizeof why);
            ecuio_detail_set("%s", why);
            return ECUIO_ERR_LOAD_FAILED;
        }
        if (rec.replies.empty()) {
            ecuio_detail_set(stretch.given()
                                 ? "the stretch holds no reply from the "
                                   "engine ECU (0x7E8) to answer with"
                                 : "the recording holds no reply from the "
                                   "engine ECU (0x7E8) to answer with");
            return ECUIO_ERR_LOAD_FAILED;
        }
        *out = std::move(rec);
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

extern "C" {

ecuio_status_t ecuio_sim_create(const char* uri, const char* ifname,
                                ecuio_sim_t** out)
{
    return ecuio_sim_create_with(uri, ifname, 0, out);
}

ecuio_status_t ecuio_sim_create_with(const char* uri, const char* ifname,
                                     uint32_t options, ecuio_sim_t** out)
{
    ecuio_detail_clear();
    if (uri == nullptr || ifname == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    if ((options & ~ECUIO_SIM_ALLOW_DESK_BUS) != 0) {
        ecuio_detail_set("option bits 0x%x aren't the simulated ECU's",
                         options & ~ECUIO_SIM_ALLOW_DESK_BUS);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    sim_plan p;
    const ecuio_status_t planned = plan_sim(uri, false, &p);
    if (planned != ECUIO_OK) {
        return planned;
    }
#if !ECUIO_HAVE_SOCKETCAN
    ecuio_detail_set("the simulated ECU needs Linux, whose vcan is a "
                     "virtual CAN bus");
    return ECUIO_ERR_NO_TRANSPORT;
#else
    std::unique_ptr<ecuio_sim> s;
    const ecuio_status_t built = build_sim(p, &s);
    if (built != ECUIO_OK) {
        return built;
    }
    const ecuio_status_t opened = open_bus(s.get(), ifname, options);
    if (opened != ECUIO_OK) {
        return opened;
    }
    *out = s.release();
    return ECUIO_OK;
#endif
}

ecuio_status_t ecuio_sim_create_unbound(const char* uri, uint32_t options,
                                        ecuio_sim_t** out)
{
    ecuio_detail_clear();
    if (uri == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    if (options != 0) {
        ecuio_detail_set("option bits 0x%x aren't an unbound sim's", options);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    sim_plan p;
    const ecuio_status_t planned = plan_sim(uri, true, &p);
    if (planned != ECUIO_OK) {
        return planned;
    }
    std::unique_ptr<ecuio_sim> s;
    const ecuio_status_t built = build_sim(p, &s);
    if (built != ECUIO_OK) {
        return built;
    }
    s->unbound = true;
    s->created = std::chrono::steady_clock::now();
    *out = s.release();
    return ECUIO_OK;
}

ecuio_status_t ecuio_sim_ask(ecuio_sim_t* sim, uint32_t request_id,
                             const uint8_t* request, uint32_t length,
                             int64_t time_us, ecuio_sim_reply_t* replies,
                             uint32_t capacity, uint32_t* count)
{
    ecuio_detail_clear();
    if (sim == nullptr || request == nullptr || count == nullptr ||
        (replies == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!sim->unbound) {
        ecuio_detail_set("this simulated ECU answers on its bus; "
                         "ecuio_sim_create_unbound makes one to ask");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (length == 0 || length > ECUIO_ISOTP_MAX) {
        ecuio_detail_set("a request is 1 to %u bytes, not %u",
                         static_cast<unsigned>(ECUIO_ISOTP_MAX),
                         static_cast<unsigned>(length));
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (time_us < ECUIO_SIM_NOW) {
        ecuio_detail_set("time_us counts from create, or is ECUIO_SIM_NOW");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    int64_t t = time_us;
    if (t == ECUIO_SIM_NOW) {
        t = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - sim->created)
                .count());
    }
    t = t < sim->ask_us ? sim->ask_us : t;
    sim->ask_us = t;
    ecuio_sim_ecu& e = sim->ecu;
    if (t - e.time_us > kCatchUpUs) {
        e.time_us += (t - e.time_us - kCatchUpUs) / ECUIO_SIM_TICK_US *
                     ECUIO_SIM_TICK_US;
    }
    /* A difference, never a sum: t may be within a tick of the end */
    while (t - e.time_us >= ECUIO_SIM_TICK_US) {
        ecuio_sim_tick(e);
    }

    uint32_t n = 0;
    const bool functional = request_id == ECUIO_SIM_FUNCTIONAL_ID;
    for (int i = 0; i < sim->node_count; ++i) {
        sim_node& node = sim->nodes[i];
        if (!functional && request_id != node.request_id) {
            continue;
        }
        ask_node(sim, node, request, length, functional, t, replies, capacity,
                 &n);
    }
    *count = n;
    return ECUIO_OK;
}

ecuio_status_t ecuio_sim_get_stats(const ecuio_sim_t* sim,
                                   ecuio_sim_stats_t* out)
{
    if (sim == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::memset(out, 0, sizeof *out);
    out->requests = sim->requests.load(std::memory_order_relaxed);
    out->replies = sim->replies.load(std::memory_order_relaxed);
    out->refused = sim->refused.load(std::memory_order_relaxed);
    out->silent = sim->silent.load(std::memory_order_relaxed);
    out->late = sim->late.load(std::memory_order_relaxed);
    out->bad_sequence = sim->bad_sequence.load(std::memory_order_relaxed);
    out->stalled = sim->stalled.load(std::memory_order_relaxed);
    out->unflowed = sim->unflowed.load(std::memory_order_relaxed);
    out->clears = sim->clears.load(std::memory_order_relaxed);
    out->pending = sim->pending.load(std::memory_order_relaxed);
    out->flags = sim->flags.load(std::memory_order_acquire);
    return ECUIO_OK;
}

ecuio_status_t ecuio_sim_set_value(ecuio_sim_t* sim, uint16_t signal_id,
                                   double value)
{
    ecuio_detail_clear();
    if (sim == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (sim->set == nullptr) {
        ecuio_detail_set("the simulated ECU answers no profile's DIDs "
                         "(no profile= in its URI)");
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < sim->set->count; ++i) {
        const ecuio_signal_def_t& d = sim->set->defs[i];
        if (d.id != signal_id || !ecuio_sim_did_def(d)) {
            continue;
        }
        uint32_t raw = 0;
        if (!ecuio_sim_raw_for(d, value, &raw)) {
            ecuio_detail_set(ecuio_sim_text_def(d)
                                 ? "signal %u is text: it answers its name"
                                 : "signal %u's field can't hold that value",
                             static_cast<unsigned>(signal_id));
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        sim->values[i].store(value, std::memory_order_relaxed);
        return ECUIO_OK;
    }
    ecuio_detail_set("signal %u isn't a Service 22 DID's field of the profile",
                     static_cast<unsigned>(signal_id));
    return ECUIO_ERR_INVALID_ARGUMENT;
}

void ecuio_sim_destroy(ecuio_sim_t* sim)
{
    if (sim == nullptr) {
        return;
    }
#if ECUIO_HAVE_SOCKETCAN
    if (!sim->unbound) {
        close_bus(sim);
    }
#endif
    delete sim;
}

} // extern "C"
