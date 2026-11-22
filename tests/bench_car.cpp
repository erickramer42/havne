// File/layer: tests/bench_car.cpp | bench_car, Qt-free, C++17
// Testing: none (a measurement, not in ctest; no tier): a car-sized set's
//          sizes, memory, rotation, focus and load with no car (ADR-0065
//          "To measure"); docs/performance.md records its figures
//
// bench_car [signals=4000] [modules=18] [reps=7] [--allow-debug]
//
// One line for each figure on stdout: "name value unit", grouped by
// section. Timings are the median and worst of reps (one warm-up first);
// sizes and bytes are exact and the same on every run. Exits 1 only on a
// wrong answer (a refused create or load, a set that doesn't hold the
// counts asked), never on a time. A build without NDEBUG prints no
// figures unless --allow-debug is given, so a Debug figure never reaches
// a row. Allocation is counted here: operator new is replaced with one
// that keeps live and peak bytes across every thread.
//
// The engine's own calls (the rotation's step, ecuio_take_focus) are the
// internals this reaches; they sit in Worker below, the one place a
// rename of them touches.

#include <ecuio/core.h>
#include <ecuio/plugin.h>
#include <ecuio/profile.h>
#include <ecuio/signals.h>

#include "bench_car_set.h"
#include "health_model.h" /* core/src */
#include "isotp.h"        /* core/src */
#include "request_engine.h" /* core/src */
#include "request_setup.h"  /* core/src */
#include "session.h"        /* core/src */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#ifndef ECUIO_PLUGINS_DIR
#error "tests/CMakeLists.txt defines ECUIO_PLUGINS_DIR, the repository's plugins/"
#endif

