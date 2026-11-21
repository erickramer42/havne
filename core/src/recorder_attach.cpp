// File/layer: core/src/recorder_attach.cpp | C++17, Qt-free; attaching a
//             recorder: its open and the session's record call (ADR-0049
//             section 3, armed too: ADR-0063)
// Testing: UNIT: RecorderTest, ConditionRecordingTest,
//          ArmedRecorderTest (test_core);
//          BRIDGE: BridgeTest (test_bridge);
//          HARNESS: Elm327SessionTest (test_elm327)
//
// ecuio_recorder_open makes the recorder, ecuio_recorder_signal_file hands
// it a signal file, and ecuio_session_record opens its files, writes the
// header and attaches it to the session's record link. The worker's puts
// are in recorder.cpp, the calls on a recorder in flight in
// recorder_control.cpp.

#include "recorder.h"

#include "condition_eval.h"
#include "error_detail.h"
#include "folder_files.h"
#include "recorder_impl.h"
#include "recorder_text.h"
#include "recording_lines.h" /* signal_line */
#include "recording_keys.h"  /* a set's keys */
#include "recording_rows.h"  /* kFormat4, the first line */
#include "session.h"
#include "sha256.h"
#include "signal_file_build.h" /* a loaded file's bytes */

#include "ecuio/internal.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ecuio_rec {

constexpr uint32_t kDefaultRingBytes = 4u * 1024u * 1024u;
/* "conditions  conditions.json,<64 hex>\n" and its NUL */
constexpr size_t kConditionsLineMax = 96;

} // namespace ecuio_rec

