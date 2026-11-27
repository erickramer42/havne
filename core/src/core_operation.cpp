// File/layer: core/src/core_operation.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SessionOperationTest (test_core); HARNESS: the transports'
//          operation cases
//
// The three public calls of a run (ecuio/operation.h) and the close's
// end of one. The start is the caller's thread's half
// (operation_session.h): every check that needs no request, the copy's
// unfinished file and its writer, an owed unlock's mark, then the run
// armed for the worker (request_operation.h). What can fail comes before
// what stays: the file before the mark, the mark before the arming, and
// the arming neither fails nor allocates.

#include "ecuio/operation.h"
#include "ecuio/version.h"
#include "consent_gate.h"
#include "conversation_bind.h"
#include "error_detail.h"
#include "operation_session.h"
#include "recorder.h"
#include "request_engine_ident.h"
#include "session.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

namespace {

using ecuio_operation::dump_sink;
using ecuio_operation::plan;
using ecuio_operation::session_run;
using ecuio_operation::sink_state;

constexpr char kClosedWords[] =
    "the session closed before the read ended: nothing was kept";

int64_t utc_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/* The bytes of a step's first request: 23 <ALFID> <address> <size>, an
 * upload's 35 <dfi> <ALFID> <address> <size> */
uint32_t request_bytes(const ecuio_operation::step& s)
{
    return (s.kind == ECUIO_STEP_UPLOAD ? 3u : 2u) + s.address_bytes +
           s.size_bytes;
}

/* Whether every request of the plan fits the adapter's bound, or the
 * refusal */
ecuio_status_t fits(const plan& p, uint32_t bound)
{
    for (const ecuio_operation::step& s : p.steps) {
        if (request_bytes(s) > bound) {
            ecuio_detail_set("step \"%s\" is asked in %u bytes: it doesn't "
                             "fit one request on this adapter (%u bytes)",
                             s.name.c_str(), request_bytes(s), bound);
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
    }
    return ECUIO_OK;
}

/* A VIN a copy's record takes: 17 digits and capitals, never I, O or Q */
bool record_vin(const char* text)
{
    size_t n = 0;
    for (; text[n] != '\0'; ++n) {
        const char c = text[n];
        const bool ok =
            (c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z' && c != 'I' && c != 'O' && c != 'Q');
        if (!ok) {
            return false;
        }
    }
    return n == 17;
}

/* The URI's scheme, the letters and digits before its colon */
std::string scheme_of(const std::string& uri)
{
    std::string out;
    for (const char c : uri) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            break;
        }
        out += c;
    }
    return out;
}

/* text's printable ASCII, for a record's plain text */
std::string plain(const char* text)
{
    std::string out;
    for (; *text != '\0'; ++text) {
        if (*text >= 0x20 && *text < 0x7F) {
            out += *text;
        }
    }
    return out;
}

/* The copy's record as far as the start knows it: its second, the
 * module, the plugin and the transport. The sink fills the rest. */
ecuio_dump_file::record record_for(const ecuio_session_t* s,
                                   const ecuio_conversation_part& part,
                                   const plan& p)
{
    ecuio_dump_file::record r;
    r.utc = utc_now();
    r.core = ECUIO_VERSION_STRING;
    r.rx_id = p.rx_id;
    r.tx_id = p.tx_id;
    {
        const std::lock_guard<std::mutex> lock(s->support_mutex);
        for (uint32_t k = 0; k < s->support_count && r.vin.empty(); ++k) {
            const ecuio_ident_vin v = ecuio_identity_vin_of(s->identity[k]);
            if (v.in && record_vin(v.text)) {
                r.vin = v.text;
            }
        }
    }
    r.plugin_id = part.plugin_id;
    r.plugin_version = part.plugin_version;
    r.operation = p.id;
    std::memcpy(r.module_sha256, part.sha256, sizeof r.module_sha256);
    std::memcpy(r.fingerprint, part.fingerprint, sizeof r.fingerprint);
    r.scheme = scheme_of(s->uri);
    r.firmware = plain(s->info.firmware_version);
    return r;
}

ecuio_status_t refuse(ecuio_status_t status, const char* words)
{
    ecuio_detail_set("%s", words);
    return status;
}

/* The request by itself */
ecuio_status_t judge_request(const ecuio_operation_request_t& q)
{
    if (q.struct_size < sizeof q) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "the request's struct_size is smaller than the struct");
    }
    if ((q.flags & ~(ECUIO_OPERATION_MAY_UNLOCK | ECUIO_OPERATION_PARKED)) !=
        0) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "the request sets a flag this Havne doesn't know");
    }
    for (const uint32_t word : q.reserved) {
        if (word != 0) {
            return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                          "the request's reserved words are zero");
        }
    }
    return ECUIO_OK;
}

