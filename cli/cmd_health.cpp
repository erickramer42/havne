// File/layer: cli/cmd_health.cpp | havne-cli, health (ADR-0054's appendix "the health
//             report as built")
// Testing: UNIT: CliHealthRunTest, CliTransferRunTest (ctest);
//          CliHealthTest (test_cli)
//
// Public headers only: what any program linking core can do.

#include "health_lines.h"
#include "jsonl.h"

#include "commands.h"
#include "session_lines.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

namespace cmd {

namespace {

// ---- health (ADR-0054's appendix "the health report as built") ----

// Microseconds since 1970 UTC: the report's time, the CLI's to give
uint64_t utc_now_us()
{
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(since).count());
}

// A live report's values: each def's newest and when it came
// (ecuio_clock_us, 0 none), a slot for every def ID so the worker's
// callback allocates nothing
struct HealthValues {
    std::mutex mutex;
    std::vector<double> value;
    std::vector<uint64_t> at_us;
};

void on_health_values(const ecuio_value_batch_t* batch, void* user_data)
{
    auto* h = static_cast<HealthValues*>(user_data);
    const uint64_t now = ecuio_clock_us();
    std::lock_guard<std::mutex> lock(h->mutex);
    for (uint32_t i = 0; i < batch->count; ++i) {
        const ecuio_value_t& v = batch->values[i];
        if (v.kind == ECUIO_VALUE_TEXT || v.kind == ECUIO_VALUE_HEX) {
            continue;
        }
        h->value[v.signal_id] = v.value;
        h->at_us[v.signal_id] = now != 0 ? now : 1;
    }
}

// The report's line, or a failed check's class then the error line
int write_health_result(Output& out, const cli::HealthArgs& a,
                        ecuio_status_t rc, const ecuio_health_report_t& r,
                        bool settled)
{
    BigLine b(cli::health_line_bytes(a));
    cli::Line& l = b.line;
    if (rc != ECUIO_OK) {
        if (r.failed_class != 0) {
            cli::health_failed_line(l, r);
            write_line(out, l);
        }
        write_error(out, rc);
        out.flush();
        return ended(out, kExitConnect);
    }
    cli::health_line(l, a, r, settled);
    write_line(out, l);
    out.flush();
    return ended(out, kExitOk);
}

} // namespace

// A recording's report: core reads the folder, the CLI gives the time; a
// Ctrl+C stops the read at core's next ask, nothing written
int run_health_recording(cli::HealthArgs& a)
{
    Output out;
    install_handlers(true);
    write_start(out);
    out.flush(); // a second interrupt leaves no half line behind
    a.options.utc_us = utc_now_us();
    a.options.latest_model_year = latest_model_year();
    ecuio_health_report_t r{};
    bool asked = false;
    const ecuio_status_t rc =
        ecuio_health_export_with(&a.options, health_interrupted, &asked, &r);
    second_interrupt_over("health report");
    if (rc == ECUIO_ERR_CANCELED) {
        return ended_interrupted(out);
    }
    return write_health_result(out, a, rc, r, true);
}