namespace {

/* ecuio_session_test_before_record_attach's (internal.h) */
std::atomic<void (*)(void*)> g_before_record_attach{nullptr};
std::atomic<void*> g_before_record_attach_user{nullptr};

/* What ecuio_session_record builds before it takes the link: the
 * recorder changes only once the link is taken, so a refused attach
 * leaves it ready for the next try */
struct attach_build {
    std::vector<def_entry> defs;
    std::vector<uint32_t> rx_ids; /* each module's reply ID */
    std::string header;
    size_t conditions_at = 0;
    int32_t link_state = ECUIO_OK;
    uint32_t link_episodes = 0;
    /* An armed recorder's: a window's header lines from core to frame
     * flags (the conditions line's place in them) and the signal lines,
     * and room for a state per condition */
    std::string armed_core;
    size_t armed_conditions_at = 0;
    std::string armed_signals;
    std::vector<uint8_t> armed_cond;
    std::vector<ecuio_recorder::tail_state> armed_tail;
    focus_named armed_focus;
    uint64_t zero_us = 0;
    uint64_t slept_at_attach = 0;
    focus_named named_focus;
    std::chrono::system_clock::time_point start_wall{};
};

/* The attach's refusals before anything is built; ECUIO_OK for none */
ecuio_status_t attach_check(ecuio_session_t* session,
                            ecuio_recorder_t* recorder)
{
    if (session == nullptr || recorder == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (session->closed.load()) {
        return ECUIO_ERR_SESSION_CLOSED;
    }
    /* A recorder has one session ever; a session one recorder at a time
     * (ADR-0056 section 3), judged under the link's mutex below */
    if (recorder->attached.load()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (recorder->signals_set != nullptr &&
        recorder->signals_set != session->active_signal_set) {
        ecuio_detail_set("the signal file isn't what decodes the session "
                         "(its profile is %s)",
                         session->info.profile);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return ECUIO_OK;
}

/* The set's defs under their keys, sorted by id */
void attach_defs(const ecuio_signal_set_t* set,
                 const std::vector<ecuio_signal_key_t>& keys,
                 std::vector<def_entry>& defs)
{
    for (uint32_t i = 0; i < keys.size(); ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        const size_t ulen = strnlen(d.unit, sizeof d.unit);
        const bool text = is_text_kind(d.kind);
        defs.push_back(
            def_entry{d.id, text, keys[i].text, csv_quoted(d.unit, ulen)});
    }
    std::sort(
        defs.begin(), defs.end(),
        [](const def_entry& a, const def_entry& b) { return a.id < b.id; });
}

/* The header's lines from the start to frame flags, and an armed
 * recorder's copy of those from core on */
void header_top(const ecuio_session_t* session,
                const ecuio_recorder_t* recorder, attach_build& b)
{
    std::string& header = b.header;
    header += started_lines(b.start_wall);
    const size_t core_at = header.size();
    header += std::string("core  ") + ecuio_version_string() + "\n";
    header += "uri  " + one_line(session->uri.c_str()) + "\n";
    header += std::string("profile  ") + session->info.profile + "\n";
    if (!recorder->signals_json.empty()) {
        header += std::string(ecuio_recording_rows::kSignalFileKey) +
                  ecuio_recording_rows::kSignalFileName + "," +
                  recorder->signals_sha + "\n";
    }
    if (!recorder->signals_plugin.empty()) {
        header += std::string(ecuio_recording_rows::kPluginKey) +
                  recorder->signals_plugin + "\n";
    }
    /* The conditions line goes here once the set is read under the
     * locks, into room kept now */
    b.conditions_at = header.size();
    /* A replay's frames carry its source's flags, so it writes its
     * source's line, which a recording from before ADR-0049 lacks */
    header += "frame flags  ";
    header += session->info.backend != ECUIO_BACKEND_REPLAY ? "complete"
              : session->source_frame_flags.empty()
                  ? "from an older recording"
                  : session->source_frame_flags.c_str();
    header += "\n";
    /* A window's header has these lines from the copy, the conditions
     * line put in where the full header takes it */
    if (recorder->armed) {
        b.armed_core = header.substr(core_at);
        b.armed_core.reserve(b.armed_core.size() + kConditionsLineMax);
        b.armed_conditions_at = b.conditions_at - core_at;
    }
}

/* A session attached to after start (ADR-0056 section 3) says what it
 * had come to: its focus in pick order (none: no line) and, once
 * started, the link's state and episodes in the link lines' words, so a
 * reader needn't wait for a transition */
void header_focus_link(ecuio_session_t* session,
                       const ecuio_recorder_t* recorder, attach_build& b)
{
    {
        std::lock_guard<std::mutex> f(session->focus_mutex);
        b.named_focus = focus_now(session);
    }
    size_t named = 0;
    /* As many lines as the keys take; a reader appends them */
    const std::vector<std::string> parts =
        focus_parts(b.defs, b.named_focus.ids, &named);
    b.header += focus_header(parts, named, b.named_focus);
    if (recorder->armed) {
        b.armed_focus = b.named_focus;
    }
    if (session->started) {
        ecuio_session_get_error_state(session, &b.link_state, &b.link_episodes);
        b.header += header_link(b.link_state, b.link_episodes);
    }
}

/* The header's signal lines, room for the conditions line, and an armed
 * recorder's copy of the signal lines and room for its states */
void header_signals(const ecuio_signal_set_t* set,
                    const std::vector<ecuio_signal_key_t>& keys,
                    const ecuio_recorder_t* recorder, attach_build& b)
{
    std::string& header = b.header;
    const size_t signals_at = header.size();
    for (uint32_t i = 0; i < keys.size(); ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        const bool enumeration = d.kind == ECUIO_VALUE_ENUM;
        header += ecuio_recording_lines::keyed_signal_line(
            keys[i].text, csv_quoted(d.name, strnlen(d.name, sizeof d.name)),
            is_text_kind(d.kind) ? "text"
            : enumeration        ? "enum"
                                 : "number",
            csv_quoted(d.unit, strnlen(d.unit, sizeof d.unit)),
            d.decimal_places, d.plot_rank, d.group);
        header += label_lines(set, d, keys[i].text);
    }
    header.reserve(header.size() + kConditionsLineMax);
    if (recorder->armed) {
        b.armed_signals = header.substr(signals_at);
        b.armed_cond.reserve(ECUIO_CONDITION_MAX_CONDITIONS);
        b.armed_tail.reserve(ECUIO_CONDITION_MAX_CONDITIONS);
    }
}

/* Everything the attach writes, built outside the locks: the defs, the
 * header and an armed recorder's pieces; ECUIO_ERR_OUT_OF_MEMORY when
 * an allocation fails */
ecuio_status_t attach_prepare(ecuio_session_t* session,
                              const ecuio_recorder_t* recorder, attach_build& b)
{
    try {
        const ecuio_signal_set_t* set = session->active_signal_set;
        b.rx_ids = session->module_rx; /* fixed since create */
        b.header = std::string(ecuio_recording_rows::kFormat4) + "\n";
        /* The file's keys when it gave them, else the set's own */
        std::vector<ecuio_signal_key_t> own_keys;
        if (recorder->signal_keys.empty() &&
            ecuio_recording_keys::set_keys(set, nullptr, &own_keys) !=
                ECUIO_OK) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        const std::vector<ecuio_signal_key_t>& keys =
            recorder->signal_keys.empty() ? own_keys : recorder->signal_keys;
        attach_defs(set, keys, b.defs);

        /* The zero and the wall clock read back to back */
        b.zero_us = ecuio_record_clock_us();
        b.slept_at_attach =
            ecuio_clock_slept_us() + recorder->slept_added.load();
        b.start_wall = std::chrono::system_clock::now();

        header_top(session, recorder, b);
        header_focus_link(session, recorder, b);
        header_signals(set, keys, recorder, b);
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    return ECUIO_OK;
}

/* An armed recorder that nothing could open a window for is refused, so
 * the caller learns it now (ADR-0063 section 7): true with the detail
 * set */
bool attach_armed_refused(
    const ecuio_recorder_t* recorder,
    const std::shared_ptr<const ecuio_condition_record>& conditions)
{
    if (recorder->armed) {
        bool records = false;
        for (size_t i = 0; conditions != nullptr &&
                           i < conditions->conditions.size() && !records;
             ++i) {
            records = conditions->conditions[i].record;
        }
        if (!records) {
            ecuio_detail_set(
                conditions == nullptr
                    ? "no condition records: the session has no set of "
                      "conditions attached"
                    : "no condition records: no enabled condition in the "
                      "set has record");
            return true;
        }
    }
    return false;
}

/* The recorder takes what was made, under the link's mutex. Its one
 * allocation, the header's text joined, comes before any change, so a
 * throw leaves the recorder ready for the next try. */
void attach_commit(ecuio_recorder_t* recorder, attach_build& b,
                   std::shared_ptr<const ecuio_condition_record>& conditions,
                   uint32_t generation,
                   const std::shared_ptr<ecuio_record_link>& link)
{
    std::string& header = b.header;
    if (conditions != nullptr) {
        char line[kConditionsLineMax];
        const int n = std::snprintf(line, sizeof line, "%s%s,%s\n",
                                    ecuio_recording_rows::kConditionsKey,
                                    ecuio_recording_rows::kConditionsName,
                                    conditions->sha256.c_str());
        const size_t len = n < 0 ? 0
                           : static_cast<size_t>(n) < sizeof line
                               ? static_cast<size_t>(n)
                               : sizeof line - 1;
        header.insert(b.conditions_at, line, len);
        if (recorder->armed) {
            b.armed_core.insert(b.armed_conditions_at, line, len);
        }
    }
    if (!recorder->armed) {
        static const char kColumns[] =
            "columns  local time, seconds since the recording started "
            "(host_us / 1e6 in the CSV files), kind, text\n\n";
        std::lock_guard<std::mutex> n(recorder->note_m);
        /* The caller's lines open() queued, then the columns */
        item& begin = recorder->items.front();
        header.reserve(header.size() + begin.text.size() + sizeof kColumns);
        header += begin.text;
        header += kColumns;
        begin.text.swap(header);
        begin.bytes = std::move(recorder->signals_json);
    }
    if (recorder->armed) {
        /* Room for each state was kept before the lock, so these sizes
         * take no allocation; conditions is not NULL (the check) */
        const size_t n = conditions->conditions.size();
        b.armed_cond.resize(n);
        b.armed_tail.resize(n);
        for (const ecuio_condition_record::entry& c : conditions->conditions) {
            if (c.record) {
                recorder->longest_before =
                    (std::max)(recorder->longest_before, uint64_t{c.before_us});
            }
        }
        recorder->scan_on.swap(b.armed_cond);
        recorder->tail_cond.swap(b.armed_tail);
        recorder->armed_core.swap(b.armed_core);
        recorder->armed_signals.swap(b.armed_signals);
        recorder->window_json.swap(recorder->signals_json);
        recorder->win_link_state = b.link_state;
        recorder->win_link_episodes = b.link_episodes;
        recorder->win_focus.ids.swap(b.armed_focus.ids);
        recorder->win_focus.pick = b.armed_focus.pick;
        recorder->win_focus.min_sweep_ms = b.armed_focus.min_sweep_ms;
    }
    recorder->conditions = std::move(conditions);
    recorder->conditions_generation = generation;
    recorder->defs = std::move(b.defs);
    recorder->rx_ids.swap(b.rx_ids);
    recorder->channels = static_cast<uint32_t>(recorder->rx_ids.size());
    recorder->link = link;
    recorder->zero_us = b.zero_us;
    recorder->slept_at_attach = b.slept_at_attach;
    recorder->start_wall = b.start_wall;
    recorder->wall_base = b.start_wall;
    recorder->named_focus = std::move(b.named_focus);
}

/* The options' refusals, after out: NULL_ARGUMENT, then INVALID_ARGUMENT */
ecuio_status_t open_refusal(const ecuio_record_options_t* options)
{
    if (options == nullptr || options->root_utf8 == nullptr ||
        (options->header_line_count > 0 && options->header_lines == nullptr)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if ((options->flags & ~ECUIO_RECORD_ARMED) != 0 ||
        options->reserved[0] != 0 || options->reserved[1] != 0 ||
        options->reserved[2] != 0 || options->reserved[3] != 0 ||
        options->root_utf8[0] == '\0') {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return ECUIO_OK;
}

/* The label, then an armed recorder's root and tail, or the recording's
 * folder (made, once it is) and its three files */
ecuio_status_t open_files(const ecuio_record_options_t* options,
                          const std::string& root, ecuio_recorder& r,
                          std::string& made)
{
    const std::string label =
        options->label_utf8 != nullptr ? reduce_label(options->label_utf8) : "";
    const std::string tail = label.empty() ? "" : " " + label;
    if ((options->flags & ECUIO_RECORD_ARMED) != 0) {
        /* Each window makes its own folder, under the same name's rule */
        r.armed = true;
        r.root = root;
        r.label_tail = tail;
    } else {
        std::string why;
        std::string leaf;
        if (!make_recording_folder(root, std::time(nullptr), tail, &leaf,
                                   &why)) {
            ecuio_detail_set("can't create the recording's folder: %s",
                             why.c_str());
            return ECUIO_ERR_RECORDING_FAILED;
        }
        made.swap(leaf); /* the search's name, never one it passed by */
        r.folder = absolute(made);
        r.published = r.folder;

        r.frames = open_file(made + "/frames.csv");
        r.signals = open_file(made + "/signals.csv");
        r.events = open_file(made + "/events.log");
        if (r.frames == nullptr || r.signals == nullptr ||
            r.events == nullptr) {
            const int e = errno;
            ecuio_detail_set("can't create the recording's files: %s",
                             e != 0 ? errno_text(e).c_str()
                                    : "the folder refused them");
            return ECUIO_ERR_RECORDING_FAILED;
        }
    }
    return ECUIO_OK;
}

/* The caller's header lines, under one key, written at attach */
void queue_caller_lines(const ecuio_record_options_t* options,
                        ecuio_recorder& r)
{
    std::string caller;
    for (uint32_t i = 0; i < options->header_line_count; ++i) {
        if (options->header_lines[i] != nullptr) {
            caller += "caller  " + one_line(options->header_lines[i]) + "\n";
        }
    }
    r.items.push_back(item{item_kind::begin, 0, caller, {}, {}});
    if (r.armed) {
        r.armed_caller = caller;
    }
}

/* The root, the files and the writer, every step after the ring: a
 * failed allocation is OUT_OF_MEMORY, a writer that can't start
 * RECORDING_FAILED in words; made the recording's folder once it is */
ecuio_status_t open_and_start(const ecuio_record_options_t* options,
                              ecuio_recorder& r, std::string& made)
{
    try {
        std::string root = options->root_utf8;
        while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) {
            root.pop_back();
        }
        std::string why;
        if (!make_root(root, &why)) {
            ecuio_detail_set("can't create the recordings' folder: %s",
                             why.c_str());
            return ECUIO_ERR_RECORDING_FAILED;
        }
        const ecuio_status_t st = open_files(options, root, r, made);
        if (st != ECUIO_OK) {
            return st;
        }
        queue_caller_lines(options, r);
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    try {
        r.writer = std::thread([rec = &r] { rec->run(); });
    } catch (const std::bad_alloc&) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    } catch (...) {
        ecuio_detail_set("can't start the recording's writer");
        return ECUIO_ERR_RECORDING_FAILED;
    }
    return ECUIO_OK;
}

/* What a refused open made in the recording's folder, once the recorder
 * closed its files: those files, then the folder if that left it empty,
 * so no list shows a recording that won't open */
void remove_unfinished(const std::string& made, const bool opened[3]) noexcept
{
    if (made.empty()) {
        return;
    }
    try {
        const char* const kFiles[3] = {"/frames.csv", "/signals.csv",
                                       "/events.log"};
        for (int k = 0; k < 3; ++k) {
            if (opened[k]) {
                ecuio_files::remove_file(made + kFiles[k]);
            }
        }
        ecuio_files::remove_folder(made);
    } catch (...) {
        /* out of memory naming them: the folder stays */
    }
}

} // namespace

extern "C" {

ecuio_status_t ecuio_recorder_open(const ecuio_record_options_t* options,
                                   ecuio_recorder_t** out)
{
    ecuio_detail_clear();
    if (out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = nullptr;
    const ecuio_status_t refused = open_refusal(options);
    if (refused != ECUIO_OK) {
        return refused;
    }

    std::unique_ptr<ecuio_recorder> r;
    try {
        r = std::make_unique<ecuio_recorder>();
        r->cap =
            options->ring_bytes != 0 ? options->ring_bytes : kDefaultRingBytes;
        r->ring.reset(new uint8_t[r->cap]);
        r->note_cap =
            options->note_lines != 0 ? options->note_lines : kDefaultNoteLines;
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }

    /* A failure from here frees the recorder, its files closed, and takes
     * back what the open made in the recording's folder */
    std::string made;
    const ecuio_status_t st = open_and_start(options, *r, made);
    if (st != ECUIO_OK) {
        const bool opened[3] = {r->frames != nullptr, r->signals != nullptr,
                                r->events != nullptr};
        r.reset();
        remove_unfinished(made, opened);
        return st;
    }
    *out = r.release();
    return ECUIO_OK;
}

ecuio_status_t ecuio_recorder_signal_file(ecuio_recorder_t* recorder,
                                          const ecuio_signal_file_t* file)
{
    if (recorder == nullptr || file == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    std::vector<ecuio_signal_key_t> keys;
    ecuio_status_t s = ecuio_recording_keys::set_keys(&file->set, file, &keys);
    if (s != ECUIO_OK) {
        return s;
    }
    const std::string& bytes = ecuio_sf::bytes_of(*file);
    s = ecuio_recorder_take_signals(recorder, &file->set, bytes.data(),
                                    bytes.size(), file->plugin.c_str());
    if (s == ECUIO_OK) {
        recorder->signal_keys = std::move(keys);
    }
    return s;
}

ecuio_status_t ecuio_session_record(ecuio_session_t* session,
                                    ecuio_recorder_t* recorder)
{
    ecuio_status_t s = attach_check(session, recorder);
    if (s != ECUIO_OK) {
        return s;
    }

    /* Built in locals first: the recorder changes only once the link is
     * taken, so a refused attach leaves it ready for the next try */
    const std::shared_ptr<ecuio_record_link> link = session->record_link;
    attach_build b;
    s = attach_prepare(session, recorder, b);
    if (s != ECUIO_OK) {
        return s;
    }

    /* The set the session judges by, read under the link's mutex and
     * callback_mutex, so it is the one the recording keeps */
    std::shared_ptr<const ecuio_condition_record> conditions;
    uint32_t generation = 0;
    {
        /* Under the mutex that orders detaches, so a closing recorder or a
         * second attach sees one state of the link, and the refusal is judged
         * here alone. The zero goes in before active, whose release publishes
         * it to the worker (dispatch pins the recorder, then reads the
         * zero). */
        std::lock_guard<std::mutex> g(link->m);
        if (link->active.load(std::memory_order_seq_cst) != nullptr) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> c(session->callback_mutex);
            conditions = session->conditions_record;
            generation = session->conditions_generation;
        }
        if (attach_armed_refused(recorder, conditions)) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        if (const auto hook = g_before_record_attach.load()) {
            hook(g_before_record_attach_user.load());
        }
        try {
            attach_commit(recorder, b, conditions, generation, link);
        } catch (...) {
            return ECUIO_ERR_OUT_OF_MEMORY;
        }
        link->zero_us.store(b.zero_us, std::memory_order_relaxed);
        link->active.store(recorder, std::memory_order_release);
    }
    recorder->attached.store(true, std::memory_order_release);
    /* A focus set since the header's read may have found no recorder to
     * pin; the compare keeps it from being written twice */
    ecuio_record_focus(session);
    return ECUIO_OK;
}

void ecuio_session_test_before_record_attach(void (*hook)(void* user),
                                             void* user)
{
    /* The user before the hook, and a clear leaves the user: a reader
     * that took the old hook still finds its own */
    if (hook != nullptr) {
        g_before_record_attach_user.store(user);
    }
    g_before_record_attach.store(hook);
}

} // extern "C"