/* The plan the request names and the session channel of its module:
 * what the session and its plugin say, before anything of the bind */
ecuio_status_t find_plan(const ecuio_session_t* s,
                         const ecuio_operation_request_t& q, const plan** out,
                         uint32_t* channel)
{
    if (!s->started) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "a run starts on a started session");
    }
    const ecuio_conversation_part* part = s->conversation.get();
    if (part == nullptr || part->operations.empty()) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "the session's plugin declares no operation");
    }
    if (q.operation >= part->operations.size()) {
        ecuio_detail_set("the session's plugin declares %u operations: "
                         "there is no operation %u",
                         static_cast<unsigned>(part->operations.size()),
                         q.operation);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if ((s->info.flags & ECUIO_SESSION_INFO_REQUESTS) == 0) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "this session asks nothing: a run needs a transport "
                      "that sends requests");
    }
    const plan& p = part->operations[q.operation];
    for (uint32_t c = 0; c < s->module_rx.size(); ++c) {
        if (s->module_rx[c] == p.rx_id && s->module_tx[c] == p.tx_id) {
            *out = &p;
            *channel = c;
            return ECUIO_OK;
        }
    }
    return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                  "the operation's module isn't one this session talks to");
}

ecuio_status_t judge_gates(const ecuio_session_t* s, uint32_t channel,
                           const ecuio_bound_channel* b, uint32_t flags)
{
    if (b == nullptr && ecuio_session_channel_wants_consent(s, channel)) {
        return refuse(ECUIO_ERR_CONSENT_REQUIRED,
                      "no consent was given to the plugin whose code talks "
                      "on this channel");
    }
    if (b == nullptr) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "no plugin's code talks to the operation's module in "
                      "this session");
    }
    if (!ecuio_plugin_code_may_run(s)) {
        return refuse(ECUIO_ERR_CONSENT_REQUIRED,
                      "no consent was given to the plugin whose plan this is");
    }
    if ((flags & ECUIO_OPERATION_PARKED) == 0) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "confirm the vehicle is parked first: ignition on, not "
                      "Ready; engine off by the key, not by start-stop; in P "
                      "or neutral with the parking brake on");
    }
    /* Every run waits for the write flow's envelope (the engine off, the
     * battery's floor): until core has it, only the tests' door starts one */
    if (!g_ecuio_conversation_door.load(std::memory_order_relaxed)) {
        return refuse(ECUIO_ERR_NOT_IMPLEMENTED,
                      "a run needs the write flow's checks before it, which "
                      "this Havne has not got");
    }
    const session_run* o = s->operation.get();
    const uint8_t stage =
        o == nullptr ? ecuio_operation::kStageIdle : o->stage.load();
    if (stage == ecuio_operation::kStageArmed ||
        stage == ecuio_operation::kStageRunning) {
        return refuse(ECUIO_ERR_IN_USE, "a run is under way in this session");
    }
    return ECUIO_OK;
}

/* The run armed for the worker: nothing here fails or allocates */
void arm(ecuio_session_t* s, session_run& o, const plan& p,
         std::unique_ptr<dump_sink> sink, uint32_t channel, uint32_t index,
         bool marked)
{
    o.p = &p;
    o.sink = std::move(sink);
    o.channel = channel;
    o.operation = index;
    o.marked = marked;
    o.cancel.store(false);
    o.r = ecuio_operation::run{};
    o.out = o.fed = o.orphan = o.word_given = o.closed_sink = false;
    o.checking = false;
    o.talk_at = channel;
    o.sid = 0;
    o.ends_at = 0;
    o.unlocks = 0;
    o.began_us = 0;
    o.entering_us = 0;
    {
        const std::lock_guard<std::mutex> lock(s->support_mutex);
        const uint32_t generation = o.view.generation + 1;
        o.view = ecuio_operation_state_t{};
        o.view.generation = generation;
        o.view.operation = index;
        o.view.phase = ECUIO_OPERATION_PHASE_CHECKING;
        o.view.bytes_total = 2 * ecuio_operation::image_bytes(p);
    }
    o.stage.store(ecuio_operation::kStageArmed, std::memory_order_release);
}

