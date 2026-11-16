// File/layer: cli/session_lines.cpp | havne-cli, what the commands with a session
//             share: the connect, the session's lines, the recording's
// Testing: UNIT: CliGoldenTest, CliConnectTest, CliReplayTest,
//          CliRecordTest, CliLogRunTest, CliRefusedRunTest, CliVinRunTest,
//          CliSessionRunTest (ctest); CliTimingTest, CliVinTest (test_cli)
//
// Public headers only: what any program linking core can do.

#include "condition_lines.h"
#include "diagnostics_line.h"
#include "timing_line.h"
#include "view_lines.h"
#include "vin_line.h"
#include "jsonl.h"
#include "refused_signals.h"
#include "scan_line.h"
#include "signal_file_lines.h"

#include "profile_source.h"
#include "session_lines.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"
#include "ecuio/vin.h"

namespace cmd {

namespace {

// How long a connect may take. Each of core's waits on the way has a
// bound of its own, the longest the surrogate's handshake and its
// OP_OPEN (8 s each); a stuck spawn held one connect past all of them
// (an antivirus holding a newly built surrogate; ADR-0031's appendix of
// 2026-11-01), so the CLI keeps a deadline of its own. HAVNE_CLI_CONNECT_MS (tests only)
// shortens it.
constexpr unsigned long kConnectMs = 60 * 1000;

unsigned long connect_ms()
{
    return env_ms("HAVNE_CLI_CONNECT_MS", kConnectMs);
}

} // namespace

// ecuio_session_create within connect_ms(): on a thread of its own, the
// detail carried back from it (it's per thread). Past the deadline, the
// error line (after the start line, when the caller hasn't written it
// yet: start_first) and exit 3 at once; the process's end ends the
// surrogate with it (its job object), wherever the connect was held.
ecuio_status_t create_within(Output& out, const char* uri,
                             ecuio_session_t** session, std::string* detail,
                             bool start_first)
{
    struct Connect {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        ecuio_status_t status = ECUIO_ERR_TRANSPORT_HARD_FAILURE;
        ecuio_session_t* session = nullptr;
        std::string detail;
        std::string uri;
    };
    auto c = std::make_shared<Connect>();
    c->uri = uri;
    std::thread([c] {
        ecuio_session_t* s = nullptr;
        const ecuio_status_t st = ecuio_session_create(c->uri.c_str(), &s);
        const std::string why = ecuio_last_error_detail();
        std::lock_guard<std::mutex> lock(c->mutex);
        c->status = st;
        c->session = s;
        c->detail = why;
        c->done = true;
        c->cv.notify_all();
    }).detach();
    const unsigned long ms = connect_ms();
    std::unique_lock<std::mutex> lock(c->mutex);
    if (!c->cv.wait_for(lock, std::chrono::milliseconds(ms),
                        [&] { return c->done; })) {
        char why[96];
        std::snprintf(why, sizeof why, "the connect didn't finish within %lu s",
                      (ms + 999) / 1000);
        if (start_first) {
            write_start(out);
        }
        write_error(out, ECUIO_ERR_TRANSPORT_HARD_FAILURE, why);
        out.flush();
        std::fflush(stderr);
        std::_Exit(kExitConnect);
    }
    *session = c->session;
    *detail = c->detail;
    return c->status;
}

namespace {

const char* backend_name(uint32_t backend)
{
    switch (backend) {
    case ECUIO_BACKEND_MOCK:
        return "mock";
    case ECUIO_BACKEND_J2534_SURROGATE:
        return "j2534";
    case ECUIO_BACKEND_REPLAY:
        return "replay";
    case ECUIO_BACKEND_ELM327:
        return "elm327";
    case ECUIO_BACKEND_SOCKETCAN:
        return "socketcan";
    default:
        return "unknown";
    }
}

} // namespace

void write_session(Output& out, const ecuio_session_t* session,
                   const ecuio_session_info_t& info)
{
    char buf[4096];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"session\"");
    l.key("backend");
    l.string(backend_name(info.backend), SIZE_MAX);
    l.key("profile");
    l.string(info.profile, sizeof info.profile);
    l.key("protocol_id");
    l.u64(info.protocol_id);
    l.key("baud");
    l.u64(info.baud);
    l.key("channels");
    l.ch('[');
    const uint32_t channels = module_count(info);
    for (uint32_t i = 0; i < channels; ++i) {
        ecuio_channel_decl_t m{};
        (void)ecuio_session_get_module(session, i, &m);
        if (i) {
            l.ch(',');
        }
        l.ch('{');
        l.key("rx");
        l.hex_id(m.rx_id);
        l.key("tx");
        l.hex_id(m.tx_id);
        l.ch('}');
    }
    l.ch(']');
    if (info.flags & ECUIO_SESSION_INFO_VBATT) {
        l.key("vbatt_mv");
        l.u64(info.vbatt_mv);
    }
    if (info.flags & ECUIO_SESSION_INFO_VERSIONS) {
        l.key("firmware");
        l.string(info.firmware_version, sizeof info.firmware_version);
        l.key("dll_version");
        l.string(info.dll_version, sizeof info.dll_version);
        l.key("api_version");
        l.string(info.api_version, sizeof info.api_version);
    }
    if (info.flags & ECUIO_SESSION_INFO_PENDING_OFF) {
        l.key("pending_off");
        l.raw("true");
    }
    if (info.flags & ECUIO_SESSION_INFO_LISTEN_ONLY) {
        l.key("listen_only");
        l.raw("true");
    }
    if (info.backend == ECUIO_BACKEND_REPLAY) {
        l.key("replay");
        l.ch('{');
        l.key("frames");
        l.u64(info.replay_frames);
        l.key("span_ms");
        l.u64(info.replay_span_ms);
        l.key("speed_milli");
        l.u64(info.replay_speed_milli);
        l.key("loop");
        l.raw((info.flags & ECUIO_SESSION_INFO_REPLAY_LOOP) ? "true" : "false");
        if (info.flags & ECUIO_SESSION_INFO_REPLAY_FAULTS) {
            l.key("faults");
            l.raw("true");
        }
        if (info.replay_rows_torn != 0) {
            l.key("rows_torn");
            l.u64(info.replay_rows_torn);
        }
        l.ch('}');
    }
    l.ch('}');
    write_line(out, l);
}

