// File/layer: tests/test_elm327_support.h | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: the helpers of test_elm327_session*.cpp (test_elm327)

#pragma once

#include <gtest/gtest.h>

#include "ecuio/conditions.h"
#include "ecuio/core.h"
#include "ecuio/internal.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "ecuio/recording.h"
#include "elm327_log.h" /* core/src */
#include "elm327_protocol.h" /* core/src: elm327_pack_count */
#include "elm327_port.h" /* core/src */
#include "elm327_tcp.h"  /* core/src */
#include "elm327_stream.h" /* core/src */
#include "ecuio/stream.h"
#include "car_sized_set.h"
#include "fake_diagnostics.h"
#include "fake_elm327.h"

#include "open_handles.h"
#include "temp_name.h"
#include "worker_hold.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h> /* before <windows.h>, which pulls in winsock.h */
#include <ws2tcpip.h>
#include <windows.h>
#endif

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

/* A port name this OS's URIs take; the fake opener ignores it. */
#ifdef _WIN32
constexpr char kPort[] = "COM7";
#else
constexpr char kPort[] = "/dev/fake-elm327";
#endif

inline std::string uri(const std::string& query = "")
{
    return std::string("elm327://") + kPort + query;
}

namespace fs = std::filesystem;

} // namespace

/* Every session here writes its wire log into a directory of this run's
 * own, so a run never prunes the user's logs in the temporary
 * directory */
inline fs::path g_log_root;

namespace {

/* A directory of a test's own for the logs it reads; the run's again at
 * the end of the scope */
class LogDir
{
public:
    explicit LogDir(const char* name) : dir_(g_log_root / name)
    {
        fs::create_directories(dir_);
        ecuio_elm327_set_log_dir(dir_.u8string().c_str());
    }
    ~LogDir() { ecuio_elm327_set_log_dir(g_log_root.u8string().c_str()); }
    LogDir(const LogDir&) = delete;
    LogDir& operator=(const LogDir&) = delete;

    const fs::path& path() const { return dir_; }

    /* The names in the directory, sorted */
    std::vector<std::string> names() const
    {
        std::vector<std::string> v;
        for (const auto& e : fs::directory_iterator(dir_)) {
            v.push_back(e.path().filename().u8string());
        }
        std::sort(v.begin(), v.end());
        return v;
    }

