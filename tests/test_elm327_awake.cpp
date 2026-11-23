// File/layer: tests/test_elm327_awake.cpp | GTest, UNIT and HARNESS tiers,
//             headless CI job (every platform)
// Testing: UNIT: Elm327AwakeTest through the seam; HARNESS: its platform
//          cases (test_elm327)
//
// A live session's keep-awake (ADR-0033's appendix "the host kept awake
// while a session reads"), on the fake ELM327: the hold until the close,
// a 0 that releases at once, a refusal, two sessions, core's own claim
// for a write step, and a release or the close while a take is held in the
// seam. The platform cases take the real hold.

#include "test_elm327_support.h"
#include "awake_seam.h"
#include "host_awake.h" /* core/src: ecuio_awake_core_hold */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>

#if defined(__linux__) && !defined(__ANDROID__)
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <unistd.h>
#endif
#endif

class Elm327AwakeTest : public ::testing::Test
{
protected:
    fake_elm327::installed fake_;
    AwakeSeam seam_;
};

namespace {

uint32_t awake(const ecuio_session_t* s)
{
    uint32_t what = 99;
    EXPECT_EQ(ecuio_session_get_awake(s, &what), ECUIO_OK);
    return what;
}

} // namespace

TEST_F(Elm327AwakeTest, ALiveSessionHoldsUntilItsClose)
{
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM, "a drive"),
              ECUIO_OK);
    EXPECT_EQ(AwakeSeam::takes.load(), 1);
    EXPECT_EQ(AwakeSeam::last_level.load(), 0u) << "a read's idle level";
    EXPECT_EQ(AwakeSeam::why(), "a drive");
    EXPECT_EQ(awake(s.s), ECUIO_AWAKE_SYSTEM);
    s.start();
    ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    EXPECT_EQ(AwakeSeam::releases.load(), 1);
    EXPECT_EQ(awake(s.s), 0u);
}

TEST_F(Elm327AwakeTest, ASecondCallTakesNothingMore)
{
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM, nullptr),
              ECUIO_OK);
    EXPECT_EQ(AwakeSeam::why(), "Reading a vehicle") << "the neutral default";
    ASSERT_EQ(ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM, "again"),
              ECUIO_OK);
    EXPECT_EQ(AwakeSeam::takes.load(), 1);
    EXPECT_EQ(AwakeSeam::releases.load(), 0);
}

TEST_F(Elm327AwakeTest, KeepAwakeWithZeroReleasesAtOnce)
{
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM, nullptr),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_keep_awake(s.s, 0, nullptr), ECUIO_OK);
    EXPECT_EQ(AwakeSeam::releases.load(), 1) << "before the call returns";
    EXPECT_EQ(awake(s.s), 0u);
    ASSERT_EQ(ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM, nullptr),
              ECUIO_OK);
    EXPECT_EQ(AwakeSeam::takes.load(), 2) << "taken again after a release";
    ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    EXPECT_EQ(AwakeSeam::releases.load(), 2);
}

TEST_F(Elm327AwakeTest, ARefusedHoldSaysWhyAndTheSessionRuns)
{
    AwakeSeam::refuse_with("no logind here");
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM, nullptr),
              ECUIO_ERR_NOT_IMPLEMENTED);
    EXPECT_STREQ(ecuio_last_error_detail(), "no logind here");
    EXPECT_EQ(awake(s.s), 0u);
    s.start();
    EXPECT_TRUE(wait_for([&] { return s.cap.count("41 0C") >= 1; }, 5000))
        << "the session reads on";
    ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    EXPECT_EQ(AwakeSeam::releases.load(), 0) << "nothing was held";
}

