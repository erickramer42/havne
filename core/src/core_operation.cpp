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
#include "ecuio/internal.h"
#include "ecuio/version.h"
#include "consent_gate.h"
#include "conversation_bind.h"
#include "error_detail.h"
#include "host_awake.h"
#include "host_power.h"
#include "operation_flash_start.h"
#include "operation_session.h"
#include "recorder.h"
#include "request_engine_ident.h"
#include "session.h"

#include <atomic>
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

std::atomic<bool> g_write_class{false};

constexpr char kClosedWords[] =
    "the session closed before the read ended: nothing was kept";
constexpr char kClosedFlashWords[] =
    "the session closed after the flash began writing: flash the module "
    "again before it runs";
constexpr char kClosedUnwrittenWords[] =
    "the session closed before the flash began writing: nothing was written";

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
    if ((q.flags & ~(ECUIO_OPERATION_MAY_UNLOCK | ECUIO_OPERATION_PARKED |
                     ECUIO_OPERATION_BATTERY_SUPPORT)) != 0) {
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

/* Whether a kernel read's download and start fit the adapter's writes */
ecuio_status_t helper_fits(const plan& p, uint32_t write_bound)
{
    const ecuio_operation::kernel& k = p.helper;
    const uint32_t opening = 3u + k.address_bytes + k.size_bytes;
    const uint32_t need = opening > 4u ? opening : 4u;
    if (p.has_kernel && write_bound < need) {
        ecuio_detail_set("the helper's download is asked in %u bytes: this "
                         "adapter carries no write that long",
                         need);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return ECUIO_OK;
}

/* A write-class run's link: J2534, or SocketCAN on no car's bus */
ecuio_status_t judge_link(const ecuio_session_t* s)
{
    const std::string scheme = scheme_of(s->uri);
    if (scheme == "elm327") {
        return refuse(ECUIO_ERR_NOT_IMPLEMENTED,
                      "this plan writes to the module, which needs a J2534 "
                      "adapter: an ELM327 sends no request longer than a "
                      "frame");
    }
    if (scheme == "socketcan" && !s->no_car_bus) {
        return refuse(ECUIO_ERR_NOT_IMPLEMENTED,
                      "this plan writes to the module, which Havne does over "
                      "J2534 only: SocketCAN runs it on vcan alone");
    }
    return ECUIO_OK;
}

ecuio_status_t judge_gates(const ecuio_session_t* s, uint32_t channel,
                           const ecuio_bound_channel* b, uint32_t flags,
                           const plan& p, bool write)
{
    const ecuio_status_t link = write ? judge_link(s) : ECUIO_OK;
    if (link != ECUIO_OK) {
        return link;
    }
    /* A kernel read writes only to RAM: the flash's gates aren't its */
    const bool flash = write && !p.has_kernel;
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
    if (flash && (flags & ECUIO_OPERATION_BATTERY_SUPPORT) == 0) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "connect battery support (a charger in its "
                      "power-supply mode) and confirm it first");
    }
    if (flash && !s->battery.source) {
        return refuse(ECUIO_ERR_NOT_IMPLEMENTED,
                      "this adapter reads no battery voltage: a write needs "
                      "one");
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

/* The host's half of the envelope, last before the copy's file: its power
 * and core's keep-awake hold. A read-class run is warned of either in
 * *checks; a write stops on a hold refused or the host's battery under
 * host_power::kWriteStopPercent. */
ecuio_status_t take_envelope(ecuio_session_t* s, bool write, uint32_t* checks,
                             bool* held)
{
    *checks = 0;
    *held = false;
    const host_power::reading power = host_power::read();
    if (write && host_power::write_stops(power)) {
        ecuio_detail_set("plug the computer in: it runs on its battery at "
                         "%d%%",
                         power.percent);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (host_power::off_mains(power)) {
        *checks |= ECUIO_OPERATION_CHECK_ON_BATTERY;
    }
    const ecuio_status_t st = ecuio_awake_core_hold(s, true);
    if (st == ECUIO_OK) {
        *held = true;
        return ECUIO_OK;
    }
    if (write) {
        return st;
    }
    ecuio_detail_clear();
    *checks |= ECUIO_OPERATION_CHECK_NOT_HELD_AWAKE;
    return ECUIO_OK;
}

/* Core's keep-awake claim, given back unless the run is armed with it */
struct awake_claim {
    ecuio_session_t* s;
    bool on;
    awake_claim(const awake_claim&) = delete;
    awake_claim& operator=(const awake_claim&) = delete;
    ~awake_claim()
    {
        if (on) {
            (void)ecuio_awake_core_hold(s, false);
        }
    }
};

/* The run armed for the worker: nothing here fails or allocates */
void arm(ecuio_session_t* s, session_run& o, const plan& p,
         std::unique_ptr<dump_sink> sink, uint32_t channel, uint32_t index,
         bool marked, bool write, uint32_t checks, bool held)
{
    o.write = write;
    o.start_checks = checks;
    o.held_awake = held;
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
        o.view.bytes_total =
            p.has_flash ? o.f.written : 2 * ecuio_operation::image_bytes(p);
        o.view.sectors = static_cast<uint16_t>(p.has_flash ? o.f.chosen : 0);
    }
    o.stage.store(ecuio_operation::kStageArmed, std::memory_order_release);
}

/* Whether the plan's level is open, as the start sees it, and a run
 * that would need the unlock without it allowed refused. The worker
 * judges the level again once the session is entered: a level it finds
 * open costs no attempt, marked or not. Entering the programming session
 * locks every level. */
ecuio_status_t judge_level(const plan& p,
                           const ecuio_conversation_report& report,
                           uint32_t flags, bool* open)
{
    const bool programming = ecuio_operation::programming_of(p);
    *open = !programming && report.level == p.level &&
            (report.unlock == ecuio_unlock_state::unlocked ||
             report.unlock == ecuio_unlock_state::already_unlocked);
    if ((flags & ECUIO_OPERATION_MAY_UNLOCK) != 0 || *open) {
        return ECUIO_OK;
    }
    if (programming) {
        ecuio_detail_set("level %02X is unlocked in the programming session "
                         "the run enters: start it with the unlock allowed",
                         p.level);
    } else {
        ecuio_detail_set("unlock level %02X first, or start the run with "
                         "the unlock allowed",
                         p.level);
    }
    return ECUIO_ERR_INVALID_ARGUMENT;
}

/* A flash's own start: the transport's writes, the copy its sectors are
 * judged by, the journal; nothing sent and no sink */
ecuio_status_t start_flash(ecuio_session_t* s, session_run& o, const plan& p,
                           const ecuio_bound_channel& b, const char* folder,
                           const char* copy)
{
    ecuio_status_t st = ecuio_operation::flash_fits(p, b.write_bound.load());
    if (st != ECUIO_OK) {
        return st;
    }
    o.current.clear();
    if (copy != nullptr) {
        st = ecuio_operation::flash_current(folder, copy, p, &o.current);
        if (st != ECUIO_OK) {
            return st;
        }
    }
    uint32_t sectors = 0;
    uint64_t bytes = 0;
    st =
        ecuio_operation::flash_journal(s, o, p, folder, copy, &sectors, &bytes);
    o.f = ecuio_operation::flash_run{};
    o.f.chosen = sectors;
    o.f.written = bytes;
    return st;
}

/* The run's file before anything is sent: a dump's copy, a flash's
 * journal */
ecuio_status_t open_files(ecuio_session_t* s, session_run& o, const plan& p,
                          const ecuio_bound_channel& b, const char* folder,
                          const char* copy, std::unique_ptr<dump_sink>* sink)
{
    o.journal.close(false);
    if (p.has_flash) {
        return start_flash(s, o, p, b, folder, copy);
    }
    *sink = std::make_unique<dump_sink>();
    std::string why;
    const ecuio_status_t st =
        (*sink)->open(p, folder, record_for(s, *s->conversation, p), &why);
    if (st == ECUIO_ERR_LOAD_FAILED) {
        ecuio_detail_set("%s", why.c_str());
    }
    return st;
}

ecuio_status_t start(ecuio_session_t* s, const ecuio_operation_request_t& q,
                     const char* folder, const char* copy)
{
    const plan* p = nullptr;
    uint32_t channel = 0;
    ecuio_status_t st = judge_request(q);
    st = st == ECUIO_OK ? find_plan(s, q, &p, &channel) : st;
    if (st != ECUIO_OK) {
        return st;
    }
    ecuio_bound_channel* b = ecuio_bound_channel_of(s, channel);
    const bool write = g_write_class.load() || ecuio_operation::class_of(*p) ==
                                                   ECUIO_OPERATION_CLASS_WRITE;
    st = judge_gates(s, channel, b, q.flags, *p, write);
    if (st != ECUIO_OK) {
        return st;
    }
    if (copy != nullptr && !p->has_flash) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "only a flash is judged against a copy");
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
    st = p->has_flash ? ECUIO_OK : fits(*p, b->talk_bound.load());
    st = st == ECUIO_OK ? helper_fits(*p, b->write_bound.load()) : st;
    if (st != ECUIO_OK) {
        return st;
    }
    bool open = false;
    st = judge_level(*p, report, q.flags, &open);
    if (st != ECUIO_OK) {
        return st;
    }
    uint32_t checks = 0;
    bool held = false;
    st = take_envelope(s, write && !p->has_kernel, &checks, &held);
    if (st != ECUIO_OK) {
        return st;
    }
    awake_claim claim{s, held};
    session_run* o = s->operation.get();
    if (o == nullptr) {
        s->operation.reset(new session_run());
        o = s->operation.get();
        s->operation_live.store(o, std::memory_order_release);
    } else if (o->sink != nullptr) {
        o->sink->close(); /* the run before: its writer joined */
    }
    std::unique_ptr<dump_sink> sink;
    st = open_files(s, *o, *p, *b, folder, copy, &sink);
    if (st != ECUIO_OK) {
        return st;
    }
    if (!open) {
        st = ecuio_session_unlock_mark(s, *b, channel, p->level, nullptr);
        if (st != ECUIO_OK) {
            o->journal.close(p->has_flash);
            return st; /* the sink's end removes the unfinished file */
        }
    }
    claim.on = false;
    arm(s, *o, *p, std::move(sink), channel, q.operation, !open, write, checks,
        held);
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
    const bool live = stage == ecuio_operation::kStageArmed ||
                      stage == ecuio_operation::kStageRunning;
    const bool flash = o->p != nullptr && o->p->has_flash;
    const bool past_floor =
        flash && (o->f.floor ||
                  (o->f.asked && ecuio_operation::flash_moves_floor(o->f)));
    o->journal.close(live && flash && !past_floor);
    if (!live) {
        return;
    }
    const bool saved =
        o->sink != nullptr && o->sink->state() == sink_state::saved;
    const uint32_t end = saved        ? ECUIO_OPERATION_END_SAVED
                         : past_floor ? ECUIO_OPERATION_END_RECOVERY_NEEDED
                                      : ECUIO_OPERATION_END_CANCELED;
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
    } else if (past_floor) {
        std::memcpy(v.words, kClosedFlashWords, sizeof kClosedFlashWords);
    } else if (flash) {
        std::memcpy(v.words, kClosedUnwrittenWords,
                    sizeof kClosedUnwrittenWords);
    } else {
        std::memcpy(v.words, kClosedWords, sizeof kClosedWords);
    }
    o->stage.store(ecuio_operation::kStageEnded);
}

extern "C" {

void ecuio_test_operation_write_class(int on)
{
    g_write_class.store(on != 0);
}

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
        return start(session, *request, folder_utf8, nullptr);
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
}

ecuio_status_t ecuio_session_operation_start_from(
    ecuio_session_t* session, const ecuio_operation_request_t* request,
    const char* folder_utf8, const char* copy_utf8)
{
    ecuio_detail_clear();
    if (session == nullptr || request == nullptr || folder_utf8 == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const std::lock_guard<std::mutex> calls(session->conversation_mutex);
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    try {
        return start(session, *request, folder_utf8, copy_utf8);
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
