// File/layer: cli/cmd_view.cpp | havne-cli, view (ADR-0049 section 5)
// Testing: UNIT: CliGoldenTest, CliViewRunTest, CliUsageTest (ctest);
//          CliViewTest, CliViewFramesTest (test_cli)
//
// Public headers only: what any program linking core can do.

#include "signal_pairs.h"
#include "view_frames.h"
#include "view_lines.h"
#include "jsonl.h"
#include "plugin_lines.h"
#include "recording_signals.h"
#include "together.h"

#include "commands.h"
#include "profile_source.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/plugin.h"
#include "ecuio/profile.h"
#include "ecuio/recording.h"
#include "ecuio/signals.h"

namespace cmd {

// A recording's signal line (view's and info's), an enum's labels on it
void write_signal_line(Output& out, const ecuio_recording_t* r,
                       uint32_t recording, uint32_t index,
                       const ecuio_recording_signal_t& s)
{
    uint32_t count = 0;
    std::vector<ecuio_enum_entry_t> labels;
    if (ecuio_recording_get_labels(r, index, nullptr, 0, &count) == ECUIO_OK &&
        count > 0) {
        labels.resize(count);
        ecuio_recording_get_labels(r, index, labels.data(), count, &count);
    }
    BigLine b(2048 + cli::labels_bytes(count));
    cli::signal_line(b.line, recording, index, s, labels.data(), count);
    write_line(out, b.line);
}

// A recording's condition lines (view's and info's), one for each cond
// line the reader kept, in events.log's order
void write_condition_lines(Output& out, const ecuio_recording_t* r,
                           uint32_t recording,
                           const ecuio_recording_info_t& info)
{
    const uint64_t kept = std::min<uint64_t>(
        info.condition_events, uint64_t{ECUIO_RECORDING_MAX_CONDITION_EVENTS});
    char buf[cli::kRecordedConditionLineBytes];
    for (uint64_t k = 0; k < kept; ++k) {
        ecuio_recording_condition_event_t e{};
        if (ecuio_recording_get_condition_event(r, k, &e) != ECUIO_OK) {
            return;
        }
        cli::Line l(buf, sizeof buf);
        cli::recorded_condition_line(l, recording, e);
        write_line(out, l);
    }
}

namespace {

// One recording opened for view: what it holds, the signals it shows,
// and where the axis puts it
struct Viewed {
    std::unique_ptr<ecuio_recording_t, RecordingCloser> r;
    ecuio_recording_info_t info{};
    ecuio_recording_signals_t signal_file{};
    ecuio_recording_conditions_t conditions{};
    std::vector<ecuio_recording_signal_t> signals;
    std::vector<uint32_t> shown;
    bool aligned = true;
    uint64_t align_host_us = ECUIO_RECORDING_NONE;
    int64_t offset_us = 0;
    std::string decoded; // --frames: the profile, or "signals.json"
    ecuio_stretch_rules_t rules{}; // --stretches: --role's indexes in