TEST_F(Elm327AwakeTest, TwoSessionsHoldApart)
{
    Session a;
    ASSERT_EQ(a.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    Session b;
    ASSERT_EQ(b.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_keep_awake(a.s, ECUIO_AWAKE_SYSTEM, nullptr),
              ECUIO_OK);
    ASSERT_EQ(ecuio_session_keep_awake(b.s, ECUIO_AWAKE_SYSTEM, nullptr),
              ECUIO_OK);
    EXPECT_EQ(AwakeSeam::takes.load(), 2);
    ASSERT_EQ(ecuio_session_close(a.s), ECUIO_OK);
    EXPECT_EQ(AwakeSeam::releases.load(), 1);
    EXPECT_EQ(awake(a.s), 0u);
    EXPECT_EQ(awake(b.s), ECUIO_AWAKE_SYSTEM) << "the other's stays";
}

/* A write step's claim (L12's) outlasts the caller's 0, at its own level */
TEST_F(Elm327AwakeTest, ZeroLeavesCoresOwnHold)
{
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_awake_core_hold(s.s, true), ECUIO_OK);
    EXPECT_EQ(AwakeSeam::last_level.load(), 1u) << "a write's sleep level";
    ASSERT_EQ(ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM, nullptr),
              ECUIO_OK);
    EXPECT_EQ(AwakeSeam::takes.load(), 2);
    ASSERT_EQ(ecuio_session_keep_awake(s.s, 0, nullptr), ECUIO_OK);
    EXPECT_EQ(AwakeSeam::releases.load(), 1);
    EXPECT_EQ(awake(s.s), ECUIO_AWAKE_SYSTEM) << "the write's holds";
    ASSERT_EQ(ecuio_awake_core_hold(s.s, false), ECUIO_OK);
    EXPECT_EQ(AwakeSeam::releases.load(), 2);
    EXPECT_EQ(awake(s.s), 0u);
    ASSERT_EQ(ecuio_awake_core_hold(s.s, true), ECUIO_OK);
    ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    EXPECT_EQ(AwakeSeam::releases.load(), 3) << "the close drops it too";
    EXPECT_EQ(ecuio_awake_core_hold(s.s, true), ECUIO_ERR_SESSION_CLOSED);
}

namespace {

/* A take on a thread of its own, held at the seam's shut gate */
struct HeldTake {
    std::thread t;
    std::atomic<int> status{-1000};
    template <typename Fn> explicit HeldTake(Fn fn)
    {
        AwakeSeam::shut();
        t = std::thread([this, fn] { status = fn(); });
    }
    ~HeldTake() { finish(); }
    HeldTake(const HeldTake&) = delete;
    HeldTake& operator=(const HeldTake&) = delete;
    int finish()
    {
        AwakeSeam::open();
        if (t.joinable()) {
            t.join();
        }
        return status.load();
    }
};

/* Whether fn returns within 5 s while the take is held; a call waiting on
 * the take returns only once the gate opens, after the 5 s */
template <typename Fn> bool returns_while_held(Fn fn)
{
    auto done = std::async(std::launch::async, fn);
    const bool at_once = done.wait_for(std::chrono::seconds(5)) ==
                         std::future_status::ready;
    AwakeSeam::open();
    done.wait();
    return at_once;
}

std::atomic<int> g_port_open_at_release{-1};

void note_the_port()
{
    std::lock_guard<std::mutex> g(fake_elm327::state().lock);
    g_port_open_at_release = fake_elm327::state().open ? 1 : 0;
}

} // namespace

TEST_F(Elm327AwakeTest, AZeroDuringATakeReturnsAtOnceAndWins)
{
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    HeldTake take([&] {
        return ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM, nullptr);
    });
    ASSERT_TRUE(AwakeSeam::a_take_is_held());
    EXPECT_TRUE(returns_while_held(
        [&] { return ecuio_session_keep_awake(s.s, 0, nullptr); }))
        << "the 0 waited on the take";
    EXPECT_EQ(take.finish(), ECUIO_OK);
    EXPECT_EQ(AwakeSeam::takes.load(), 1);
    EXPECT_EQ(AwakeSeam::releases.load(), 1) << "the take that lost";
    EXPECT_EQ(awake(s.s), 0u) << "the 0 came last";
}