namespace {

// One ECU's diagnostics once a read has completed (diagnostics_line.h)
void write_diagnostics(Output& out, const ecuio_channel_diagnostics_t& d)
{
    char buf[8192];
    cli::Line l(buf, sizeof buf);
    cli::diagnostics_line(l, d);
    write_line(out, l);
}

} // namespace

// The latest model year a VIN can carry (ADR-0043): this year plus one,
// the bridge's bound too
uint32_t latest_model_year()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &now) != 0) {
        return 0;
    }
#else
    if (gmtime_r(&now, &utc) == nullptr) {
        return 0;
    }
#endif
    return static_cast<uint32_t>(utc.tm_year + 1900 + 1);
}

// A line for each ECU whose identity has settled since the last call
// (vin_line.h): generations holds each channel's last written. Ahead of
// them, the vehicle line once the first channel's VIN is in and new
// (vehicle, the VIN written last)
void poll_identity(Output& out, ecuio_session_t* session, uint32_t channels,
                   uint32_t* generations, char (&vehicle)[ECUIO_VIN_TEXT_LEN])
{
    for (uint32_t c = 0; c < channels; ++c) {
        ecuio_channel_identity_t id;
        if (ecuio_session_get_channel_identity(session, c, &id) != ECUIO_OK) {
            continue;
        }
        if (c == 0 && cli::vehicle_due(id, vehicle)) {
            char buf[512];
            cli::Line l(buf, sizeof buf);
            cli::vehicle_line(l, id, latest_model_year());
            write_line(out, l);
        }
        if ((id.flags & ECUIO_IDENTITY_COMPLETE) &&
            id.generation != generations[c]) {
            generations[c] = id.generation;
            char buf[4096];
            cli::Line l(buf, sizeof buf);
            cli::identity_line(l, id, latest_model_year());
            write_line(out, l);
        }
    }
}