/* ---- Allocation, counted in bytes ---- */
namespace mem {

constexpr size_t kHead = 16; /* the size, kept ahead of the block */
std::atomic<size_t> g_live{0};
std::atomic<size_t> g_peak{0};

void* take(size_t n) noexcept
{
    unsigned char* p = static_cast<unsigned char*>(std::malloc(n + kHead));
    if (p == nullptr) {
        return nullptr;
    }
    std::memcpy(p, &n, sizeof n);
    const size_t live = g_live.fetch_add(n) + n;
    size_t peak = g_peak.load();
    while (live > peak && !g_peak.compare_exchange_weak(peak, live)) {
    }
    return p + kHead;
}

void give(void* block) noexcept
{
    if (block == nullptr) {
        return;
    }
    unsigned char* p = static_cast<unsigned char*>(block) - kHead;
    size_t n = 0;
    std::memcpy(&n, p, sizeof n);
    g_live.fetch_sub(n);
    std::free(p);
}

void* take_or_throw(size_t n)
{
    void* p = take(n);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

size_t live()
{
    return g_live.load();
}

/* Peak counts from now */
void reset_peak()
{
    g_peak.store(g_live.load());
}

size_t peak()
{
    return g_peak.load();
}

} // namespace mem

void* operator new(size_t n)
{
    return mem::take_or_throw(n);
}
void* operator new[](size_t n)
{
    return mem::take_or_throw(n);
}
void* operator new(size_t n, const std::nothrow_t&) noexcept
{
    return mem::take(n);
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept
{
    return mem::take(n);
}
void operator delete(void* p) noexcept
{
    mem::give(p);
}
void operator delete[](void* p) noexcept
{
    mem::give(p);
}
void operator delete(void* p, size_t) noexcept
{
    mem::give(p);
}
void operator delete[](void* p, size_t) noexcept
{
    mem::give(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept
{
    mem::give(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept
{
    mem::give(p);
}

namespace {

using bench_car_set::file;

constexpr uint32_t kPicks = 20;
constexpr unsigned kCalls = 200; /* focus calls timed for each figure */
constexpr unsigned kSteps = 200000;

int g_wrong = 0;

void wrong(const char* what)
{
    std::printf("WRONG %s\n", what);
    ++g_wrong;
}

double now_us()
{
    using namespace std::chrono;
    return duration<double, std::micro>(steady_clock::now().time_since_epoch())
        .count();
}

struct Stat {
    double median = 0;
    double worst = 0;
};

Stat stat_of(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return Stat{v[v.size() / 2], v.back()};
}

void line(const char* name, double value, const char* unit)
{
    std::printf("%s %.3f %s\n", name, value, unit);
}

void line_n(const char* name, size_t value, const char* unit)
{
    std::printf("%s %zu %s\n", name, value, unit);
}

void timing(const char* name, const Stat& s, const char* unit)
{
    char n[96];
    std::snprintf(n, sizeof n, "%s_median", name);
    line(n, s.median, unit);
    std::snprintf(n, sizeof n, "%s_worst", name);
    line(n, s.worst, unit);
}

/* ---- The engine's internals, reached in this one place ---- */
struct Worker {
    ecuio_session_t* session = nullptr;
    std::unique_ptr<ecuio_request_engine> engine; /* made by build */
    ecuio_focus_seen seen;

    /* The requests built as a requesting attach builds them (no probe, no
     * identification, no diagnostics); false when the build refused */
    bool build(const ecuio_signal_set_t* set)
    {
        engine = std::make_unique<ecuio_request_engine>();
        request_params params;
        params.probe = false;
        params.identify = false;
        params.diagnose = false;
        bool probe = false;
        bool identify = false;
        return ecuio_build_requests(
                   session, set, params, session->module_rx.data(),
                   session->module_tx.data(), session->info.module_count,
                   engine.get(), &probe, &identify) == ECUIO_OK;
    }
    int32_t step(uint32_t channel)
    {
        return ecuio_engine_next_of(engine.get(), channel, false);
    }
    void take_focus() { ecuio_take_focus(session, engine.get(), seen); }
    void forget_maps() { seen.table = 0; }
    uint32_t rows() const { return engine->count; }
    uint32_t rotating() const
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < engine->count; ++i) {
            n += ecuio_engine_in_rotation(engine->requests[i]) ? 1u : 0u;
        }
        return n;
    }
    uint32_t modules() const { return session->info.module_count; }
};

void close(Worker& w)
{
    w.engine.reset();
    ecuio_session_destroy(w.session);
}

/* A mock session on a set registered under name, or on the default
 * profile when set is NULL, and its requests built */
bool open(Worker& w, const char* name, const ecuio_signal_set_t* set)
{
    const std::string uri =
        set != nullptr ? std::string("mock://car?profile=") + name
                       : std::string("mock://car");
    if (ecuio_session_create(uri.c_str(), &w.session) != ECUIO_OK) {
        wrong("session create refused");
        return false;
    }
    if (!w.build(set)) {
        wrong("build requests refused");
        return false;
    }
    return true;
}

struct Held {
    size_t live = 0;
    size_t peak = 0;
    uint32_t modules = 0;
    uint32_t rows = 0;
};

Held held_by(Worker& w, size_t before)
{
    return Held{mem::live() - before, mem::peak() - before, w.modules(),
                w.rows()};
}

/* 1: bytes of the state each module carries */
void sizes(unsigned modules)
{
    std::printf("# 1. sizeof a module's state, and times %u modules\n", modules);
    const struct {
        const char* name;
        size_t size;
    } rows[] = {
        {"engine_channel", sizeof(ecuio_engine_channel)},
        {"request_row", sizeof(ecuio_request)},
        {"support_report", sizeof(ecuio_channel_support_t)},
        {"identity_report", sizeof(ecuio_channel_identity_t)},
        {"diagnostics_report", sizeof(ecuio_channel_diagnostics_t)},
        {"health_ecu", sizeof(ecuio_health::ecu)},
        {"isotp_rx_slot", sizeof(isotp_rx_slot)},
        {"isotp_tx", sizeof(isotp_tx)},
    };
    for (const auto& r : rows) {
        char n[64];
        std::snprintf(n, sizeof n, "sizeof_%s", r.name);
        line_n(n, r.size, "bytes");
        std::snprintf(n, sizeof n, "sizeof_%s_x_modules", r.name);
        line_n(n, r.size * modules, "bytes");
    }
}

/* 2: what a session holds, the empty one and the set's */
bool memory(Worker& car, const char* name, const ecuio_signal_set_t* set)
{
    std::printf("# 2. a session's memory: create and build requests\n");
    Held empty;
    {
        Worker w;
        const size_t before = mem::live();
        mem::reset_peak();
        if (!open(w, name, nullptr)) {
            return false;
        }
        empty = held_by(w, before);
        close(w);
        line_n("empty_session_live", empty.live, "bytes");
        line_n("empty_session_peak", empty.peak, "bytes");
        line("empty_session_left_behind",
             double(mem::live()) - double(before), "bytes");
    }
    const size_t before = mem::live();
    mem::reset_peak();
    if (!open(car, name, set)) {
        return false;
    }
    const Held full = held_by(car, before);
    line_n("session_modules", full.modules, "modules");
    line_n("session_request_rows", full.rows, "rows");
    line_n("session_live", full.live, "bytes");
    line_n("session_peak", full.peak, "bytes");
    const size_t extra = full.live - std::min(full.live, empty.live);
    line_n("set_extra_live", extra, "bytes");
    line("set_extra_per_module", double(extra) / full.modules, "bytes");
    line("set_extra_per_signal", double(extra) / set->count, "bytes");
    line("peak_over_live", double(full.peak) / double(full.live), "x");
    return true;
}

/* 3: the rotation's step with nothing picked and a pick of 20 */
double steps_ns(Worker& w, unsigned reps)
{
    std::vector<double> ns;
    for (unsigned r = 0; r <= reps; ++r) {
        int64_t sink = 0;
        const double t0 = now_us();
        for (unsigned i = 0; i < kSteps; ++i) {
            sink += w.step(0);
        }
        const double t = (now_us() - t0) * 1000.0 / kSteps;
        if (sink == 0) {
            wrong("the rotation gave no request");
        }
        if (r > 0) {
            ns.push_back(t);
        }
    }
    return stat_of(ns).median;
}

std::vector<uint16_t> picks_of(const ecuio_signal_set_t* set, unsigned first)
{
    std::vector<uint16_t> ids;
    const uint32_t rx = set->channels[0].rx_id;
    for (uint32_t i = 0; i < set->count && ids.size() < first + kPicks; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0 && d.message_id == rx) {
            ids.push_back(d.id);
        }
    }
    return std::vector<uint16_t>(ids.begin() + first, ids.end());
}

/* 3 and 4: the rotation's step, and the focus calls' own work on the
 * calling thread (no transport: the work is the same, the hop is not) */
bool focus_costs(Worker& w, const ecuio_signal_set_t* set, unsigned reps)
{
    std::printf("# 3. rotation and focus, 4. one focus change\n");
    const auto a = picks_of(set, 0);
    const auto b = picks_of(set, kPicks);
    if (a.size() != kPicks || b.size() != kPicks) {
        wrong("module 0 has too few numbers to pick");
        return false;
    }
    line_n("rows_in_rotation_nothing_picked", w.rotating(), "rows");
    line("step_nothing_picked", steps_ns(w, reps), "ns");
    const auto focus = [&](const std::vector<uint16_t>& ids) {
        return ecuio_session_set_focus(w.session, ids.data(),
                                       static_cast<uint32_t>(ids.size()));
    };
    if (focus(a) != ECUIO_OK) {
        wrong("focus refused");
        return false;
    }
    w.take_focus();
    line_n("rows_in_rotation_pick_of_20", w.rotating(), "rows");
    if (w.rotating() != kPicks) {
        wrong("a pick of 20 leaves other than 20 rows in the rotation");
    }
    line("step_pick_of_20", steps_ns(w, reps), "ns");

    std::vector<double> set_in, refresh, plain, end, change;
    const auto timed = [&](std::vector<double>& into, auto&& call) {
        const double t0 = now_us();
        call();
        into.push_back(now_us() - t0);
    };
    for (unsigned i = 0; i < kCalls; ++i) {
        timed(set_in, [&] { (void)focus(a); });
        w.forget_maps();
        timed(refresh, [&] { w.take_focus(); });
        (void)focus({});
        timed(end, [&] { w.take_focus(); });
        (void)focus(a);
        timed(plain, [&] { w.take_focus(); });
        timed(change, [&] {
            (void)focus(b);
            w.take_focus();
        });
        timed(change, [&] {
            (void)focus(a);
            w.take_focus();
        });
    }
    (void)focus({});
    w.take_focus();
    timing("set_focus_call", stat_of(set_in), "us");
    timing("take_focus_in_with_map_refresh", stat_of(refresh), "us");
    timing("take_focus_in_maps_current", stat_of(plain), "us");
    timing("take_focus_end", stat_of(end), "us");
    timing("focus_change", stat_of(change), "us");
    return true;
}

/* 5: the loader alone over generated text at three sizes */
void loader(unsigned modules, unsigned reps)
{
    std::printf("# 5. ecuio_signal_file_load_with, a set's bounds\n");
    for (const unsigned n : {1000u, 4000u, ECUIO_SIGNAL_SET_MAX_SIGNALS}) {
        const std::string text = file(n, modules);
        std::vector<double> ms;
        for (unsigned r = 0; r <= reps; ++r) {
            ecuio_signal_file_t* f = nullptr;
            const double t0 = now_us();
            const ecuio_status_t st = ecuio_signal_file_load_with(
                text.data(), text.size(), ECUIO_SIGNAL_FILE_LOAD_SET, &f);
            const double t = (now_us() - t0) / 1000.0;
            if (st != ECUIO_OK || f == nullptr) {
                wrong("a generated file's load was refused");
                std::printf("  %s\n", ecuio_last_error_detail());
                return;
            }
            if (ecuio_signal_file_set(f)->count != n) {
                wrong("the loaded set's signal count is not the generator's");
            }
            ecuio_signal_file_free(f);
            if (r > 0) {
                ms.push_back(t);
            }
        }
        char name[48];
        std::snprintf(name, sizeof name, "load_%u_signals", n);
        line_n((std::string(name) + "_file_bytes").c_str(), text.size(),
               "bytes");
        timing(name, stat_of(ms), "ms");
    }
}

/* 6: a variant's written copy, bytes for each signal */
void copy_line(const char* name, ecuio_plugin_t* plugin)
{
    const ecuio_signal_file_t* f = ecuio_plugin_variant_file(plugin, 0);
    size_t bytes = 0;
    (void)ecuio_signal_file_text(f, &bytes);
    const size_t signals = ecuio_signal_file_set(f)->count;
    char n[64];
    std::snprintf(n, sizeof n, "%s_copy_bytes", name);
    line_n(n, bytes, "bytes");
    std::snprintf(n, sizeof n, "%s_copy_signals", name);
    line_n(n, signals, "signals");
    const double per = double(bytes) / double(signals);
    std::snprintf(n, sizeof n, "%s_copy_bytes_per_signal", name);
    line(n, per, "bytes");
    std::snprintf(n, sizeof n, "%s_signals_in_4mib", name);
    line(n, ECUIO_SIGNAL_SET_MAX_BYTES / per, "signals");
}

int read_package(void* user, const char* path, const char** bytes,
                 size_t* length)
{
    auto* files = static_cast<std::map<std::string, std::string>*>(user);
    auto it = files->find(path);
    if (it == files->end()) {
        std::ifstream in(std::string(ECUIO_PLUGINS_DIR) + "/example-car/" + path,
                         std::ios::binary);
        if (!in) {
            return 1;
        }
        std::ostringstream all;
        all << in.rdbuf();
        it = files->emplace(path, all.str()).first;
    }
    *bytes = it->second.data();
    *length = it->second.size();
    return 0;
}

void written_copies(unsigned modules)
{
    std::printf("# 6. a set's written copy\n");
    const std::string text = file(1000, modules);
    ecuio_plugin_wrap_options_t o{};
    o.struct_size = sizeof o;
    o.id = "bench-car";
    o.variant = "base";
    char* packed = nullptr;
    size_t packed_length = 0;
    ecuio_plugin_t* generated = nullptr;
    if (ecuio_plugin_wrap(text.data(), text.size(), &o, &packed,
                          &packed_length) != ECUIO_OK ||
        ecuio_plugin_load(packed, packed_length, nullptr, nullptr,
                          &generated) != ECUIO_OK) {
        wrong("the generated file would not wrap");
        std::printf("  %s\n", ecuio_last_error_detail());
    } else {
        copy_line("generated", generated);
    }
    ecuio_plugin_text_free(packed);
    ecuio_plugin_free(generated);

    std::map<std::string, std::string> files;
    const char* manifest = nullptr;
    size_t manifest_length = 0;
    ecuio_plugin_t* car = nullptr;
    if (read_package(&files, "manifest.json", &manifest, &manifest_length) !=
            0 ||
        ecuio_plugin_load(manifest, manifest_length, read_package, &files,
                          &car) != ECUIO_OK) {
        wrong("example-car would not load");
        std::printf("  %s\n", ecuio_last_error_detail());
        return;
    }
    copy_line("example_car_whole", car);
    ecuio_plugin_free(car);
}

/* A generated set loaded and registered as a profile, then undone */
struct Loaded {
    ecuio_signal_file_t* file = nullptr;
    const ecuio_signal_set_t* set = nullptr;
    const char* name;
    bool registered = false;

    Loaded(unsigned signals, unsigned modules, const char* profile)
        : name(profile)
    {
        const std::string text = bench_car_set::file(signals, modules);
        if (ecuio_signal_file_load_with(text.data(), text.size(),
                                        ECUIO_SIGNAL_FILE_LOAD_SET,
                                        &file) != ECUIO_OK) {
            wrong("the generated file was refused");
            std::printf("  %s\n", ecuio_last_error_detail());
            return;
        }
        set = ecuio_signal_file_set(file);
        const unsigned asked = bench_car_set::modules_for(signals, modules);
        if (set->count != signals || set->channel_count != asked) {
            wrong("the loaded set does not hold the counts asked");
        }
        ecuio_validation_t v{};
        registered = ecuio_profile_register(name, set, &v) == ECUIO_OK;
        if (!registered) {
            wrong("the profile was refused");
        }
    }
    ~Loaded()
    {
        if (registered) {
            ecuio_profile_unregister(name);
        }
        ecuio_signal_file_free(file);
    }
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;
    bool ok() const { return registered; }
};

/* A set whose requests all fit one rotation (a session asks 1024 at
 * most; past that it asks only what is picked): every row is asked */
void whole_rotation(unsigned modules, unsigned reps)
{
    std::printf("# 3b. a set the rotation holds whole\n");
    Loaded car(1100, modules, "bench-car-whole");
    Worker w;
    if (!car.ok() || !open(w, car.name, car.set)) {
        return;
    }
    line_n("whole_rotation_rows", w.rows(), "rows");
    if (w.rotating() != w.rows()) {
        wrong("a set within the rotation has rows out of it");
    }
    line("step_whole_rotation", steps_ns(w, reps), "ns");
    close(w);
}

/* The generated set, run through 2 to 4 */
bool car_sized(unsigned signals, unsigned modules, unsigned reps)
{
    Loaded car(signals, modules, "bench-car");
    if (!car.ok()) {
        return false;
    }
    Worker w;
    bool ok = memory(w, car.name, car.set);
    if (ok && w.modules() != car.set->channel_count) {
        wrong("the session holds other than the asked modules");
    }
    ok = ok && focus_costs(w, car.set, reps);
    close(w);
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    unsigned numbers[3] = {4000, 18, 7};
    unsigned seen = 0;
    bool allow_debug = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--allow-debug") == 0) {
            allow_debug = true;
        } else if (seen < 3) {
            numbers[seen++] = static_cast<unsigned>(std::atoi(argv[i]));
        }
    }
    const unsigned signals = numbers[0];
    const unsigned modules = numbers[1];
    const unsigned reps = std::max(numbers[2], 1u);
    if (signals == 0 || signals > ECUIO_SIGNAL_SET_MAX_SIGNALS || modules == 0 ||
        modules > ECUIO_MAX_CHANNELS) {
        std::printf("bench_car [signals<=%u] [modules<=%u] [reps]\n",
                    ECUIO_SIGNAL_SET_MAX_SIGNALS, unsigned{ECUIO_MAX_CHANNELS});
        return 2;
    }
#ifdef NDEBUG
    const int optimized = 1;
    (void)allow_debug;
#else
    const int optimized = 0;
    if (!allow_debug) {
        std::printf("bench_car: not an optimized build; no figures without "
                    "--allow-debug\n");
        return 2;
    }
#endif
    std::printf("build optimized=%d pointer_bits=%d signals=%u modules=%u "
                "reps=%u\n",
                optimized, int(sizeof(void*) * 8), signals, modules, reps);
    sizes(bench_car_set::modules_for(signals, modules));
    car_sized(signals, modules, reps);
    whole_rotation(modules, reps);
    loader(modules, reps);
    written_copies(modules);
    return g_wrong != 0 ? 1 : 0;
}