    /* The one wire log here, whole */
    std::string only_log() const
    {
        std::vector<std::string> logs;
        for (const std::string& n : names()) {
            if (elm327_log_kept_name(n.c_str())) {
                logs.push_back(n);
            }
        }
        EXPECT_EQ(logs.size(), 1u);
        if (logs.empty()) {
            return "";
        }
        std::ifstream in(dir_ / logs.back(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

private:
    fs::path dir_;
};

/* Whether text holds each of parts, in this order */
inline ::testing::AssertionResult in_order(const std::string& text,
                                    const std::vector<std::string>& parts)
{
    size_t at = 0;
    for (const std::string& p : parts) {
        const size_t found = text.find(p, at);
        if (found == std::string::npos) {
            return ::testing::AssertionFailure()
                   << "'" << p << "' missing after offset " << at << " in:\n"
                   << text;
        }
        at = found + p.size();
    }
    return ::testing::AssertionSuccess();
}

/* An environment variable set for a scope, as the transport reads it
 * (GetEnvironmentVariable on Windows), restored after */
class EnvSet
{
public:
    EnvSet(const char* name, const char* value) : name_(name)
    {
#ifdef _WIN32
        char old[64];
        const DWORD n = GetEnvironmentVariableA(name, old, sizeof(old));
        had_ = n > 0 && n < sizeof(old);
        if (had_) {
            old_ = old;
        }
        SetEnvironmentVariableA(name, value);
#else
        const char* old = getenv(name);
        had_ = old != nullptr;
        if (had_) {
            old_ = old;
        }
        if (value != nullptr) {
            setenv(name, value, 1);
        } else {
            unsetenv(name);
        }
#endif
    }
    ~EnvSet()
    {
#ifdef _WIN32
        SetEnvironmentVariableA(name_.c_str(), had_ ? old_.c_str() : nullptr);
#else
        if (had_) {
            setenv(name_.c_str(), old_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }
    EnvSet(const EnvSet&) = delete;
    EnvSet& operator=(const EnvSet&) = delete;

private:
    std::string name_;
    std::string old_;
    bool had_ = false;
};

inline bool wait_for(const std::function<bool()>& pred, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

/* The worker's 5 s waits shortened for a scope; a running worker takes
 * them on its next pass */
class ShortTimeouts
{
public:
    ShortTimeouts(uint32_t prompt_ms, uint32_t monitor_check_ms)
    {
        ecuio_elm327_set_timeouts_ms(prompt_ms, monitor_check_ms);
    }
    ~ShortTimeouts() { ecuio_elm327_set_timeouts_ms(0, 0); }
    ShortTimeouts(const ShortTimeouts&) = delete;
    ShortTimeouts& operator=(const ShortTimeouts&) = delete;
};

inline std::string hex_of(const uint8_t* d, size_t n)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) {
            s += ' ';
        }
        s += digits[d[i] >> 4];
        s += digits[d[i] & 0x0F];
    }
    return s;
}

/* Every frame the session delivered: ID, flags and bytes, plus the
 * length, so a long reply is checked without its text. */
struct Capture {
    struct Frame {
        uint32_t id;
        uint32_t flags;
        std::vector<uint8_t> data;
    };
    std::mutex m;
    std::vector<Frame> frames;

    static void cb(const ecuio_frame_t* f, void* user)
    {
        auto* c = static_cast<Capture*>(user);
        std::lock_guard<std::mutex> g(c->m);
        c->frames.push_back(
            {f->arbitration_id, f->flags,
             std::vector<uint8_t>(f->data, f->data + f->data_length)});
    }

    /* Frames whose bytes start with prefix (hex, spaced). */
    size_t count(const std::string& prefix)
    {
        std::lock_guard<std::mutex> g(m);
        size_t n = 0;
        for (const Frame& f : frames) {
            n += hex_of(f.data.data(), f.data.size()).rfind(prefix, 0) == 0;
        }
        return n;
    }

    bool find(const std::string& prefix, Frame* out)
    {
        std::lock_guard<std::mutex> g(m);
        for (const Frame& f : frames) {
            if (hex_of(f.data.data(), f.data.size()).rfind(prefix, 0) == 0) {
                *out = f;
                return true;
            }
        }
        return false;
    }
};

/* A started session with a capture, destroyed at the end of the scope. */
struct Session {
    ecuio_session_t* s = nullptr;
    Capture cap;

    ecuio_status_t create(const std::string& u)
    {
        return ecuio_session_create(u.c_str(), &s);
    }
    void start()
    {
        ASSERT_NE(s, nullptr);
        ASSERT_EQ(ecuio_register_frame_callback(s, Capture::cb, &cap),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_start(s), ECUIO_OK);
    }
    int32_t state(uint32_t* episodes = nullptr)
    {
        int32_t last = -1;
        uint32_t count = 0;
        EXPECT_EQ(ecuio_session_get_error_state(s, &last, &count), ECUIO_OK);
        if (episodes != nullptr) {
            *episodes = count;
        }
        return last;
    }
    ~Session() { ecuio_session_destroy(s); }
};

/* Runs fn on the fake's adapter under its lock; returns its result. */
template <typename T, typename Fn> T on_fake(Fn&& fn)
{
    T out{};
    fake_elm327::with([&](fake_elm327::adapter& a) { out = fn(a); });
    return out;
}

inline uint32_t fake_count(const char* prefix)
{
    return on_fake<uint32_t>(
        [&](fake_elm327::adapter& a) { return a.count(prefix); });
}

/* The start's emissions probe's request (request_probe.h): 01 00 with no
 * count digit, typed once at attach under the functional header */
constexpr char kProbeCommand[] = "0100";

inline std::vector<std::string> fake_commands()
{
    return on_fake<std::vector<std::string>>([](fake_elm327::adapter& a) {
        std::vector<std::string> v;
        for (uint32_t i = 0; i < a.commands && i < a.kRecordDepth; ++i) {
            v.emplace_back(a.recorded[i]);
        }
        return v;
    });
}

inline void set_knobs(const std::function<void(fake_elm327::knobs&)>& fn)
{
    fake_elm327::with([&](fake_elm327::adapter& a) { fn(a.k); });
}

} // namespace

class Elm327SessionTest : public ::testing::Test
{
protected:
    fake_elm327::installed fake_;
};

namespace {

/* PIDs out of the fakes' ECU for a scope (fake_ecu::withheld_pids):
 * its bitmaps leave them out and asking gets 7F 01 12 */
struct Withheld {
    explicit Withheld(std::initializer_list<unsigned> pids)
    {
        for (unsigned p : pids) {
            fake_ecu::withheld_pids()[p / 32] |= 1u << (p % 32);
        }
    }
    ~Withheld()
    {
        for (std::atomic<uint32_t>& w : fake_ecu::withheld_pids()) {
            w = 0;
        }
    }
};

} // namespace

/* Five modules beside the engine ECU, each answering a DID of its own */
struct SixModules {
    SixModules()
    {
        EXPECT_TRUE(fake_ecu::set_module_dids(
            "7E9:D801=00 11;7EA:D802=00 12;7EB:D803=00 13;"
            "7EC:D804=00 14;7ED:D805=00 15"));
    }
    ~SixModules() { fake_ecu::set_module_dids(nullptr); }
    SixModules(const SixModules&) = delete;
    SixModules& operator=(const SixModules&) = delete;
};
const char kSixModulesQuery[] =
    "?profile=uds-generic&did=1E1C,7E9:D801,7EA:D802,7EB:D803,7EC:D804,"
    "7ED:D805&identify=0&diagnose=0";

namespace {

/* The fakes' ECUs back in the default session before and after a case
 * (the state is per process), and the module table empty */
struct SessionsReset {
    SessionsReset() { fake_ecu::reset_sessions(); }
    ~SessionsReset()
    {
        fake_ecu::set_module_dids(nullptr);
        fake_ecu::reset_sessions();
    }
};

constexpr char kExtendedQuery[] =
    "?profile=uds-generic&did=1E1C&session=03&identify=0&diagnose=0";

/* A session in the extended one, its first DID read */
inline void entered(Session& s)
{
    ASSERT_EQ(s.create(uri(kExtendedQuery)), ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(wait_for([&] { return s.cap.count("62 1E 1C") >= 1; }, 5000));
}

} // namespace

/* ---- Passive listening (ADR-0026's appendix "passive listening") ---- */

namespace {

/* A message on the fake's bus from CAN ID id, at once */
inline void on_bus(uint32_t id, const std::vector<uint8_t>& m)
{
    fake_elm327::with([&](fake_elm327::adapter& a) {
        a.bus(id, m.data(), m.size(), fake_elm327::steady_us());
    });
}

inline bool fake_monitoring()
{
    return on_fake<bool>(
        [](fake_elm327::adapter& a) { return a.monitoring(); });
}

/* 49 02 01 and a 17-character VIN: a multi-frame message */
const std::vector<uint8_t> kVinReply = {0x49, 0x02, 0x01, 'F', 'A', 'K', 'E',
                                        'M',  'O',  'N',  'I', 'T', 'O', 'R',
                                        'V',  'I',  'N',  '0', '0', '1'};

/* A session listening: started, and the fake monitoring */
inline void listen_on(Session& s, const std::string& u = uri("?profile=raw"))
{
    ASSERT_EQ(s.create(u), ECUIO_OK) << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(wait_for(fake_monitoring, 3000));
}

} // namespace

/* ---- The TCP port over loopback (ADR-0026's appendix "WiFi over TCP") --
 * The fake ELM327 behind a listener on 127.0.0.1, the transport's socket
 * connecting to it through the OS's opener: the real TCP code, its
 * timeouts and the far side's close, on every platform. Loopback only, so
 * no firewall asks. */

namespace {

#ifdef _WIN32
using test_sock = SOCKET;
const test_sock kNoSock = INVALID_SOCKET;
inline void close_sock(test_sock s)
{
    closesocket(s);
}
#else
using test_sock = int;
const test_sock kNoSock = -1;
inline void close_sock(test_sock s)
{
    close(s);
}
#endif

/* Whether s has something to read within ms */
inline bool readable(test_sock s, int ms)
{
    fd_set r;
    FD_ZERO(&r);
    FD_SET(s, &r);
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select(static_cast<int>(s) + 1, &r, nullptr, nullptr, &tv) > 0;
}

class AppStream
{
public:
    /* How the app answers an open */
    enum class answer { opened, failed, nothing };

    explicit AppStream(const char* name)
    {
        adapter_.clear();
        ecuio_stream_callbacks_t cb{};
        cb.open = on_open;
        cb.write = on_write;
        cb.close = on_close;
        cb.user = this;
        registered_ = ecuio_stream_register(name, &cb, &stream_);
        pump_ = std::thread([this] { run(); });
    }
    ~AppStream()
    {
        stop_ = true;
        if (pump_.joinable()) {
            pump_.join();
        }
        if (stream_ != nullptr) {
            (void)ecuio_stream_unregister(stream_);
        }
    }
    AppStream(const AppStream&) = delete;
    AppStream& operator=(const AppStream&) = delete;

    ecuio_status_t registered() const { return registered_; }
    ecuio_status_t unregister()
    {
        const ecuio_status_t st = ecuio_stream_unregister(stream_);
        if (st == ECUIO_OK) {
            stream_ = nullptr;
        }
        return st;
    }
    void answer_with(answer a, const char* reason = "")
    {
        std::lock_guard<std::mutex> g(lock_);
        answer_ = a;
        reason_ = reason;
    }
    /* The link drops under the session */
    void fail(const char* reason)
    {
        {
            std::lock_guard<std::mutex> g(lock_);
            up_ = false;
        }
        ecuio_stream_failed(stream_, reason);
    }
    void push(const std::string& bytes)
    {
        ecuio_stream_received(stream_,
                              reinterpret_cast<const uint8_t*>(bytes.data()),
                              static_cast<uint32_t>(bytes.size()));
    }
    uint32_t opens() const { return opens_.load(); }
    uint32_t closes() const { return closes_.load(); }
    uint32_t requests()
    {
        std::lock_guard<std::mutex> g(lock_);
        return adapter_.requests;
    }

private:
    ecuio_stream_t* stream_ = nullptr;
    ecuio_status_t registered_ = ECUIO_ERR_NOT_IMPLEMENTED;
    std::mutex lock_;
    fake_elm327::adapter adapter_;
    answer answer_ = answer::opened;
    std::string reason_;
    bool up_ = false;
    std::atomic<bool> open_asked_{false};
    std::atomic<uint32_t> opens_{0};
    std::atomic<uint32_t> closes_{0};
    std::atomic<bool> stop_{false};
    std::thread pump_;

    /* Core's calls return at once, as the ADR asks: the open is answered
     * from the pump thread */
    static void on_open(void* user)
    {
        auto* self = static_cast<AppStream*>(user);
        self->opens_.fetch_add(1);
        self->open_asked_ = true;
    }
    static void on_write(void* user, const uint8_t* data, uint32_t len)
    {
        auto* self = static_cast<AppStream*>(user);
        std::lock_guard<std::mutex> g(self->lock_);
        if (self->up_) {
            self->adapter_.from_host(data, len, fake_elm327::steady_us());
        }
    }
    static void on_close(void* user)
    {
        auto* self = static_cast<AppStream*>(user);
        self->closes_.fetch_add(1);
        std::lock_guard<std::mutex> g(self->lock_);
        self->up_ = false;
    }

    void run()
    {
        uint8_t buf[512];
        while (!stop_.load()) {
            if (open_asked_.exchange(false)) {
                answer a = answer::nothing;
                std::string reason;
                {
                    std::lock_guard<std::mutex> g(lock_);
                    a = answer_;
                    reason = reason_;
                    if (a == answer::opened) {
                        up_ = true;
                        adapter_.drop_output();
                    }
                }
                if (a == answer::opened) {
                    ecuio_stream_opened(stream_);
                } else if (a == answer::failed) {
                    ecuio_stream_failed(stream_, reason.c_str());
                }
            }
            size_t n = 0;
            {
                std::lock_guard<std::mutex> g(lock_);
                if (up_) {
                    n = adapter_.to_host(buf, sizeof(buf),
                                         fake_elm327::steady_us());
                }
            }
            if (n > 0) {
                ecuio_stream_received(stream_, buf, static_cast<uint32_t>(n));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
};

constexpr char kBt[] = "bt:00:11:22:33:44:55";

inline std::string bt_uri(const std::string& query)
{
    return std::string("elm327://") + kBt + query;
}

/* Conditions c0, c1, ... on these whens, attached to the session */
inline void attach_whens(ecuio_session_t* s, const std::vector<std::string>& whens)
{
    std::string json = R"({"havne_conditions":1,"conditions":[)";
    for (size_t i = 0; i < whens.size(); ++i) {
        const std::string k = std::to_string(i);
        json += (i == 0 ? "" : ",") + std::string(R"({"key":"c)") + k +
                R"(","name":"C)" + k + R"(","when":)" + whens[i] + "}";
    }
    json += "]}";
    ecuio_condition_set_t* set = nullptr;
    ASSERT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_set_conditions(s, set, nullptr), ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_condition_set_free(set);
}

} // namespace