// A session line per ECU whose session changed and a scan line per ECU
// whose DID scan moved (cli/scan_line.h), every change when final; the
// channels' reports into reports[], for --until-scanned
void poll_scan(Output& out, ecuio_session_t* session, uint32_t channels,
               cli::ScanSeen* seen, cli::SessionSeen* sessions,
               ecuio_channel_support_t* reports, bool final)
{
    const uint64_t now = ecuio_clock_us();
    for (uint32_t c = 0; c < channels; ++c) {
        ecuio_channel_support_t& s = reports[c];
        if (ecuio_session_get_channel_support(session, c, &s) != ECUIO_OK) {
            s = ecuio_channel_support_t{};
            continue;
        }
        if (cli::session_due(s, sessions[c])) {
            char buf[512];
            cli::Line l(buf, sizeof buf);
            cli::session_line(l, s);
            write_line(out, l);
        }
        if (cli::scan_due(s, seen[c], now, final)) {
            char buf[512];
            cli::Line l(buf, sizeof buf);
            cli::scan_line(l, s);
            write_line(out, l);
        }
    }
}

// A session line per ECU whose session changed (cli/scan_line.h), as
// poll_scan writes it, for the commands that write no scan lines
void poll_sessions(Output& out, ecuio_session_t* session, uint32_t channels,
                   cli::SessionSeen* seen)
{
    for (uint32_t c = 0; c < channels; ++c) {
        ecuio_channel_support_t s;
        if (ecuio_session_get_channel_support(session, c, &s) != ECUIO_OK) {
            continue;
        }
        if (cli::session_due(s, seen[c])) {
            char buf[512];
            cli::Line l(buf, sizeof buf);
            cli::session_line(l, s);
            write_line(out, l);
        }
    }
}

namespace {

// The car's model year, from the first channel's VIN (the vehicle's,
// ADR-0027 section 3) decoded as the identity line decodes it; 0 with
// none
uint32_t session_model_year(const ecuio_session_t* session)
{
    ecuio_channel_identity_t id;
    char vin[ECUIO_VIN_TEXT_LEN];
    ecuio_vin_info_t v;
    if (ecuio_session_get_channel_identity(session, 0, &id) != ECUIO_OK ||
        ecuio_identity_vin(&id, vin, sizeof vin) != ECUIO_OK ||
        vin[0] == '\0' ||
        ecuio_vin_decode(vin, latest_model_year(), &v) != ECUIO_OK) {
        return 0;
    }
    return v.model_year;
}

} // namespace

// A line for each ECU whose diagnostics read has completed since the
// last call: generations holds each channel's last written. After any,
// once no read is still under way, the car's inspection line
// (ADR-0045) over every channel's report
void poll_diagnostics(Output& out, ecuio_session_t* session, uint32_t channels,
                      uint32_t* generations)
{
    std::vector<ecuio_channel_diagnostics_t> reports(channels);
    bool wrote = false;
    for (uint32_t c = 0; c < channels; ++c) {
        ecuio_channel_diagnostics_t& d = reports[c];
        if (ecuio_session_get_channel_diagnostics(session, c, &d) != ECUIO_OK) {
            d = ecuio_channel_diagnostics_t{};
            continue;
        }
        if ((d.flags & ECUIO_DIAG_COMPLETE) && d.generation != generations[c]) {
            generations[c] = d.generation;
            write_diagnostics(out, d);
            wrote = true;
        }
    }
    if (!wrote) {
        return;
    }
    const uint32_t year = session_model_year(session);
    ecuio_inspection_t i;
    if (ecuio_diag_inspect(reports.data(), channels, year, &i) == ECUIO_OK &&
        i.verdict != ECUIO_INSPECTION_READING &&
        i.verdict != ECUIO_INSPECTION_UNKNOWN) {
        char buf[1024];
        cli::Line l(buf, sizeof buf);
        cli::inspection_line(l, i, year);
        write_line(out, l);
    }
}