// A live session's report: start, session, then once every asked report
// has settled (cli::reports_settled) and no shown signal has gained its
// first value for kHealthQuietUs, or at --seconds or Ctrl+C, the report
// with what's held, each value aged from its arrival. Reads only: core's
// diagnostics and identification ask 01, 02, 03, 07, 09 and 0A, never 04.
int run_health_session(cli::HealthArgs& a, const char* uri)
{
    Output out;
    write_start(out);
    auto values =
        std::unique_ptr<HealthValues>(new (std::nothrow) HealthValues);
    try {
        if (!values) {
            throw std::bad_alloc();
        }
        values->value.assign(65536, 0.0);
        values->at_us.assign(65536, 0);
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "havne-cli: out of memory for the values\n");
        return kExitFailed;
    }
    ecuio_session_t* session = nullptr;
    std::string detail;
    ecuio_status_t st = create_within(out, uri, &session, &detail);
    if (st != ECUIO_OK) {
        write_error(out, st, detail.c_str());
        out.flush();
        return kExitConnect;
    }
    ecuio_session_info_t info;
    std::memset(&info, 0, sizeof info);
    ecuio_session_get_info(session, &info);
    write_session(out, info);
    out.flush();
    ecuio_register_values_callback(session, on_health_values, values.get());
    install_handlers();
    st = ecuio_session_start(session);
    if (st != ECUIO_OK) {
        write_error(out, st);
        out.flush();
        ecuio_session_destroy(session);
        return kExitConnect;
    }
    const uint32_t channels =
        info.channel_count < ECUIO_SESSION_INFO_MAX_CHANNELS
            ? info.channel_count
            : ECUIO_SESSION_INFO_MAX_CHANNELS;
    const ecuio_signal_set_t* set = ecuio_session_signal_set(session);
    const auto settled_now = [&]() {
        bool all = true;
        for (uint32_t c = 0; c < channels; ++c) {
            ecuio_channel_diagnostics_t d{};
            ecuio_channel_identity_t id{};
            (void)ecuio_session_get_channel_diagnostics(session, c, &d);
            (void)ecuio_session_get_channel_identity(session, c, &id);
            all = all && cli::reports_settled(d, id);
        }
        return all;
    };
    const auto shown_with_value = [&]() {
        uint32_t n = 0;
        std::lock_guard<std::mutex> lock(values->mutex);
        for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
            if (cli::shown_def(set->defs[i]) &&
                values->at_us[set->defs[i].id] != 0) {
                ++n;
            }
        }
        return n;
    };
    // The shown defs still owed a first value: PIDs the bitmaps list
    const auto owed_now = [&]() {
        ecuio_channel_support_t supports[ECUIO_SESSION_INFO_MAX_CHANNELS]{};
        for (uint32_t c = 0; c < channels; ++c) {
            (void)ecuio_session_get_channel_support(session, c, &supports[c]);
        }
        uint32_t n = 0;
        std::lock_guard<std::mutex> lock(values->mutex);
        for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
            const ecuio_signal_def_t& d = set->defs[i];
            if (cli::awaited(d, values->at_us[d.id] != 0, supports, channels)) {
                ++n;
            }
        }
        return n;
    };
    const uint64_t deadline = ecuio_clock_us() + a.seconds * 1000000u;
    cli::ValuesQuiet quiet;
    for (;;) {
        const uint64_t now = ecuio_clock_us();
        const bool settled_here = settled_now();
        const bool quiet_here =
            settled_here &&
            quiet.quiet(shown_with_value(), now, cli::kHealthQuietUs);
        if (cli::health_wait_over(settled_here, quiet_here,
                                  quiet_here ? owed_now() : 0, now, deadline) ||
            g_interrupted.load()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const bool settled = settled_now();
    std::vector<ecuio_health_value_t> held;
    {
        const uint64_t now = ecuio_clock_us();
        std::lock_guard<std::mutex> lock(values->mutex);
        for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
            const uint16_t id = set->defs[i].id;
            if (values->at_us[id] != 0) {
                ecuio_health_value_t v{};
                v.def_id = id;
                v.value = values->value[id];
                v.age_us =
                    now > values->at_us[id] ? now - values->at_us[id] : 0;
                held.push_back(v);
            }
        }
    }
    a.options.session = session;
    a.options.values = held.empty() ? nullptr : held.data();
    a.options.value_count = static_cast<uint32_t>(held.size());
    a.options.utc_us = utc_now_us();
    a.options.latest_model_year = latest_model_year();
    ecuio_health_report_t r{};
    const ecuio_status_t rc = ecuio_health_export(&a.options, &r);
    ecuio_session_destroy(session);
    return write_health_result(out, a, rc, r, settled);
}

} // namespace cmd