ecuio_status_t start(ecuio_session_t* s, const ecuio_operation_request_t& q,
                     const char* folder)
{
    const plan* p = nullptr;
    uint32_t channel = 0;
    ecuio_status_t st = judge_request(q);
    st = st == ECUIO_OK ? find_plan(s, q, &p, &channel) : st;
    if (st != ECUIO_OK) {
        return st;
    }
    ecuio_bound_channel* b = ecuio_bound_channel_of(s, channel);
    st = judge_gates(s, channel, b, q.flags);
    if (st != ECUIO_OK) {
        return st;
    }
    ecuio_conversation_report report;
    {
        const std::lock_guard<std::mutex> lock(s->support_mutex);
        report = b->published;
    }
    if (report.state != ecuio_conversation_state::talking) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "the module's code isn't talking on this channel");
    }
    st = fits(*p, b->talk_bound.load());
    if (st != ECUIO_OK) {
        return st;
    }
    /* The worker judges the level again once the session is entered: a
     * level it finds open costs no attempt, marked or not */
    const bool open = report.level == p->level &&
                      (report.unlock == ecuio_unlock_state::unlocked ||
                       report.unlock == ecuio_unlock_state::already_unlocked);
    if (!open && (q.flags & ECUIO_OPERATION_MAY_UNLOCK) == 0) {
        ecuio_detail_set("unlock level %02X first, or start the run with "
                         "the unlock allowed",
                         p->level);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    session_run* o = s->operation.get();
    if (o == nullptr) {
        s->operation.reset(new session_run());
        o = s->operation.get();
        s->operation_live.store(o, std::memory_order_release);
    } else if (o->sink != nullptr) {
        o->sink->close(); /* the run before: its writer joined */
    }
    auto sink = std::make_unique<dump_sink>();
    std::string why;
    st = sink->open(*p, folder, record_for(s, *s->conversation, *p), &why);
    if (st != ECUIO_OK) {
        if (st == ECUIO_ERR_LOAD_FAILED) {
            ecuio_detail_set("%s", why.c_str());
        }
        return st;
    }
    if (!open) {
        st = ecuio_session_unlock_mark(s, *b, channel, p->level, nullptr);
        if (st != ECUIO_OK) {
            return st; /* the sink's end removes the unfinished file */
        }
    }
    arm(s, *o, *p, std::move(sink), channel, q.operation, !open);
    return ECUIO_OK;
}

} // namespace

void ecuio_operation_free(ecuio_operation::session_run* o) noexcept
{
    delete o;
}

void ecuio_operation_closed(ecuio_session* s) noexcept
{
    session_run* o = s->operation.get();
    if (o == nullptr) {
        return;
    }
    /* Joined before the end is told: a seal under way may still place */
    if (o->sink != nullptr) {
        o->sink->close();
    }
    const uint8_t stage = o->stage.load();
    if (stage != ecuio_operation::kStageArmed &&
        stage != ecuio_operation::kStageRunning) {
        return;
    }
    const bool saved = o->sink->state() == sink_state::saved;
    const uint32_t end =
        saved ? ECUIO_OPERATION_END_SAVED : ECUIO_OPERATION_END_CANCELED;
    /* An armed run never began: it has no start line to end */
    if (stage == ecuio_operation::kStageRunning) {
        ecuio_record_operation(s->record_link.get(), o->p->id.c_str(),
                               o->p->rx_id, ecuio_operation::image_bytes(*o->p),
                               end);
    }
    const std::lock_guard<std::mutex> lock(s->support_mutex);
    ecuio_operation_state_t& v = o->view;
    v.phase = ECUIO_OPERATION_PHASE_DONE;
    v.pass = 0;
    v.end = end;
    if (saved) {
        std::memcpy(v.name, o->sink->name(), sizeof v.name);
    } else {
        std::memcpy(v.words, kClosedWords, sizeof kClosedWords);
    }
    o->stage.store(ecuio_operation::kStageEnded);
}

extern "C" {

ecuio_status_t
ecuio_session_operation_start(ecuio_session_t* session,
                              const ecuio_operation_request_t* request,
                              const char* folder_utf8)
{
    ecuio_detail_clear();
    if (session == nullptr || request == nullptr || folder_utf8 == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    /* the whole call, so a second caller or close's release waits */
    const std::lock_guard<std::mutex> calls(session->conversation_mutex);
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    try {
        return start(session, *request, folder_utf8);
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_session_operation_cancel(ecuio_session_t* session)
{
    if (session == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    session_run* o = session->operation_live.load(std::memory_order_acquire);
    if (o != nullptr) {
        const uint8_t stage = o->stage.load(std::memory_order_acquire);
        if (stage == ecuio_operation::kStageArmed ||
            stage == ecuio_operation::kStageRunning) {
            o->cancel.store(true, std::memory_order_relaxed);
        }
    }
    return ECUIO_OK;
}

ecuio_status_t ecuio_session_get_operation(const ecuio_session_t* session,
                                           ecuio_operation_state_t* out)
{
    if (session == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out->struct_size < sizeof *out) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const session_run* o =
        session->operation_live.load(std::memory_order_acquire);
    if (o == nullptr) {
        *out = ecuio_operation_state_t{};
    } else {
        const std::lock_guard<std::mutex> lock(session->support_mutex);
        *out = o->view;
    }
    out->struct_size = sizeof *out;
    return ECUIO_OK;
}

} // extern "C"