// Reply timing's snapshots (ADR-0042 section 3): every 10 s of session
// time, and once at the end (final), a timing line for each request whose
// counts changed since the last; rows is sized before the session starts
void poll_timing(Output& out, ecuio_session_t* session,
                 cli::TimingSnapshots& snaps,
                 std::vector<ecuio_request_timing_t>& rows, bool final)
{
    ecuio_session_timing_t t;
    uint32_t count = 0;
    if (ecuio_session_get_timing(session, &t, nullptr, 0, &count) != ECUIO_OK ||
        count == 0 || (!final && !snaps.due(t.snapshot_us)) ||
        ecuio_session_get_timing(session, &t, rows.data(),
                                 static_cast<uint32_t>(rows.size()),
                                 &count) != ECUIO_OK) {
        return;
    }
    snaps.taken(t.snapshot_us);
    const uint32_t n =
        count < rows.size() ? count : static_cast<uint32_t>(rows.size());
    for (uint32_t i = 0; i < n; ++i) {
        if (!snaps.changed(i, rows[i])) {
            continue;
        }
        ecuio_channel_decl_t m{}; /* rx 0 for a channel the session lacks */
        (void)ecuio_session_get_module(session, rows[i].channel, &m);
        const uint32_t rx = m.rx_id;
        char buf[1024];
        cli::Line l(buf, sizeof buf);
        cli::timing_line(l, t.snapshot_us, rx, rows[i], t.interval_clock);
        write_line(out, l);
    }
}

// The end line's ISO-TP counts, where core reassembles (ADR-0040 section 5)
void write_isotp(cli::Line& l, const ecuio_session_t* session)
{
    ecuio_session_timing_t t;
    uint32_t count = 0;
    if (ecuio_session_get_timing(session, &t, nullptr, 0, &count) != ECUIO_OK ||
        (t.flags & ECUIO_TIMING_ISOTP) == 0) {
        return;
    }
    l.key("isotp");
    l.ch('{');
    l.key("first_frames");
    l.u64(t.isotp_first_frames);
    l.key("completed");
    l.u64(t.isotp_completed);
    l.key("dropped");
    l.u64(t.isotp_dropped);
    l.key("malformed");
    l.u64(t.isotp_malformed);
    l.ch('}');
}

// The signals the session's frames come without (ADR-0039 section 6), as ids
// the signals line names, whenever core's list changes; last holds the
// list last written, read sized to the set
void poll_absent(Output& out, ecuio_session_t* session,
                 std::vector<uint16_t>& read, std::vector<uint16_t>& last)
{
    uint32_t count = 0;
    if (read.empty() ||
        ecuio_session_get_absent_signals(session, read.data(),
                                         static_cast<uint32_t>(read.size()),
                                         &count) != ECUIO_OK) {
        return;
    }
    count = std::min<uint32_t>(count, static_cast<uint32_t>(read.size()));
    if (count == last.size() &&
        std::equal(last.begin(), last.end(), read.begin())) {
        return;
    }
    last.assign(read.begin(), read.begin() + count);
    BigLine b(64 + size_t{count} * 8);
    cli::Line& l = b.line;
    l.raw("{\"type\":\"absent\"");
    l.key("ids");
    l.ch('[');
    for (uint32_t i = 0; i < count; ++i) {
        if (i) {
            l.ch(',');
        }
        l.u64(last[i]);
    }
    l.ch(']');
    l.ch('}');
    write_line(out, l);
}

// A refused line whenever the signals their ECUs refused change, or the
// refusals past a report's list (cli/refused_signals.h, ADR-0051 section
// 3), from the channels' reports poll_scan read; an emptied list writes
// []. With --values only, as the signals they name.
void poll_refused(Output& out, const ecuio_signal_set_t* set,
                  const ecuio_channel_support_t* reports, uint32_t channels,
                  std::vector<cli::RefusedSignal>& last,
                  std::vector<cli::RefusedMore>& last_more)
{
    std::vector<cli::RefusedSignal> now =
        cli::refused_signals(set, reports, channels);
    std::vector<cli::RefusedMore> more =
        cli::refused_unlisted(set, reports, channels);
    const auto same = [](const cli::RefusedSignal& a,
                         const cli::RefusedSignal& b) {
        return a.id == b.id && a.length == b.length && a.nrc == b.nrc &&
               std::equal(a.request, a.request + sizeof a.request, b.request);
    };
    const auto same_more = [](const cli::RefusedMore& a,
                              const cli::RefusedMore& b) {
        return a.rx_id == b.rx_id && a.count == b.count;
    };
    if (now.size() == last.size() &&
        std::equal(now.begin(), now.end(), last.begin(), same) &&
        more.size() == last_more.size() &&
        std::equal(more.begin(), more.end(), last_more.begin(), same_more)) {
        return;
    }
    last = std::move(now);
    last_more = std::move(more);
    BigLine b(cli::refused_line_bytes(last.size(), last_more.size()));
    cli::refused_line(b.line, last, last_more);
    write_line(out, b.line);
}