/* The close racing a take: neither waits, and nothing stays held */
TEST_F(Elm327AwakeTest, ACloseDuringATakeReturnsAtOnceAndHoldsNothing)
{
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    HeldTake take([&] {
        return ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM, nullptr);
    });
    ASSERT_TRUE(AwakeSeam::a_take_is_held());
    EXPECT_TRUE(returns_while_held([&] { return ecuio_session_close(s.s); }))
        << "the close waited on the take";
    EXPECT_EQ(take.finish(), ECUIO_ERR_SESSION_CLOSED);
    EXPECT_EQ(AwakeSeam::releases.load(), AwakeSeam::takes.load());
    EXPECT_EQ(awake(s.s), 0u);
}

TEST_F(Elm327AwakeTest, CoresFalseDuringItsTakeReturnsAtOnceAndWins)
{
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    HeldTake take([&] { return ecuio_awake_core_hold(s.s, true); });
    ASSERT_TRUE(AwakeSeam::a_take_is_held());
    EXPECT_TRUE(returns_while_held(
        [&] { return ecuio_awake_core_hold(s.s, false); }));
    EXPECT_EQ(take.finish(), ECUIO_OK);
    EXPECT_EQ(AwakeSeam::releases.load(), 1);
    EXPECT_EQ(awake(s.s), 0u);
}

/* The hold covers the whole close: released once the port is shut */
TEST_F(Elm327AwakeTest, TheCloseReleasesAfterTheDetach)
{
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM, nullptr),
              ECUIO_OK);
    s.start();
    g_port_open_at_release = -1;
    AwakeSeam::on_release = &note_the_port;
    ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    AwakeSeam::on_release = nullptr;
    EXPECT_EQ(AwakeSeam::releases.load(), 1);
    EXPECT_EQ(g_port_open_at_release.load(), 0) << "released before the detach";
}

// ---- The platform's own hold ----

#if defined(_WIN32)

/* Windows lists a request only to an administrator: the take itself */
TEST_F(Elm327AwakeTest, WindowsTakesARealRequest)
{
    AwakeSeam::off();
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM,
                                       "Elm327AwakeTest"),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(awake(s.s), ECUIO_AWAKE_SYSTEM);
    ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    EXPECT_EQ(awake(s.s), 0u);
}

#elif defined(__linux__) && !defined(__ANDROID__)

namespace {

/* systemd-inhibit's list, whole; "" without the tool */
std::string inhibitors()
{
    FILE* p = popen("systemd-inhibit --list --no-pager 2>/dev/null", "r");
    if (p == nullptr) {
        return {};
    }
    std::string out;
    char buf[512];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) {
        out.append(buf, n);
    }
    pclose(p);
    return out;
}

/* Whether the take's words are polkit's refusal of the lock (a runner's
 * user with no logind session), as logind answers it */
bool polkit_refused(const std::string& detail)
{
    for (const char* words :
         {"logind: Access denied", "logind: Permission denied",
          "logind: Interactive authentication required"}) {
        if (detail.find(words) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/* Both buses' addresses for a scope, the earlier values back after it */
struct BusAddresses {
    std::string old_system, old_session;
    bool had_system = false, had_session = false;
    explicit BusAddresses(const std::string& address)
    {
        keep("DBUS_SYSTEM_BUS_ADDRESS", &old_system, &had_system);
        keep("DBUS_SESSION_BUS_ADDRESS", &old_session, &had_session);
        setenv("DBUS_SYSTEM_BUS_ADDRESS", address.c_str(), 1);
        setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), 1);
    }
    ~BusAddresses()
    {
        put("DBUS_SYSTEM_BUS_ADDRESS", old_system, had_system);
        put("DBUS_SESSION_BUS_ADDRESS", old_session, had_session);
    }
    BusAddresses(const BusAddresses&) = delete;
    BusAddresses& operator=(const BusAddresses&) = delete;
    static void keep(const char* name, std::string* value, bool* had)
    {
        const char* v = getenv(name);
        *had = v != nullptr;
        *value = v != nullptr ? v : "";
    }
    static void put(const char* name, const std::string& value, bool had)
    {
        if (had) {
            setenv(name, value.c_str(), 1);
        } else {
            unsetenv(name);
        }
    }
};

} // namespace

/* Where logind lists its locks, the take must hold one there: polkit's
 * refusal skips, any other refusal fails */