    // The index of the signal a text names, its key or a name one signal
    // has (ecuio_recording_find_signal), or UINT32_MAX; *several says a
    // name more than one signal has, core's detail saying their keys
    uint32_t find(const std::string& text, bool* several = nullptr) const
    {
        uint32_t k = UINT32_MAX;
        const bool found =
            ecuio_recording_find_signal(r.get(), text.c_str(), &k) == ECUIO_OK;
        if (several != nullptr) {
            size_t named = 0;
            for (const ecuio_recording_signal_t& s : signals) {
                named +=
                    text == std::string(s.name, strnlen(s.name, sizeof s.name))
                        ? 1u
                        : 0u;
            }
            *several = !found && named > 1;
        }
        return found ? k : UINT32_MAX;
    }
};

// The signal file a registered profile is the set of, when view was given
// it (--signals' file, a --plugin's variant); NULL for any other profile
const ecuio_signal_file_t* given_file(const std::string& profile)
{
    for (const ecuio_signal_file_t* f : {g_together, g_signal_file}) {
        if (f != nullptr && profile == ecuio_signal_file_profile(f)) {
            return f;
        }
    }
    for (uint32_t v = 0; g_plugin != nullptr; ++v) {
        const ecuio_signal_file_t* f = ecuio_plugin_variant_file(g_plugin, v);
        if (f == nullptr) {
            break;
        }
        if (profile == ecuio_signal_file_profile(f)) {
            return f;
        }
    }
    return nullptr;
}

// view's open of one recording: its signals.csv, or with --frames its
// frames.csv through the set cli/view_frames.h's rules pick, *decoded
// naming it. A failure's why for the error line, never a path.
ecuio_status_t open_viewed(const ViewArgs& args, const char* folder,
                           ecuio_recording_t** r, std::string* decoded,
                           std::string* why)
{
    if (!args.frames) {
        const ecuio_status_t st =
            ecuio_recording_open(folder, view_open_interrupted, nullptr, r);
        *why = ecuio_last_error_detail();
        return st;
    }
    // A folder with no frames.csv (one that isn't there, too) is refused
    // in the reader's words before a profile is asked for, as the app's
    // viewer refuses it
    if (!is_file((std::string(folder) + "/frames.csv").c_str())) {
        *why = "the folder has no frames.csv";
        return ECUIO_ERR_LOAD_FAILED;
    }
    const cli::RecordedSignals recorded = recorded_signals(folder);
    bool own = false;
    if (args.profile.empty() && recorded.names_file()) {
        ecuio_recording_t* plain = nullptr;
        ecuio_recording_signals_t sig{};
        if (ecuio_recording_open(folder, nullptr, nullptr, &plain) ==
                ECUIO_OK &&
            ecuio_recording_get_signals(plain, &sig) == ECUIO_OK) {
            own = (sig.flags & ECUIO_RECORDING_SIGNALS_MATCH) != 0;
        }
        ecuio_recording_close(plain);
    }
    const cli::FramesSource source =
        cli::frames_source(args.profile, recorded, own);
    ecuio_signal_file_t* file = nullptr;
    const ecuio_signal_set_t* set = nullptr;
    // The file the set is of, where there is one: through it a computed
    // signal goes by its author's key, as a recording of the file keeps it
    const ecuio_signal_file_t* keyed = nullptr;
    if (source.own_file) {
        const ecuio_status_t st = load_signal_file(
            (std::string(folder) + "/signals.json").c_str(), &file, why, true);
        if (st != ECUIO_OK) {
            return st;
        }
        set = ecuio_signal_file_set(file);
        keyed = file;
        *decoded = "signals.json";
    } else if (source.profile.empty()) {
        *why = source.why;
        return ECUIO_ERR_UNKNOWN_PROFILE;
    } else {
        const bool registered =
            ecuio_profile_lookup(source.profile.c_str(), &set) == ECUIO_OK;
        *why = cli::profile_refusal(source.profile, registered,
                                    set != nullptr ? set->count : 0);
        if (!why->empty()) {
            return ECUIO_ERR_UNKNOWN_PROFILE;
        }
        *decoded = source.profile;
        keyed = given_file(source.profile);
        if (keyed != nullptr) {
            set = ecuio_signal_file_set(keyed);
        }
    }
    const ecuio_status_t st = ecuio_recording_open_frames_with(
        folder, set, keyed, view_open_interrupted, nullptr, r);
    *why = ecuio_last_error_detail();
    ecuio_signal_file_free(file); /* the reader keeps its own copy */
    return st;
}

// view --frames' --signals and --plugin: the file's profile, or each
// variant's, registered for the process's life, and the --profile to
// decode with when none was given. -1 to go on; else the exit code: a
// file that can't be read or is refused is start and an error line (3),
// a --profile that isn't the file's or a variant's a usage error (2)
int use_view_together(Output& out, ViewArgs& args)
{
    TogetherSet set;
    const int rc = merge_together(out, args.together, &set);
    if (rc >= 0) {
        return rc;
    }
    if (!args.profile.empty() && args.profile != set.profile) {
        std::fprintf(stderr, "havne-cli: --profile isn't --together's, %s\n",
                     set.profile.c_str());
        ecuio_signal_file_free(set.merged);
        return kExitUsage;
    }
    args.profile = set.profile;
    if (set.merged == nullptr) {
        return -1;
    }
    const ecuio_status_t st = ecuio_profile_register(
        set.profile.c_str(), ecuio_signal_file_set(set.merged), nullptr);
    if (st != ECUIO_OK) {
        const std::string why = ecuio_last_error_detail();
        ecuio_signal_file_free(set.merged);
        write_start(out);
        write_error(out, st, why.c_str());
        return ended(out, kExitConnect);
    }
    g_together = set.merged;
    return -1;
}

int use_view_signals(Output& out, ViewArgs& args)
{
    std::string why;
    ecuio_status_t st = ECUIO_OK;
    if (args.together.given) {
        return use_view_together(out, args);
    }
    if (args.signals_path != nullptr) {
        ecuio_signal_file_t* file = nullptr;
        st = load_signal_file(args.signals_path, &file, &why);
        if (st == ECUIO_OK) {
            const char* profile = ecuio_signal_file_profile(file);
            if (!args.profile.empty() && args.profile != profile) {
                std::fprintf(stderr,
                             "havne-cli: --profile isn't the signal file's, "
                             "%s\n",
                             profile);
                ecuio_signal_file_free(file);
                return kExitUsage;
            }
            st = ecuio_profile_register(profile, ecuio_signal_file_set(file),
                                        nullptr);
            if (st == ECUIO_OK) {
                g_signal_file = file;
                args.profile = profile;
                return -1;
            }
            why = ecuio_last_error_detail();
            ecuio_signal_file_free(file);
        }
    } else if (args.plugin_path != nullptr) {
        ecuio_plugin_t* p = nullptr;
        st = load_package(args.plugin_path, &p, &why);
        if (st == ECUIO_OK) {
            const ecuio_plugin_info_t info = plugin_info(p);
            std::string names;
            bool known = false;
            for (uint32_t v = 0; v < info.variant_count; ++v) {
                const std::string name =
                    cli::plugin_profile(info.id, variant_of(p, v).c_str());
                names += (v == 0 ? "" : ", ") + name;
                known = known || name == args.profile;
            }
            if (args.profile.empty() && info.variant_count == 1) {
                args.profile = names;
                known = true;
            }
            if (!known) {
                std::fprintf(stderr,
                             "havne-cli: --profile names the variant to "
                             "decode with: %s\n",
                             names.c_str());
                ecuio_plugin_free(p);
                return kExitUsage;
            }
            st = register_variants(p, &why);
            if (st == ECUIO_OK) {
                g_plugin = p;
                return -1;
            }
            (void)ecuio_plugin_free(p);
        }
    } else {
        return -1;
    }
    write_start(out);
    write_error(out, st, why.c_str());
    return ended(out, kExitConnect);
}

// The start of a recording's nth pull (1 the first) under its rules, over
// the whole recording: *at_us, left alone with fewer pulls than that.
// asked is view_interrupted's
ecuio_status_t nth_pull(const Viewed& v, uint32_t n, bool* asked,
                        uint64_t* at_us)
{
    ecuio_stretch_rules_t rules = v.rules;
    rules.kinds = ECUIO_STRETCH_PULLS;
    const uint64_t end = v.info.duration_us + 1;
    /* The count first: n is the caller's, up to 100,000 */
    uint32_t count = 0;
    ecuio_status_t rc = ecuio_recording_stretches_with(
        v.r.get(), &rules, 0, end, view_interrupted, asked, nullptr, 0, &count);
    if (rc != ECUIO_OK || count < n) {
        return rc;
    }
    std::vector<ecuio_recording_stretch_t> found(n);
    bool second = false;
    rc = ecuio_recording_stretches_with(v.r.get(), &rules, 0, end,
                                        view_second_interrupted, &second,
                                        found.data(), n, &count);
    if (rc == ECUIO_OK && count >= n) {
        *at_us = found[n - 1].from_us;
    }
    return rc;
}

// ecuio_recording_stats over [from_us, to_us], and for a span of one
// microsecond, which that call refuses, the microsecond's own column
ecuio_status_t closed_stats(const Viewed& v, const uint32_t* ids,
                            uint32_t count, uint64_t from_us, uint64_t to_us,
                            ecuio_recording_stats_t* out)
{
    if (from_us < to_us) {
        return ecuio_recording_stats(v.r.get(), ids, count, from_us, to_us,
                                     out);
    }
    std::vector<ecuio_recording_column_t> cols(count);
    const ecuio_status_t rc = ecuio_recording_query(
        v.r.get(), ids, count, from_us, from_us + 1, cols.data(), 1);
    for (uint32_t k = 0; rc == ECUIO_OK && k < count; ++k) {
        const bool text =
            (v.signals[ids[k]].flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0;
        out[k] = cli::stats_of_instant(cols[k], text, from_us);
    }
    return rc;
}

// cli::columns_own_window's views: each shown signal's count, range and
// mean in every axis column from the reader's statistics over the column's
// own microseconds, a row of axis.columns per signal in cols and in means
ecuio_status_t own_columns(const Viewed& v, const cli::Axis& axis,
                           std::vector<ecuio_recording_column_t>& cols,
                           std::vector<double>& means)
{
    const size_t signals = v.shown.size();
    cols.assign(signals * axis.columns, {});
    std::vector<ecuio_recording_stats_t> stats(signals);
    for (uint32_t c = 0; c < axis.columns; ++c) {
        uint64_t from = 0;
        uint64_t to = 0;
        if (!cli::own_span(axis, v.offset_us, c, from, to)) {
            continue;
        }
        const ecuio_status_t rc =
            closed_stats(v, v.shown.data(), static_cast<uint32_t>(signals),
                         from, to, stats.data());
        if (rc != ECUIO_OK) {
            return rc;
        }
        for (size_t n = 0; n < signals; ++n) {
            cols[n * axis.columns + c] = cli::column_of_stats(stats[n]);
            means[n * axis.columns + c] = stats[n].mean;
        }
    }
    return ECUIO_OK;
}

// The axis's last microsecond (the window's t1_us), which a query's
// half-open span leaves out: its samples into each signal's last column
ecuio_status_t add_end(const Viewed& v, const cli::Window& w,
                       std::vector<ecuio_recording_column_t>& cols)
{
    const size_t signals = v.shown.size();
    std::vector<ecuio_recording_column_t> got(signals);
    const ecuio_status_t rc = ecuio_recording_query(
        v.r.get(), v.shown.data(), static_cast<uint32_t>(signals), w.t1_us,
        w.t1_us + 1, got.data(), 1);
    for (size_t n = 0; rc == ECUIO_OK && n < signals; ++n) {
        cli::add_column(cols[(n + 1) * w.count - 1], got[n]);
    }
    return rc;
}

// Each shown signal's mean in each of the window's columns, into means
// (a row of axis_columns per signal, NaN where a column is empty or the
// signal is text): from the column's count and range where they give it,
// else from the reader's statistics over the samples the column counts
ecuio_status_t column_means(const Viewed& v, const cli::Window& w,
                            const cli::Axis& axis,
                            const std::vector<ecuio_recording_column_t>& cols,
                            std::vector<double>& means)
{
    const uint32_t axis_columns = axis.columns;
    const bool ends = cli::query_leaves_end(w, axis);
    const uint32_t partial = w.partial ? 1 : 0;
    std::vector<uint32_t> ids;
    std::vector<size_t> rows;
    std::vector<ecuio_recording_stats_t> stats;
    for (uint32_t j = 0; j < w.count; ++j) {
        ids.clear();
        rows.clear();
        for (size_t n = 0; n < v.shown.size(); ++n) {
            const ecuio_recording_signal_t& s = v.signals[v.shown[n]];
            double mean = NAN;
            if ((s.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0) {
                continue;
            }
            if (cli::mean_of_column(cols[n * w.count + j], mean)) {
                means[n * axis_columns + w.first + j] = mean;
            } else {
                ids.push_back(v.shown[n]);
                rows.push_back(n);
            }
        }
        uint64_t from = 0;
        uint64_t to = 0;
        bool spans = partial != 0 && j == 0
                         ? cli::column_span(0, w.partial_t1_us, 1, 0, from, to)
                         : cli::column_span(w.t0_us, w.t1_us, w.count - partial,
                                            j - partial, from, to);
        if (ends && j + 1 == w.count) {
            /* The last column's samples run to the axis's end, add_end's */
            from = spans ? from : w.t1_us;
            to = w.t1_us;
            spans = true;
        }
        if (ids.empty() || !spans || from >= to) {
            continue;
        }
        stats.assign(ids.size(), ecuio_recording_stats_t{});
        const ecuio_status_t rc = ecuio_recording_stats(
            v.r.get(), ids.data(), static_cast<uint32_t>(ids.size()), from, to,
            stats.data());
        if (rc != ECUIO_OK) {
            return rc;
        }
        for (size_t k = 0; k < ids.size(); ++k) {
            means[rows[k] * axis_columns + w.first + j] = stats[k].mean;
        }
    }
    return ECUIO_OK;
}

// kept's means views[i]'s signal k takes its delta from (*from, its place):
// the first before i showing k's pair as a number in k's unit; else NULL
const double* delta_source(const std::vector<Viewed>& views, uint32_t i,
                           uint32_t k, uint32_t columns,
                           const std::vector<std::vector<double>>& kept,
                           std::vector<std::vector<uint32_t>>& pairs,
                           uint32_t* from)
{
    const Viewed& v = views[i];
    for (uint32_t j = 0; j < i; ++j) {
        const Viewed& u = views[j];
        if (kept[j].empty()) {
            continue;
        }
        if (pairs[j].empty()) {
            pairs[j] = cli::pair_signals(
                static_cast<uint32_t>(v.signals.size()),
                [&](uint32_t n) -> const auto& { return v.signals[n]; },
                static_cast<uint32_t>(u.signals.size()),
                [&](uint32_t n) -> const auto& { return u.signals[n]; });
        }
        const uint32_t t = pairs[j][k];
        const auto at = std::find(u.shown.begin(), u.shown.end(), t);
        if (at == u.shown.end() ||
            (u.signals[t].flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0 ||
            !cli::same_unit(v.signals[k], u.signals[t])) {
            continue;
        }
        *from = j;
        return kept[j].data() +
               static_cast<size_t>(at - u.shown.begin()) * columns;
    }
    return nullptr;
}

} // namespace

// A recording read offline (ADR-0049 section 5): every recording opens,
// then the names are checked (a name in none is a usage error, before any
// output), then the lines. A failed open or query is an error line, exit 3.
int run_view(ViewArgs& args)
{
    Output out;
    const int used = use_view_signals(out, args);
    if (used >= 0) {
        return used;
    }
    // An interrupt ends the opens and searches at their next ask, and the
    // lines between recordings: the interrupted end line, exit 1
    install_handlers();
    bool asked = false;
    std::vector<Viewed> views(args.folders.size());
    for (size_t i = 0; i < args.folders.size(); ++i) {
        ecuio_recording_t* r = nullptr;
        std::string decoded;
        std::string why;
        const ecuio_status_t rc =
            open_viewed(args, args.folders[i], &r, &decoded, &why);
        if (rc == ECUIO_ERR_CANCELED && g_interrupted.load()) {
            write_start(out);
            return ended_interrupted(out);
        }
        if (rc != ECUIO_OK) {
            write_start(out);
            write_error(out, rc, why.c_str());
            return ended(out, kExitConnect);
        }
        Viewed& v = views[i];
        v.r.reset(r);
        v.decoded = decoded;
        ecuio_recording_get_info(r, &v.info);
        ecuio_recording_get_signals(r, &v.signal_file);
        ecuio_recording_get_conditions(r, &v.conditions);
        v.signals.resize(v.info.signal_count);
        for (uint32_t k = 0; k < v.info.signal_count; ++k) {
            ecuio_recording_get_signal(r, k, &v.signals[k]);
        }
    }
    auto in_any = [&](const std::string& name) {
        return std::any_of(views.begin(), views.end(), [&](const Viewed& v) {
            return v.find(name) != UINT32_MAX;
        });
    };
    // A name two signals of one recording have names neither: usage, in
    // core's words, which list their keys
    const auto several = [&](const std::string& name) {
        for (const Viewed& v : views) {
            bool many = false;
            (void)v.find(name, &many);
            if (many) {
                std::fprintf(stderr, "havne-cli: recording %u: %s\n",
                             static_cast<unsigned>(&v - views.data()),
                             ecuio_last_error_detail());
                return true;
            }
        }
        return false;
    };
    for (const std::string& name : args.signals) {
        if (several(name)) {
            return kExitUsage;
        }
        if (!in_any(name)) {
            std::fprintf(stderr, "havne-cli: no recording has a signal '%s'\n",
                         name.c_str());
            return kExitUsage;
        }
    }
    const bool threshold = args.align.kind == cli::AlignOn::rise ||
                           args.align.kind == cli::AlignOn::fall;
    if (threshold) {
        if (several(args.align.signal)) {
            return kExitUsage;
        }
        if (!in_any(args.align.signal)) {
            std::fprintf(stderr, "havne-cli: no recording has a signal '%s'\n",
                         args.align.signal.c_str());
            return kExitUsage;
        }
        for (const Viewed& v : views) {
            const uint32_t k = v.find(args.align.signal);
            if (k != UINT32_MAX &&
                (v.signals[k].flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0) {
                std::fprintf(stderr,
                             "havne-cli: '%s' is text: it crosses no level\n",
                             args.align.signal.c_str());
                return kExitUsage;
            }
        }
    }

    // --role's names, each a number signal of every recording
    for (const auto& role : args.roles) {
        if (several(role.second)) {
            return kExitUsage;
        }
    }
    for (Viewed& v : views) {
        v.rules = args.rules;
        for (const auto& role : args.roles) {
            const uint32_t k = v.find(role.second);
            if (k == UINT32_MAX ||
                (v.signals[k].flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0) {
                std::fprintf(stderr,
                             "havne-cli: --role names '%s', which recording %u "
                             "has no number signal of\n",
                             role.second.c_str(),
                             static_cast<unsigned>(&v - views.data()));
                return kExitUsage;
            }
            cli::role_field(v.rules, role.first) = k;
        }
    }

    // The signals each shows: those named, in order, or its ranked ones by
    // rank
    for (Viewed& v : views) {
        if (!args.signals.empty()) {
            for (const std::string& name : args.signals) {
                const uint32_t k = v.find(name);
                if (k != UINT32_MAX) {
                    v.shown.push_back(k);
                }
            }
        } else {
            for (uint32_t k = 0; k < v.signals.size(); ++k) {
                if (v.signals[k].plot_rank != 0) {
                    v.shown.push_back(k);
                }
            }
            std::stable_sort(
                v.shown.begin(), v.shown.end(), [&](uint32_t a, uint32_t b) {
                    return v.signals[a].plot_rank < v.signals[b].plot_rank;
                });
        }
    }

    // Where each recording's zero lands: the event at 0, or the UTC
    // starts' differences from the earliest
    uint64_t utc_min = UINT64_MAX;
    for (const Viewed& v : views) {
        if (v.info.utc_start_us != 0) {
            utc_min = (std::min)(utc_min, v.info.utc_start_us);
        }
    }
    for (Viewed& v : views) {
        switch (args.align.kind) {
        case cli::AlignOn::none:
            break;
        case cli::AlignOn::utc:
            v.aligned = v.info.utc_start_us != 0;
            if (v.aligned) {
                v.offset_us =
                    static_cast<int64_t>(v.info.utc_start_us - utc_min);
            }
            break;
        default: {
            ecuio_recording_find_t what{};
            if (threshold) {
                what.kind = ECUIO_RECORDING_FIND_THRESHOLD;
                what.signal = v.find(args.align.signal);
                what.direction = args.align.kind == cli::AlignOn::rise
                                     ? ECUIO_RECORDING_FIND_RISING
                                     : ECUIO_RECORDING_FIND_FALLING;
                what.level = args.align.level;
                what.hysteresis = args.hysteresis;
            } else if (args.align.kind == cli::AlignOn::engine_start) {
                what.kind = ECUIO_RECORDING_FIND_ENGINE_START;
            } else {
                what.kind = ECUIO_RECORDING_FIND_MARKER;
                what.label_utf8 = args.align.label.c_str();
            }
            uint64_t at = ECUIO_RECORDING_NONE;
            if (!threshold || what.signal != UINT32_MAX) {
                const std::vector<uint32_t>& pulls = args.align.pulls;
                const ecuio_status_t rc =
                    args.align.kind == cli::AlignOn::pull
                        ? nth_pull(
                              v,
                              pulls[pulls.size() == 1 ? 0
                                                      : static_cast<size_t>(
                                                            &v - views.data())],
                              &asked, &at)
                        : ecuio_recording_find_with(v.r.get(), &what, 0,
                                                    view_interrupted, &asked,
                                                    &at);
                if (rc == ECUIO_ERR_CANCELED) {
                    write_start(out);
                    return ended_interrupted(out);
                }
                if (rc != ECUIO_OK) {
                    write_start(out);
                    write_error(out, rc, cli::read_error_detail(rc));
                    return ended(out, kExitConnect);
                }
            }
            v.aligned = at != ECUIO_RECORDING_NONE;
            v.align_host_us = at;
            v.offset_us = v.aligned ? -static_cast<int64_t>(at) : 0;
            break;
        }
        }
    }

    // The axis: given, or from the first recording's start to the last's
    // end on it
    int64_t from = INT64_MAX;
    int64_t to = INT64_MIN;
    for (const Viewed& v : views) {
        if (v.aligned) {
            from = (std::min)(from, v.offset_us);
            to = (std::max)(to, v.offset_us +
                                    static_cast<int64_t>(v.info.duration_us));
        }
    }
    if (from == INT64_MAX) {
        from = 0;
        to = 0;
    }
    if (to <= from) {
        to = from + 1000000; /* nothing recorded: a second */
    }
    if (args.has_from) {
        from = args.from_us;
    }
    if (args.has_to) {
        to = args.to_us;
    }
    cli::Axis axis;
    if (!cli::make_axis(from, to, args.columns, axis)) {
        std::fprintf(stderr, "havne-cli: --from must come before --to, here "
                             "on the recordings' axis\n");
        return kExitUsage;
    }
    for (const cli::Span& span : args.spans) {
        if (span.from_us < axis.from_us || span.to_us > axis.to_us) {
            std::fprintf(stderr,
                         "havne-cli: a --span lies outside the axis, %.6g s "
                         "to %.6g s\n",
                         static_cast<double>(axis.from_us) / 1e6,
                         static_cast<double>(axis.to_us) / 1e6);
            return kExitUsage;
        }
    }

    write_start(out);
    char buf[2048];
    for (uint32_t i = 0; i < views.size(); ++i) {
        const Viewed& v = views[i];
        cli::Line l(buf, sizeof buf);
        cli::opened_line(l, i, v.info, v.signal_file, v.conditions,
                         v.decoded.empty() ? nullptr : v.decoded.c_str());
        write_line(out, l);
        for (uint32_t k = 0; k < v.signals.size(); ++k) {
            write_signal_line(out, v.r.get(), i, k, v.signals[k]);
        }
        for (uint32_t k = 0; k < v.info.marker_count; ++k) {
            ecuio_recording_marker_t m{};
            ecuio_recording_get_marker(v.r.get(), k, &m);
            BigLine b(256 + static_cast<size_t>(m.label_length) * 6);
            cli::marker_line(b.line, i, k, m);
            write_line(out, b.line);
        }
        write_condition_lines(out, v.r.get(), i, v.info);
    }
    if (args.align.kind != cli::AlignOn::none) {
        for (uint32_t i = 0; i < views.size(); ++i) {
            const Viewed& v = views[i];
            BigLine b(512 + std::strlen(args.align_text) * 6);
            cli::align_line(b.line, i, args.align_text, v.aligned,
                            v.align_host_us, v.offset_us);
            write_line(out, b.line);
        }
    }
    for (uint32_t i = 0; args.stretches && i < views.size(); ++i) {
        const Viewed& v = views[i];
        const ecuio_recording_signal_t* by_role[4] = {};
        const uint32_t order[4] = {ECUIO_RECORDING_ROLE_THROTTLE,
                                   ECUIO_RECORDING_ROLE_ENGINE_LOAD,
                                   ECUIO_RECORDING_ROLE_VEHICLE_SPEED,
                                   ECUIO_RECORDING_ROLE_ENGINE_SPEED};
        for (size_t n = 0; n < 4; ++n) {
            ecuio_stretch_rules_t rules = v.rules;
            uint32_t k = cli::role_field(rules, order[n]);
            if (k == UINT32_MAX) {
                ecuio_recording_role_signal(v.r.get(), order[n], &k);
            }
            by_role[n] = k != UINT32_MAX ? &v.signals[k] : nullptr;
        }
        cli::Line l(buf, sizeof buf);
        cli::roles_line(l, i, by_role, v.rules);
        write_line(out, l);
        /* The axis in the recording's own time; nothing before its start */
        const int64_t t0 = (std::max<int64_t>)(axis.from_us - v.offset_us, 0);
        const int64_t t1 = axis.to_us - v.offset_us;
        if (!v.aligned || t1 <= t0) {
            continue;
        }
        uint32_t count = 0;
        ecuio_status_t rc = ecuio_recording_stretches_with(
            v.r.get(), &v.rules, static_cast<uint64_t>(t0),
            static_cast<uint64_t>(t1), view_interrupted, &asked, nullptr, 0,
            &count);
        std::vector<ecuio_recording_stretch_t> found(count);
        if (rc == ECUIO_OK && count != 0) {
            bool second = false;
            rc = ecuio_recording_stretches_with(
                v.r.get(), &v.rules, static_cast<uint64_t>(t0),
                static_cast<uint64_t>(t1), view_second_interrupted, &second,
                found.data(), count, &count);
            found.resize((std::min<size_t>)(found.size(), count));
        }
        if (rc == ECUIO_ERR_CANCELED) {
            return ended_interrupted(out);
        }
        if (rc != ECUIO_OK) {
            write_error(out, rc, cli::read_error_detail(rc));
            return ended(out, kExitConnect);
        }
        for (const ecuio_recording_stretch_t& s : found) {
            cli::Line sl(buf, sizeof buf);
            cli::stretch_line(sl, i, s, v.signals[s.signal], v.offset_us);
            write_line(out, sl);
        }
    }
    {
        cli::Line l(buf, sizeof buf);
        cli::axis_line(l, axis);
        write_line(out, l);
    }
    std::vector<ecuio_recording_column_t> cols;
    // Each aligned recording's means, which a later one's delta is taken from
    std::vector<std::vector<double>> kept(views.size());
    for (uint32_t i = 0; i < views.size(); ++i) {
        const Viewed& v = views[i];
        if (g_interrupted.load()) {
            return ended_interrupted(out);
        }
        if (!v.aligned || v.shown.empty()) {
            continue;
        }
        cli::Window w;
        const bool window = cli::window_of(axis, v.offset_us, w);
        const size_t signals = v.shown.size();
        /* Statistics for every axis column and no query: an aligned view's
         * wide columns, and a recording only the axis's end reaches */
        const bool by_stats =
            axis.to_us - v.offset_us >= 0 &&
            (!window || cli::columns_own_window(
                            args.align.kind != cli::AlignOn::none, axis));
        const bool any = by_stats || window;
        if (by_stats) {
            w = cli::Window{};
            w.count = axis.columns;
        } else if (window) {
            /* Signal by signal, w.count columns each: the partial column's
             * own query first, then the rest's */
            cols.assign(signals * w.count, {});
            const uint32_t partial = w.partial ? 1 : 0;
            const uint32_t rest = w.count - partial;
            std::vector<ecuio_recording_column_t> got(signals *
                                                      (std::max)(rest, 1u));
            ecuio_status_t rc = ECUIO_OK;
            if (partial != 0) {
                rc = ecuio_recording_query(v.r.get(), v.shown.data(),
                                           static_cast<uint32_t>(signals), 0,
                                           w.partial_t1_us, got.data(), 1);
                for (size_t n = 0; rc == ECUIO_OK && n < signals; ++n) {
                    cols[n * w.count] = got[n];
                }
            }
            if (rc == ECUIO_OK && rest != 0) {
                rc = ecuio_recording_query(v.r.get(), v.shown.data(),
                                           static_cast<uint32_t>(signals),
                                           w.t0_us, w.t1_us, got.data(), rest);
                for (size_t n = 0; rc == ECUIO_OK && n < signals; ++n) {
                    std::copy(
                        got.begin() + static_cast<std::ptrdiff_t>(n * rest),
                        got.begin() +
                            static_cast<std::ptrdiff_t>((n + 1) * rest),
                        cols.begin() +
                            static_cast<std::ptrdiff_t>(n * w.count + partial));
                }
            }
            if (rc == ECUIO_OK && cli::query_leaves_end(w, axis)) {
                rc = add_end(v, w, cols);
            }
            if (rc != ECUIO_OK) {
                write_error(out, rc, cli::read_error_detail(rc));
                return ended(out, kExitConnect);
            }
        }
        std::vector<double> means(signals * axis.columns, NAN);
        const ecuio_status_t mean_rc =
            !any       ? ECUIO_OK
            : by_stats ? own_columns(v, axis, cols, means)
                       : column_means(v, w, axis, cols, means);
        if (mean_rc != ECUIO_OK) {
            write_error(out, mean_rc, cli::read_error_detail(mean_rc));
            return ended(out, kExitConnect);
        }
        std::vector<double> delta(axis.columns);
        std::vector<std::vector<uint32_t>> pairs(i);
        for (size_t n = 0; n < v.shown.size(); ++n) {
            const ecuio_recording_signal_t& s = v.signals[v.shown[n]];
            const double* mean = means.data() + n * axis.columns;
            const double* less = nullptr;
            uint32_t delta_from = 0;
            const double* first =
                (s.flags & ECUIO_RECORDING_SIGNAL_TEXT) != 0
                    ? nullptr
                    : delta_source(views, i, v.shown[n], axis.columns, kept,
                                   pairs, &delta_from);
            if (first != nullptr) {
                /* NaN where either column is empty */
                for (uint32_t c = 0; c < axis.columns; ++c) {
                    delta[c] = mean[c] - first[c];
                }
                less = delta.data();
            }
            BigLine b(cli::columns_line_bytes(axis.columns));
            cli::columns_line(b.line, i, s, w, axis.columns,
                              any ? cols.data() + n * w.count : nullptr, mean,
                              less, delta_from);
            write_line(out, b.line);
        }
        if (i + 1 < views.size()) {
            kept[i] = std::move(means); // the last one's are never a delta's
        }
    }
    if (args.has_at) {
        std::vector<char> text(32768);
        for (uint32_t i = 0; i < views.size(); ++i) {
            const Viewed& v = views[i];
            if (!v.aligned) {
                continue;
            }
            const int64_t t = args.at_us - v.offset_us;
            BigLine b(256 + v.shown.size() *
                                (128 + cli::kKeyFieldBytes + text.size() * 6));
            cli::at_begin(b.line, i, args.at_us);
            for (size_t n = 0; n < v.shown.size(); ++n) {
                uint64_t host = ECUIO_RECORDING_NONE;
                text[0] = '\0';
                if (t >= 0) {
                    const ecuio_status_t rc = ecuio_recording_text_at(
                        v.r.get(), v.shown[n], static_cast<uint64_t>(t),
                        text.data(), static_cast<uint32_t>(text.size()), &host);
                    if (rc != ECUIO_OK) {
                        write_error(out, rc, cli::read_error_detail(rc));
                        return ended(out, kExitConnect);
                    }
                }
                cli::at_value(b.line, n == 0, v.signals[v.shown[n]], host,
                              text.data());
            }
            cli::at_end(b.line);
            write_line(out, b.line);
        }
    }
    for (const cli::Span& span : args.spans) {
        for (uint32_t i = 0; i < views.size(); ++i) {
            const Viewed& v = views[i];
            if (g_interrupted.load()) {
                return ended_interrupted(out);
            }
            if (!v.aligned || v.shown.empty()) {
                continue;
            }
            /* The span in the recording's own time: a part before its
             * start holds nothing, and nothing is in force there */
            const int64_t t0 = span.from_us - v.offset_us;
            const int64_t t1 = span.to_us - v.offset_us;
            std::vector<ecuio_recording_stats_t> st(v.shown.size(),
                                                    cli::no_stats());
            if (t1 >= 0) {
                const ecuio_status_t rc = closed_stats(
                    v, v.shown.data(), static_cast<uint32_t>(v.shown.size()),
                    static_cast<uint64_t>((std::max<int64_t>)(t0, 0)),
                    static_cast<uint64_t>(t1), st.data());
                if (rc != ECUIO_OK) {
                    write_error(out, rc, cli::read_error_detail(rc));
                    return ended(out, kExitConnect);
                }
                for (ecuio_recording_stats_t& s : st) {
                    if (t0 < 0) {
                        s.first = NAN;
                    }
                }
            }
            for (size_t n = 0; n < v.shown.size(); ++n) {
                cli::Line l(buf, sizeof buf);
                cli::stats_line(l, i, v.signals[v.shown[n]], span, st[n]);
                write_line(out, l);
            }
        }
    }
    return ended(out, kExitOk);
}

} // namespace cmd