// The session's signals: each def's id, name, unit, decimals and group,
// its "gauge" when the session's signal file gives one, then for an enum
// "enum":true and its "labels" ({"<raw>": label}, as a signal file's
// "values"), for a computed def "computed":true
void write_signals(Output& out, const ecuio_signal_set_t* set,
                   const ecuio_signal_file_t* file)
{
    const uint32_t count = set ? set->count : 0;
    size_t bytes = 256 + size_t{count} * 512;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t labels = 0;
        cli::def_labels(*set, set->defs[i], &labels);
        bytes += cli::labels_bytes(labels);
        if (ecuio_signal_file_gauge(file, &set->defs[i]) != nullptr) {
            bytes += cli::kGaugeBytes;
        }
    }
    BigLine b(bytes);
    cli::Line& l = b.line;
    l.raw("{\"type\":\"signals\"");
    l.key("signals");
    l.ch('[');
    for (uint32_t i = 0; i < count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (i) {
            l.ch(',');
        }
        l.ch('{');
        l.key("id");
        l.u64(d.id);
        l.key("name");
        l.string(d.name, sizeof d.name);
        l.key("unit");
        l.string(d.unit, sizeof d.unit);
        l.key("decimals");
        l.u64(d.decimal_places);
        l.key("group");
        const char* group = ecuio_signal_group_name(d.group);
        l.string(group, std::strlen(group));
        cli::gauge_field(l, ecuio_signal_file_gauge(file, &d));
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            l.key("computed");
            l.raw("true");
        } else if (d.kind == ECUIO_VALUE_ENUM) {
            uint32_t n = 0;
            const ecuio_enum_entry_t* labels = cli::def_labels(*set, d, &n);
            l.key("enum");
            l.raw("true");
            l.key("labels");
            cli::labels_object(l, labels, n);
        }
        l.ch('}');
    }
    l.ch(']');
    l.ch('}');
    write_line(out, l);
}

namespace {

// A recording line: started with its folder, failed with core's reason,
// closed with whether every file ended whole
void write_recording(Output& out, const char* state, const char* key,
                     const char* text)
{
    char buf[8192];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"recording\"");
    l.key("state");
    l.string(state, SIZE_MAX);
    if (key != nullptr) {
        l.key(key);
        l.string(text, 4096);
    }
    l.ch('}');
    write_line(out, l);
}

} // namespace

// stream --record and log: a recorder opened under root (with the label)
// and attached before the session starts; false (and the recording line
// says why) when it can't be
bool open_recording(Output& out, ecuio_session_t* session, const char* root,
                    const char* label, ecuio_recorder_t** recorder,
                    bool armed)
{
    const std::string caller =
        std::string("havne-cli ") + ecuio_version_string();
    const char* lines[] = {caller.c_str()};
    ecuio_record_options_t o;
    std::memset(&o, 0, sizeof o);
    o.root_utf8 = root;
    o.label_utf8 = label;
    o.header_lines = lines;
    o.header_line_count = 1;
    o.flags = armed ? ECUIO_RECORD_ARMED : 0u;
    ecuio_status_t st = ecuio_recorder_open(&o, recorder);
    if (st != ECUIO_OK) {
        const char* detail = ecuio_last_error_detail();
        write_recording(out, "failed", "reason",
                        *detail ? detail : ecuio_status_string(st));
        return false;
    }
    // The signal file decoding the session goes in as signals.json
    const ecuio_signal_file_t* file = session_signal_file();
    if (file != nullptr) {
        st = ecuio_recorder_signal_file(*recorder, file);
    }
    std::string why;
    if (st == ECUIO_OK) {
        st = ecuio_session_record(session, *recorder);
        why = ecuio_last_error_detail();
    }
    if (st != ECUIO_OK) {
        ecuio_recorder_close(*recorder, "the session couldn't be recorded");
        *recorder = nullptr;
        write_recording(out, "failed", "reason",
                        armed && !why.empty() ? why.c_str()
                                              : ecuio_status_string(st));
        return false;
    }
    if (armed) {
        /* Armed, no folder yet: the log's loop names each window's */
        return true;
    }
    char folder[4096];
    uint32_t len = 0;
    ecuio_recorder_get_folder(*recorder, folder, sizeof folder, &len);
    write_recording(out, "started", "folder", folder);
    return true;
}