TEST_F(Elm327AwakeTest, LogindListsTheHoldUntilTheClose)
{
    if (inhibitors().find("WHO") == std::string::npos) {
        GTEST_SKIP() << "no logind here (systemd-inhibit can't list)";
    }
    AwakeSeam::off();
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    const char* why = "Elm327AwakeTest-logind";
    const ecuio_status_t st =
        ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM, why);
    const std::string detail = ecuio_last_error_detail();
    if (st != ECUIO_OK && polkit_refused(detail)) {
        GTEST_SKIP() << "polkit refused this user the lock: " << detail;
    }
    ASSERT_EQ(st, ECUIO_OK) << detail;
    const std::string held = inhibitors();
    EXPECT_NE(held.find(why), std::string::npos) << held;
    ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    EXPECT_EQ(inhibitors().find(why), std::string::npos);
}

/* A bus that takes the connection and never answers: the take refuses
 * inside its second, where sd_bus_call alone waits out its own, longer
 * bound on the authentication */
TEST_F(Elm327AwakeTest, AWedgedBusRefusesInsideTheSecond)
{
    const char* tmp = getenv("TMPDIR");
    const std::string path = std::string(tmp != nullptr ? tmp : "/tmp") +
                             "/havne-wedged-" + std::to_string(getpid());
    const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(listener, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ASSERT_LT(path.size(), sizeof addr.sun_path);
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    unlink(path.c_str());
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr),
              0);
    ASSERT_EQ(listen(listener, 4), 0);
    ecuio_status_t st = ECUIO_OK;
    std::string detail;
    std::chrono::steady_clock::duration took{};
    {
        BusAddresses wedged("unix:path=" + path);
        AwakeSeam::off();
        Session s;
        ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
        const auto from = std::chrono::steady_clock::now();
        st = ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM, nullptr);
        took = std::chrono::steady_clock::now() - from;
        detail = ecuio_last_error_detail();
    }
    close(listener);
    unlink(path.c_str());
    if (detail.find("libsystemd.so.0") != std::string::npos) {
        GTEST_SKIP() << detail;
    }
    EXPECT_EQ(st, ECUIO_ERR_NOT_IMPLEMENTED);
    EXPECT_NE(detail.find("timed out"), std::string::npos) << detail;
    EXPECT_LT(took, std::chrono::seconds(5)) << "the second's bound";
}

#elif defined(__APPLE__) && TARGET_OS_OSX

namespace {

/* Whether this process holds an assertion of that name */
bool asserted(const char* name)
{
    CFDictionaryRef all = nullptr;
    if (IOPMCopyAssertionsByProcess(&all) != kIOReturnSuccess ||
        all == nullptr) {
        return false;
    }
    const int pid = getpid();
    CFNumberRef key = CFNumberCreate(nullptr, kCFNumberIntType, &pid);
    const auto* mine =
        static_cast<CFArrayRef>(CFDictionaryGetValue(all, key));
    CFStringRef want = CFStringCreateWithCString(nullptr, name,
                                                 kCFStringEncodingUTF8);
    bool found = false;
    for (CFIndex i = 0; mine != nullptr && i < CFArrayGetCount(mine); ++i) {
        const auto* a =
            static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(mine, i));
        const auto* n = static_cast<CFStringRef>(
            CFDictionaryGetValue(a, CFSTR(kIOPMAssertionNameKey)));
        found = found || (n != nullptr && CFEqual(n, want));
    }
    CFRelease(want);
    CFRelease(key);
    CFRelease(all);
    return found;
}

} // namespace

TEST_F(Elm327AwakeTest, MacOsListsTheAssertion)
{
    AwakeSeam::off();
    Session s;
    ASSERT_EQ(s.create(uri()), ECUIO_OK) << ecuio_last_error_detail();
    const char* why = "Elm327AwakeTest-macos";
    ASSERT_EQ(ecuio_session_keep_awake(s.s, ECUIO_AWAKE_SYSTEM, why),
              ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_TRUE(asserted(why));
    ASSERT_EQ(ecuio_session_close(s.s), ECUIO_OK);
    EXPECT_FALSE(asserted(why));
}

#endif