// Each drain: the lines typed since the last are markers, each a mark
// line; a recording that failed says so once
void poll_recording(Output& out, ecuio_recorder_t* recorder, MarkQueue* marks,
                    bool* failed)
{
    if (recorder == nullptr) {
        return;
    }
    std::vector<std::string> typed;
    if (marks != nullptr) {
        std::lock_guard<std::mutex> g(marks->m);
        typed.swap(marks->lines);
    }
    for (const std::string& label : typed) {
        if (ecuio_recorder_mark(recorder, label.c_str()) != ECUIO_OK) {
            continue;
        }
        char buf[8192];
        cli::Line l(buf, sizeof buf);
        l.raw("{\"type\":\"mark\"");
        l.key("label");
        l.string(label.c_str(), 4096);
        l.ch('}');
        write_line(out, l);
    }
    ecuio_recorder_state_t st;
    if (!*failed && ecuio_recorder_get_state(recorder, &st) == ECUIO_OK &&
        st.state == ECUIO_RECORDING_FAILED) {
        *failed = true;
        write_recording(out, "failed", "reason", st.reason);
    }
}

// The recorder closed, after its session or, when that close hangs past
// an interrupt, while still attached (ADR-0049 section 1): either way the
// files end whole, with the summary
void close_recording(Output& out, ecuio_recorder_t** recorder, const char* why,
                     bool* failed, bool armed)
{
    if (*recorder == nullptr) {
        return;
    }
    char buf[192];
    cli::Line l(buf, sizeof buf);
    const bool whole = cli::close_with_line(l, *recorder, why, armed);
    *recorder = nullptr;
    *failed = *failed || !whole;
    write_line(out, l);
}

// A health line on each change of the session's error state (the code or
// its episode count); the state read is left in last_error and episodes
void poll_health(Output& out, ecuio_session_t* session, int32_t* last_error,
                 uint32_t* episodes)
{
    int32_t now_error = ECUIO_OK;
    uint32_t now_episodes = 0;
    ecuio_session_get_error_state(session, &now_error, &now_episodes);
    if (now_error == *last_error && now_episodes == *episodes) {
        return;
    }
    *last_error = now_error;
    *episodes = now_episodes;
    char buf[256];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"health\"");
    l.key("code");
    l.u64(static_cast<uint32_t>(now_error));
    l.key("message");
    l.string(ecuio_status_string(static_cast<ecuio_status_t>(now_error)),
             SIZE_MAX);
    l.key("episodes");
    l.u64(now_episodes);
    l.ch('}');
    write_line(out, l);
}

// Whether a session asks its signals (an adapter's, a sim's): the mock,
// a replay and a listening session take no pick, as in the app
bool session_asks(ecuio_session_t* session)
{
    ecuio_session_info_t info;
    std::memset(&info, 0, sizeof info);
    return ecuio_session_get_info(session, &info) == ECUIO_OK &&
           (info.flags & ECUIO_SESSION_INFO_REQUESTS) != 0;
}

// A replay that doesn't loop has ended when every row is read and every
// row its faults didn't take (ADR-0041) has been written: frames lost to
// ?faults= never arrive, so the rows alone would wait forever
bool replay_done(const ecuio_session_t* session,
                 const ecuio_session_info_t& info, uint64_t covered)
{
    ecuio_replay_faults_t f;
    if (ecuio_session_get_replay_faults(session, &f) != ECUIO_OK) {
        return covered >= info.replay_frames;
    }
    return f.frames >= info.replay_frames &&
           covered + f.dropped + f.overflowed >= info.replay_frames;
}

} // namespace cmd
